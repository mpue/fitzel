#include "SongPanel.hpp"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <imgui.h>
#include <imgui_internal.h>

#include <fitzel/asset/Vfs.hpp>
#include <fitzel/audio/Audio.hpp>
#include <fitzel/audio/Mixer.hpp>
#include <fitzel/audio/SampleData.hpp>
#include <fitzel/audio/Vst3.hpp>

#include "FolderDialog.hpp"
#include "SongMidiIn.hpp"
#include "SongPanelState.hpp"
#include "UiStyle.hpp"

namespace songui {

namespace fsys = std::filesystem;
using sg::Clip;
using sg::Note;
using sg::Track;
using sg::TrackKind;
using sg::InstrumentKind;

// --- Looks ---------------------------------------------------------------------------

namespace {
const ImU32 kPalette[12] = {
    IM_COL32(231, 96, 76, 255),  IM_COL32(236, 140, 52, 255), IM_COL32(232, 196, 40, 255),
    IM_COL32(110, 196, 92, 255), IM_COL32(48, 190, 160, 255), IM_COL32(64, 160, 226, 255),
    IM_COL32(120, 120, 236, 255), IM_COL32(170, 104, 210, 255), IM_COL32(226, 110, 170, 255),
    IM_COL32(150, 164, 170, 255), IM_COL32(200, 120, 70, 255), IM_COL32(80, 150, 120, 255),
};

} // namespace

ImU32 trackColor(const sg::Song& s, const Track& t) {
    if (t.color) return t.color;
    for (std::size_t i = 0; i < s.tracks.size(); ++i)
        if (s.tracks[i].uid == t.uid) return kPalette[i % 12];
    return kPalette[0];
}

const std::vector<SnapDef>& snaps() {
    static const std::vector<SnapDef> v = {
        {"Bar", 0},  {"1/2", 1920},    {"1/4", 960},     {"1/8", 480},      {"1/16", 240},
        {"1/32", 120}, {"1/4 T", 640}, {"1/8 T", 320},   {"1/16 T", 160},   {"Off", 1},
    };
    return v;
}

const char* drumName(int p) {
    static const char* names[] = {
        "Kick 2", "Kick", "Side stick", "Snare", "Clap", "Snare 2", "Low tom 2", "Closed hat", "Low tom",
        "Pedal hat", "Mid tom 2", "Open hat", "Mid tom", "High tom 2", "Crash", "High tom", "Ride",
        "China", "Ride bell", "Tambourine", "Splash", "Cowbell", "Crash 2", "Vibraslap", "Ride 2",
        "High bongo", "Low bongo", "Mute conga", "High conga", "Low conga", "High timbale", "Low timbale",
        "High agogo", "Low agogo", "Cabasa", "Maracas", "Short whistle", "Long whistle", "Short guiro",
        "Long guiro", "Claves", "High block", "Low block", "Mute cuica", "Open cuica", "Mute triangle",
        "Open triangle"};
    if (p < 35 || p > 81) return nullptr;
    return names[p - 35];
}

namespace {

// The raw MIDI devices there are, with the card's name. Looked for every two
// seconds, not every frame: a keyboard plugged in shows up soon enough.
std::vector<std::pair<std::string, std::string>> findMidiDevices();

const std::vector<std::pair<std::string, std::string>>& midiDevices() {
    static std::vector<std::pair<std::string, std::string>> cache;
    static double at = -100.0;
    if (ImGui::GetTime() - at > 2.0) {
        cache = findMidiDevices();
        at = ImGui::GetTime();
    }
    return cache;
}

std::vector<std::pair<std::string, std::string>> findMidiDevices() {
    std::vector<std::pair<std::string, std::string>> out;
#ifdef __linux__
    std::error_code ec;
    for (const auto& e : fsys::directory_iterator("/dev/snd", ec)) {
        const std::string n = e.path().filename().string();
        int card = 0, dev = 0;
        if (std::sscanf(n.c_str(), "midiC%dD%d", &card, &dev) != 2) continue;
        // The port's own name ("KL Essential 61 mk3"), else the card's.
        std::string label = n, name;
        std::ifstream port("/proc/asound/card" + std::to_string(card) + "/midi" + std::to_string(dev));
        std::ifstream id("/proc/asound/card" + std::to_string(card) + "/id");
        if ((port && std::getline(port, name) && !name.empty()) || (id && std::getline(id, name) && !name.empty()))
            label = name + " (" + n + ")";
        out.push_back({e.path().string(), label});
    }
    std::sort(out.begin(), out.end());
#endif
    return out;
}

} // namespace

// --- State helpers -------------------------------------------------------------------------

void Panel::State::edited() {
    ++rev;
    dirty = true;
}

void Panel::State::checkpoint(const sg::Song& before) {
    undo.push_back({before, sel});
    if (undo.size() > 100) undo.erase(undo.begin());
    redo.clear();
}

std::int64_t Panel::State::gridOf(int i) const {
    const auto& sn = snaps();
    i = std::clamp(i, 0, static_cast<int>(sn.size()) - 1);
    return sn[static_cast<std::size_t>(i)].ticks == 0 ? song.ticksPerBar() : sn[static_cast<std::size_t>(i)].ticks;
}

std::int64_t Panel::State::snapTo(std::int64_t t, int i) const {
    if (ImGui::GetIO().KeyAlt) return t;
    const std::int64_t q = gridOf(i);
    if (q <= 1) return t;
    return static_cast<std::int64_t>(std::llround(static_cast<double>(t) / static_cast<double>(q))) * q;
}

bool Panel::State::isSel(int uid) const { return std::find(sel.begin(), sel.end(), uid) != sel.end(); }

void Panel::State::select(int uid, bool add) {
    if (!add) sel.clear();
    if (uid >= 0 && !isSel(uid)) sel.push_back(uid);
}

namespace { double g_idleTick = 0.0; }   // the playhead with no audio device to keep it

double Panel::State::playTicks() const {
    return player.isValid() ? player.ticks() : g_idleTick;
}

bool Panel::State::playing() const { return player.isValid() && player.engine().playing(); }

void Panel::State::seek(std::int64_t t) {
    t = std::max<std::int64_t>(0, t);
    if (player.isValid()) player.engine().seek(t);
    else g_idleTick = static_cast<double>(t);
}

void Panel::State::liveNote(int trackUid, int pitch, int vel, bool on) {
    if (player.isValid()) player.engine().liveNote(trackUid, pitch, static_cast<float>(vel) / 127.0f, on);
}

void Panel::State::audition(int trackUid, int pitch, int vel) {
    if (heard.track >= 0 && heard.pitch >= 0) liveNote(heard.track, heard.pitch, 0, false);
    liveNote(trackUid, pitch, vel, true);
    heard = {trackUid, pitch, ImGui::GetTime() + 0.25};
}

std::string Panel::State::soundPath(const std::string& name) const {
    if (!host || !host->resolveSound) return name;
    const double now = ImGui::GetTime();
    auto it = pathCache.find(name);
    if (it != pathCache.end() && now - it->second.second < 3.0) return it->second.first;
    std::string path = host->resolveSound(name);
    pathCache[name] = {path, now};
    return path;
}

const fitzel::Waveform* Panel::State::waveform(const std::string& name) {
    if (name.empty()) return nullptr;
    const std::string path = soundPath(name);
    auto& slot = waves[path];
    if (!slot) {
        slot = std::make_unique<WaveSlot>();
        slot->job = std::async(std::launch::async, [path] {
            fitzel::Waveform w;
            fitzel::readWaveform(path, w, 200.0f);
            return w;
        });
    }
    if (!slot->ready && slot->job.valid() &&
        slot->job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        slot->wave  = slot->job.get();
        slot->ready = true;
    }
    return slot->ready ? &slot->wave : nullptr;
}

double Panel::State::fileLength(const std::string& name) {
    if (name.empty()) return 0.0;
    const std::string path = soundPath(name);
    auto it = fileSeconds.find(path);
    if (it == fileSeconds.end()) it = fileSeconds.emplace(path, fitzel::soundFileSeconds(path)).first;
    return it->second;
}

void Panel::State::say(const std::string& msg, bool bad) {
    status    = msg;
    statusBad = bad;
    statusAt  = ImGui::GetTime();
    std::fprintf(stderr, "[song] %s\n", msg.c_str());
}

std::string Panel::State::songsDir() const {
    if (projectFile.empty()) return {};
    return (fsys::path(projectFile).parent_path() / "content" / "songs").generic_string();
}

namespace {

using State = Panel::State;

// --- The last song, remembered -----------------------------------------------------------
std::string g_lastSong;

void remember(const std::string& path) {
    if (path == g_lastSong) return;
    g_lastSong = path;
    if (ImGui::GetCurrentContext()) ImGui::MarkIniSettingsDirty();
}

void* iniOpen(ImGuiContext*, ImGuiSettingsHandler*, const char*) { return &g_lastSong; }
void iniLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
    if (std::strncmp(line, "LastSong=", 9) == 0) g_lastSong = line + 9;
}
void iniWrite(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
    if (g_lastSong.empty()) return;
    buf->appendf("[%s][State]\nLastSong=%s\n\n", h->TypeName, g_lastSong.c_str());
}

// --- Songs -------------------------------------------------------------------------------

sg::Song starterSong() {
    sg::Song s;
    s.name = "Song";
    Track& d = s.addTrack(TrackKind::Instrument, "Drums");
    d.instrument.kind = InstrumentKind::Drums;
    Track& k = s.addTrack(TrackKind::Instrument, "Keys");
    k.instrument.patch = "builtin:Keys";
    Track& b = s.addTrack(TrackKind::Instrument, "Bass");
    b.instrument.patch = "builtin:Bass";
    s.loopEnd = s.ticksPerBar() * 4;
    return s;
}

void resetView(State& st) {
    st.sel.clear();
    st.selTrack = st.song.tracks.empty() ? -1 : st.song.tracks.front().uid;
    st.editClip = -1;
    st.ppt = 0.0;
    st.view0 = 0.0;
    st.prPpt = 0.0;
    st.prFitFor = -1;
    st.recording = false;
    st.drag = {};
    st.pr = {};
}

void stopTransport(State& st) {
    if (st.player.isValid()) {
        st.player.engine().stop();
        st.player.engine().allNotesOff();
    }
    st.recording = false;
}

void newSong(State& st) {
    stopTransport(st);
    st.song  = starterSong();
    st.path.clear();
    st.dirty = false;
    st.undo.clear();
    st.redo.clear();
    resetView(st);
    st.seek(0);
    st.edited();
    st.dirty = false;
}

std::vector<std::string> songFiles(const State& st) {
    std::vector<std::string> out;
    const std::string dir = st.songsDir();
    std::error_code ec;
    if (dir.empty() || !fsys::is_directory(dir, ec)) return out;
    for (const auto& e : fsys::directory_iterator(dir, ec))
        if (e.path().extension() == ".json") out.push_back(e.path().generic_string());
    std::sort(out.begin(), out.end());
    return out;
}

bool openSong(State& st, const std::string& path) {
    sg::Song s;
    std::string err;
    if (!sg::Song::load(path, s, &err)) {
        st.say("Could not open " + fsys::path(path).filename().string() + ": " + err, true);
        return false;
    }
    stopTransport(st);
    st.song = std::move(s);
    st.path = path;
    st.undo.clear();
    st.redo.clear();
    resetView(st);
    st.seek(0);
    st.edited();
    st.dirty = false;
    st.say("Opened " + fsys::path(path).filename().string());
    remember(path);
    return true;
}

bool saveTo(State& st, const std::string& path) {
    std::error_code ec;
    fsys::create_directories(fsys::path(path).parent_path(), ec);
    // What the plugins remember goes in with the notes.
    if (st.player.isValid()) st.player.engine().captureStates(st.song);
    std::string err;
    if (!st.song.save(path, &err)) {
        st.say("Could not save: " + err, true);
        return false;
    }
    st.path  = path;
    st.dirty = false;
    st.say("Saved " + fsys::path(path).filename().string());
    remember(path);
    return true;
}

// Save where it was, or ask for a name.
void save(State& st) {
    if (!st.path.empty()) { saveTo(st, st.path); return; }
    if (st.songsDir().empty()) {
        st.say("Open or create a project first: songs are kept in its content/songs folder.", true);
        return;
    }
    std::snprintf(st.nameBuf, sizeof st.nameBuf, "%s", st.song.name.c_str());
    st.askName = true;
}

std::string safeName(std::string n) {
    for (char& c : n)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
    return n.empty() ? std::string("Song") : n;
}

bool isSound(const std::string& p) {
    std::string e = fsys::path(p).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".wav" || e == ".ogg" || e == ".mp3" || e == ".flac";
}

bool isMidi(const std::string& p) {
    std::string e = fsys::path(p).extension().string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return e == ".mid" || e == ".midi";
}

// A MIDI file into the song: onto `trackUid` as one clip when that is a track
// with notes, else as tracks of its own.
void importMidiFile(State& st, const std::string& path, std::int64_t at, int trackUid) {
    sg::MidiFile mf;
    std::string err;
    if (!sg::MidiFile::load(path, mf, &err)) {
        st.say("Could not read " + fsys::path(path).filename().string() + ": " + err, true);
        return;
    }
    st.checkpoint();
    Track* onto = st.song.track(trackUid);
    const bool empty = st.song.contentEnd() == 0;
    if (onto && onto->hasNotes()) {
        Clip c;
        c.uid  = st.song.newUid();
        c.start = at;
        c.name = fsys::path(path).stem().string();
        for (const sg::MidiPart& p : mf.parts) c.notes.insert(c.notes.end(), p.notes.begin(), p.notes.end());
        c.sortNotes();
        const std::int64_t bar = st.song.ticksPerBar();
        c.length = std::max(bar, ((mf.length + bar - 1) / bar) * bar);
        onto->clips.push_back(c);
        onto->sortClips();
        st.select(c.uid, false);
        st.editClip = c.uid;
    } else {
        // A first import into an empty song takes the file's tempo.
        const auto made = sg::importMidi(st.song, mf, at, empty);
        st.sel.clear();
        for (int uid : made)
            if (const Track* t = st.song.track(uid))
                for (const Clip& c : t->clips) st.sel.push_back(c.uid);
    }
    st.edited();
    st.ppt = empty ? 0.0 : st.ppt;
    st.say("Imported " + fsys::path(path).filename().string() + " (" + std::to_string(mf.parts.size()) + " part" +
           (mf.parts.size() == 1 ? "" : "s") + ")");
}

// A sound into the song at `at`: on `trackUid` when that is an audio track,
// else on a new one.
void addSoundClip(State& st, const std::string& name, std::int64_t at, int trackUid) {
    if (name.empty()) return;
    st.checkpoint();
    Track* t = st.song.track(trackUid);
    if (!t || t->kind != TrackKind::Audio) t = &st.song.addTrack(TrackKind::Audio, fsys::path(name).stem().string());
    Clip c;
    c.uid   = st.song.newUid();
    c.start = std::max<std::int64_t>(0, at);
    c.file  = name;
    c.name  = fsys::path(name).stem().string();
    const double secs = st.fileLength(name);
    c.length = std::max<std::int64_t>(sg::kPpq / 4, secs > 0.0 ? st.song.tickAt(secs) : st.song.ticksPerBar());
    t->clips.push_back(c);
    t->sortClips();
    st.selTrack = t->uid;
    st.select(c.uid, false);
    st.editClip = c.uid;
    st.edited();
}

// --- Clip operations ----------------------------------------------------------------------

void deleteSelection(State& st) {
    if (st.sel.empty()) return;
    st.checkpoint();
    for (Track& t : st.song.tracks)
        t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return st.isSel(c.uid); }),
                      t.clips.end());
    if (st.isSel(st.editClip)) st.editClip = -1;
    st.sel.clear();
    st.edited();
}

void duplicateSelection(State& st) {
    if (st.sel.empty()) return;
    std::int64_t a = INT64_MAX, b = 0;
    for (int uid : st.sel)
        if (const Clip* c = st.song.clip(uid)) { a = std::min(a, c->start); b = std::max(b, c->end()); }
    if (a == INT64_MAX) return;
    st.checkpoint();
    // Right after the selection, rounded up to the grid so a bar stays a bar.
    const std::int64_t q = std::max<std::int64_t>(1, st.gridOf(st.arrSnap));
    const std::int64_t span = ((b - a + q - 1) / q) * q;
    std::vector<int> made;
    for (Track& t : st.song.tracks) {
        std::vector<Clip> add;
        for (const Clip& c : t.clips)
            if (st.isSel(c.uid)) {
                Clip d = c;
                d.uid = st.song.newUid();
                d.start += span;
                add.push_back(d);
                made.push_back(d.uid);
            }
        t.clips.insert(t.clips.end(), add.begin(), add.end());
        t.sortClips();
    }
    st.sel = made;
    if (!made.empty()) st.editClip = made.front();
    st.edited();
}

// Every clip in `uids` that `at` falls inside, cut in two there.
void splitClips(State& st, std::int64_t at, const std::vector<int>& uids) {
    bool any = false;
    for (int uid : uids)
        if (const Clip* c = st.song.clip(uid); c && at > c->start && at < c->end()) any = true;
    if (!any) return;
    st.checkpoint();
    std::vector<int> rights;
    for (Track& t : st.song.tracks) {
        std::vector<Clip> add;
        for (Clip& c : t.clips) {
            if (std::find(uids.begin(), uids.end(), c.uid) == uids.end() || at <= c.start || at >= c.end()) continue;
            Clip r = c;
            r.uid    = st.song.newUid();
            r.start  = at;
            r.length = c.end() - at;
            const std::int64_t cut = at - c.start;
            if (t.kind == TrackKind::Audio) {
                r.sourceOffset = c.sourceOffset + st.song.seconds(cut);
                r.fadeIn = 0.0f;
                c.fadeOut = 0.0f;
            } else {
                r.offset = c.offset + cut;
            }
            c.length = cut;
            add.push_back(r);
            rights.push_back(r.uid);
        }
        t.clips.insert(t.clips.end(), add.begin(), add.end());
        t.sortClips();
    }
    for (int r : rights) st.sel.push_back(r);
    st.edited();
}

int rowOf(const sg::Song& s, int trackUid) {
    for (std::size_t i = 0; i < s.tracks.size(); ++i)
        if (s.tracks[i].uid == trackUid) return static_cast<int>(i);
    return -1;
}

bool compatible(TrackKind a, TrackKind b) { return (a == TrackKind::Audio) == (b == TrackKind::Audio); }

void copySelection(State& st) {
    st.clipBoard.clear();
    int top = INT32_MAX;
    std::int64_t first = INT64_MAX;
    for (std::size_t i = 0; i < st.song.tracks.size(); ++i)
        for (const Clip& c : st.song.tracks[i].clips)
            if (st.isSel(c.uid)) { top = std::min(top, static_cast<int>(i)); first = std::min(first, c.start); }
    if (top == INT32_MAX) return;
    for (std::size_t i = 0; i < st.song.tracks.size(); ++i)
        for (const Clip& c : st.song.tracks[i].clips)
            if (st.isSel(c.uid)) {
                Clip k = c;
                k.start -= first;
                st.clipBoard.push_back({st.song.tracks[i].kind, static_cast<int>(i) - top, k});
            }
    st.say(std::to_string(st.clipBoard.size()) + " clip(s) copied");
}

// At the playhead, the top one on the selected track; the rest below it as
// they were. A clip whose row is not of its kind goes to a new track.
void paste(State& st, std::int64_t at) {
    if (st.clipBoard.empty()) return;
    st.checkpoint();
    int base = rowOf(st.song, st.selTrack);
    if (base < 0) base = 0;
    std::vector<int> made;
    for (const State::ClipCopy& cc : st.clipBoard) {
        const int row = base + cc.trackOffset;
        Track* t = (row >= 0 && row < static_cast<int>(st.song.tracks.size())) ? &st.song.tracks[static_cast<std::size_t>(row)] : nullptr;
        if (!t || !compatible(t->kind, cc.kind)) t = &st.song.addTrack(cc.kind);
        Clip c = cc.clip;
        c.uid = st.song.newUid();
        c.start += at;
        t->clips.push_back(c);
        t->sortClips();
        made.push_back(c.uid);
    }
    st.sel = made;
    st.edited();
}

// --- Bouncing and exporting ---------------------------------------------------------------

sg::Files filesOf(const Host& h) {
    sg::Files f;
    f.sound = h.resolveSound;
    f.patch = h.resolvePatch;
    return f;
}

void startBounce(State& st, const Host& h) {
    if (st.song.length() <= 0) { st.say("Nothing to bounce: the song is empty.", true); return; }
    // Into the project's sounds, where a dropped sound goes too: the bounce
    // is an asset the moment it is written.
    std::string dir = st.projectFile.empty() ? std::string()
                                             : (fsys::path(st.projectFile).parent_path() / "sounds").generic_string();
    std::error_code ec;
    if (!dir.empty()) fsys::create_directories(dir, ec);
    std::string out;
    if (!ed::saveFile(out, dir, safeName(st.song.name) + ".wav", "WAV sound", "*.wav", "wav")) return;
    sg::Song copy = st.song;
    if (st.player.isValid()) st.player.engine().captureStates(copy);
    st.bounce = std::make_unique<sg::Bounce>();
    st.bounce->start(copy, filesOf(h), 48000.0, 2.0);
    st.bounceTo = out;
}

void stepBounce(State& st, const Host& h) {
    if (!st.bounce) return;
    // A quarter of a second of song a frame keeps the editor drawing.
    for (int i = 0; i < 6 && !st.bounce->done(); ++i)
        if (!st.bounce->step(2048)) break;
    if (!st.bounce->done()) return;
    fitzel::SampleData data = st.bounce->take();
    const std::string problems = st.bounce->problems();
    st.bounce.reset();
    std::string err;
    if (fitzel::saveWav(st.bounceTo, data, fitzel::WavFormat::Pcm24, &err)) {
        st.say("Bounced to " + fsys::path(st.bounceTo).filename().string() +
                   (problems.empty() ? std::string() : " -- but: " + problems),
               !problems.empty());
        if (h.soundsChanged) h.soundsChanged();
    } else {
        st.say("Could not write the bounce: " + err, true);
    }
}

void exportMidiFile(State& st) {
    std::string dir = st.projectFile.empty() ? std::string()
                                             : (fsys::path(st.projectFile).parent_path() / "content" / "midi").generic_string();
    std::error_code ec;
    if (!dir.empty()) fsys::create_directories(dir, ec);
    std::string out;
    if (!ed::saveFile(out, dir, safeName(st.song.name) + ".mid", "MIDI file", "*.mid", "mid")) return;
    const auto bytes = sg::exportMidi(st.song);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (f) st.say("Exported " + fsys::path(out).filename().string());
    else st.say("Could not write " + out, true);
}

// --- Recording -----------------------------------------------------------------------------

// Where notes played in go: the selected track if it plays notes, else the
// first one that does.
int noteTarget(const State& st) {
    if (st.recording && st.song.track(st.recTrack)) return st.recTrack;
    if (const Track* t = st.song.track(st.selTrack); t && t->hasNotes()) return t->uid;
    for (const Track& t : st.song.tracks)
        if (t.hasNotes()) return t.uid;
    return -1;
}

void recordNote(State& st, std::int64_t start, std::int64_t end, int pitch, int vel) {
    Track* t = st.song.track(st.recTrack);
    if (!t) return;
    const std::int64_t bar = st.song.ticksPerBar();
    Clip* c = nullptr;
    for (Clip& k : t->clips)
        if (k.uid == st.recClip) c = &k;
    if (!c) {
        Clip k;
        k.uid    = st.song.newUid();
        k.start  = (start / bar) * bar;
        k.length = bar;
        k.name   = t->name + " take";
        t->clips.push_back(k);
        t->sortClips();
        st.recClip = k.uid;
        for (Clip& q : t->clips)
            if (q.uid == st.recClip) c = &q;
        st.select(st.recClip, false);
        st.editClip = st.recClip;
    }
    if (start < c->start) {   // played before the take's first bar
        const std::int64_t grow = c->start - (start / bar) * bar;
        c->start -= grow;
        c->length += grow;
        for (Note& n : c->notes) n.tick += grow;
    }
    Note n;
    n.tick     = start - c->start + c->offset;
    n.length   = std::max<std::int64_t>(sg::kPpq / 32, end - start);
    n.pitch    = pitch;
    n.velocity = std::clamp(vel, 1, 127);
    c->notes.push_back(n);
    c->sortNotes();
    const std::int64_t need = n.tick - c->offset + n.length;
    if (need > c->length) c->length = ((need + bar - 1) / bar) * bar;
    st.edited();
}

// A note played in, into the take when recording: `now` is where the song
// was when the key moved.
void recordIn(State& st, int pitch, int vel, bool on, std::int64_t now) {
    if (!st.recording || !st.playing()) return;
    if (on) {
        st.recOpen[pitch] = {now, vel};
        return;
    }
    const auto it = st.recOpen.find(pitch);
    if (it == st.recOpen.end()) return;
    std::int64_t start = it->second.tick, end = now;
    // Held over the loop's end: it ends there.
    if (end < start) end = st.song.loop ? st.song.loopEnd : start + sg::kPpq / 4;
    recordNote(st, start, end, pitch, it->second.vel);
    st.recOpen.erase(it);
}

// A note from the computer keyboard: sounded on the track it goes to (its off
// where its on went), recorded when recording.
void playIn(State& st, int pitch, int vel, bool on) {
    int target = -1;
    if (on) {
        target = noteTarget(st);
        if (target < 0) return;
        st.heldTrack[pitch] = target;
    } else {
        const auto it = st.heldTrack.find(pitch);
        if (it == st.heldTrack.end()) return;
        target = it->second;
        st.heldTrack.erase(it);
    }
    st.liveNote(target, pitch, vel, on);
    recordIn(st, pitch, vel, on, static_cast<std::int64_t>(st.playTicks()));
}

// A MIDI keyboard: its notes are sounded on the reader's thread as they
// arrive (onEvent), so here they only go into the take.
void openMidi(State& st, const std::string& device, const std::string& label) {
    st.midiIn.reset();
    for (int p = 0; p < 128; ++p)
        if (st.midiHeld[p] >= 0) st.liveNote(st.midiHeld[p], p, 0, false);
    std::fill(std::begin(st.midiHeld), std::end(st.midiHeld), -1);
    st.midiDevice.clear();
    if (device.empty()) return;
    st.midiIn = std::make_unique<MidiIn>();
    st.midiIn->onEvent = [&st](MidiIn::Ev& e) {
        if (!st.player.isValid()) return;
        int target;
        if (e.on) {
            target = st.liveTarget.load(std::memory_order_relaxed);
            st.midiHeld[e.pitch] = target;
        } else {
            target = st.midiHeld[e.pitch];
            st.midiHeld[e.pitch] = -1;
        }
        if (target < 0) return;
        st.player.engine().liveNote(target, e.pitch, static_cast<float>(e.vel) / 127.0f, e.on);
        e.tick  = st.player.ticks();
        e.track = target;
    };
    if (st.midiIn->open(device)) {
        st.midiDevice = device;
        st.say("Playing from " + label);
    } else {
        st.midiIn.reset();
        st.say("Could not open " + device + " (another program may have it)", true);
    }
}

void startRecording(State& st) {
    const Track* t = st.song.track(st.selTrack);
    if (!t || !t->hasNotes()) {
        for (const Track& o : st.song.tracks)
            if (o.hasNotes()) { t = &o; break; }
    }
    if (!t) { st.say("Add an instrument or MIDI track to record on.", true); return; }
    st.checkpoint();
    st.recTrack  = t->uid;
    st.selTrack  = t->uid;
    st.recClip   = -1;
    st.recording = true;
    st.recOpen.clear();
    if (st.player.isValid() && !st.playing()) st.player.engine().play();
    st.say("Recording on " + t->name + ": play the computer keyboard (turn Keys on), the piano roll's keys or a MIDI keyboard");
}

void stopRecording(State& st) {
    if (!st.recording) return;
    const auto now = static_cast<std::int64_t>(st.playTicks());
    for (const auto& [pitch, o] : st.recOpen) recordNote(st, o.tick, std::max(now, o.tick + 1), pitch, o.vel);
    st.recOpen.clear();
    st.recording = false;
    st.recClip = -1;
}

void togglePlay(State& st) {
    if (!st.player.isValid()) return;
    if (st.playing()) {
        stopRecording(st);
        st.player.engine().stop();
    } else {
        st.player.engine().play();
    }
}

// --- Little widgets ---------------------------------------------------------------------

enum class Glyph { Play, Pause, Stop, Record, ToStart, Loop, Click, Follow };

bool glyphButton(const char* id, Glyph g, bool on, float size, const char* tip, ImU32 onColor = 0) {
    const ImGuiStyle& style = ImGui::GetStyle();
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, onColor ? ImGui::ColorConvertU32ToFloat4(onColor)
                                                           : style.Colors[ImGuiCol_ButtonActive]);
    const bool pressed = ImGui::Button(id, ImVec2(size * 1.5f, size));
    if (on) ImGui::PopStyleColor();
    if (tip) ImGui::SetItemTooltip("%s", tip);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    const float r = size * 0.26f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 ink = ImGui::GetColorU32(ImGuiCol_Text);
    switch (g) {
    case Glyph::Play:
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.8f, c.y + r), ImVec2(c.x + r, c.y), ink);
        break;
    case Glyph::Pause:
        dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r), ImVec2(c.x - r * 0.25f, c.y + r), ink);
        dl->AddRectFilled(ImVec2(c.x + r * 0.25f, c.y - r), ImVec2(c.x + r * 0.8f, c.y + r), ink);
        break;
    case Glyph::Stop:
        dl->AddRectFilled(ImVec2(c.x - r * 0.8f, c.y - r * 0.8f), ImVec2(c.x + r * 0.8f, c.y + r * 0.8f), ink);
        break;
    case Glyph::Record:
        dl->AddCircleFilled(c, r * 0.9f, on ? IM_COL32(255, 90, 80, 255) : IM_COL32(220, 70, 60, 255));
        break;
    case Glyph::ToStart:
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r * 0.65f, c.y + r), ink);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.9f, c.y - r), ImVec2(c.x + r * 0.9f, c.y + r), ImVec2(c.x - r * 0.55f, c.y), ink);
        break;
    case Glyph::Loop:
        dl->AddCircle(c, r * 0.95f, ink, 16, 1.8f);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.95f - 3.5f, c.y - 1.0f), ImVec2(c.x + r * 0.95f + 3.5f, c.y - 1.0f),
                              ImVec2(c.x + r * 0.95f, c.y + 4.0f), ink);
        break;
    case Glyph::Click:
        dl->AddTriangle(ImVec2(c.x - r * 0.75f, c.y + r), ImVec2(c.x + r * 0.75f, c.y + r), ImVec2(c.x, c.y - r), ink, 1.6f);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x + r * 0.8f, c.y - r * 0.9f), ink, 1.6f);
        break;
    case Glyph::Follow:
        dl->AddLine(ImVec2(c.x - r * 0.3f, c.y - r), ImVec2(c.x - r * 0.3f, c.y + r), ink, 1.6f);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.1f, c.y - r * 0.55f), ImVec2(c.x + r * 0.1f, c.y + r * 0.55f),
                              ImVec2(c.x + r, c.y), ink);
        break;
    }
    return pressed;
}

// A level meter, vertical, bottom up; 0 dBFS at the top, -48 at the bottom.
void meter(ImDrawList* dl, ImVec2 a, ImVec2 b, float level) {
    dl->AddRectFilled(a, b, IM_COL32(16, 18, 22, 255));
    if (level <= 1e-5f) return;
    const float db = 20.0f * std::log10(level);
    const float k  = std::clamp((db + 48.0f) / 48.0f, 0.0f, 1.0f);
    const float y  = b.y - (b.y - a.y) * k;
    const ImU32 col = db > -1.0f ? IM_COL32(240, 80, 60, 255) : db > -9.0f ? IM_COL32(230, 200, 70, 255) : IM_COL32(90, 200, 110, 255);
    dl->AddRectFilled(ImVec2(a.x, y), b, col);
}

// --- The menu bar ---------------------------------------------------------------------------

// Back a step, the selection with it -- what was being worked on is what
// is selected again.
void undoStep(State& st) {
    if (st.undo.empty()) return;
    st.redo.push_back({st.song, st.sel});
    st.song = std::move(st.undo.back().song);
    st.sel  = std::move(st.undo.back().sel);
    st.undo.pop_back();
    st.edited();
}

void redoStep(State& st) {
    if (st.redo.empty()) return;
    st.undo.push_back({st.song, st.sel});
    st.song = std::move(st.redo.back().song);
    st.sel  = std::move(st.redo.back().sel);
    st.redo.pop_back();
    st.edited();
}


void menuBar(State& st, const Host& h) {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("Song")) {
        if (ImGui::MenuItem("New")) newSong(st);
        if (ImGui::BeginMenu("Open", !st.songsDir().empty())) {
            const auto files = songFiles(st);
            if (files.empty()) ImGui::TextDisabled("No songs in content/songs yet");
            for (const std::string& f : files)
                if (ImGui::MenuItem(fsys::path(f).stem().string().c_str(), nullptr, f == st.path)) openSong(st, f);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) save(st);
        if (ImGui::MenuItem("Save as...", nullptr, false, !st.songsDir().empty())) {
            std::snprintf(st.nameBuf, sizeof st.nameBuf, "%s", st.song.name.c_str());
            st.askName = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Import MIDI file...")) {
            std::string f;
            const std::string dir = st.projectFile.empty() ? std::string()
                                    : (fsys::path(st.projectFile).parent_path() / "content").generic_string();
            if (ed::pickFile(f, dir, "MIDI files", "*.mid;*.midi")) {
                const std::int64_t bar = st.song.ticksPerBar();
                const auto at = static_cast<std::int64_t>(st.playTicks()) / bar * bar;
                importMidiFile(st, f, at, -1);
            }
        }
        if (ImGui::MenuItem("Export MIDI file...")) exportMidiFile(st);
        if (ImGui::MenuItem("Bounce to WAV...", nullptr, false, !st.bounce)) startBounce(st, h);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !st.undo.empty())) undoStep(st);
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !st.redo.empty())) redoStep(st);
        ImGui::Separator();
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, !st.sel.empty())) copySelection(st);
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, !st.sel.empty())) { copySelection(st); deleteSelection(st); }
        if (ImGui::MenuItem("Paste at playhead", "Ctrl+V", false, !st.clipBoard.empty()))
            paste(st, st.snapTo(static_cast<std::int64_t>(st.playTicks()), st.arrSnap));
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !st.sel.empty())) duplicateSelection(st);
        if (ImGui::MenuItem("Split at playhead", "S", false, !st.sel.empty()))
            splitClips(st, static_cast<std::int64_t>(st.playTicks()), st.sel);
        if (ImGui::MenuItem("Delete", "Del", false, !st.sel.empty())) deleteSelection(st);
        if (ImGui::MenuItem("Select all", "Ctrl+A")) {
            st.sel.clear();
            for (const Track& t : st.song.tracks)
                for (const Clip& c : t.clips) st.sel.push_back(c.uid);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Track")) {
        auto add = [&](TrackKind k) {
            st.checkpoint();
            Track& t = st.song.addTrack(k);
            st.selTrack = t.uid;
            st.edited();
        };
        if (ImGui::MenuItem("Add instrument track")) add(TrackKind::Instrument);
        if (ImGui::MenuItem("Add MIDI track")) add(TrackKind::Midi);
        if (ImGui::MenuItem("Add audio track")) add(TrackKind::Audio);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Zoom to fit the song", "F")) st.ppt = 0.0;
        if (ImGui::MenuItem("Follow the playhead", nullptr, st.follow)) st.follow = !st.follow;
        ImGui::EndMenu();
    }
    // What happened last, for a few seconds, at the right.
    if (!st.status.empty() && ImGui::GetTime() - st.statusAt < 8.0) {
        const float w = ImGui::CalcTextSize(st.status.c_str()).x;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 20.0f, ImGui::GetWindowWidth() - w - 24.0f));
        ImGui::TextColored(st.statusBad ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f) : ImVec4(0.65f, 0.85f, 0.65f, 1.0f), "%s",
                           st.status.c_str());
    }
    ImGui::EndMenuBar();
}

// --- The transport -------------------------------------------------------------------------

void transportBar(State& st) {
    const float em = ImGui::GetFontSize();
    const float bh = ImGui::GetFrameHeight();
    sg::Song& s = st.song;
    const bool rolling = st.playing();

    if (glyphButton("##start", Glyph::ToStart, false, bh, "To the start (Home)")) st.seek(s.loop ? s.loopStart : 0);
    ImGui::SameLine(0, 2);
    if (glyphButton("##play", rolling ? Glyph::Pause : Glyph::Play, rolling, bh, "Play / stop (Space)")) togglePlay(st);
    ImGui::SameLine(0, 2);
    if (glyphButton("##stop", Glyph::Stop, false, bh, "Stop; pressed again, back to the start")) {
        if (rolling) togglePlay(st);
        else st.seek(s.loop ? s.loopStart : 0);
    }
    ImGui::SameLine(0, 2);
    if (glyphButton("##rec", Glyph::Record, st.recording, bh, "Record notes on the selected track (R)",
                    IM_COL32(120, 40, 36, 255))) {
        if (st.recording) stopRecording(st);
        else startRecording(st);
    }
    ImGui::SameLine(0, 8);
    if (glyphButton("##loop", Glyph::Loop, s.loop, bh, "Loop the range set in the ruler (L)")) {
        st.checkpoint();
        s.loop = !s.loop;
        st.edited();
    }
    ImGui::SameLine(0, 2);
    if (glyphButton("##click", Glyph::Click, st.metronome, bh, "Metronome (C)")) {
        st.metronome = !st.metronome;
        if (st.player.isValid()) st.player.engine().setMetronome(st.metronome);
    }
    ImGui::SameLine(0, 2);
    if (glyphButton("##follow", Glyph::Follow, st.follow, bh, "The view follows the playhead")) st.follow = !st.follow;

    // Where it is: bars and beats, and time.
    ImGui::SameLine(0, 12);
    {
        const auto at = static_cast<std::int64_t>(st.playTicks());
        const std::string bbt = sg::barBeatTick(s, at);
        const std::string clock = sg::clockTime(s.seconds(at));
        if (ImFont* bold = ui::boldFont()) ImGui::PushFont(bold);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(bbt.c_str());
        if (ui::boldFont()) ImGui::PopFont();
        ImGui::SameLine(0, 8);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", clock.c_str());
    }

    ImGui::SameLine(0, 14);
    float bpm = static_cast<float>(s.bpm);
    const float bpm0 = bpm;
    if (ui::stepper("##bpm", bpm, 1.0f, 20.0f, 300.0f, "%.1f BPM", em * 9.0f)) {
        sg::Song before = s;
        before.bpm = bpm0;
        st.checkpoint(before);
        s.bpm = bpm;
        st.edited();
    }
    ImGui::SetItemTooltip("Tempo -- one click is one beat a minute; Ctrl-click the number to type it");

    ImGui::SameLine(0, 6);
    {
        char sig[16];
        std::snprintf(sig, sizeof sig, "%d/%d", s.beatsPerBar, s.beatUnit);
        ImGui::SetNextItemWidth(em * 4.0f);
        if (ImGui::BeginCombo("##meter", sig)) {
            static const int choices[][2] = {{2, 4}, {3, 4}, {4, 4}, {5, 4}, {6, 8}, {7, 8}, {9, 8}, {12, 8}};
            for (const auto& c : choices) {
                char l[16];
                std::snprintf(l, sizeof l, "%d/%d", c[0], c[1]);
                if (ImGui::Selectable(l, s.beatsPerBar == c[0] && s.beatUnit == c[1])) {
                    st.checkpoint();
                    s.beatsPerBar = c[0];
                    s.beatUnit = c[1];
                    st.edited();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("Meter");
    }

    ImGui::SameLine(0, 14);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Grid");
    ImGui::SameLine(0, 4);
    ImGui::SetNextItemWidth(em * 4.5f);
    if (ImGui::BeginCombo("##snap", snaps()[static_cast<std::size_t>(st.arrSnap)].label)) {
        for (int i = 0; i < static_cast<int>(snaps().size()); ++i)
            if (ImGui::Selectable(snaps()[static_cast<std::size_t>(i)].label, st.arrSnap == i)) st.arrSnap = i;
        ImGui::EndCombo();
    }
    ImGui::SetItemTooltip("What clips snap to in the arrangement (hold Alt to place freely)");

    ImGui::SameLine(0, 14);
    static const char* tools[] = {"Select", "Draw", "Erase", "Split"};
    static const char* tips[] = {"Select, move, trim (1)", "Draw clips and notes (2)", "Erase clips and notes (3)",
                                 "Split a clip where you click (4)"};
    for (int i = 0; i < 4; ++i) {
        if (i) ImGui::SameLine(0, 2);
        const bool on = static_cast<int>(st.tool) == i;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        if (ImGui::Button(tools[i])) st.tool = static_cast<Tool>(i);
        if (on) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tips[i]);
    }

    ImGui::SameLine(0, 14);
    {
        const bool on = st.qwerty;
        if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        char l[32];
        std::snprintf(l, sizeof l, on ? "Keys C%d###qw" : "Keys###qw", st.octave);
        if (ImGui::Button(l)) {
            st.qwerty = !st.qwerty;
            for (const auto& [k, p] : st.qwHeld) playIn(st, p, 0, false);
            st.qwHeld.clear();
        }
        if (on) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("The computer keyboard plays the selected track:\n"
                              "A W S E D F T G Z/Y H U J K O L P = C to D# above,\n"
                              "- and + change the octave. Letter shortcuts are off meanwhile.");
    }

    // A MIDI keyboard, when there is one.
    const auto& devices = midiDevices();
    if (!devices.empty()) {
        ImGui::SameLine(0, 6);
        ImGui::SetNextItemWidth(em * 8.0f);
        std::string cur = "MIDI in: off";
        for (const auto& d : devices)
            if (d.first == st.midiDevice) cur = d.second;
        if (ImGui::BeginCombo("##midiin", cur.c_str())) {
            if (ImGui::Selectable("Off", st.midiDevice.empty())) openMidi(st, {}, {});
            for (const auto& d : devices)
                if (ImGui::Selectable(d.second.c_str(), d.first == st.midiDevice)) openMidi(st, d.first, d.second);
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("A MIDI keyboard plays the selected track (and records with Record on)");
    }

    // The master: its fader and its meter, at the right.
    ImGui::SameLine(0, 14);
    {
        float v = s.masterVolume;
        const float v0 = v;
        ImGui::SetNextItemWidth(em * 8.5f);
        const float db = v > 1e-4f ? 20.0f * std::log10(v) : -96.0f;
        char lbl[32];
        std::snprintf(lbl, sizeof lbl, db <= -60.0f ? "Master -inf" : "Master %+.1f dB", db);
        if (ImGui::SliderFloat("##master", &v, 0.0f, 2.0f, lbl)) {
            s.masterVolume = v;
            st.edited();
        }
        if (ImGui::IsItemActivated()) {
            sg::Song before = s;
            before.masterVolume = v0;
            st.checkpoint(before);
        }
        ImGui::SameLine(0, 4);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(6.0f, bh));
        if (st.player.isValid()) st.masterHold = std::max(st.player.engine().takeMasterPeak(), st.masterHold * 0.92f);
        meter(ImGui::GetWindowDrawList(), p, ImVec2(p.x + 6.0f, p.y + bh), st.masterHold);
    }

    if (st.bounce) {
        ImGui::SameLine(0, 12);
        ImGui::ProgressBar(st.bounce->progress(), ImVec2(em * 8.0f, bh), "Bouncing...");
        ImGui::SameLine(0, 4);
        if (ImGui::SmallButton("Cancel")) {
            st.bounce.reset();
            st.say("Bounce cancelled");
        }
    }
    if (!st.player.isValid() && !st.playerError.empty()) {
        ImGui::SameLine(0, 12);
        ImGui::TextDisabled("(silent: %s)", st.playerError.c_str());
    }
}

// --- Track headers -------------------------------------------------------------------------

struct HeaderActions {
    int remove = -1, duplicate = -1, moveFrom = -1, moveTo = -1;
};

void instrumentMenu(State& st, Track& t, const Host& h) {
    auto set = [&](InstrumentKind k, const std::string& patch, const std::string& uid, const std::string& name) {
        st.checkpoint();
        t.instrument.kind       = k;
        t.instrument.patch      = patch.empty() ? t.instrument.patch : patch;
        t.instrument.pluginUid  = uid;
        t.instrument.pluginName = name;
        t.instrument.state.clear();
        st.edited();
    };
    if (ImGui::BeginMenu("fitzel synth")) {
        for (const std::string& n : sg::factoryPatchNames())
            if (ImGui::MenuItem(n.c_str(), nullptr, t.instrument.kind == InstrumentKind::Synth && t.instrument.patch == "builtin:" + n))
                set(InstrumentKind::Synth, "builtin:" + n, {}, {});
        const auto mine = h.patchNames ? h.patchNames() : std::vector<std::string>{};
        if (!mine.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("The project's patches");
            for (const std::string& n : mine)
                if (ImGui::MenuItem(n.c_str(), nullptr, t.instrument.kind == InstrumentKind::Synth && t.instrument.patch == n))
                    set(InstrumentKind::Synth, n, {}, {});
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Drum kit (General MIDI)", nullptr, t.instrument.kind == InstrumentKind::Drums))
        set(InstrumentKind::Drums, {}, {}, {});
    if (ImGui::BeginMenu("VST3 instrument")) {
        int n = 0;
        if (fitzel::vst3::supported())
            for (const auto& p : fitzel::vst3::plugins()) {
                if (!p.instrument) continue;
                ++n;
                const std::string l = p.name + "##" + p.uid;
                if (ImGui::MenuItem(l.c_str(), p.vendor.c_str(), t.instrument.pluginUid == p.uid))
                    set(InstrumentKind::Vst3, {}, p.uid, p.name);
            }
        if (!n) ImGui::TextDisabled(fitzel::vst3::supported() ? "No VST3 instruments found" : "No VST3 on this platform");
        if (ImGui::BeginMenu("Effects too")) {
            for (const auto& p : fitzel::vst3::plugins()) {
                if (p.instrument) continue;
                const std::string l = p.name + "##fx" + p.uid;
                if (ImGui::MenuItem(l.c_str(), p.vendor.c_str(), t.instrument.pluginUid == p.uid))
                    set(InstrumentKind::Vst3, {}, p.uid, p.name);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Rescan plugins")) fitzel::vst3::plugins(true);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("None", nullptr, t.instrument.kind == InstrumentKind::None)) set(InstrumentKind::None, {}, {}, {});
    if (t.instrument.kind == InstrumentKind::Synth && t.instrument.patch.rfind("builtin:", 0) != 0) {
        ImGui::Separator();
        if (ImGui::MenuItem("Reload the patch") && st.player.isValid()) {
            st.player.engine().reloadInstrument(t.uid);
            st.edited();
        }
    }
}

void trackHeader(State& st, const Host& h, int row, ImVec2 p, float hw, float lh, HeaderActions& act) {
    sg::Song& s = st.song;
    Track& t = s.tracks[static_cast<std::size_t>(row)];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float em = ImGui::GetFontSize();
    const float fh = ImGui::GetFrameHeight();
    const ImU32 col = trackColor(s, t);
    const bool selected = st.selTrack == t.uid;
    ImGui::PushID(t.uid);

    dl->AddRectFilled(p, ImVec2(p.x + hw, p.y + lh - 1.0f), selected ? IM_COL32(54, 60, 72, 255) : IM_COL32(38, 42, 50, 255));
    dl->AddRectFilled(p, ImVec2(p.x + 4.0f, p.y + lh - 1.0f), col);

    // The whole header: select, context menu, drag to reorder.
    ImGui::SetCursorScreenPos(p);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##hdr", ImVec2(hw - 8.0f, lh - 4.0f), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (h.probe) h.probe("header:" + std::to_string(t.uid), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        st.selTrack = t.uid;
        st.prFocus = false;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("##trackmenu");
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        st.renameTrack = t.uid;
        st.renameFocus = true;
        std::snprintf(st.nameBuf, sizeof st.nameBuf, "%s", t.name.c_str());
    }
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoDisableHover)) {
        ImGui::SetDragDropPayload("SONG_TRACK", &row, sizeof(int));
        ImGui::Text("Move %s", t.name.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("SONG_TRACK")) {
            act.moveFrom = *static_cast<const int*>(pl->Data);
            act.moveTo   = row;
        }
        ImGui::EndDragDropTarget();
    }

    // Row 1: name, mute, solo.
    const float y1 = p.y + 4.0f;
    const float bw = fh * 1.05f;
    if (st.renameTrack == t.uid) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + 10.0f, y1));
        ImGui::SetNextItemWidth(hw - bw * 2.0f - 26.0f);
        if (st.renameFocus) { ImGui::SetKeyboardFocusHere(); st.renameFocus = false; }
        if (ImGui::InputText("##rename", st.nameBuf, sizeof st.nameBuf, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            st.checkpoint();
            t.name = st.nameBuf;
            st.edited();
            st.renameTrack = -1;
        }
        if (ImGui::IsItemDeactivated() && !ImGui::IsItemDeactivatedAfterEdit()) st.renameTrack = -1;
    } else {
        const char* kind = t.kind == TrackKind::Audio ? "AUD" : t.kind == TrackKind::Midi ? "MIDI" : "INS";
        dl->AddText(ImVec2(p.x + 10.0f, y1 + (fh - em) * 0.5f), IM_COL32(150, 156, 168, 255), kind);
        const float kx = ImGui::CalcTextSize("MIDI").x + 16.0f;
        ImGui::PushClipRect(ImVec2(p.x + kx, p.y), ImVec2(p.x + hw - bw * 2.0f - 12.0f, p.y + lh), true);
        if (ImFont* bold = ui::boldFont()) ImGui::PushFont(bold);
        dl->AddText(ImVec2(p.x + kx, y1 + (fh - em) * 0.5f), IM_COL32(236, 238, 242, 255), t.name.c_str());
        if (ui::boldFont()) ImGui::PopFont();
        ImGui::PopClipRect();
    }
    auto toggle = [&](const char* id, bool& v, ImU32 on, const char* tip) {
        if (v) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(on));
        if (ImGui::Button(id, ImVec2(bw, fh))) {
            st.checkpoint();
            v = !v;
            st.edited();
        }
        if (v) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", tip);
    };
    ImGui::SetCursorScreenPos(ImVec2(p.x + hw - bw * 2.0f - 12.0f, y1));
    toggle("M", t.mute, IM_COL32(200, 120, 40, 255), "Mute");
    ImGui::SameLine(0, 2);
    toggle("S", t.solo, IM_COL32(200, 180, 40, 255), "Solo: only soloed tracks sound");

    // Row 2: what it plays and its fader.
    const float y2 = y1 + fh + 3.0f;
    const float faderW = em * 5.0f;
    ImGui::SetCursorScreenPos(ImVec2(p.x + 10.0f, y2));
    const float leftW = hw - faderW - 28.0f;
    if (t.kind == TrackKind::Instrument) {
        const std::string err = st.player.isValid() ? st.player.engine().instrumentError(t.uid) : std::string();
        const bool vst = t.instrument.kind == InstrumentKind::Vst3;
        const float editW = vst ? fh * 1.2f : 0.0f;
        std::string label = t.instrument.label();
        if (!err.empty()) label = "! " + label;
        if (!err.empty()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.45f, 1.0f));
        if (ImGui::Button((label + "##instr").c_str(), ImVec2(leftW - editW - (vst ? 2.0f : 0.0f), fh))) ImGui::OpenPopup("##instrmenu");
        if (!err.empty()) ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", err.empty() ? "What this track plays: a fitzel synth patch, the drum kit or a VST3 instrument"
                                                : err.c_str());
        if (ImGui::BeginPopup("##instrmenu")) {
            instrumentMenu(st, t, h);
            ImGui::EndPopup();
        }
        if (vst) {
            ImGui::SameLine(0, 2);
            auto plug = st.player.isValid() ? st.player.engine().plugin(t.uid) : nullptr;
            const bool open = plug && plug->editorOpen();
            ImGui::BeginDisabled(!plug || !plug->hasEditor());
            if (open) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
            if (ImGui::Button("E##edit", ImVec2(editW, fh))) {
                if (open) plug->closeEditor();
                else if (!plug->openEditor(s.name + " - " + t.name)) st.say(t.instrument.pluginName + ": its editor would not open", true);
            }
            if (open) ImGui::PopStyleColor();
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("The plugin's own window");
        }
    } else if (t.kind == TrackKind::Midi) {
        const Track* target = s.track(t.target);
        std::string label = target ? "> " + target->name : std::string("> (no instrument)");
        ImGui::SetNextItemWidth(leftW - em * 3.6f);
        if (ImGui::BeginCombo("##target", label.c_str())) {
            for (const Track& o : s.tracks)
                if (o.kind == TrackKind::Instrument && ImGui::Selectable((o.name + "##" + std::to_string(o.uid)).c_str(), o.uid == t.target)) {
                    st.checkpoint();
                    t.target = o.uid;
                    st.edited();
                }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The instrument track this MIDI track plays");
        ImGui::SameLine(0, 2);
        ImGui::SetNextItemWidth(em * 3.4f);
        char ch[16];
        std::snprintf(ch, sizeof ch, "Ch %d", t.channel + 1);
        if (ImGui::BeginCombo("##ch", ch, ImGuiComboFlags_NoArrowButton)) {
            for (int c = 0; c < 16; ++c) {
                char l[16];
                std::snprintf(l, sizeof l, "Channel %d", c + 1);
                if (ImGui::Selectable(l, t.channel == c)) {
                    st.checkpoint();
                    t.channel = c;
                    st.edited();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SetItemTooltip("The MIDI channel its notes go out on -- for a plugin that plays several parts");
    } else {
        ImGui::Dummy(ImVec2(leftW, fh));
    }
    if (t.kind != TrackKind::Midi) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + hw - faderW - 14.0f, y2));
        ImGui::SetNextItemWidth(faderW);
        float v = t.volume;
        const float v0 = v;
        const float db = v > 1e-4f ? 20.0f * std::log10(v) : -96.0f;
        char lbl[24];
        std::snprintf(lbl, sizeof lbl, db <= -60.0f ? "-inf dB" : "%+.1f dB", db);
        if (ImGui::SliderFloat("##vol", &v, 0.0f, 2.0f, lbl)) {
            t.volume = v;
            st.edited();
        }
        if (ImGui::IsItemActivated()) {
            sg::Song before = s;
            before.tracks[static_cast<std::size_t>(row)].volume = v0;
            st.checkpoint(before);
        }
        ImGui::SetItemTooltip("Volume (dB). Ctrl-click to type; the pan and transpose are in the right-click menu");
    }

    // Row 3, when the track is tall enough: pan and transpose.
    const float y3 = y2 + fh + 3.0f;
    if (y3 + fh <= p.y + lh - 2.0f) {
        ImGui::SetCursorScreenPos(ImVec2(p.x + 10.0f, y3));
        if (t.kind != TrackKind::Midi) {
            ImGui::SetNextItemWidth(em * 5.0f);
            float pan = t.pan;
            const float pan0 = pan;
            char pl[24];
            if (std::fabs(pan) < 0.01f) std::snprintf(pl, sizeof pl, "Pan C");
            else std::snprintf(pl, sizeof pl, "Pan %s%d", pan < 0 ? "L" : "R", static_cast<int>(std::lround(std::fabs(pan) * 100)));
            if (ImGui::SliderFloat("##pan", &pan, -1.0f, 1.0f, pl)) {
                t.pan = pan;
                st.edited();
            }
            if (ImGui::IsItemActivated()) {
                sg::Song before = s;
                before.tracks[static_cast<std::size_t>(row)].pan = pan0;
                st.checkpoint(before);
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                st.checkpoint();
                t.pan = 0.0f;
                st.edited();
            }
            ImGui::SameLine(0, 4);
        }
        if (t.hasNotes()) {
            float tr = static_cast<float>(t.transpose);
            if (ui::stepper("##tr", tr, 1.0f, -48.0f, 48.0f, "%+.0f st", em * 6.0f)) {
                st.checkpoint();
                t.transpose = static_cast<int>(tr);
                st.edited();
            }
            ImGui::SetItemTooltip("Transpose every note the track plays (semitones)");
        }
    }

    // The meter, at the right edge.
    if (t.makesSound()) {
        float& hold = st.peakHold[std::min(row, 63)];
        if (st.player.isValid()) hold = std::max(st.player.engine().takePeak(t.uid), hold * 0.9f);
        meter(dl, ImVec2(p.x + hw - 9.0f, p.y + 3.0f), ImVec2(p.x + hw - 4.0f, p.y + lh - 4.0f), hold);
    }

    // The bottom edge: drag for height.
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + lh - 4.0f));
    ImGui::InvisibleButton("##height", ImVec2(hw, 5.0f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        t.height = std::clamp(t.height + ImGui::GetIO().MouseDelta.y, fh * 2.0f + 12.0f, 400.0f);
        st.dirty = true;
    }

    if (ImGui::BeginPopup("##trackmenu")) {
        ImGui::TextDisabled("%s", t.name.c_str());
        if (ImGui::MenuItem("Rename")) {
            st.renameTrack = t.uid;
            st.renameFocus = true;
            std::snprintf(st.nameBuf, sizeof st.nameBuf, "%s", t.name.c_str());
        }
        if (ImGui::BeginMenu("Color")) {
            for (int i = 0; i < 12; ++i) {
                if (i % 6) ImGui::SameLine(0, 3);
                ImGui::PushID(i);
                if (ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(kPalette[i]), ImGuiColorEditFlags_NoTooltip, ImVec2(fh, fh))) {
                    st.checkpoint();
                    t.color = kPalette[i];
                    st.edited();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopID();
            }
            if (ImGui::MenuItem("By position")) { st.checkpoint(); t.color = 0; st.edited(); }
            ImGui::EndMenu();
        }
        if (t.kind != TrackKind::Midi) {
            float pan = t.pan;
            ImGui::SetNextItemWidth(em * 10.0f);
            if (ImGui::SliderFloat("Pan", &pan, -1.0f, 1.0f, "%.2f")) { t.pan = pan; st.edited(); }
        }
        if (t.hasNotes()) {
            float tr = static_cast<float>(t.transpose);
            if (ui::stepper("##trm", tr, 1.0f, -48.0f, 48.0f, "Transpose %+.0f", em * 10.0f)) {
                st.checkpoint();
                t.transpose = static_cast<int>(tr);
                st.edited();
            }
        }
        if (ImGui::BeginMenu("Height")) {
            if (ImGui::MenuItem("Small")) t.height = fh * 2.0f + 12.0f;
            if (ImGui::MenuItem("Medium")) t.height = fh * 3.0f + 16.0f;
            if (ImGui::MenuItem("Large")) t.height = fh * 5.0f + 20.0f;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Move up", nullptr, false, row > 0)) { act.moveFrom = row; act.moveTo = row - 1; }
        if (ImGui::MenuItem("Move down", nullptr, false, row + 1 < static_cast<int>(s.tracks.size()))) {
            act.moveFrom = row;
            act.moveTo = row + 1;
        }
        if (ImGui::MenuItem("Duplicate")) act.duplicate = row;
        if (ImGui::MenuItem("Delete")) act.remove = row;
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void applyHeaderActions(State& st, const HeaderActions& act) {
    sg::Song& s = st.song;
    const int n = static_cast<int>(s.tracks.size());
    if (act.remove >= 0 && act.remove < n) {
        st.checkpoint();
        const int uid = s.tracks[static_cast<std::size_t>(act.remove)].uid;
        for (const Clip& c : s.tracks[static_cast<std::size_t>(act.remove)].clips) {
            st.sel.erase(std::remove(st.sel.begin(), st.sel.end(), c.uid), st.sel.end());
            if (c.uid == st.editClip) st.editClip = -1;
        }
        s.tracks.erase(s.tracks.begin() + act.remove);
        for (Track& t : s.tracks)
            if (t.target == uid) t.target = 0;
        if (st.selTrack == uid) st.selTrack = s.tracks.empty() ? -1 : s.tracks[static_cast<std::size_t>(std::min(act.remove, static_cast<int>(s.tracks.size()) - 1))].uid;
        st.edited();
    }
    if (act.duplicate >= 0 && act.duplicate < n) {
        st.checkpoint();
        Track t = s.tracks[static_cast<std::size_t>(act.duplicate)];
        t.uid = s.newUid();
        t.name += " copy";
        for (Clip& c : t.clips) c.uid = s.newUid();
        s.tracks.insert(s.tracks.begin() + act.duplicate + 1, t);
        st.selTrack = t.uid;
        st.edited();
    }
    if (act.moveFrom >= 0 && act.moveTo >= 0 && act.moveFrom != act.moveTo && act.moveFrom < n && act.moveTo < n) {
        st.checkpoint();
        Track t = s.tracks[static_cast<std::size_t>(act.moveFrom)];
        s.tracks.erase(s.tracks.begin() + act.moveFrom);
        s.tracks.insert(s.tracks.begin() + act.moveTo, t);
        st.edited();
    }
}

// --- The arrangement -----------------------------------------------------------------------

struct ClipRect {
    int    uid, row;
    ImVec2 a, b;
};

void drawClip(State& st, ImDrawList* dl, const Track& t, const Clip& c, ImVec2 a, ImVec2 b, ImU32 col, bool sel,
              float visX0, float visX1) {
    const float em = ImGui::GetFontSize();
    const sg::Song& s = st.song;
    const float alpha = c.mute ? 0.35f : 1.0f;
    const ImU32 cc = c.color ? c.color : col;
    const float headH = std::min(b.y - a.y, std::round(em * 1.15f));
    dl->AddRectFilled(a, b, mul(cc, 0.38f, 0.92f * alpha), 3.0f);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + headH), mul(cc, 0.95f, alpha), 3.0f, ImDrawFlags_RoundCornersTop);
    const double ppt = st.ppt;
    const float x0 = std::max(a.x, visX0), x1 = std::min(b.x, visX1);
    auto xOfTick = [&](std::int64_t tk) { return a.x + static_cast<float>(static_cast<double>(tk - c.start) * ppt); };

    if (t.kind == TrackKind::Audio) {
        // The waveform: its peak as the outline, its RMS as the body.
        if (const fitzel::Waveform* w = st.waveform(c.file)) {
            const float midY = (a.y + headH + b.y) * 0.5f, half = (b.y - a.y - headH) * 0.46f;
            const double secPerPx = s.secondsPerTick() / ppt;
            for (float x = x0; x < x1; x += 1.0f) {
                const double sec = c.sourceOffset + static_cast<double>(x - a.x) * secPerPx;
                const float pk = w->peakOver(static_cast<float>(sec), static_cast<float>(sec + secPerPx)) * c.gain;
                const float rm = w->rmsOver(static_cast<float>(sec), static_cast<float>(sec + secPerPx)) * c.gain;
                if (pk <= 0.0f) continue;
                dl->AddLine(ImVec2(x, midY - std::min(pk, 1.0f) * half), ImVec2(x, midY + std::min(pk, 1.0f) * half), mul(cc, 0.85f, 0.75f * alpha));
                dl->AddLine(ImVec2(x, midY - std::min(rm, 1.0f) * half), ImVec2(x, midY + std::min(rm, 1.0f) * half), mul(cc, 1.25f, alpha));
            }
        } else {
            dl->AddText(ImVec2(a.x + 4.0f, a.y + headH + 2.0f), IM_COL32(255, 255, 255, 90), "reading...");
        }
        // Fades, and their handles.
        const float fiX = a.x + static_cast<float>(s.tickAt(c.fadeIn) * ppt);
        const float foX = b.x - static_cast<float>(s.tickAt(c.fadeOut) * ppt);
        if (c.fadeIn > 0.0f) dl->AddLine(ImVec2(a.x, b.y), ImVec2(fiX, a.y + headH), IM_COL32(255, 255, 255, 150), 1.5f);
        if (c.fadeOut > 0.0f) dl->AddLine(ImVec2(foX, a.y + headH), ImVec2(b.x, b.y), IM_COL32(255, 255, 255, 150), 1.5f);
        if (sel) {
            dl->AddRectFilled(ImVec2(fiX - 3.0f, a.y + headH - 3.0f), ImVec2(fiX + 3.0f, a.y + headH + 3.0f), IM_COL32(255, 255, 255, 220));
            dl->AddRectFilled(ImVec2(foX - 3.0f, a.y + headH - 3.0f), ImVec2(foX + 3.0f, a.y + headH + 3.0f), IM_COL32(255, 255, 255, 220));
        }
    } else {
        // The notes, scaled into the clip's height by their range.
        std::vector<Note> played;
        sg::playedNotes(c, played);
        int lo = 127, hi = 0;
        for (const Note& n : c.notes) { lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
        if (lo > hi) { lo = 60; hi = 72; }
        if (hi - lo < 12) { const int mid = (lo + hi) / 2; lo = mid - 6; hi = mid + 6; }
        const float top = a.y + headH + 2.0f, bot = b.y - 2.0f;
        const float rowH = std::max(1.0f, (bot - top) / static_cast<float>(hi - lo + 1));
        for (const Note& n : played) {
            const float nx0 = xOfTick(n.tick), nx1 = std::max(nx0 + 1.5f, xOfTick(n.end()));
            if (nx1 < x0 || nx0 > x1) continue;
            const float ny = bot - static_cast<float>(n.pitch - lo + 1) * rowH;
            dl->AddRectFilled(ImVec2(nx0, ny), ImVec2(nx1, ny + std::max(1.5f, rowH - 0.5f)), mul(cc, 1.35f, alpha));
        }
        // Where a looping clip comes round.
        if (c.loop > 0)
            for (std::int64_t k = c.loop - (c.offset % c.loop); k < c.length; k += c.loop) {
                const float lx = xOfTick(c.start + k);
                if (lx > x0 && lx < x1) dl->AddLine(ImVec2(lx, a.y + headH), ImVec2(lx, b.y), IM_COL32(255, 255, 255, 60));
            }
    }
    // The name.
    ImGui::PushClipRect(ImVec2(std::max(a.x, visX0), a.y), ImVec2(std::min(b.x, visX1), b.y), true);
    const std::string label = c.name.empty() ? (t.kind == TrackKind::Audio ? c.file : t.name) : c.name;
    dl->AddText(ImVec2(std::max(a.x, visX0) + 4.0f, a.y + (headH - em) * 0.5f), mul(IM_COL32(20, 22, 26, 255), 1.0f, alpha),
                (label + (c.loop > 0 ? "  (loop)" : "")).c_str());
    ImGui::PopClipRect();
    dl->AddRect(a, b, sel ? IM_COL32(255, 255, 255, 235) : mul(cc, 0.7f, alpha), 3.0f, 0, sel ? 2.0f : 1.0f);
}

// The ruler over the lanes: bars, the loop range above them.
void ruler(State& st, const Host& h, ImVec2 a, ImVec2 b) {
    sg::Song& s = st.song;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const float em = ImGui::GetFontSize();
    const float loopH = (b.y - a.y) * 0.42f;
    auto xOf = [&](double t) { return a.x + static_cast<float>((t - st.view0) * st.ppt); };
    auto tOf = [&](float x) { return st.view0 + static_cast<double>(x - a.x) / st.ppt; };

    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("##ruler", ImVec2(b.x - a.x, b.y - a.y), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    if (h.probe) h.probe("ruler", a, b);
    const bool hovered = ImGui::IsItemHovered();

    dl->AddRectFilled(a, b, IM_COL32(30, 33, 40, 255));
    dl->AddRectFilled(a, ImVec2(b.x, a.y + loopH), IM_COL32(24, 26, 32, 255));
    ImGui::PushClipRect(a, b, true);
    // The loop range.
    {
        const float l0 = xOf(static_cast<double>(s.loopStart)), l1 = xOf(static_cast<double>(s.loopEnd));
        const ImU32 lc = s.loop ? IM_COL32(90, 150, 230, 210) : IM_COL32(120, 128, 140, 120);
        dl->AddRectFilled(ImVec2(l0, a.y + 2.0f), ImVec2(l1, a.y + loopH - 1.0f), lc, 2.0f);
    }
    // Bars and beats; numbers as often as there is room for them.
    const std::int64_t bar = s.ticksPerBar(), beat = s.ticksPerBeat();
    const float barPx = static_cast<float>(static_cast<double>(bar) * st.ppt);
    int every = 1;
    while (barPx * static_cast<float>(every) < em * 2.6f && every < 4096) every *= 2;
    const double t0 = std::max(0.0, tOf(a.x)), t1 = tOf(b.x);
    for (std::int64_t k = static_cast<std::int64_t>(t0) / bar; static_cast<double>(k * bar) <= t1; ++k) {
        const float x = xOf(static_cast<double>(k * bar));
        const bool labelled = k % every == 0;
        dl->AddLine(ImVec2(x, labelled ? a.y + loopH : b.y - em * 0.6f), ImVec2(x, b.y), IM_COL32(150, 156, 168, 200));
        if (labelled) {
            char n[16];
            std::snprintf(n, sizeof n, "%lld", static_cast<long long>(k + 1));
            dl->AddText(ImVec2(x + 3.0f, a.y + loopH), IM_COL32(210, 214, 222, 255), n);
        }
        if (barPx > em * 3.0f)
            for (std::int64_t q = beat; q < bar; q += beat) {
                const float bx = xOf(static_cast<double>(k * bar + q));
                dl->AddLine(ImVec2(bx, b.y - em * 0.3f), ImVec2(bx, b.y), IM_COL32(120, 126, 138, 160));
            }
    }
    // The song's end.
    if (const std::int64_t end = s.length(); end > 0) {
        const float x = xOf(static_cast<double>(end));
        dl->AddTriangleFilled(ImVec2(x - 5.0f, b.y), ImVec2(x, b.y - 7.0f), ImVec2(x, b.y), IM_COL32(230, 90, 80, 220));
    }
    ImGui::PopClipRect();

    // Pressing: above, the loop range; below, the playhead.
    ArrDrag& d = st.drag;
    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        d = {};
        d.press = io.MousePos;
        d.pressTick = static_cast<std::int64_t>(std::max(0.0, tOf(io.MousePos.x)));
        if (io.MousePos.y < a.y + loopH) {
            const float l0 = xOf(static_cast<double>(s.loopStart)), l1 = xOf(static_cast<double>(s.loopEnd));
            d.base = s;
            d.v0 = s.loopStart;
            d.v1 = s.loopEnd;
            // A loop that is on: its edges resize it, its middle moves it. Off,
            // the range shown is only the last one, and a drag draws a new one.
            if (!s.loop) d.mode = ArrDrag::Loop;
            else if (std::fabs(io.MousePos.x - l0) < 6.0f) d.mode = ArrDrag::LoopL;
            else if (std::fabs(io.MousePos.x - l1) < 6.0f) d.mode = ArrDrag::LoopR;
            else if (io.MousePos.x > l0 && io.MousePos.x < l1) d.mode = ArrDrag::LoopMove;
            else d.mode = ArrDrag::Loop;
        } else {
            d.mode = ArrDrag::Scrub;
        }
    }
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && io.MousePos.y < a.y + loopH) {
        st.checkpoint();
        s.loop = !s.loop;
        st.edited();
    }
    if (ImGui::IsItemActive() && d.mode != ArrDrag::None) {
        const auto t = static_cast<std::int64_t>(std::max(0.0, tOf(io.MousePos.x)));
        const std::int64_t g = st.gridOf(st.arrSnap);
        switch (d.mode) {
        case ArrDrag::Scrub: st.seek(st.snapTo(t, st.arrSnap)); break;
        case ArrDrag::Loop:
            if (std::fabs(io.MousePos.x - d.press.x) > 3.0f) {
                std::int64_t p0 = st.snapTo(d.pressTick, st.arrSnap), p1 = st.snapTo(t, st.arrSnap);
                if (p1 < p0) std::swap(p0, p1);
                if (p1 - p0 < g) p1 = p0 + g;
                s.loopStart = p0;
                s.loopEnd = p1;
                s.loop = true;
                d.moved = true;
                st.edited();
            }
            break;
        case ArrDrag::LoopL:
            s.loopStart = std::clamp(st.snapTo(t, st.arrSnap), std::int64_t{0}, s.loopEnd - 1);
            d.moved = true;
            st.edited();
            break;
        case ArrDrag::LoopR:
            s.loopEnd = std::max(s.loopStart + 1, st.snapTo(t, st.arrSnap));
            d.moved = true;
            st.edited();
            break;
        case ArrDrag::LoopMove: {
            std::int64_t delta = st.snapTo(d.v0 + (t - d.pressTick), st.arrSnap) - d.v0;
            delta = std::max(delta, -d.v0);
            s.loopStart = d.v0 + delta;
            s.loopEnd = d.v1 + delta;
            d.moved = true;
            st.edited();
            break;
        }
        default: break;
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (d.moved && d.mode >= ArrDrag::Loop && d.mode <= ArrDrag::LoopMove) st.checkpoint(d.base);
        if (d.mode == ArrDrag::Scrub) st.seek(st.snapTo(static_cast<std::int64_t>(std::max(0.0, tOf(io.MousePos.x))), st.arrSnap));
        d = {};
    }
    // The wheel over the ruler: zoom with Ctrl, otherwise along.
    if (hovered && io.MouseWheel != 0.0f) {
        if (io.KeyCtrl) {
            const double keep = tOf(io.MousePos.x);
            st.ppt = std::clamp(st.ppt * std::pow(1.18, static_cast<double>(io.MouseWheel)), 0.002, 2.0);
            st.view0 = std::max(0.0, keep - static_cast<double>(io.MousePos.x - a.x) / st.ppt);
        } else {
            st.view0 = std::max(0.0, st.view0 - static_cast<double>(io.MouseWheel) * (b.x - a.x) * 0.12 / st.ppt);
        }
    }
}

void laneMenu(State& st, const Host& h, int trackUid, std::int64_t at) {
    Track* t = st.song.track(trackUid);
    if (!t) return;
    if (t->hasNotes()) {
        if (ImGui::MenuItem("New MIDI clip here")) {
            st.checkpoint();
            Clip c;
            c.uid = st.song.newUid();
            const std::int64_t bar = st.song.ticksPerBar();
            c.start = at / bar * bar;
            c.length = bar;
            c.name = t->name;
            t->clips.push_back(c);
            t->sortClips();
            st.select(c.uid, false);
            st.editClip = c.uid;
            st.edited();
        }
    } else if (ImGui::BeginMenu("Add a sound here")) {
        const auto names = h.soundNames ? h.soundNames() : std::vector<std::string>{};
        if (names.empty()) ImGui::TextDisabled("No sounds in the project yet -- drop a file on the window");
        for (const std::string& n : names)
            if (ImGui::MenuItem(n.c_str())) addSoundClip(st, n, at, trackUid);
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Paste here", nullptr, false, !st.clipBoard.empty())) {
        st.selTrack = trackUid;
        paste(st, at);
    }
    if (ImGui::MenuItem("Set the loop to this bar")) {
        st.checkpoint();
        const std::int64_t bar = st.song.ticksPerBar();
        st.song.loopStart = at / bar * bar;
        st.song.loopEnd = st.song.loopStart + bar;
        st.song.loop = true;
        st.edited();
    }
}

void clipMenu(State& st, int uid) {
    Track* owner = nullptr;
    Clip* c = st.song.clip(uid, &owner);
    if (!c || !owner) return;
    ImGui::TextDisabled("%s", c->name.empty() ? owner->name.c_str() : c->name.c_str());
    static char buf[128];
    if (ImGui::IsWindowAppearing()) std::snprintf(buf, sizeof buf, "%s", c->name.c_str());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::InputText("Name", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
        st.checkpoint();
        c->name = buf;
        st.edited();
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::MenuItem("Open in the editor below")) st.editClip = uid;
    if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateSelection(st);
    if (ImGui::MenuItem("Split at the playhead", "S")) splitClips(st, static_cast<std::int64_t>(st.playTicks()), st.sel);
    if (ImGui::MenuItem("Mute", "M", c->mute)) {
        st.checkpoint();
        const bool to = !c->mute;
        for (int u : st.sel)
            if (Clip* k = st.song.clip(u)) k->mute = to;
        st.edited();
    }
    if (owner->hasNotes() && ImGui::MenuItem("Loop its content", nullptr, c->loop > 0)) {
        st.checkpoint();
        c->loop = c->loop > 0 ? 0 : std::max<std::int64_t>(st.song.ticksPerBeat(), c->length);
        st.edited();
    }
    if (owner->kind == TrackKind::Audio && ImGui::MenuItem("Length: the rest of the file")) {
        const double secs = st.fileLength(c->file) - c->sourceOffset;
        if (secs > 0.0) {
            st.checkpoint();
            c->length = std::max<std::int64_t>(1, st.song.tickAt(secs));
            st.edited();
        }
    }
    if (ImGui::BeginMenu("Color")) {
        for (int i = 0; i < 12; ++i) {
            if (i % 6) ImGui::SameLine(0, 3);
            ImGui::PushID(i);
            if (ImGui::ColorButton("##c", ImGui::ColorConvertU32ToFloat4(kPalette[i]), ImGuiColorEditFlags_NoTooltip,
                                   ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()))) {
                st.checkpoint();
                for (int u : st.sel)
                    if (Clip* k = st.song.clip(u)) k->color = kPalette[i];
                st.edited();
            }
            ImGui::PopID();
        }
        if (ImGui::MenuItem("The track's")) {
            st.checkpoint();
            for (int u : st.sel)
                if (Clip* k = st.song.clip(u)) k->color = 0;
            st.edited();
        }
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Delete", "Del")) deleteSelection(st);
}

void arrangement(State& st, const Host& h, float height) {
    sg::Song& s = st.song;
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float em = ImGui::GetFontSize();
    const float fh = ImGui::GetFrameHeight();
    const float hw = std::round(em * 16.0f);
    const float rulerH = std::round(em * 2.2f);
    const float hsH = std::round(em * 0.75f);
    const ImVec2 o = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float laneX0 = o.x + hw;
    const float laneX1 = o.x + w - style.ScrollbarSize;
    const float laneW = std::max(60.0f, laneX1 - laneX0);
    st.laneW = laneW;
    const std::int64_t bar = s.ticksPerBar();
    if (st.ppt <= 0.0) {
        const double span = static_cast<double>(std::max<std::int64_t>(s.length(), bar * 8)) * 1.08;
        st.ppt = static_cast<double>(laneW) / span;
        st.view0 = 0.0;
    }
    auto xOf = [&](double t) { return laneX0 + static_cast<float>((t - st.view0) * st.ppt); };
    auto tOf = [&](float x) { return st.view0 + static_cast<double>(x - laneX0) / st.ppt; };

    // The corner over the headers.
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(o, ImVec2(laneX0, o.y + rulerH), IM_COL32(30, 33, 40, 255));
        ImGui::SetCursorScreenPos(ImVec2(o.x + 6.0f, o.y + (rulerH - fh) * 0.5f));
        auto addBtn = [&](const char* label, TrackKind k, const char* tip, const char* probeKey) {
            if (ImGui::SmallButton(label)) {
                st.checkpoint();
                Track& t = s.addTrack(k);
                st.selTrack = t.uid;
                st.edited();
            }
            if (h.probe) h.probe(probeKey, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            ImGui::SetItemTooltip("%s", tip);
        };
        addBtn("+ Instrument", TrackKind::Instrument, "A track with an instrument and the MIDI clips it plays", "add:instrument");
        ImGui::SameLine(0, 3);
        addBtn("+ MIDI", TrackKind::Midi, "MIDI clips that play another track's instrument", "add:midi");
        ImGui::SameLine(0, 3);
        addBtn("+ Audio", TrackKind::Audio, "Sound files on the timeline", "add:audio");
    }

    ruler(st, h, ImVec2(laneX0, o.y), ImVec2(laneX1, o.y + rulerH));

    // --- The lanes ---------------------------------------------------------------
    const float lanesH = std::max(40.0f, height - rulerH - hsH);
    ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + rulerH));
    ImGui::BeginChild("##lanes", ImVec2(w, lanesH), ImGuiChildFlags_None,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 co = ImGui::GetCursorScreenPos();
    const float visY0 = ImGui::GetWindowPos().y, visY1 = visY0 + lanesH;
    std::vector<float> rowY(s.tracks.size() + 1);
    {
        float y = co.y;
        for (std::size_t i = 0; i < s.tracks.size(); ++i) {
            rowY[i] = y;
            s.tracks[i].height = std::max(s.tracks[i].height, fh * 2.0f + 12.0f);
            y += s.tracks[i].height;
        }
        rowY[s.tracks.size()] = y;
    }
    const float lanesBottom = rowY.back();
    st.rowTop = rowY;
    st.rowUid.clear();
    for (const Track& t : s.tracks) st.rowUid.push_back(t.uid);
    st.laneX0 = laneX0;
    auto rowAt = [&](float y) -> int {
        for (std::size_t i = 0; i < s.tracks.size(); ++i)
            if (y >= rowY[i] && y < rowY[i + 1]) return static_cast<int>(i);
        return -1;
    };

    // Backgrounds and the grid under everything.
    std::vector<ClipRect> rects;
    const std::int64_t grid = st.gridOf(st.arrSnap);
    for (std::size_t i = 0; i < s.tracks.size(); ++i) {
        const Track& t = s.tracks[i];
        const float y0 = rowY[i], y1 = rowY[i + 1];
        if (y1 < visY0 || y0 > visY1) continue;
        dl->AddRectFilled(ImVec2(laneX0, y0), ImVec2(laneX1, y1 - 1.0f),
                          st.selTrack == t.uid ? IM_COL32(36, 40, 48, 255) : IM_COL32(28, 31, 37, 255));
        dl->AddLine(ImVec2(o.x, y1 - 1.0f), ImVec2(laneX1, y1 - 1.0f), IM_COL32(16, 18, 22, 255));
    }
    ImGui::PushClipRect(ImVec2(laneX0, visY0), ImVec2(laneX1, visY1), true);
    {
        const double t0 = std::max(0.0, tOf(laneX0)), t1 = tOf(laneX1);
        const std::int64_t beat = s.ticksPerBeat();
        const bool beats = beat * st.ppt > 6.0;
        for (std::int64_t k = static_cast<std::int64_t>(t0) / beat * beat; static_cast<double>(k) <= t1; k += beat) {
            const float x = xOf(static_cast<double>(k));
            const bool isBar = k % bar == 0;
            if (!isBar && !beats) continue;
            dl->AddLine(ImVec2(x, co.y), ImVec2(x, lanesBottom), isBar ? IM_COL32(70, 76, 88, 255) : IM_COL32(44, 48, 58, 255));
        }
        if (s.loop) {
            dl->AddRectFilled(ImVec2(xOf(static_cast<double>(s.loopStart)), co.y), ImVec2(xOf(static_cast<double>(s.loopEnd)), lanesBottom),
                              IM_COL32(90, 150, 230, 18));
        }
        // The clips.
        for (std::size_t i = 0; i < s.tracks.size(); ++i) {
            const Track& t = s.tracks[i];
            const float y0 = rowY[i] + 2.0f, y1 = rowY[i + 1] - 3.0f;
            if (y1 < visY0 || y0 > visY1) continue;
            const ImU32 col = trackColor(s, t);
            for (const Clip& c : t.clips) {
                const float x0 = xOf(static_cast<double>(c.start)), x1 = std::max(x0 + 3.0f, xOf(static_cast<double>(c.end())));
                if (x1 < laneX0 || x0 > laneX1) continue;
                rects.push_back({c.uid, static_cast<int>(i), ImVec2(x0, y0), ImVec2(x1, y1)});
                drawClip(st, dl, t, c, ImVec2(x0, y0), ImVec2(x1, y1), col, st.isSel(c.uid), laneX0, laneX1);
                if (h.probe) h.probe("clip:" + std::to_string(c.uid), ImVec2(x0, y0), ImVec2(x1, y1));
            }
        }
        // What is being drawn or lassoed.
        if (st.drag.mode == ArrDrag::DrawClip && st.drag.v1 > st.drag.v0 && st.drag.lane >= 0 &&
            st.drag.lane < static_cast<int>(s.tracks.size())) {
            const float y0 = rowY[static_cast<std::size_t>(st.drag.lane)] + 2.0f, y1 = rowY[static_cast<std::size_t>(st.drag.lane) + 1] - 3.0f;
            dl->AddRectFilled(ImVec2(xOf(static_cast<double>(st.drag.v0)), y0), ImVec2(xOf(static_cast<double>(st.drag.v1)), y1),
                              IM_COL32(255, 255, 255, 50), 3.0f);
        }
        if (st.drag.mode == ArrDrag::Rubber)
            dl->AddRect(st.drag.press, io.MousePos, IM_COL32(255, 255, 255, 160));
    }
    ImGui::PopClipRect();

    // The headers (widgets).
    HeaderActions act;
    for (std::size_t i = 0; i < s.tracks.size(); ++i) {
        const float y0 = rowY[i];
        if (rowY[i + 1] < visY0 - 40.0f || y0 > visY1 + 40.0f) continue;
        trackHeader(st, h, static_cast<int>(i), ImVec2(o.x, y0), hw, s.tracks[i].height, act);
    }

    // One button over every lane: what the pointer does there is worked out here.
    const float hitH = std::max(lanesBottom - co.y, 1.0f);
    ImGui::SetCursorScreenPos(ImVec2(laneX0, co.y));
    ImGui::InvisibleButton("##lanesbtn", ImVec2(laneW, hitH), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    for (std::size_t i = 0; i < s.tracks.size(); ++i)
        if (h.probe) h.probe("lane:" + std::to_string(s.tracks[i].uid), ImVec2(laneX0, rowY[i]), ImVec2(laneX1, rowY[i + 1]));
    const bool lanesHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);

    // Drops: sounds from the Assets panel.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("ASSET_GUID")) {
            const std::string guid(static_cast<const char*>(pl->Data), static_cast<std::size_t>(pl->DataSize));
            const std::string snd = h.soundOfAsset ? h.soundOfAsset(guid) : std::string();
            const int row = rowAt(io.MousePos.y);
            if (!snd.empty())
                addSoundClip(st, snd, st.snapTo(static_cast<std::int64_t>(std::max(0.0, tOf(io.MousePos.x))), st.arrSnap),
                             row >= 0 ? s.tracks[static_cast<std::size_t>(row)].uid : -1);
        }
        ImGui::EndDragDropTarget();
    }

    // What the pointer is over: a clip, and which part of it.
    enum Zone { Body, Left, Right, FadeL, FadeR };
    auto hit = [&](ImVec2 p, Zone& zone) -> const ClipRect* {
        for (auto it = rects.rbegin(); it != rects.rend(); ++it) {
            if (p.x < it->a.x || p.x > it->b.x || p.y < it->a.y || p.y > it->b.y) continue;
            const float edge = std::min(7.0f, (it->b.x - it->a.x) * 0.25f);
            zone = Body;
            const Clip* c = s.clip(it->uid);
            const Track& t = s.tracks[static_cast<std::size_t>(it->row)];
            if (t.kind == TrackKind::Audio && c && st.isSel(c->uid) && p.y < it->a.y + em * 1.3f) {
                const float fiX = it->a.x + static_cast<float>(s.tickAt(c->fadeIn) * st.ppt);
                const float foX = it->b.x - static_cast<float>(s.tickAt(c->fadeOut) * st.ppt);
                if (std::fabs(p.x - fiX) < 6.0f) { zone = FadeL; return &*it; }
                if (std::fabs(p.x - foX) < 6.0f) { zone = FadeR; return &*it; }
            }
            if (p.x < it->a.x + edge) zone = Left;
            else if (p.x > it->b.x - edge) zone = Right;
            return &*it;
        }
        return nullptr;
    };

    Zone hz = Body;
    const ClipRect* under = lanesHovered ? hit(io.MousePos, hz) : nullptr;
    if (under && st.drag.mode == ArrDrag::None && st.tool == Tool::Select) {
        if (hz == Left || hz == Right) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        else if (hz == FadeL || hz == FadeR) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    ArrDrag& d = st.drag;
    const auto mouseTick = static_cast<std::int64_t>(std::max(0.0, tOf(io.MousePos.x)));
    static int menuClip = -1, menuTrack = -1;
    static std::int64_t menuTick = 0;

    if (ImGui::IsItemActivated() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        st.prFocus = false;
        d = {};
        d.press = io.MousePos;
        d.pressTick = mouseTick;
        d.lane = rowAt(io.MousePos.y);
        d.track = d.lane >= 0 ? s.tracks[static_cast<std::size_t>(d.lane)].uid : -1;
        if (d.track >= 0) st.selTrack = d.track;
        Zone z = Body;
        const ClipRect* c = hit(io.MousePos, z);
        if (st.tool == Tool::Erase) {
            if (c) {
                st.select(c->uid, false);
                deleteSelection(st);
            }
        } else if (st.tool == Tool::Split) {
            if (c) splitClips(st, st.snapTo(mouseTick, st.arrSnap), {c->uid});
        } else if (c) {
            d.anchor = c->uid;
            d.base = s;
            if (io.KeyCtrl) {
                // Ctrl: toggle on a click, copy on a drag (decided once it moves).
                d.copy = true;
            } else if (io.KeyShift) {
                st.select(c->uid, true);
            } else if (!st.isSel(c->uid)) {
                st.select(c->uid, false);
            }
            const Clip* cl = s.clip(c->uid);
            d.v0 = cl->start;
            d.v1 = cl->end();
            d.f0 = cl->sourceOffset;
            d.mode = z == Left ? ArrDrag::TrimL : z == Right ? ArrDrag::TrimR : z == FadeL ? ArrDrag::FadeIn
                   : z == FadeR ? ArrDrag::FadeOut : ArrDrag::Move;
        } else if (st.tool == Tool::Draw && d.lane >= 0 && s.tracks[static_cast<std::size_t>(d.lane)].hasNotes()) {
            d.mode = ArrDrag::DrawClip;
            const std::int64_t g = std::max<std::int64_t>(1, grid);
            d.v0 = io.KeyAlt ? mouseTick : (mouseTick / g) * g;
            d.v1 = d.v0 + g;
        } else {
            d.mode = ArrDrag::Rubber;
            d.before = (io.KeyShift || io.KeyCtrl) ? st.sel : std::vector<int>{};
        }
    }

    if (ImGui::IsItemActive() && d.mode != ArrDrag::None && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const bool far = std::fabs(io.MousePos.x - d.press.x) > 3.0f || std::fabs(io.MousePos.y - d.press.y) > 3.0f;
        const std::int64_t minLen = std::max<std::int64_t>(sg::kPpq / 16, std::min<std::int64_t>(grid, sg::kPpq));
        switch (d.mode) {
        case ArrDrag::Move: {
            if (!far && !d.moved) break;
            // A Ctrl-drag copies what is selected -- the clip pressed on with it.
            if (!d.moved && d.copy && !st.isSel(d.anchor)) st.sel.push_back(d.anchor);
            d.moved = true;
            sg::Song n = d.base;
            std::int64_t delta = st.snapTo(d.v0 + (mouseTick - d.pressTick), st.arrSnap) - d.v0;
            // Rows: all the selection moves the same number, or none.
            int dRow = 0;
            const int now = rowAt(io.MousePos.y);
            if (now >= 0 && d.lane >= 0) dRow = now - d.lane;
            std::int64_t earliest = INT64_MAX;
            bool rowsOk = true;
            for (std::size_t i = 0; i < n.tracks.size(); ++i)
                for (const Clip& c : n.tracks[i].clips)
                    if (st.isSel(c.uid)) {
                        earliest = std::min(earliest, c.start);
                        const int to = static_cast<int>(i) + dRow;
                        if (to < 0 || to >= static_cast<int>(n.tracks.size()) ||
                            !compatible(n.tracks[i].kind, n.tracks[static_cast<std::size_t>(to)].kind))
                            rowsOk = false;
                    }
            if (!rowsOk) dRow = 0;
            if (earliest != INT64_MAX) delta = std::max(delta, -earliest);
            std::vector<std::pair<int, Clip>> moving;   // (row it goes to, clip)
            for (std::size_t i = 0; i < n.tracks.size(); ++i) {
                auto& clips = n.tracks[i].clips;
                for (auto it = clips.begin(); it != clips.end();) {
                    if (!st.isSel(it->uid)) { ++it; continue; }
                    Clip c = *it;
                    c.start += delta;
                    if (d.copy) {
                        c.uid = -c.uid;   // marked; given real uids on release
                        ++it;
                    } else {
                        it = clips.erase(it);
                    }
                    moving.push_back({static_cast<int>(i) + dRow, c});
                }
            }
            for (auto& [row, c] : moving) {
                n.tracks[static_cast<std::size_t>(row)].clips.push_back(c);
                n.tracks[static_cast<std::size_t>(row)].sortClips();
            }
            s = std::move(n);
            st.edited();
            break;
        }
        case ArrDrag::TrimR: {
            d.moved = true;
            Track* owner = nullptr;
            if (Clip* c = s.clip(d.anchor, &owner)) {
                std::int64_t e = std::max(c->start + minLen, st.snapTo(mouseTick, st.arrSnap));
                if (owner->kind == TrackKind::Audio) {
                    const double secs = st.fileLength(c->file);
                    if (secs > 0.0) e = std::min(e, c->start + s.tickAt(secs - c->sourceOffset));
                }
                c->length = std::max<std::int64_t>(1, e - c->start);
                st.edited();
            }
            break;
        }
        case ArrDrag::TrimL: {
            d.moved = true;
            Track* owner = nullptr;
            if (Clip* c = s.clip(d.anchor, &owner)) {
                std::int64_t lo = 0;
                if (owner->kind == TrackKind::Audio) lo = std::max<std::int64_t>(0, d.v0 - s.tickAt(d.f0));
                const std::int64_t ns = std::clamp(st.snapTo(mouseTick, st.arrSnap), lo, d.v1 - minLen);
                const Clip* orig = d.base.clip(d.anchor);
                c->start  = ns;
                c->length = d.v1 - ns;
                if (owner->kind == TrackKind::Audio) c->sourceOffset = std::max(0.0, d.f0 + s.seconds(ns - d.v0));
                else if (orig) c->offset = orig->offset + (ns - d.v0);
                owner->sortClips();
                st.edited();
            }
            break;
        }
        case ArrDrag::FadeIn:
        case ArrDrag::FadeOut: {
            d.moved = true;
            if (Clip* c = s.clip(d.anchor)) {
                const double len = s.seconds(c->length);
                if (d.mode == ArrDrag::FadeIn) c->fadeIn = static_cast<float>(std::clamp(s.seconds(mouseTick - c->start), 0.0, len));
                else c->fadeOut = static_cast<float>(std::clamp(s.seconds(c->end() - mouseTick), 0.0, len));
                st.edited();
            }
            break;
        }
        case ArrDrag::DrawClip: {
            const std::int64_t g = std::max<std::int64_t>(1, grid);
            d.v1 = std::max(d.v0 + g, st.snapTo(mouseTick, st.arrSnap));
            d.moved = d.moved || far;
            break;
        }
        case ArrDrag::Rubber: {
            if (!far && !d.moved) break;
            d.moved = true;
            const ImVec2 a(std::min(d.press.x, io.MousePos.x), std::min(d.press.y, io.MousePos.y));
            const ImVec2 b(std::max(d.press.x, io.MousePos.x), std::max(d.press.y, io.MousePos.y));
            st.sel = d.before;
            for (const ClipRect& r : rects)
                if (r.b.x >= a.x && r.a.x <= b.x && r.b.y >= a.y && r.a.y <= b.y && !st.isSel(r.uid)) st.sel.push_back(r.uid);
            break;
        }
        default: break;
        }
    }

    if (ImGui::IsItemDeactivated() && d.mode != ArrDrag::None) {
        switch (d.mode) {
        case ArrDrag::Move:
            if (d.moved) {
                // Copies get uids of their own and become the selection.
                if (d.copy) {
                    std::vector<int> made;
                    for (Track& t : s.tracks)
                        for (Clip& c : t.clips)
                            if (c.uid < 0) { c.uid = s.newUid(); made.push_back(c.uid); }
                    st.sel = made;
                }
                st.checkpoint(d.base);
            } else if (d.copy) {
                // A Ctrl-click: in or out of the selection.
                if (st.isSel(d.anchor)) st.sel.erase(std::remove(st.sel.begin(), st.sel.end(), d.anchor), st.sel.end());
                else st.sel.push_back(d.anchor);
            } else if (!io.KeyShift) {
                st.select(d.anchor, false);
            }
            st.editClip = d.anchor;
            break;
        case ArrDrag::TrimL:
        case ArrDrag::TrimR:
        case ArrDrag::FadeIn:
        case ArrDrag::FadeOut:
            if (d.moved) st.checkpoint(d.base);
            st.editClip = d.anchor;
            break;
        case ArrDrag::DrawClip:
            if (d.track >= 0) {
                st.checkpoint();
                if (Track* t = s.track(d.track)) {
                    Clip c;
                    c.uid = s.newUid();
                    c.start = d.v0;
                    c.length = std::max<std::int64_t>(d.v1 - d.v0, 1);
                    c.name = t->name;
                    t->clips.push_back(c);
                    t->sortClips();
                    st.select(c.uid, false);
                    st.editClip = c.uid;
                    st.edited();
                }
            }
            break;
        case ArrDrag::Rubber:
            if (!d.moved) {
                // A click on an empty lane: nothing selected, the playhead there.
                if (!io.KeyShift && !io.KeyCtrl) st.sel.clear();
                if (!st.playing()) st.seek(st.snapTo(d.pressTick, st.arrSnap));
            }
            break;
        default: break;
        }
        d = {};
    }

    // Double-click: an empty lane gets a clip; a clip opens below. The press
    // that made it a double-click is no drag of its own.
    if (lanesHovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && st.tool == Tool::Select) {
        d = {};
        Zone z;
        if (const ClipRect* c = hit(io.MousePos, z)) {
            st.editClip = c->uid;
        } else if (const int row = rowAt(io.MousePos.y); row >= 0) {
            Track& t = s.tracks[static_cast<std::size_t>(row)];
            if (t.hasNotes()) {
                st.checkpoint();
                Clip c;
                c.uid = s.newUid();
                c.start = mouseTick / bar * bar;
                c.length = bar;
                c.name = t.name;
                t.clips.push_back(c);
                t.sortClips();
                st.select(c.uid, false);
                st.editClip = c.uid;
                st.edited();
            } else {
                menuTrack = t.uid;
                menuTick = st.snapTo(mouseTick, st.arrSnap);
                menuClip = -1;
                ImGui::OpenPopup("##lanemenu");
            }
        }
    }

    // Right-click: a clip's menu, or the lane's.
    if (lanesHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        Zone z;
        const ClipRect* c = hit(io.MousePos, z);
        if (st.tool == Tool::Draw && c) {
            st.select(c->uid, false);
            deleteSelection(st);
        } else if (c) {
            if (!st.isSel(c->uid)) st.select(c->uid, false);
            menuClip = c->uid;
            ImGui::OpenPopup("##clipmenu");
        } else if (const int row = rowAt(io.MousePos.y); row >= 0) {
            menuTrack = s.tracks[static_cast<std::size_t>(row)].uid;
            menuTick = st.snapTo(mouseTick, st.arrSnap);
            st.selTrack = menuTrack;
            ImGui::OpenPopup("##lanemenu");
        }
    }
    if (ImGui::BeginPopup("##clipmenu")) {
        clipMenu(st, menuClip);
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("##lanemenu")) {
        laneMenu(st, h, menuTrack, menuTick);
        ImGui::EndPopup();
    }

    // Under the last track: room to add one, and to drop a sound on.
    ImGui::SetCursorScreenPos(ImVec2(o.x + 8.0f, lanesBottom + 6.0f));
    if (s.tracks.empty())
        ui::hint("No tracks. Add one with the buttons above, or drop a sound or a MIDI file here.");
    ImGui::SetCursorScreenPos(ImVec2(o.x, lanesBottom + fh * 2.5f));
    ImGui::Dummy(ImVec2(1.0f, 1.0f));

    // The wheel: up and down the tracks, along with Shift, zoom with Ctrl.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        if (io.KeyCtrl && io.MouseWheel != 0.0f) {
            const double keep = tOf(io.MousePos.x);
            st.ppt = std::clamp(st.ppt * std::pow(1.18, static_cast<double>(io.MouseWheel)), 0.002, 2.0);
            st.view0 = std::max(0.0, keep - static_cast<double>(io.MousePos.x - laneX0) / st.ppt);
        } else if (io.KeyShift && io.MouseWheel != 0.0f) {
            st.view0 = std::max(0.0, st.view0 - static_cast<double>(io.MouseWheel) * laneW * 0.12 / st.ppt);
        } else if (io.MouseWheel != 0.0f) {
            ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseWheel * em * 4.0f);
        }
        if (io.MouseWheelH != 0.0f) st.view0 = std::max(0.0, st.view0 - static_cast<double>(io.MouseWheelH) * laneW * 0.12 / st.ppt);
    }
    ImGui::EndChild();

    applyHeaderActions(st, act);

    // --- The playhead, over the ruler and the lanes --------------------------------------
    {
        ImDrawList* pd = ImGui::GetWindowDrawList();
        const double at = st.playTicks();
        const float x = xOf(at);
        pd->PushClipRect(ImVec2(laneX0, o.y), ImVec2(laneX1, o.y + rulerH + lanesH), true);
        pd->AddLine(ImVec2(x, o.y), ImVec2(x, o.y + rulerH + lanesH), st.recording ? IM_COL32(255, 90, 80, 255) : IM_COL32(255, 255, 255, 220), 1.5f);
        pd->AddTriangleFilled(ImVec2(x - 6.0f, o.y + rulerH - 9.0f), ImVec2(x + 6.0f, o.y + rulerH - 9.0f), ImVec2(x, o.y + rulerH), IM_COL32(255, 255, 255, 230));
        pd->PopClipRect();
        // Following: a page on when it reaches the right.
        if (st.follow && st.playing() && st.drag.mode == ArrDrag::None) {
            const double vis = static_cast<double>(laneW) / st.ppt;
            if (at > st.view0 + vis * 0.92 || at < st.view0) st.view0 = std::max(0.0, at - vis * 0.08);
        }
    }

    // --- Along: a scroller under the lanes ------------------------------------------------
    {
        const ImVec2 a(laneX0, o.y + rulerH + lanesH + 2.0f), b(laneX1, o.y + rulerH + lanesH + hsH - 1.0f);
        ImDrawList* pd = ImGui::GetWindowDrawList();
        const double vis = static_cast<double>(laneW) / st.ppt;
        const double total = std::max({static_cast<double>(s.length()) + static_cast<double>(bar) * 8.0, st.view0 + vis, vis});
        const float tx0 = a.x + static_cast<float>(st.view0 / total) * (b.x - a.x);
        const float tx1 = a.x + static_cast<float>((st.view0 + vis) / total) * (b.x - a.x);
        pd->AddRectFilled(a, b, IM_COL32(24, 26, 32, 255), 3.0f);
        pd->AddRectFilled(ImVec2(tx0, a.y), ImVec2(std::max(tx0 + 12.0f, tx1), b.y), IM_COL32(90, 96, 110, 255), 3.0f);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("##hscroll", ImVec2(b.x - a.x, b.y - a.y));
        if (ImGui::IsItemActive()) {
            const double per = total / static_cast<double>(b.x - a.x);
            if (ImGui::IsItemActivated() && (io.MousePos.x < tx0 || io.MousePos.x > tx1))
                st.view0 = std::max(0.0, static_cast<double>(io.MousePos.x - a.x) * per - vis * 0.5);
            else
                st.view0 = std::max(0.0, st.view0 + static_cast<double>(io.MouseDelta.x) * per);
        }
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + height));
    }
}

// --- Keys -------------------------------------------------------------------------------

// The computer keyboard as a keyboard: two rows of a piano.
void qwertyKeys(State& st) {
    static const std::pair<ImGuiKey, int> keys[] = {
        {ImGuiKey_A, 0},  {ImGuiKey_W, 1},  {ImGuiKey_S, 2},  {ImGuiKey_E, 3},  {ImGuiKey_D, 4},
        {ImGuiKey_F, 5},  {ImGuiKey_T, 6},  {ImGuiKey_G, 7},  {ImGuiKey_Y, 8},  {ImGuiKey_Z, 8},
        {ImGuiKey_H, 9},  {ImGuiKey_U, 10}, {ImGuiKey_J, 11}, {ImGuiKey_K, 12}, {ImGuiKey_O, 13},
        {ImGuiKey_L, 14}, {ImGuiKey_P, 15},
    };
    for (const auto& [k, semi] : keys) {
        if (ImGui::IsKeyPressed(k, false) && !st.qwHeld.count(k)) {
            const int pitch = std::clamp((st.octave + 1) * 12 + semi, 0, 127);
            st.qwHeld[k] = pitch;
            playIn(st, pitch, st.qwVel, true);
        }

    }
    if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) st.octave = std::max(0, st.octave - 1);
    if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false)) st.octave = std::min(8, st.octave + 1);
}

void keys(State& st) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    const bool ctrl = io.KeyCtrl;
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) togglePlay(st);
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) redoStep(st); else undoStep(st); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) { redoStep(st); return; }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S, false)) { save(st); return; }
    if (st.qwerty) qwertyKeys(st);
    // The pane under the lanes has its own (notes rather than clips).
    if (st.prFocus && editorPaneKeys(st)) return;
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) st.seek(0);
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) st.seek(st.song.length());
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_RightArrow)) {
        const std::int64_t step = io.KeyShift ? st.song.ticksPerBar() : st.gridOf(st.arrSnap);
        const auto now = static_cast<std::int64_t>(std::llround(st.playTicks()));
        const std::int64_t base = (now / step) * step;
        st.seek(ImGui::IsKeyPressed(ImGuiKey_LeftArrow) ? (base == now ? base - step : base) : base + step);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) deleteSelection(st);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) st.sel.clear();
    if (ctrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelection(st);
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) { copySelection(st); deleteSelection(st); }
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) paste(st, st.snapTo(static_cast<std::int64_t>(st.playTicks()), st.arrSnap));
        if (ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateSelection(st);
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            st.sel.clear();
            for (const Track& t : st.song.tracks)
                for (const Clip& c : t.clips) st.sel.push_back(c.uid);
        }
        return;
    }
    if (st.qwerty) return;   // the letters are notes now
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) splitClips(st, static_cast<std::int64_t>(st.playTicks()), st.sel);
    if (ImGui::IsKeyPressed(ImGuiKey_M, false) && !st.sel.empty()) {
        st.checkpoint();
        const Clip* first = st.song.clip(st.sel.front());
        const bool to = first ? !first->mute : true;
        for (int u : st.sel)
            if (Clip* c = st.song.clip(u)) c->mute = to;
        st.edited();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_L, false)) { st.checkpoint(); st.song.loop = !st.song.loop; st.edited(); }
    if (ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        st.metronome = !st.metronome;
        if (st.player.isValid()) st.player.engine().setMetronome(st.metronome);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_R, false)) { if (st.recording) stopRecording(st); else startRecording(st); }
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) st.ppt = 0.0;
    if (ImGui::IsKeyPressed(ImGuiKey_1, false)) st.tool = Tool::Select;
    if (ImGui::IsKeyPressed(ImGuiKey_2, false)) st.tool = Tool::Draw;
    if (ImGui::IsKeyPressed(ImGuiKey_3, false)) st.tool = Tool::Erase;
    if (ImGui::IsKeyPressed(ImGuiKey_4, false)) st.tool = Tool::Split;
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) st.ppt = std::min(2.0, st.ppt * 1.25);
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) st.ppt = std::max(0.002, st.ppt / 1.25);
}

// Files dropped on the window: sounds become audio clips, MIDI files tracks
// (or a clip, on a track that plays notes) -- where they were dropped.
void takeDrops(State& st, const Host& h) {
    if (!h.drops || h.drops->empty()) return;
    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) return;
    ImGuiIO& io = ImGui::GetIO();
    const double t = std::max(0.0, st.view0 + static_cast<double>(io.MousePos.x - st.laneX0) / std::max(st.ppt, 1e-6));
    const auto at = st.snapTo(static_cast<std::int64_t>(t), st.arrSnap);
    // The track under the pointer, by the rows as they were last drawn.
    int track = -1;
    for (std::size_t i = 0; i + 1 < st.rowTop.size() && i < st.rowUid.size(); ++i)
        if (io.MousePos.y >= st.rowTop[i] && io.MousePos.y < st.rowTop[i + 1]) track = st.rowUid[i];
    auto& dr = *h.drops;
    for (auto it = dr.begin(); it != dr.end();) {
        if (isSound(*it)) {
            const std::string name = h.importSound ? h.importSound(*it) : fsys::path(*it).filename().string();
            if (!name.empty()) addSoundClip(st, name, at, track);
            else st.say("Could not bring " + fsys::path(*it).filename().string() + " into the project", true);
            it = dr.erase(it);
        } else if (isMidi(*it)) {
            const std::int64_t bar = st.song.ticksPerBar();
            importMidiFile(st, *it, at / bar * bar, track);
            it = dr.erase(it);
        } else {
            ++it;
        }
    }
}

void saveAsPopup(State& st) {
    if (st.askName) {
        ImGui::OpenPopup("Save song as");
        st.askName = false;
    }
    if (ImGui::BeginPopupModal("Save song as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Name (saved in content/songs):");
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        const bool enter = ImGui::InputText("##songname", st.nameBuf, sizeof st.nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
        const std::string name = safeName(st.nameBuf);
        const std::string path = (fsys::path(st.songsDir()) / (name + ".json")).generic_string();
        std::error_code ec;
        if (fsys::exists(path, ec) && path != st.path) ui::hint("There is a song of that name: saving replaces it.");
        if (ImGui::Button("Save", ImVec2(ImGui::GetFontSize() * 7.0f, 0)) || enter) {
            st.song.name = name;
            saveTo(st, path);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 7.0f, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

} // namespace

// --- The panel ------------------------------------------------------------------------------

void install() {
    ImGuiSettingsHandler h;
    h.TypeName   = "FitzelSongs";
    h.TypeHash   = ImHashStr("FitzelSongs");
    h.ReadOpenFn = iniOpen;
    h.ReadLineFn = iniLine;
    h.WriteAllFn = iniWrite;
    ImGui::AddSettingsHandler(&h);
}

Panel::Panel() : m(std::make_unique<State>()) {
    std::fill(std::begin(m->midiHeld), std::end(m->midiHeld), -1);
    m->song = starterSong();
    resetView(*m);
}

Panel::~Panel() = default;

void Panel::draw(bool& show, const Host& h) {
    State& st = *m;
    st.host = &h;
    if (st.projectFile != h.projectFile) {
        // A project opened while the panel kept a song of another: that song is
        // saved where it was (if it had a place) and a fresh one begun.
        if (!st.projectFile.empty()) projectChanged(h);
        st.projectFile = h.projectFile;
        // ...or the one this project had open last time, if it is still there.
        std::error_code ec;
        const std::string dir = st.songsDir();
        if (!dir.empty() && g_lastSong.rfind(dir, 0) == 0 && fsys::exists(g_lastSong, ec) && st.path != g_lastSong &&
            !st.dirty)
            openSong(st, g_lastSong);
    }
    st.shown = show;
    // Closed: the preview stops and leaves the mixer.
    if (!show) {
        if (st.player.isValid() && st.player.isRunning()) {
            stopRecording(st);
            st.player.stop();
        }
        st.focused = false;
        return;
    }

    // The preview voice, made the first time it is needed.
    if (!st.player.isValid() && !st.playerTried && h.audio) {
        st.playerTried = true;
        st.player = fitzel::SongPlayer::create(*h.audio, &st.playerError);
        if (st.player.isValid()) {
            st.player.engine().setFiles(filesOf(h));
            st.player.engine().setMetronome(st.metronome);
        }
    }
    if (st.player.isValid()) {
        if (h.mixer && (st.routedMixer != h.mixer || st.routedStrip != h.strip)) {
            st.player.setOutput(*h.mixer, h.strip);
            st.routedMixer = h.mixer;
            st.routedStrip = h.strip;
        }
        st.player.start();
        if (st.sentRev != st.rev) {
            st.player.engine().setSong(st.song);
            st.sentRev = st.rev;
        }
    }
    if (st.heard.pitch >= 0 && ImGui::GetTime() > st.heard.until) {
        st.liveNote(st.heard.track, st.heard.pitch, 0, false);
        st.heard = {};
    }
    // Where a MIDI keyboard plays (its reader thread reads this), and what it
    // played since the last frame, for the take.
    st.liveTarget.store(noteTarget(st), std::memory_order_relaxed);
    if (st.midiIn)
        for (const MidiIn::Ev& e : st.midiIn->take()) recordIn(st, e.pitch, e.vel, e.on, static_cast<std::int64_t>(e.tick));
    // A key of the computer keyboard let go: whatever had the focus then, the
    // note ends -- a release seen by nobody is a note that never stops.
    for (auto it = st.qwHeld.begin(); it != st.qwHeld.end();) {
        if (!ImGui::IsKeyDown(static_cast<ImGuiKey>(it->first))) {
            playIn(st, it->second, 0, false);
            it = st.qwHeld.erase(it);
        } else {
            ++it;
        }
    }
    stepBounce(st, h);

    ImGui::SetNextWindowSize(ImVec2(1280.0f, 760.0f), ImGuiCond_FirstUseEver);
    const std::string title = std::string("Song Sequencer - ") + st.song.name + (st.dirty ? " *" : "") + "###Song Sequencer";
    if (!ImGui::Begin(title.c_str(), &show, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        st.focused = false;
        return;
    }
    st.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (st.focused) st.focusFrame = ImGui::GetFrameCount();

    // The song stays valid whatever was pressed: a selection or an edited clip
    // that names something gone names nothing.
    st.sel.erase(std::remove_if(st.sel.begin(), st.sel.end(), [&](int u) { return !st.song.clip(u); }), st.sel.end());
    if (st.editClip >= 0 && !st.song.clip(st.editClip)) st.editClip = -1;
    if (!st.song.track(st.selTrack)) st.selTrack = st.song.tracks.empty() ? -1 : st.song.tracks.front().uid;

    menuBar(st, h);
    transportBar(st);

    const float em = ImGui::GetFontSize();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (st.paneH <= 0.0f) st.paneH = std::round(avail.y * 0.42f);
    st.paneH = std::clamp(st.paneH, em * 6.0f, std::max(em * 6.0f, avail.y - em * 10.0f));
    const float arrH = std::max(em * 8.0f, avail.y - st.paneH - 6.0f);
    const ImVec2 arrAt = ImGui::GetCursorScreenPos();
    arrangement(st, h, arrH);

    // The splitter between the lanes and the pane.
    ImGui::SetCursorScreenPos(ImVec2(arrAt.x, arrAt.y + arrH));
    ImGui::InvisibleButton("##split", ImVec2(avail.x, 6.0f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) st.paneH = std::max(em * 6.0f, st.paneH - ImGui::GetIO().MouseDelta.y);
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), IM_COL32(20, 22, 26, 255));

    ImGui::SetCursorScreenPos(ImVec2(arrAt.x, arrAt.y + arrH + 6.0f));
    drawEditorPane(st, h, ImVec2(avail.x, std::max(1.0f, avail.y - arrH - 6.0f)));

    takeDrops(st, h);
    saveAsPopup(st);
    if (st.focused) keys(st);
    ImGui::End();
}

bool Panel::wantsSmoothFrames() const {
    const State& st = *m;
    if (!st.shown) return false;
    return st.playing() || st.recording || st.drag.mode != ArrDrag::None || st.pr.mode != PrDrag::None ||
           (st.qwerty && st.focused) || !st.qwHeld.empty() || st.bounce != nullptr || st.heard.pitch >= 0 ||
           st.masterHold > 0.01f;
}

bool Panel::hasKeyboard() const { return m->focused || ImGui::GetFrameCount() - m->focusFrame <= 1; }

void Panel::beforePlay(const Host& h) {
    State& st = *m;
    st.host = &h;
    stopRecording(st);
    if (st.player.isValid()) st.player.stop();
    if (st.dirty && !st.path.empty()) saveTo(st, st.path);
}

void Panel::projectChanged(const Host& h) {
    State& st = *m;
    st.host = &h;
    if (st.dirty && !st.path.empty()) saveTo(st, st.path);
    newSong(st);
    st.projectFile = h.projectFile;
    if (st.player.isValid()) st.player.engine().setFiles(filesOf(h));
    st.waves.clear();
    st.fileSeconds.clear();
}

bool Panel::open(const std::string& songName, const Host& h) {
    State& st = *m;
    st.host = &h;
    if (st.projectFile != h.projectFile) {
        if (!st.projectFile.empty()) projectChanged(h);
        st.projectFile = h.projectFile;
    }
    if (songName.empty() || st.songsDir().empty()) return false;
    fsys::path f = fsys::path(songName).is_absolute() ? fsys::path(songName) : fsys::path(st.songsDir()) / songName;
    if (!f.has_extension()) f += ".json";
    if (f.generic_string() == st.path) return true;
    if (st.dirty && !st.path.empty()) saveTo(st, st.path);
    return openSong(st, f.generic_string());
}

fitzel::song::Song& Panel::song() { return m->song; }
void Panel::edited() { m->edited(); }
int Panel::editedClip() const { return m->editClip; }
void Panel::editClip(int uid) {
    m->editClip = uid;
    m->select(uid, false);
}
double Panel::playhead() const { return m->playTicks(); }
bool Panel::canUndo() const { return !m->undo.empty(); }
const std::vector<int>& Panel::selection() const { return m->sel; }

} // namespace songui
