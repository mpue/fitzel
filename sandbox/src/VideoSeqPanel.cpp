#include "VideoSeqPanel.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <fitzel/audio/Song.hpp>
#include <fitzel/ui/ImGuiTexture.hpp>
#include <imgui_internal.h>

#include "FolderDialog.hpp"
#include "ImageDoc.hpp"   // img::systemFonts
#include "UiStyle.hpp"
#include "VideoSeqPanelState.hpp"

namespace vsequi {

namespace fs = std::filesystem;

namespace {

double now() {
    using clock = std::chrono::steady_clock;
    static const auto t0 = clock::now();
    return std::chrono::duration<double>(clock::now() - t0).count();
}

std::string g_lastFilm;   // imgui.ini: the film open last

void* iniOpen(ImGuiContext*, ImGuiSettingsHandler*, const char*) { return &g_lastFilm; }
void  iniLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
    if (std::strncmp(line, "Film=", 5) == 0) g_lastFilm = line + 5;
}
void iniWrite(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
    buf->appendf("[%s][Last]\n", h->TypeName);
    buf->appendf("Film=%s\n\n", g_lastFilm.c_str());
}

std::string safeName(std::string n) {
    for (char& c : n)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') c = '_';
    while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
    return n.empty() ? std::string("Untitled") : n;
}

bool inside(ImVec2 p, ImVec2 a, ImVec2 b) { return p.x >= a.x && p.y >= a.y && p.x < b.x && p.y < b.y; }

} // namespace

// --- Shared looks ------------------------------------------------------------------------

std::string timecode(double t, double fps, bool frames) {
    if (t < 0.0) t = 0.0;
    const double f = fps > 0.0 ? fps : 30.0;
    const std::int64_t total = static_cast<std::int64_t>(std::floor(t * f + 1e-6));
    const std::int64_t perSec = static_cast<std::int64_t>(std::llround(f));
    const std::int64_t secs = static_cast<std::int64_t>(std::floor(t + 1e-6));
    const int fr = static_cast<int>(perSec > 0 ? total - static_cast<std::int64_t>(std::floor(secs * f + 1e-6)) : 0);
    char b[48];
    if (frames)
        std::snprintf(b, sizeof b, "%02lld:%02lld:%02lld:%02d", static_cast<long long>(secs / 3600),
                      static_cast<long long>(secs / 60 % 60), static_cast<long long>(secs % 60), std::max(0, fr));
    else
        std::snprintf(b, sizeof b, "%02lld:%02lld:%02lld", static_cast<long long>(secs / 3600), static_cast<long long>(secs / 60 % 60),
                      static_cast<long long>(secs % 60));
    return b;
}

ImU32 clipColor(const vs::Clip& c) {
    switch (c.kind) {
    case vs::ClipKind::Video: return IM_COL32(70, 120, 200, 255);
    case vs::ClipKind::Image: return IM_COL32(48, 160, 150, 255);
    case vs::ClipKind::Text:  return IM_COL32(150, 104, 210, 255);
    case vs::ClipKind::Color: return IM_COL32(130, 134, 144, 255);
    case vs::ClipKind::Audio: return vs::isSongFile(c.file) ? IM_COL32(220, 140, 50, 255) : IM_COL32(70, 165, 95, 255);
    }
    return IM_COL32(120, 120, 120, 255);
}

vs::ClipKind kindOfFile(const std::string& file) {
    if (vs::isImageFile(file)) return vs::ClipKind::Image;
    if (vs::isAudioFile(file)) return vs::ClipKind::Audio;
    return vs::ClipKind::Video;
}

// --- State --------------------------------------------------------------------------------

void Panel::State::edited() {
    ++rev;
    dirty = true;
}

void Panel::State::checkpoint(const vs::Film& before) {
    undo.push_back({before, sel});
    if (undo.size() > 200) undo.erase(undo.begin());
    redo.clear();
}

void Panel::State::say(const std::string& msg, bool bad) {
    status    = msg;
    statusBad = bad;
    statusAt  = ImGui::GetTime();
}

bool Panel::State::isSel(int uid) const { return std::find(sel.begin(), sel.end(), uid) != sel.end(); }

void Panel::State::select(int uid, bool add) {
    if (!add) sel.clear();
    if (uid < 0) return;
    auto it = std::find(sel.begin(), sel.end(), uid);
    if (it != sel.end()) {
        if (add) sel.erase(it);
    } else {
        sel.push_back(uid);
    }
}

void Panel::State::seek(double t) {
    playhead = std::max(0.0, t);
    if (playing) {
        playFrom = playhead;
        playWall = now();
        if (player.isValid() && mixedRev == rev && playhead >= mixFrom) {
            const std::int64_t f = static_cast<std::int64_t>((playhead - mixFrom) * vs::kRate);
            player.play(f);
        }
    }
}

void Panel::State::play() {
    const double endT = loop && film.markOut >= 0 ? film.markOut : film.duration();
    if (playhead >= endT - film.frame() * 0.5) playhead = loop && film.markIn >= 0 ? film.markIn : 0.0;
    playing  = true;
    playFrom = playhead;
    playWall = now();
    if (player.isValid() && mixedRev == rev && playhead >= mixFrom)
        player.play(static_cast<std::int64_t>((playhead - mixFrom) * vs::kRate));
}

void Panel::State::stop() {
    if (playing) playhead = film.snap(playhead);
    playing = false;
    if (player.isValid()) player.stop();
}

std::string Panel::State::projectDir() const {
    return projectFile.empty() ? std::string() : fs::path(projectFile).parent_path().generic_string();
}

std::string Panel::State::filmsDir() const {
    const std::string d = projectDir();
    return d.empty() ? std::string() : (fs::path(d) / "content" / "films").generic_string();
}

std::string Panel::State::relative(const std::string& abs) const {
    const std::string d = projectDir();
    if (d.empty()) return fs::path(abs).generic_string();
    std::error_code ec;
    const fs::path rel = fs::relative(fs::path(abs), fs::path(d), ec);
    const std::string r = rel.generic_string();
    if (ec || r.empty() || r.rfind("..", 0) == 0) return fs::path(abs).generic_string();
    return r;
}

std::string Panel::State::absolute(const std::string& file) const {
    if (file.empty()) return file;
    const fs::path p(file);
    if (p.is_absolute()) return p.generic_string();
    const std::string d = projectDir();
    return d.empty() ? p.generic_string() : (fs::path(d) / p).generic_string();
}

double Panel::State::sourceSeconds(const vs::Clip& c) {
    if (c.kind == vs::ClipKind::Text || c.kind == vs::ClipKind::Color || c.kind == vs::ClipKind::Image) return -1.0;
    if (vs::isSongFile(c.file)) {
        auto s = lib.sound(c.file);
        return s && s->ready ? static_cast<double>(s->frames()) / vs::kRate : -1.0;
    }
    auto mi = lib.info(c.file);
    if (!mi || !mi->ok || mi->still) return -1.0;
    return mi->duration > 0.0 ? mi->duration : -1.0;
}

double Panel::State::end() const { return std::max(film.duration(), 10.0); }

ThumbTex* Panel::State::thumbsOf(const std::string& file) {
    if (file.empty() || vs::isAudioFile(file)) return nullptr;
    ThumbTex& t = thumbs[file];
    if (!t.strip) {
        t.strip = lib.thumbs(file);
        if (t.strip && t.strip->ready && !t.strip->strip.empty())
            t.tex = fitzel::Texture::fromPixels(t.strip->strip.px.data(), t.strip->strip.w, t.strip->strip.h, 4);
    }
    return t.strip ? &t : nullptr;
}

std::string Panel::State::filmName() const { return film.name.empty() ? std::string("Untitled") : film.name; }

// --- Edits ----------------------------------------------------------------------------------

namespace {

// The first track of `kind` that is free over [a, b) (and not locked), from
// `prefer` on; -1 when none is.
int freeTrack(const vs::Film& f, vs::TrackKind kind, double a, double b, int prefer) {
    auto freeOn = [&](const vs::Track& t) {
        if (t.kind != kind || t.locked) return false;
        for (const vs::Clip& c : t.clips)
            if (c.start < b - 1e-6 && c.end() > a + 1e-6) return false;
        return true;
    };
    if (const vs::Track* t = f.track(prefer); t && freeOn(*t)) return t->uid;
    // Video: from V1 (the bottom of the pile) up; audio: from A1 down.
    if (kind == vs::TrackKind::Video) {
        for (std::size_t i = f.tracks.size(); i-- > 0;)
            if (freeOn(f.tracks[i])) return f.tracks[i].uid;
    } else {
        for (const vs::Track& t : f.tracks)
            if (freeOn(t)) return t.uid;
    }
    return -1;
}

} // namespace

void bringIn(Panel::State& st, const std::string& path) {
    const std::string f = st.relative(path);
    if (std::find(st.film.media.begin(), st.film.media.end(), f) == st.film.media.end()) {
        st.film.media.push_back(f);
        st.edited();
    }
}

int addMediaClip(Panel::State& st, const std::string& fileIn, int trackUid, double at) {
    const std::string file = st.relative(st.absolute(fileIn));
    vs::Clip c;
    c.kind  = kindOfFile(file);
    c.file  = file;
    c.name  = fs::path(file).stem().string();
    c.start = st.film.snap(std::max(0.0, at));
    double len = 5.0;
    if (vs::isSongFile(file)) {
        fitzel::song::Song song;
        if (fitzel::song::Song::load(st.absolute(file), song)) len = song.seconds(song.length()) + 2.0;
    } else if (c.kind != vs::ClipKind::Image) {
        const vs::MediaInfo mi = st.lib.infoNow(file);
        if (!mi.ok) {
            st.say(fs::path(file).filename().string() + ": " + mi.error, true);
            return -1;
        }
        if (mi.still) c.kind = vs::ClipKind::Image;
        else if (!mi.video) c.kind = vs::ClipKind::Audio;
        if (!mi.still && mi.duration > 0.0) len = mi.duration;
    }
    c.length = std::max(st.film.frame(), st.film.snap(len));
    const vs::TrackKind tk = c.kind == vs::ClipKind::Audio ? vs::TrackKind::Audio : vs::TrackKind::Video;
    st.checkpoint();
    int target = -1;
    if (const vs::Track* t = st.film.track(trackUid); t && t->kind == tk && !t->locked) target = t->uid;
    if (target < 0) target = freeTrack(st.film, tk, c.start, c.end(), st.selTrack);
    if (target < 0) target = st.film.addTrack(tk);
    const int uid = st.film.addClip(target, c);
    st.select(uid, false);
    st.selTrack = target;
    bringIn(st, file);
    st.edited();
    return uid;
}

int addMadeClip(Panel::State& st, vs::ClipKind kind, int trackUid, double at) {
    vs::Clip c;
    c.kind   = kind;
    c.name   = kind == vs::ClipKind::Text ? "Title" : "Colour";
    c.start  = st.film.snap(std::max(0.0, at));
    c.length = st.film.snap(kind == vs::ClipKind::Text ? 4.0 : 3.0);
    if (kind == vs::ClipKind::Text) {
        c.text.text = "Title";
        c.fadeIn = c.fadeOut = 0.5f;
    }
    st.checkpoint();
    int target = -1;
    if (const vs::Track* t = st.film.track(trackUid); t && t->kind == vs::TrackKind::Video && !t->locked) target = t->uid;
    // A title goes over the pictures: on a track above the clips it covers.
    if (target < 0) {
        for (const vs::Track& t : st.film.tracks) {
            if (t.kind != vs::TrackKind::Video || t.locked) continue;
            bool free = true;
            for (const vs::Clip& o : t.clips)
                if (o.start < c.end() - 1e-6 && o.end() > c.start + 1e-6) free = false;
            if (free) { target = t.uid; break; }
        }
    }
    if (target < 0) target = st.film.addTrack(vs::TrackKind::Video);
    const int uid = st.film.addClip(target, c);
    st.select(uid, false);
    st.edited();
    return uid;
}

void deleteClips(Panel::State& st, const std::vector<int>& uidsIn, bool ripple) {
    std::vector<int> uids;
    for (int u : uidsIn)
        if (const vs::Track* t = st.film.trackOf(u); t && !t->locked) uids.push_back(u);
    if (uids.empty()) return;
    st.checkpoint();
    if (ripple) st.film.rippleDelete(uids);
    else
        for (int u : uids) st.film.removeClip(u);
    st.sel.clear();
    st.edited();
}

void splitAt(Panel::State& st, double t, std::vector<int> uids) {
    t = st.film.snap(t);
    // Nothing chosen (or nothing chosen under the playhead): every clip there.
    std::vector<int> under;
    for (const vs::Track& tr : st.film.tracks)
        for (const vs::Clip& c : tr.clips)
            if (!tr.locked && c.start < t - 1e-6 && c.end() > t + 1e-6 && (uids.empty() || std::find(uids.begin(), uids.end(), c.uid) != uids.end()))
                under.push_back(c.uid);
    if (under.empty() && !uids.empty()) return splitAt(st, t, {});
    if (under.empty()) {
        st.say("Nothing under the playhead to cut.");
        return;
    }
    st.checkpoint();
    int cuts = 0;
    for (int u : under) cuts += st.film.split(u, t) > 0 ? 1 : 0;
    st.edited();
    st.say("Cut " + std::to_string(cuts) + (cuts == 1 ? " clip" : " clips") + " at " + timecode(t, st.film.fps) + ".");
}

void copyClips(Panel::State& st) {
    st.board.clear();
    double first = 1e30;
    int topRow = 1 << 30;
    for (int u : st.sel)
        if (const vs::Clip* c = st.film.clip(u)) {
            first = std::min(first, c->start);
            topRow = std::min(topRow, st.film.rowOf(st.film.trackOf(u)->uid));
        }
    for (int u : st.sel)
        if (const vs::Clip* c = st.film.clip(u)) {
            const vs::Track* t = st.film.trackOf(u);
            vs::Clip copy = *c;
            copy.start -= first;
            st.board.push_back({t->kind, st.film.rowOf(t->uid) - topRow, copy});
        }
    if (!st.board.empty()) st.say("Copied " + std::to_string(st.board.size()) + (st.board.size() == 1 ? " clip." : " clips."));
}

void pasteClips(Panel::State& st, double at) {
    if (st.board.empty()) return;
    st.checkpoint();
    std::vector<int> made;
    // Onto the selected track (and the ones under it), each clip on a track of
    // its own kind.
    int base = st.film.rowOf(st.selTrack);
    for (const auto& b : st.board) {
        vs::Clip c = b.clip;
        c.uid = 0;
        c.start = st.film.snap(at + b.clip.start);
        int row = base >= 0 ? base + b.rowOffset : -1;
        int target = -1;
        if (row >= 0 && row < static_cast<int>(st.film.tracks.size()) && st.film.tracks[static_cast<std::size_t>(row)].kind == b.kind &&
            !st.film.tracks[static_cast<std::size_t>(row)].locked)
            target = st.film.tracks[static_cast<std::size_t>(row)].uid;
        if (target < 0) target = freeTrack(st.film, b.kind, c.start, c.end(), -1);
        if (target < 0) target = st.film.addTrack(b.kind);
        made.push_back(st.film.addClip(target, c));
    }
    st.sel = made;
    st.edited();
}

void duplicateClips(Panel::State& st) {
    if (st.sel.empty()) return;
    double a = 1e30, b = -1e30;
    for (int u : st.sel)
        if (const vs::Clip* c = st.film.clip(u)) { a = std::min(a, c->start); b = std::max(b, c->end()); }
    st.checkpoint();
    std::vector<int> made;
    for (int u : st.sel) {
        const vs::Track* t = st.film.trackOf(u);
        const vs::Clip* c = st.film.clip(u);
        if (!t || !c || t->locked) continue;
        vs::Clip copy = *c;
        copy.uid = 0;
        copy.start = st.film.snap(c->start + (b - a));
        made.push_back(st.film.addClip(t->uid, copy));
    }
    st.sel = made;
    st.edited();
}

void detachSound(Panel::State& st, int uid) {
    vs::Clip* v = st.film.clip(uid);
    if (!v || v->kind != vs::ClipKind::Video) return;
    st.checkpoint();
    vs::Clip a = *v;
    a.uid  = 0;
    a.kind = vs::ClipKind::Audio;
    a.name = v->name + " (sound)";
    a.muted = false;
    int target = freeTrack(st.film, vs::TrackKind::Audio, a.start, a.end(), -1);
    if (target < 0) target = st.film.addTrack(vs::TrackKind::Audio);
    v = st.film.clip(uid);
    v->muted = true;
    const int au = st.film.addClip(target, a);
    st.sel = {uid, au};
    st.edited();
    st.say("The sound is a clip of its own now; the video's own is off.");
}

// --- Files -------------------------------------------------------------------------------------

namespace {

void remember(const std::string& path) {
    g_lastFilm = path;
    ImGui::MarkIniSettingsDirty();
}

void resetView(Panel::State& st) {
    st.pps = 0.0;
    st.view0 = 0.0;
    st.scrollY = 0.0f;
    st.sel.clear();
    st.selTrack = st.film.tracks.empty() ? -1 : st.film.tracks.front().uid;
    st.playhead = 0.0;
}

void newFilm(Panel::State& st) {
    st.stop();
    st.film = vs::Film::starter();
    st.path.clear();
    st.dirty = false;
    st.undo.clear();
    st.redo.clear();
    st.edited();
    st.dirty = false;
    resetView(st);
}

bool openFilm(Panel::State& st, const std::string& path) {
    vs::Film f;
    std::string err;
    if (!vs::Film::load(path, f, &err)) {
        st.say("Could not open " + fs::path(path).filename().string() + ": " + err, true);
        return false;
    }
    st.stop();
    st.film = std::move(f);
    st.path = fs::path(path).generic_string();
    st.undo.clear();
    st.redo.clear();
    st.edited();
    st.dirty = false;
    resetView(st);
    remember(st.path);
    st.say("Opened " + st.film.name + ".");
    return true;
}

bool saveTo(Panel::State& st, const std::string& path) {
    std::string err;
    if (!st.film.save(path, &err)) {
        st.say("Could not save: " + err, true);
        return false;
    }
    st.path  = fs::path(path).generic_string();
    st.dirty = false;
    remember(st.path);
    st.say("Saved " + fs::path(path).filename().string() + ".");
    return true;
}

void save(Panel::State& st) {
    if (!st.path.empty()) {
        saveTo(st, st.path);
        return;
    }
    if (st.filmsDir().empty()) {
        st.say("Open a project first: films are kept in it (content/films).", true);
        return;
    }
    std::snprintf(st.nameBuf, sizeof st.nameBuf, "%s", st.filmName().c_str());
    st.askName = true;
}

std::vector<std::string> filmFiles(const Panel::State& st) {
    std::vector<std::string> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(st.filmsDir(), ec))
        if (e.path().extension() == ".json") out.push_back(e.path().generic_string());
    std::sort(out.begin(), out.end());
    return out;
}

// The project's files a film can use, by folder, for the bin's "From the project".
std::vector<std::string> projectMedia(const Panel::State& st) {
    std::vector<std::string> out;
    const std::string d = st.projectDir();
    if (d.empty()) return out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(d, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (!name.empty() && (name[0] == '.' || name == "build" || name == "bin" || name == "obj" || name == "cache" || name == "export"))
                it.disable_recursion_pending();
            continue;
        }
        if (it.depth() > 4) continue;
        const std::string p = it->path().generic_string();
        if (vs::isVideoFile(p) || vs::isImageFile(p) || vs::isAudioFile(p)) out.push_back(st.relative(p));
        if (out.size() > 2000) break;
    }
    std::sort(out.begin(), out.end());
    return out;
}

// --- Undo -----------------------------------------------------------------------------

void undoStep(Panel::State& st) {
    if (st.undo.empty()) return;
    st.redo.push_back({st.film, st.sel});
    st.film = std::move(st.undo.back().film);
    st.sel  = std::move(st.undo.back().sel);
    st.undo.pop_back();
    st.edited();
}

void redoStep(Panel::State& st) {
    if (st.redo.empty()) return;
    st.undo.push_back({st.film, st.sel});
    st.film = std::move(st.redo.back().film);
    st.sel  = std::move(st.redo.back().sel);
    st.redo.pop_back();
    st.edited();
}

// --- Export --------------------------------------------------------------------------------------

std::string defaultExportPath(const Panel::State& st, vs::Format f) {
    std::string dir = st.projectDir();
    const char* sub = f == vs::Format::Fvid ? "videos" : f == vs::Format::Wav ? "sounds" : "exports";
    if (dir.empty()) {
        const char* home = std::getenv("HOME");
#ifdef _WIN32
        if (!home) home = std::getenv("USERPROFILE");
#endif
        dir = home ? (fs::path(home) / "Videos").generic_string() : fs::current_path().generic_string();
        sub = "";
    }
    const fs::path p = (*sub ? fs::path(dir) / sub : fs::path(dir)) / (safeName(st.filmName()) + vs::formatExtension(f));
    return p.generic_string();
}

// Songs whose sound is not in the library yet (or is from before they were
// saved again).
std::vector<std::string> staleSongs(Panel::State& st) {
    std::vector<std::string> out;
    for (const vs::Track& t : st.film.tracks)
        for (const vs::Clip& c : t.clips)
            if (c.kind == vs::ClipKind::Audio && vs::isSongFile(c.file) && st.lib.songStale(c.file) &&
                std::find(st.bounceFailed.begin(), st.bounceFailed.end(), c.file) == st.bounceFailed.end() &&
                std::find(out.begin(), out.end(), c.file) == out.end())
                out.push_back(c.file);
    return out;
}

void startExport(Panel::State& st) {
    if (st.bounce || !staleSongs(st).empty()) {
        st.exportPending = true;
        st.say("Bouncing the songs first -- the export starts after.");
        return;
    }
    st.exportPending = false;
    st.exportSet.path = st.exportPath;
    if (st.exportRange == 1 && (st.film.markIn >= 0 || st.film.markOut >= 0)) {
        st.exportSet.from = std::max(0.0, st.film.markIn);
        st.exportSet.to   = st.film.markOut >= 0 ? st.film.markOut : -1.0;
    } else {
        st.exportSet.from = 0.0;
        st.exportSet.to   = -1.0;
    }
    std::string err;
    if (!st.exporter.start(st.film, st.exportSet, st.lib, &err)) {
        st.say("Export: " + err, true);
        return;
    }
    st.exportWatch = true;
    st.say("Exporting to " + fs::path(st.exportSet.path).filename().string() + "...");
}

// --- Songs -------------------------------------------------------------------------------------

void stepSongs(Panel::State& st, const Host& h) {
    if (!st.bounce) {
        const std::vector<std::string> stale = staleSongs(st);
        if (stale.empty()) {
            if (st.exportPending) startExport(st);
            return;
        }
        const std::string file = stale.front();
        fitzel::song::Song song;
        std::string err;
        if (!fitzel::song::Song::load(st.absolute(file), song, &err)) {
            st.bounceFailed.push_back(file);
            st.say(fs::path(file).filename().string() + ": " + err, true);
            return;
        }
        fitzel::song::Files files;
        files.sound = h.resolveSound;
        files.patch = h.resolvePatch;
        st.bounce = std::make_unique<fitzel::song::Bounce>();
        st.bounce->start(song, files, static_cast<double>(vs::kRate), 2.0);
        st.bounceFile = file;
    }
    // As much as about 8 ms a frame allows: the editor keeps drawing.
    const double until = now() + 0.008;
    while (!st.bounce->done() && now() < until)
        if (!st.bounce->step(4096)) break;
    if (!st.bounce->done()) return;
    fitzel::SampleData data = st.bounce->take();
    const std::string problems = st.bounce->problems();
    st.bounce.reset();
    st.lib.putSound(st.bounceFile, std::move(data));
    if (!problems.empty()) st.say(fs::path(st.bounceFile).stem().string() + ": " + problems, true);
    st.bounceFile.clear();
    st.edited();   // the mix has a new sound in it
    st.dirty = st.dirty;   // a bounce is not an edit of the film
}

// --- The preview's sound ----------------------------------------------------------------------

void stepSound(Panel::State& st, const Host& h) {
    if (!st.player.isValid() && !st.playerTried && h.audio) {
        st.playerTried = true;
        st.player = fitzel::SamplePlayer::create(*h.audio, 2, vs::kRate);
    }
    // A finished mix goes into the player.
    if (st.mixJob.valid() && st.mixJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        Panel::State::Mix mix = st.mixJob.get();
        st.mixWhole = mix.whole;
        if (st.player.isValid()) {
            st.player.setSamples(mix.pcm);
            st.mixFrom  = st.mixJobFrom;
            st.mixedRev = st.mixRev;
            if (st.playing) {
                const double t = st.playhead;
                if (t >= st.mixFrom) st.player.play(static_cast<std::int64_t>((t - st.mixFrom) * vs::kRate));
            }
        }
    }
    if (!st.player.isValid() || st.mixJob.valid()) return;
    // A new mix after an edit (not mid-drag: the release is the edit), when the
    // playhead left the part mixed, or when sounds it lacked have come in.
    const double span = std::min(600.0, st.film.duration() + 1.0);
    const bool outside = st.film.duration() > 600.0 &&
                         (st.playhead < st.mixFrom || st.playhead > st.mixFrom + st.mixSpan - 30.0);
    // Sounds the last mix lacked (still decoding): again once they are in --
    // not every frame while a song bounces, which brings its own edit.
    const bool lacking = !st.mixWhole && !st.lib.busy() && !st.bounce && now() - st.mixAt > 1.5;
    if ((st.mixRev != st.rev || outside || lacking) && st.drag.mode == Drag::None) {
        st.mixRev     = st.rev;
        st.mixAt      = now();
        st.mixJobFrom = st.film.duration() > 600.0 ? std::max(0.0, st.playhead - 5.0) : 0.0;
        st.mixSpan    = span;
        auto film = std::make_shared<vs::Film>(st.film);
        vs::Library* lib = &st.lib;
        const double from = st.mixJobFrom;
        st.mixJob = std::async(std::launch::async, [film, lib, from, span] {
            Panel::State::Mix mix;
            mix.pcm = vs::mixdown(*film, from, from + span, *lib, false, &mix.whole);
            return mix;
        });
    }
}

// --- Little widgets --------------------------------------------------------------------------

enum class Glyph { Play, Pause, ToStart, ToEnd, Back, Forward, Loop };

bool glyphButton(const char* id, Glyph g, bool on, float size, const char* tip) {
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
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
    case Glyph::ToStart:
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r * 0.65f, c.y + r), ink);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.9f, c.y - r), ImVec2(c.x + r * 0.9f, c.y + r), ImVec2(c.x - r * 0.55f, c.y), ink);
        break;
    case Glyph::ToEnd:
        dl->AddRectFilled(ImVec2(c.x + r * 0.65f, c.y - r), ImVec2(c.x + r, c.y + r), ink);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.9f, c.y - r), ImVec2(c.x - r * 0.9f, c.y + r), ImVec2(c.x + r * 0.55f, c.y), ink);
        break;
    case Glyph::Back:
        dl->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x - r * 0.65f, c.y + r), ink);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.5f, c.y - r * 0.8f), ImVec2(c.x + r * 0.5f, c.y + r * 0.8f), ImVec2(c.x - r * 0.4f, c.y), ink);
        break;
    case Glyph::Forward:
        dl->AddRectFilled(ImVec2(c.x + r * 0.65f, c.y - r), ImVec2(c.x + r, c.y + r), ink);
        dl->AddTriangleFilled(ImVec2(c.x - r * 0.5f, c.y - r * 0.8f), ImVec2(c.x - r * 0.5f, c.y + r * 0.8f), ImVec2(c.x + r * 0.4f, c.y), ink);
        break;
    case Glyph::Loop:
        dl->AddCircle(c, r * 0.95f, ink, 16, 1.8f);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.95f - 3.5f, c.y - 1.0f), ImVec2(c.x + r * 0.95f + 3.5f, c.y - 1.0f),
                              ImVec2(c.x + r * 0.95f, c.y + 4.0f), ink);
        break;
    }
    return pressed;
}

// A number the editor's way -- a minus, the value, a plus, a click a step
// (Shift: ten) -- and typed when the value is known: the field in the middle
// takes text. Returns true on a change.
bool numField(const char* label, float& v, float step, float lo, float hi, const char* fmt, float width = 0.0f) {
    const float em = ImGui::GetFontSize();
    if (width <= 0.0f) width = em * 9.0f;
    bool changed = false;
    ImGui::PushID(label);
    const float bs = ImGui::GetFrameHeight();
    const float k = ImGui::GetIO().KeyShift ? 10.0f : 1.0f;
    if (ImGui::Button("-", ImVec2(bs, bs))) { v = std::max(lo, v - step * k); changed = true; }
    ImGui::SameLine(0.0f, 2.0f);
    ImGui::SetNextItemWidth(std::max(em * 3.0f, width - 2.0f * bs - 4.0f));
    float t = v;
    if (ImGui::InputFloat("##v", &t, 0.0f, 0.0f, fmt)) { v = std::clamp(t, lo, hi); changed = true; }
    ImGui::SameLine(0.0f, 2.0f);
    if (ImGui::Button("+", ImVec2(bs, bs))) { v = std::min(hi, v + step * k); changed = true; }
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    ImGui::PopID();
    return changed;
}

// A time: seconds, stepped a frame a click (Shift: ten), the timecode beside it.
bool timeField(const char* label, double& v, double lo, double hi, double fps) {
    float f = static_cast<float>(v);
    const float frame = static_cast<float>(1.0 / std::max(1.0, fps));
    if (!numField(label, f, frame, static_cast<float>(lo), static_cast<float>(hi), "%.3f s", ImGui::GetFontSize() * 11.0f)) return false;
    v = std::round(static_cast<double>(f) * fps) / fps;
    return true;
}

// --- The menu bar -------------------------------------------------------------------------------

void menuBar(Panel::State& st, const Host& h) {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New film")) {
            if (st.dirty && !st.path.empty()) saveTo(st, st.path);
            newFilm(st);
        }
        if (ImGui::BeginMenu("Open", !st.filmsDir().empty())) {
            const auto files = filmFiles(st);
            if (files.empty()) ImGui::TextDisabled("No films in this project yet");
            for (const std::string& f : files)
                if (ImGui::MenuItem(fs::path(f).stem().string().c_str(), nullptr, f == st.path)) {
                    if (st.dirty && !st.path.empty()) saveTo(st, st.path);
                    openFilm(st, f);
                }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Save", "Ctrl+S")) save(st);
        if (ImGui::MenuItem("Save as...", nullptr, false, !st.filmsDir().empty())) {
            std::snprintf(st.nameBuf, sizeof st.nameBuf, "%s", st.filmName().c_str());
            st.askName = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Import media...")) {
            std::vector<std::string> picked;
            if (ed::pickFiles(picked, st.projectDir(), "Video, pictures, sound",
                              "*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.gif;*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.wav;*.ogg;*.mp3;*.flac;*.m4a"))
                for (const std::string& p : picked) bringIn(st, p);
        }
        if (ImGui::MenuItem("Export...", "Ctrl+E")) st.exportDialog = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !st.undo.empty())) undoStep(st);
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, !st.redo.empty())) redoStep(st);
        ImGui::Separator();
        if (ImGui::MenuItem("Cut at the playhead", "S")) splitAt(st, st.playhead, st.sel);
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, !st.sel.empty())) copyClips(st);
        if (ImGui::MenuItem("Paste at the playhead", "Ctrl+V", false, !st.board.empty())) pasteClips(st, st.playhead);
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !st.sel.empty())) duplicateClips(st);
        if (ImGui::MenuItem("Delete", "Del", false, !st.sel.empty())) deleteClips(st, st.sel, false);
        if (ImGui::MenuItem("Delete and close the gap", "Shift+Del", false, !st.sel.empty())) deleteClips(st, st.sel, true);
        if (ImGui::MenuItem("Select all", "Ctrl+A")) {
            st.sel.clear();
            for (const vs::Track& t : st.film.tracks)
                for (const vs::Clip& c : t.clips) st.sel.push_back(c.uid);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Add")) {
        if (ImGui::MenuItem("Title at the playhead", "T")) addMadeClip(st, vs::ClipKind::Text, -1, st.playhead);
        if (ImGui::MenuItem("Colour at the playhead")) addMadeClip(st, vs::ClipKind::Color, -1, st.playhead);
        ImGui::Separator();
        if (ImGui::MenuItem("Video track")) { st.checkpoint(); st.film.addTrack(vs::TrackKind::Video); st.edited(); }
        if (ImGui::MenuItem("Audio track")) { st.checkpoint(); st.film.addTrack(vs::TrackKind::Audio); st.edited(); }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("The whole film", "F")) st.pps = 0.0;
        if (ImGui::MenuItem("Follow the playhead", nullptr, st.follow)) st.follow = !st.follow;
        if (ImGui::MenuItem("Snap to cuts", "N", st.snap)) st.snap = !st.snap;
        ImGui::Separator();
        if (ImGui::MenuItem("Safe areas", nullptr, st.safeAreas)) st.safeAreas = !st.safeAreas;
        const char* res[] = {"Preview: as fits", "Preview: full size", "Preview: half size", "Preview: quarter size"};
        const int vals[] = {0, 1, 2, 4};
        for (int i = 0; i < 4; ++i)
            if (ImGui::MenuItem(res[i], nullptr, st.previewDiv == vals[i])) st.previewDiv = vals[i];
        ImGui::EndMenu();
    }
    (void)h;
    ImGui::EndMenuBar();
}

// --- The transport -------------------------------------------------------------------------------

void transportBar(Panel::State& st) {
    const float em = ImGui::GetFontSize();
    const float bs = std::round(em * 1.7f);
    if (glyphButton("##start", Glyph::ToStart, false, bs, "To the start (Home)")) st.seek(st.film.markIn >= 0 ? st.film.markIn : 0.0);
    ImGui::SameLine(0, 3);
    if (glyphButton("##back", Glyph::Back, false, bs, "One frame back (Left; Shift: a second)")) st.seek(st.playhead - st.film.frame());
    ImGui::SameLine(0, 3);
    if (glyphButton("##play", st.playing ? Glyph::Pause : Glyph::Play, st.playing, bs, "Play / pause (Space)")) {
        if (st.playing) st.stop();
        else st.play();
    }
    ImGui::SameLine(0, 3);
    if (glyphButton("##fwd", Glyph::Forward, false, bs, "One frame on (Right; Shift: a second)")) st.seek(st.playhead + st.film.frame());
    ImGui::SameLine(0, 3);
    if (glyphButton("##end", Glyph::ToEnd, false, bs, "To the end (End)")) st.seek(st.film.duration());
    ImGui::SameLine(0, 8);
    if (glyphButton("##loop", Glyph::Loop, st.loop, bs, st.loop ? "Looping (the range if one is set)" : "Loop")) st.loop = !st.loop;
    ImGui::SameLine(0, 12);
    // The time, big.
    if (ImFont* bold = ui::boldFont()) ImGui::PushFont(bold);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", timecode(st.playhead, st.film.fps).c_str());
    if (ui::boldFont()) ImGui::PopFont();
    ImGui::SameLine();
    ImGui::TextDisabled("/ %s", timecode(st.film.duration(), st.film.fps).c_str());
    ImGui::SameLine(0, 14);
    if (st.film.markIn >= 0 || st.film.markOut >= 0) {
        ImGui::TextDisabled("range %s - %s", st.film.markIn >= 0 ? timecode(st.film.markIn, st.film.fps).c_str() : "start",
                            st.film.markOut >= 0 ? timecode(st.film.markOut, st.film.fps).c_str() : "end");
        ImGui::SameLine(0, 14);
    }
    ImGui::TextDisabled("%dx%d  %.4g fps", st.film.width, st.film.height, st.film.fps);

    // On the right: the export, or its progress.
    const float right = ImGui::GetWindowContentRegionMax().x;
    if (st.exporter.running()) {
        const float w = em * 22.0f;
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + em, right - w));
        char lbl[96];
        std::snprintf(lbl, sizeof lbl, "%s  %d/%d", st.exporter.stage().c_str(), st.exporter.framesDone(), st.exporter.framesTotal());
        ImGui::ProgressBar(st.exporter.progress(), ImVec2(w - em * 5.0f, bs), lbl);
        ImGui::SameLine(0, 4);
        if (ImGui::Button("Cancel", ImVec2(em * 4.5f, bs))) st.exporter.cancel();
    } else if (st.exportPending) {
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + em, right - em * 16.0f));
        ImGui::TextDisabled("Bouncing songs for the export...");
    } else {
        ImGui::SameLine(std::max(ImGui::GetCursorPosX() + em, right - em * 7.0f));
        if (ImGui::Button("Export...", ImVec2(em * 7.0f, bs))) st.exportDialog = true;
        ImGui::SetItemTooltip("Write the film as a video (Ctrl+E)");
    }
}

// --- The bin ------------------------------------------------------------------------------------

void bin(Panel::State& st, const Host& h, ImVec2 size) {
    const float em = ImGui::GetFontSize();
    ImGui::BeginChild("##bin", size, ImGuiChildFlags_Borders);
    ui::sectionText("Media");
    if (ImGui::Button("Import...")) {
        std::vector<std::string> picked;
        if (ed::pickFiles(picked, st.projectDir(), "Video, pictures, sound",
                          "*.mp4;*.mov;*.mkv;*.webm;*.avi;*.m4v;*.gif;*.png;*.jpg;*.jpeg;*.bmp;*.tga;*.wav;*.ogg;*.mp3;*.flac;*.m4a"))
            for (const std::string& p : picked) bringIn(st, p);
    }
    ImGui::SetItemTooltip("Files from anywhere (also: drop them on the window)");
    ImGui::SameLine();
    if (ImGui::Button("From the project")) ImGui::OpenPopup("##projectMedia");
    ImGui::SetItemTooltip("The project's recordings, pictures, sounds and songs");
    if (ImGui::Button("+ Title")) addMadeClip(st, vs::ClipKind::Text, -1, st.playhead);
    ImGui::SetItemTooltip("A title at the playhead (T)");
    ImGui::SameLine();
    if (ImGui::Button("+ Colour")) addMadeClip(st, vs::ClipKind::Color, -1, st.playhead);
    ImGui::SetItemTooltip("A plain colour at the playhead (black for a fade through black)");
    if (ImGui::BeginPopup("##projectMedia")) {
        static std::vector<std::string> list;
        static double listedAt = -100.0;
        if (ImGui::IsWindowAppearing() || ImGui::GetTime() - listedAt > 5.0) {
            list = projectMedia(st);
            listedAt = ImGui::GetTime();
        }
        if (list.empty()) ImGui::TextDisabled(st.projectDir().empty() ? "No project open" : "Nothing a film can use in the project yet");
        std::string folder;
        for (const std::string& f : list) {
            const std::string parent = fs::path(f).parent_path().generic_string();
            if (parent != folder) {
                folder = parent;
                ImGui::SeparatorText(folder.empty() ? "(project)" : folder.c_str());
            }
            const bool have = std::find(st.film.media.begin(), st.film.media.end(), f) != st.film.media.end();
            if (ImGui::MenuItem(fs::path(f).filename().string().c_str(), nullptr, have)) {
                if (!have) bringIn(st, st.absolute(f));
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ui::searchBox("##binFilter", st.binFilter, sizeof st.binFilter, "Filter...");
    ImGui::Separator();
    ImGui::BeginChild("##binList");
    const float th = std::round(em * 2.6f);
    int remove = -1;
    for (int i = 0; i < static_cast<int>(st.film.media.size()); ++i) {
        const std::string& f = st.film.media[static_cast<std::size_t>(i)];
        const std::string name = fs::path(f).filename().string();
        if (!ui::icontains(name.c_str(), st.binFilter)) continue;
        ImGui::PushID(i);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float w = ImGui::GetContentRegionAvail().x;
        const bool picked = st.binSel == i;
        if (ImGui::Selectable("##row", picked, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(w, th + 4.0f))) {
            st.binSel = i;
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) addMediaClip(st, f, st.selTrack, st.playhead);
        }
        if (h.probe) h.probe("bin:" + std::to_string(i), ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("VSEQ_MEDIA", &i, sizeof i);
            ImGui::TextUnformatted(name.c_str());
            ImGui::EndDragDropSource();
        }
        auto mi = st.lib.info(f);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\nDrag onto a track, or double-click: at the playhead", st.absolute(f).c_str());
        if (ImGui::BeginPopupContextItem("##binMenu")) {
            if (ImGui::MenuItem("Add at the playhead")) addMediaClip(st, f, st.selTrack, st.playhead);
            bool used = false;
            for (const vs::Track& t : st.film.tracks)
                for (const vs::Clip& c : t.clips) used = used || c.file == f;
            if (ImGui::MenuItem("Take out of the bin", nullptr, false, !used)) remove = i;
            if (used) ImGui::TextDisabled("In use on the timeline");
            ImGui::EndPopup();
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // The picture of it.
        const ImVec2 ta(p.x + 2.0f, p.y + 2.0f);
        float tw = th * 16.0f / 9.0f;
        const vs::ClipKind kind = kindOfFile(f);
        if (kind == vs::ClipKind::Audio) {
            dl->AddRectFilled(ta, ImVec2(ta.x + tw, ta.y + th), vs::isSongFile(f) ? IM_COL32(120, 80, 30, 255) : IM_COL32(40, 90, 55, 255), 3.0f);
            if (auto snd = st.lib.sound(f); snd && snd->ready && !snd->wave.empty()) {
                for (int x = 0; x < static_cast<int>(tw); ++x) {
                    const float t0 = snd->wave.seconds * x / tw, t1 = snd->wave.seconds * (x + 1) / tw;
                    const float pk = std::sqrt(snd->wave.peakOver(t0, t1)) * th * 0.45f;
                    dl->AddLine(ImVec2(ta.x + x, ta.y + th * 0.5f - pk), ImVec2(ta.x + x, ta.y + th * 0.5f + pk), IM_COL32(180, 240, 190, 200));
                }
            }
            dl->AddText(ImVec2(ta.x + 4.0f, ta.y + 2.0f), IM_COL32(255, 255, 255, 220), vs::isSongFile(f) ? "song" : "sound");
        } else if (ThumbTex* tt = st.thumbsOf(f); tt && tt->strip->ready && tt->tex.isValid()) {
            const vs::ThumbStrip& s = *tt->strip;
            tw = th * static_cast<float>(s.w) / std::max(1, s.h);
            const int idx = s.count / 3;
            const float aw = static_cast<float>(s.strip.w), ah = static_cast<float>(s.strip.h);
            const float u0 = (idx % s.perRow) * s.w / aw, v0 = (idx / s.perRow) * s.h / ah;
            dl->AddImage(fitzel::imTexture(tt->tex.id()), ta, ImVec2(ta.x + tw, ta.y + th), ImVec2(u0, v0), ImVec2(u0 + s.w / aw, v0 + s.h / ah));
        } else {
            dl->AddRectFilled(ta, ImVec2(ta.x + tw, ta.y + th), IM_COL32(40, 44, 52, 255), 3.0f);
        }
        // What it is.
        const float tx = ta.x + tw + em * 0.5f;
        dl->PushClipRect(ImVec2(tx, p.y), ImVec2(p.x + w, p.y + th + 4.0f), true);
        const bool bad = mi && !mi->ok;
        dl->AddText(ImVec2(tx, p.y + 2.0f), bad ? IM_COL32(255, 140, 130, 255) : IM_COL32(230, 232, 238, 255), name.c_str());
        std::string what;
        if (!mi) what = "reading...";
        else if (!mi->ok) what = mi->error;
        else {
            char b[96];
            if (mi->still) std::snprintf(b, sizeof b, "picture  %dx%d", mi->width, mi->height);
            else if (mi->video) std::snprintf(b, sizeof b, "%s  %dx%d  %.3g fps%s", timecode(mi->duration, 30.0, false).c_str(), mi->width, mi->height, mi->fps, mi->audio ? "  sound" : "");
            else std::snprintf(b, sizeof b, "%s  sound", timecode(mi->duration, 30.0, false).c_str());
            what = b;
        }
        if (vs::isSongFile(f)) what = "a song of the Song Sequencer";
        dl->AddText(ImVec2(tx, p.y + 2.0f + em * 1.15f), IM_COL32(150, 155, 165, 255), what.c_str());
        dl->PopClipRect();
        ImGui::PopID();
    }
    if (st.film.media.empty())
        ui::hint("Nothing brought in yet. Import files, take them from the project, or drop them on this window -- "
                 "recordings, pictures, sounds and the Song Sequencer's songs.");
    ImGui::EndChild();
    if (remove >= 0) {
        st.film.media.erase(st.film.media.begin() + remove);
        st.binSel = -1;
        st.edited();
    }
    ImGui::EndChild();
}

// --- The viewer ------------------------------------------------------------------------------------

// Where clip `c` is drawn on the film's own frame (film pixels): its centre,
// size and turn -- at time `t` (`which` 0), or in its start (1) or end (2)
// look. False when it is not a picture or its size is not known yet.
bool clipBox(Panel::State& st, const vs::Clip& c, double t, float& cx, float& cy, float& w, float& h, float& rot, int which) {
    const float W = static_cast<float>(st.film.width), H = static_cast<float>(st.film.height);
    const vs::Look lk = which == 2 ? c.lookEnd : which == 1 ? c.look : c.lookAt(t);
    cx = W * 0.5f + lk.x * W;
    cy = H * 0.5f + lk.y * H;
    rot = lk.rotation;
    switch (c.kind) {
    case vs::ClipKind::Video:
    case vs::ClipKind::Image: {
        auto mi = st.lib.info(c.file);
        if (!mi || !mi->ok) return false;
        vs::fitSize(c.fit, mi->width, mi->height, st.film.width, st.film.height, w, h);
        break;
    }
    case vs::ClipKind::Text: {
        auto img = vs::textImage(c.text, st.film.height);
        if (!img) return false;
        w = static_cast<float>(img->w);
        h = static_cast<float>(img->h);
        break;
    }
    case vs::ClipKind::Color:
        w = W;
        h = H;
        break;
    case vs::ClipKind::Audio: return false;
    }
    w *= lk.scale;
    h *= lk.scale;
    return true;
}

void viewer(Panel::State& st, const Host& h, ImVec2 size) {
    const float em = ImGui::GetFontSize();
    ImGui::BeginChild("##viewer", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 a = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    dl->AddRectFilled(a, ImVec2(a.x + avail.x, a.y + avail.y), IM_COL32(12, 12, 14, 255));
    const float aspect = static_cast<float>(st.film.width) / std::max(1, st.film.height);
    const float barH = em * 1.4f;
    float vw = avail.x - 8.0f, vh = vw / aspect;
    if (vh > avail.y - barH - 8.0f) {
        vh = std::max(10.0f, avail.y - barH - 8.0f);
        vw = vh * aspect;
    }
    const ImVec2 va(std::round(a.x + (avail.x - vw) * 0.5f), std::round(a.y + 4.0f + (avail.y - barH - 8.0f - vh) * 0.5f));
    const ImVec2 vb(va.x + vw, va.y + vh);
    st.viewA = va;
    st.viewB = vb;
    if (h.probe) h.probe("viewer", va, vb);

    // The size the preview is drawn at: the viewer's pixels (or a set part of
    // the film's), in steps -- a new size is a new decoder for every clip.
    int pw = st.film.width, ph = st.film.height;
    if (st.previewDiv > 0) {
        pw /= st.previewDiv;
        ph /= st.previewDiv;
    } else {
        int div = 1;
        while (div < 8 && st.film.width / (div * 2) >= vw * 0.9f) div *= 2;
        pw /= div;
        ph /= div;
    }
    pw = std::max(16, pw & ~1);
    ph = std::max(16, ph & ~1);
    st.askedW = pw;
    st.askedH = ph;

    if (st.viewTex.isValid() && st.shownT >= 0.0) dl->AddImage(fitzel::imTexture(st.viewTex.id()), va, vb);
    else dl->AddRectFilled(va, vb, IM_COL32(0, 0, 0, 255));
    if (st.safeAreas) {
        for (float k : {0.9f, 0.8f}) {
            const float ix = vw * (1.0f - k) * 0.5f, iy = vh * (1.0f - k) * 0.5f;
            dl->AddRect(ImVec2(va.x + ix, va.y + iy), ImVec2(vb.x - ix, vb.y - iy), IM_COL32(255, 255, 255, 70));
        }
        dl->AddLine(ImVec2((va.x + vb.x) * 0.5f - 8, (va.y + vb.y) * 0.5f), ImVec2((va.x + vb.x) * 0.5f + 8, (va.y + vb.y) * 0.5f), IM_COL32(255, 255, 255, 90));
        dl->AddLine(ImVec2((va.x + vb.x) * 0.5f, (va.y + vb.y) * 0.5f - 8), ImVec2((va.x + vb.x) * 0.5f, (va.y + vb.y) * 0.5f + 8), IM_COL32(255, 255, 255, 90));
    }
    if (!vs::ffmpegReady()) {
        const char* msg = "No ffmpeg: video clips cannot be shown or exported.";
        dl->AddText(ImVec2(va.x + 10, va.y + 10), IM_COL32(255, 190, 120, 255), msg);
        dl->AddText(ImVec2(va.x + 10, va.y + 10 + em * 1.3f), IM_COL32(220, 220, 220, 255),
                    (std::string("Install it (") + vs::ffmpegInstallHint() + ") and open the editor again.").c_str());
    }

    // The selected picture's box and its handles: drag inside to move it, a
    // corner to size it, the knob over it to turn it.
    const vs::Clip* sc = st.sel.size() == 1 ? st.film.clip(st.sel.front()) : nullptr;
    const vs::Track* stTrack = sc ? st.film.trackOf(sc->uid) : nullptr;
    ImGuiIO& io = ImGui::GetIO();
    const float k = vw / std::max(1, st.film.width);
    bool overHandle = false;
    if (sc && sc->visual() && stTrack && !stTrack->locked) {
        // A clip that moves is set up by its two looks: the start's while the
        // playhead is in its first half, the end's after (and the box shows
        // that look, not the one in between on the picture).
        const bool viewDrag = st.drag.mode == Drag::ViewMove || st.drag.mode == Drag::ViewScale || st.drag.mode == Drag::ViewRotate;
        const bool endLook = viewDrag ? st.drag.endLook : sc->animate && st.playhead > sc->start + sc->length * 0.5;
        float cx, cy, w, hh, rot;
        const bool active = st.playhead >= sc->start && st.playhead < sc->end();
        if (clipBox(st, *sc, st.playhead, cx, cy, w, hh, rot, endLook ? 2 : 1)) {
            const ImVec2 c(va.x + cx * k, va.y + cy * k);
            const float rad = rot * 3.14159265f / 180.0f, cs = std::cos(rad), sn = std::sin(rad);
            auto pt = [&](float x, float y) { return ImVec2(c.x + (x * cs - y * sn) * k, c.y + (x * sn + y * cs) * k); };
            const ImVec2 q[4] = {pt(-w / 2, -hh / 2), pt(w / 2, -hh / 2), pt(w / 2, hh / 2), pt(-w / 2, hh / 2)};
            const ImU32 col = active ? IM_COL32(255, 200, 60, 230) : IM_COL32(255, 200, 60, 90);
            dl->PushClipRect(a, ImVec2(a.x + avail.x, a.y + avail.y), true);
            dl->AddPolyline(q, 4, col, ImDrawFlags_Closed, 1.5f);
            const float hs = em * 0.35f;
            for (const ImVec2& p : q) dl->AddRectFilled(ImVec2(p.x - hs, p.y - hs), ImVec2(p.x + hs, p.y + hs), col);
            const ImVec2 knob = pt(0, -hh / 2 - 26.0f / k);
            const ImVec2 topMid = pt(0, -hh / 2);
            dl->AddLine(topMid, knob, col, 1.2f);
            dl->AddCircleFilled(knob, hs * 1.2f, col);
            dl->PopClipRect();
            if (sc->animate) {
                const char* what = endLook ? "Setting the END of the clip's movement" : "Setting the START of the clip's movement";
                const ImVec2 ts = ImGui::CalcTextSize(what);
                dl->AddRectFilled(ImVec2(va.x + 4, vb.y - em * 1.45f), ImVec2(va.x + 12 + ts.x, vb.y - 2.0f), IM_COL32(0, 0, 0, 170), 3.0f);
                dl->AddText(ImVec2(va.x + 8, vb.y - em * 1.35f), IM_COL32(255, 210, 100, 255), what);
            }
            // Presses.
            auto near = [&](ImVec2 p, float r) { return (io.MousePos.x - p.x) * (io.MousePos.x - p.x) + (io.MousePos.y - p.y) * (io.MousePos.y - p.y) < r * r; };
            int corner = -1;
            for (int i = 0; i < 4; ++i)
                if (near(q[i], hs * 2.2f)) corner = i;
            const bool onKnob = near(knob, hs * 2.5f);
            // Inside the turned box?
            const float lx = ((io.MousePos.x - c.x) * cs + (io.MousePos.y - c.y) * sn) / k;
            const float ly = (-(io.MousePos.x - c.x) * sn + (io.MousePos.y - c.y) * cs) / k;
            const bool in = std::abs(lx) <= w / 2 && std::abs(ly) <= hh / 2;
            const bool hovered = ImGui::IsWindowHovered() && inside(io.MousePos, a, ImVec2(a.x + avail.x, a.y + avail.y));
            overHandle = hovered && (corner >= 0 || onKnob || in);
            if (hovered && st.drag.mode == Drag::None) {
                if (corner >= 0) ImGui::SetMouseCursor((corner % 2) ? ImGuiMouseCursor_ResizeNESW : ImGuiMouseCursor_ResizeNWSE);
                else if (onKnob) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                else if (in) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && (corner >= 0 || onKnob || in)) {
                    st.drag = {};
                    st.drag.base = st.film;
                    st.drag.press = io.MousePos;
                    st.drag.anchor = sc->uid;
                    st.drag.endLook = endLook;
                    const vs::Look& l = endLook ? sc->lookEnd : sc->look;
                    if (corner >= 0) {
                        st.drag.mode = Drag::ViewScale;
                        st.drag.f0 = l.scale;
                        st.drag.f1 = std::max(1.0f, std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y));
                    } else if (onKnob) {
                        st.drag.mode = Drag::ViewRotate;
                        st.drag.f0 = l.rotation;
                        st.drag.f1 = std::atan2(io.MousePos.y - c.y, io.MousePos.x - c.x);
                    } else {
                        st.drag.mode = Drag::ViewMove;
                        st.drag.f0 = l.x;
                        st.drag.f1 = l.y;
                    }
                }
            }
            if (st.drag.mode == Drag::ViewMove || st.drag.mode == Drag::ViewScale || st.drag.mode == Drag::ViewRotate) {
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    vs::Film f = st.drag.base;
                    if (vs::Clip* e = f.clip(st.drag.anchor)) {
                        vs::Look& l = st.drag.endLook ? e->lookEnd : e->look;
                        const float dx = io.MousePos.x - st.drag.press.x, dy = io.MousePos.y - st.drag.press.y;
                        if (std::abs(dx) + std::abs(dy) > 2.0f) st.drag.moved = true;
                        if (st.drag.mode == Drag::ViewMove) {
                            l.x = st.drag.f0 + dx / vw;
                            l.y = st.drag.f1 + dy / vh;
                            // The middle pulls: a picture centred is the usual wish.
                            if (!io.KeyCtrl) {
                                if (std::abs(l.x) * vw < 6.0f) l.x = 0.0f;
                                if (std::abs(l.y) * vh < 6.0f) l.y = 0.0f;
                            }
                        } else if (st.drag.mode == Drag::ViewScale) {
                            const float d = std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y);
                            l.scale = std::clamp(st.drag.f0 * d / st.drag.f1, 0.02f, 20.0f);
                            if (!io.KeyCtrl && std::abs(l.scale - 1.0f) < 0.03f) l.scale = 1.0f;
                        } else {
                            const float ang = std::atan2(io.MousePos.y - c.y, io.MousePos.x - c.x);
                            float deg = st.drag.f0 + (ang - st.drag.f1) * 180.0f / 3.14159265f;
                            if (io.KeyShift) deg = std::round(deg / 15.0f) * 15.0f;
                            else if (std::abs(std::remainder(deg, 90.0f)) < 2.5f) deg = std::round(deg / 90.0f) * 90.0f;
                            l.rotation = deg;
                        }
                    }
                    st.film = std::move(f);
                    st.edited();
                } else {
                    if (st.drag.moved) st.checkpoint(st.drag.base);
                    st.drag.mode = Drag::None;
                }
            }
        }
    }
    // A click on the picture (not on a handle) picks the clip shown there.
    if (!overHandle && st.drag.mode == Drag::None && ImGui::IsWindowHovered() && inside(io.MousePos, va, vb) &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const float fx = (io.MousePos.x - va.x) / k, fy = (io.MousePos.y - va.y) / k;
        int pick = -1;
        for (std::size_t r = 0; r < st.film.tracks.size() && pick < 0; ++r) {   // the top track first
            if (st.film.tracks[r].kind != vs::TrackKind::Video || st.film.tracks[r].hidden) continue;
            const auto act = st.film.activeAt(r, st.playhead);
            for (auto it = act.rbegin(); it != act.rend(); ++it) {
                float cx, cy, w, hh, rot;
                if (!clipBox(st, *it->clip, st.playhead, cx, cy, w, hh, rot, 0)) continue;
                const float rad = rot * 3.14159265f / 180.0f;
                const float lx = (fx - cx) * std::cos(rad) + (fy - cy) * std::sin(rad);
                const float ly = -(fx - cx) * std::sin(rad) + (fy - cy) * std::cos(rad);
                if (std::abs(lx) <= w / 2 && std::abs(ly) <= hh / 2) { pick = it->clip->uid; break; }
            }
        }
        st.select(pick, false);
        if (pick >= 0) st.selTrack = st.film.trackOf(pick)->uid;
    }

    // The line under the picture: what is being looked at.
    ImGui::SetCursorScreenPos(ImVec2(a.x + 6.0f, a.y + avail.y - barH));
    const bool settled = st.shownComplete && std::abs(st.shownT - st.film.snap(st.playhead)) < 1e-6 && st.shownRev == st.rev;
    ImGui::TextDisabled("%s   preview %dx%d%s", timecode(st.playhead, st.film.fps).c_str(), pw, ph, settled || st.playing ? "" : "   ...");
    if (!st.problems.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "  %s", st.problems.front().c_str());
    }
    ImGui::EndChild();
}

// --- The inspector ------------------------------------------------------------------------------

void lookFields(Panel::State& st, vs::Look& l, bool& changed, const char* id) {
    ImGui::PushID(id);
    float x = l.x * 100.0f, y = l.y * 100.0f, s = l.scale * 100.0f, o = l.opacity * 100.0f;
    if (numField("Left / right", x, 1.0f, -300.0f, 300.0f, "%.1f %%")) { l.x = x / 100.0f; changed = true; }
    if (numField("Up / down", y, 1.0f, -300.0f, 300.0f, "%.1f %%")) { l.y = y / 100.0f; changed = true; }
    if (numField("Size", s, 5.0f, 1.0f, 2000.0f, "%.0f %%")) { l.scale = s / 100.0f; changed = true; }
    if (numField("Turn", l.rotation, 5.0f, -3600.0f, 3600.0f, "%.1f deg")) changed = true;
    if (numField("Opacity", o, 5.0f, 0.0f, 100.0f, "%.0f %%")) { l.opacity = o / 100.0f; changed = true; }
    ImGui::PopID();
    (void)st;
}

bool colorField(const char* label, vs::Rgba& c, bool alpha = true) {
    float v[4] = {c.r, c.g, c.b, c.a};
    const bool ch = alpha ? ImGui::ColorEdit4(label, v, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)
                          : ImGui::ColorEdit3(label, v, ImGuiColorEditFlags_NoInputs);
    if (ch) c = {v[0], v[1], v[2], alpha ? v[3] : 1.0f};
    return ch;
}

void filmSettings(Panel::State& st, bool& changed) {
    ui::sectionText("The film");
    static char name[128];
    static const vs::Film* shownFor = nullptr;
    static std::uint64_t shownRev = 0;
    if (shownFor != &st.film || ImGui::IsWindowAppearing() || (shownRev != st.rev && !ImGui::IsAnyItemActive())) {
        std::snprintf(name, sizeof name, "%s", st.film.name.c_str());
        shownFor = &st.film;
        shownRev = st.rev;
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::InputText("Name", name, sizeof name)) { st.film.name = name; changed = true; }
    struct Size { const char* label; int w, h; };
    static const Size sizes[] = {{"1920 x 1080  (HD)", 1920, 1080}, {"1280 x 720", 1280, 720}, {"3840 x 2160  (4K)", 3840, 2160},
                                 {"2560 x 1440", 2560, 1440}, {"1080 x 1920  (upright, phones)", 1080, 1920},
                                 {"1080 x 1080  (square)", 1080, 1080}, {"640 x 360  (a billboard in the game)", 640, 360}};
    char cur[64];
    std::snprintf(cur, sizeof cur, "%d x %d", st.film.width, st.film.height);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (ImGui::BeginCombo("Size", cur)) {
        for (const Size& s : sizes)
            if (ImGui::Selectable(s.label, s.w == st.film.width && s.h == st.film.height)) {
                st.film.width = s.w;
                st.film.height = s.h;
                changed = true;
            }
        ImGui::EndCombo();
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    char fcur[32];
    std::snprintf(fcur, sizeof fcur, "%.5g", st.film.fps);
    if (ImGui::BeginCombo("Frames a second", fcur)) {
        for (double r : {23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 60.0}) {
            char lbl[16];
            std::snprintf(lbl, sizeof lbl, "%.5g", r);
            if (ImGui::Selectable(lbl, std::abs(st.film.fps - r) < 1e-3)) {
                st.film.fps = r;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    if (colorField("Background", st.film.background, false)) changed = true;
    ImGui::Spacing();
    ui::hint("Pick a clip on the timeline (or in the picture) to set it up. The size and rate are the film's: every "
             "export is drawn at them unless it says otherwise.");
    ImGui::Spacing();
    ui::sectionText("Keys");
    ImGui::TextDisabled("Space play    J K L back / stop / play\n"
                        "Left Right a frame (Shift: a second)\n"
                        "Up Down the cut before / after\n"
                        "S cut at the playhead    T title\n"
                        "Del delete    Shift+Del close the gap\n"
                        "I O the range    Alt+X no range\n"
                        "Alt-drag a copy    Ctrl-drag no snap\n"
                        "F the whole film    N snap\n"
                        "Ctrl+Z Y C X V D A S E");
}

void inspector(Panel::State& st, const Host& h, ImVec2 size) {
    ImGui::BeginChild("##inspector", size, ImGuiChildFlags_Borders);
    const vs::Film before = st.film;
    bool changed = false;
    static ImGuiID lastEdit = 0;
    if (st.sel.size() != 1) {
        if (st.sel.size() > 1) {
            ui::sectionText("Clips");
            ImGui::Text("%zu clips picked", st.sel.size());
            if (ImGui::Button("Fade all in and out")) {
                for (int u : st.sel)
                    if (vs::Clip* c = st.film.clip(u)) c->fadeIn = c->fadeOut = static_cast<float>(std::min(0.5, c->length / 2));
                changed = true;
            }
            if (ImGui::Button("Delete them")) deleteClips(st, st.sel, false);
            ImGui::Separator();
        }
        filmSettings(st, changed);
    } else if (vs::Clip* c = st.film.clip(st.sel.front())) {
        const double fps = st.film.fps;
        ui::sectionText(c->kind == vs::ClipKind::Text ? "Title" : c->kind == vs::ClipKind::Color ? "Colour"
                        : c->kind == vs::ClipKind::Image ? "Picture" : c->kind == vs::ClipKind::Audio ? (vs::isSongFile(c->file) ? "Song" : "Sound") : "Video");
        static char name[128];
        static int nameFor = -1;
        if (nameFor != c->uid || (!ImGui::IsAnyItemActive() && name != c->name)) {
            std::snprintf(name, sizeof name, "%s", c->name.c_str());
            nameFor = c->uid;
        }
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::InputText("Name", name, sizeof name)) { c->name = name; changed = true; }
        if (!c->file.empty()) {
            ImGui::TextDisabled("%s", c->file.c_str());
            if (auto mi = st.lib.info(c->file); mi && !mi->ok) ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "%s", mi->error.c_str());
        }
        ImGui::Separator();
        const double src = st.sourceSeconds(*c);
        double start = c->start, length = c->length, in = c->in;
        if (timeField("Starts", start, 0.0, 36000.0, fps)) { c->start = start; changed = true; }
        if (timeField("Lasts", length, 1.0 / fps, src > 0 ? (src - c->in) / c->speed : 36000.0, fps)) { c->length = length; changed = true; }
        ImGui::TextDisabled("ends at %s", timecode(c->end(), fps).c_str());
        if (c->kind == vs::ClipKind::Video || c->kind == vs::ClipKind::Audio) {
            if (timeField("From the source at", in, 0.0, src > 0 ? std::max(0.0, src - c->length * c->speed) : 36000.0, fps)) { c->in = in; changed = true; }
            float sp = static_cast<float>(c->speed);
            if (numField("Speed", sp, 0.25f, 0.05f, 20.0f, "%.2fx")) {
                const double srcLen = c->length * c->speed;
                c->speed = sp;
                c->length = st.film.snap(std::max(1.0 / fps, srcLen / sp));
                changed = true;
            }
        }
        float fi = c->fadeIn, fo = c->fadeOut;
        const float frame = static_cast<float>(1.0 / fps);
        if (numField("Fade in", fi, frame * 5.0f, 0.0f, static_cast<float>(c->length), "%.2f s")) { c->fadeIn = fi; changed = true; }
        if (numField("Fade out", fo, frame * 5.0f, 0.0f, static_cast<float>(c->length), "%.2f s")) { c->fadeOut = fo; changed = true; }
        if (c->kind == vs::ClipKind::Video || c->kind == vs::ClipKind::Audio) {
            ImGui::Separator();
            float vol = c->volume * 100.0f;
            if (numField("Volume", vol, 5.0f, 0.0f, 400.0f, "%.0f %%")) { c->volume = vol / 100.0f; changed = true; }
            if (c->kind == vs::ClipKind::Video) {
                bool snd = !c->muted;
                if (ImGui::Checkbox("Its own sound", &snd)) { c->muted = !snd; changed = true; }
                auto mi = st.lib.info(c->file);
                if (mi && mi->audio && !c->muted) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Onto its own track")) detachSound(st, c->uid);
                }
            }
        }
        if (c->visual()) {
            ImGui::Separator();
            if (c->kind == vs::ClipKind::Video || c->kind == vs::ClipKind::Image) {
                int fit = static_cast<int>(c->fit);
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
                if (ImGui::Combo("Fills the frame", &fit, "All of it seen (bars)\0The frame covered (cut)\0Pulled to the frame\0")) {
                    c->fit = static_cast<vs::Fit>(fit);
                    changed = true;
                }
            }
            ImGui::TextDisabled(c->animate ? "At the start" : "Where it is");
            lookFields(st, c->look, changed, "look");
            if (ImGui::Checkbox("Moves over the clip", &c->animate)) {
                if (c->animate && c->lookEnd == vs::Look{}) c->lookEnd = c->look;
                changed = true;
            }
            ImGui::SetItemTooltip("From the start look to the end look, evenly over the clip");
            if (c->animate) {
                ImGui::TextDisabled("At the end");
                lookFields(st, c->lookEnd, changed, "end");
                if (ImGui::SmallButton("A slow zoom in")) {
                    c->lookEnd = c->look;
                    c->lookEnd.scale = c->look.scale * 1.15f;
                    changed = true;
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Swap")) {
                    std::swap(c->look, c->lookEnd);
                    changed = true;
                }
            }
            if (ImGui::SmallButton("Put back")) {
                c->look = {};
                c->lookEnd = {};
                c->animate = false;
                changed = true;
            }
            ImGui::SetItemTooltip("Centred, full size, upright, solid");
        }
        if (c->kind == vs::ClipKind::Text) {
            ImGui::Separator();
            static char text[2048];
            static int textFor = -1;
            if (textFor != c->uid || (!ImGui::IsAnyItemActive() && c->text.text != text)) {
                std::snprintf(text, sizeof text, "%s", c->text.text.c_str());
                textFor = c->uid;
            }
            if (ImGui::InputTextMultiline("##text", text, sizeof text, ImVec2(-1, ImGui::GetFontSize() * 4.0f))) {
                c->text.text = text;
                changed = true;
            }
            const auto& fonts = img::systemFonts();
            const std::string curFont = c->text.font.empty() ? std::string("Default (") + fs::path(vs::defaultFont()).stem().string() + ")"
                                                             : fs::path(c->text.font).stem().string();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            if (ImGui::BeginCombo("Font", curFont.c_str())) {
                if (ImGui::Selectable("Default", c->text.font.empty())) { c->text.font.clear(); changed = true; }
                for (const auto& f : fonts)
                    if (ImGui::Selectable(f.name.c_str(), f.path == c->text.font)) { c->text.font = f.path; changed = true; }
                ImGui::EndCombo();
            }
            float sz = c->text.size * 100.0f;
            if (numField("Size", sz, 0.5f, 0.5f, 100.0f, "%.1f %% tall")) { c->text.size = sz / 100.0f; changed = true; }
            if (colorField("Colour", c->text.color)) changed = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Bold", &c->text.bold)) changed = true;
            ImGui::SameLine();
            if (ImGui::Checkbox("Shadow", &c->text.shadow)) changed = true;
            int al = c->text.align;
            bool alChanged = ImGui::RadioButton("Left", &al, 0);
            ImGui::SameLine();
            alChanged |= ImGui::RadioButton("Centred", &al, 1);
            ImGui::SameLine();
            alChanged |= ImGui::RadioButton("Right", &al, 2);
            if (alChanged) {
                c->text.align = al;
                changed = true;
            }
            float ol = c->text.outline * 100.0f;
            if (numField("Outline", ol, 1.0f, 0.0f, 30.0f, "%.0f %%")) { c->text.outline = ol / 100.0f; changed = true; }
            ImGui::SameLine();
            if (colorField("##olc", c->text.outlineColor)) changed = true;
            float bx = c->text.box * 100.0f;
            if (numField("Plate", bx, 10.0f, 0.0f, 100.0f, "%.0f %%")) { c->text.box = bx / 100.0f; changed = true; }
            ImGui::SameLine();
            if (colorField("##bxc", c->text.boxColor)) changed = true;
        }
        if (c->kind == vs::ClipKind::Color) {
            ImGui::Separator();
            if (colorField("Colour", c->color)) changed = true;
        }
        if (vs::isSongFile(c->file)) {
            ImGui::Separator();
            if (st.bounce && st.bounceFile == c->file) ImGui::ProgressBar(st.bounce->progress(), ImVec2(-1, 0), "Bouncing...");
            if (ImGui::Button("Open in the Song Sequencer") && h.openSong) h.openSong(fs::path(c->file).stem().string());
            ui::hint("The song is played as the Song Sequencer last saved it. Save it there and the film hears the change.");
        }
        ImGui::Separator();
        const bool under = st.playhead > c->start && st.playhead < c->end();
        if (ImGui::Button("Cut at the playhead") && under) splitAt(st, st.playhead, {c->uid});
        ImGui::SameLine();
        if (ImGui::Button("Delete")) deleteClips(st, {c->uid}, false);
    }
    // One step of undo for one field's edit, however many frames it took.
    if (changed) {
        const ImGuiID active = ImGui::GetActiveID();
        if (active == 0 || active != lastEdit) st.checkpoint(before);
        lastEdit = active;
        st.film.sortClips();
        st.edited();
    } else if (ImGui::GetActiveID() == 0) {
        lastEdit = 0;
    }
    ImGui::EndChild();
}

// --- The export dialog -----------------------------------------------------------------------------

void exportDialog(Panel::State& st) {
    if (st.exportDialog) {
        ImGui::OpenPopup("Export the film");
        st.exportDialog = false;
        vs::ExportSettings& s = st.exportSet;
        s.width  = st.film.width;
        s.height = st.film.height;
        s.fps    = st.film.fps;
        st.exportPreset = 0;
        std::snprintf(st.exportPath, sizeof st.exportPath, "%s", defaultExportPath(st, s.format).c_str());
        st.exportRange = (st.film.markIn >= 0 || st.film.markOut >= 0) ? 1 : 0;
    }
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 34.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Export the film", nullptr, ImGuiWindowFlags_NoSavedSettings)) return;
    vs::ExportSettings& s = st.exportSet;
    const float em = ImGui::GetFontSize();
    const bool ff = vs::ffmpegReady();
    ImGui::SetNextItemWidth(em * 18.0f);
    if (ImGui::BeginCombo("Kind", vs::formatName(s.format))) {
        for (int i = 0; i < static_cast<int>(vs::Format::Count); ++i) {
            const auto f = static_cast<vs::Format>(i);
            const bool usable = ff || !vs::formatNeedsFfmpeg(f);
            if (ImGui::Selectable(vs::formatName(f), f == s.format, usable ? 0 : ImGuiSelectableFlags_Disabled)) {
                // The file's name follows the kind, if it was the suggested one.
                const std::string old = defaultExportPath(st, s.format);
                const bool wasDefault = old == st.exportPath ||
                                        fs::path(st.exportPath).replace_extension(vs::formatExtension(f)).generic_string() ==
                                            fs::path(old).replace_extension(vs::formatExtension(f)).generic_string();
                s.format = f;
                if (wasDefault) std::snprintf(st.exportPath, sizeof st.exportPath, "%s", defaultExportPath(st, f).c_str());
                else std::snprintf(st.exportPath, sizeof st.exportPath, "%s",
                                   fs::path(st.exportPath).replace_extension(vs::formatExtension(f)).generic_string().c_str());
                if (f == vs::Format::Fvid) {
                    // The game's own video: small and slow by design (VideoTexture.hpp).
                    st.exportPreset = 4;
                    s.fps = std::min(st.film.fps, 15.0);
                }
            }
        }
        ImGui::EndCombo();
    }
    switch (s.format) {
    case vs::Format::Mp4:    ui::hint("Plays everywhere: the one to share or upload."); break;
    case vs::Format::Hevc:   ui::hint("Half the size of H.264 at the same look; not every player has it."); break;
    case vs::Format::WebM:   ui::hint("For the web. Slow to write."); break;
    case vs::Format::ProRes: ui::hint("Big, barely compressed: for cutting on in another program."); break;
    case vs::Format::Fvid:   ui::hint("Pictures only, for a screen or billboard in the game (a material's video). Into videos/, an asset at once."); break;
    case vs::Format::Wav:    ui::hint("Just the mixed sound, 24 bit, into sounds/."); break;
    default: break;
    }
    if (!ff) ImGui::TextColored(ImVec4(1, 0.7f, 0.4f, 1), "No ffmpeg found -- %s -- only .fvid and WAV can be written.", vs::ffmpegInstallHint());
    ImGui::Separator();
    if (s.format != vs::Format::Wav) {
        // Sizes in the film's own shape.
        const double aspect = static_cast<double>(st.film.width) / std::max(1, st.film.height);
        const int heights[] = {st.film.height, 2160, 1440, 1080, 360, 720, 540};
        const char* names[] = {"As the film", "2160", "1440", "1080", "360 (game video)", "720", "540"};
        std::string labels[7];
        for (int i = 0; i < 7; ++i) {
            const int hh = heights[i] & ~1, ww = static_cast<int>(std::lround(hh * aspect)) & ~1;
            char b[64];
            std::snprintf(b, sizeof b, "%s  (%d x %d)", names[i], ww, hh);
            labels[i] = b;
        }
        ImGui::SetNextItemWidth(em * 18.0f);
        if (ImGui::BeginCombo("Size", labels[st.exportPreset].c_str())) {
            for (int i = 0; i < 7; ++i)
                if (ImGui::Selectable(labels[i].c_str(), i == st.exportPreset)) st.exportPreset = i;
            ImGui::EndCombo();
        }
        s.height = heights[st.exportPreset] & ~1;
        s.width  = static_cast<int>(std::lround(s.height * aspect)) & ~1;
        float fps = static_cast<float>(s.fps);
        if (numField("Frames a second", fps, 1.0f, 1.0f, 120.0f, "%.3g", em * 10.0f)) s.fps = fps;
        if (s.format != vs::Format::ProRes) {
            float q = static_cast<float>(s.quality);
            if (numField("Quality", q, 5.0f, 10.0f, 100.0f, "%.0f", em * 10.0f)) s.quality = static_cast<int>(q);
        }
    }
    if (s.format != vs::Format::Fvid && s.format != vs::Format::Wav) ImGui::Checkbox("With the sound", &s.sound);
    const bool hasRange = st.film.markIn >= 0 || st.film.markOut >= 0;
    ImGui::RadioButton("The whole film", &st.exportRange, 0);
    ImGui::SameLine();
    if (!hasRange) ImGui::BeginDisabled();
    ImGui::RadioButton("The range (I to O)", &st.exportRange, 1);
    if (!hasRange) ImGui::EndDisabled();
    const double from = st.exportRange == 1 && hasRange ? std::max(0.0, st.film.markIn) : 0.0;
    const double to = st.exportRange == 1 && st.film.markOut >= 0 ? st.film.markOut : st.film.duration();
    ImGui::TextDisabled("%s of film, %d frames", timecode(to - from, st.film.fps).c_str(),
                        static_cast<int>(std::llround((to - from) * (s.format == vs::Format::Wav ? st.film.fps : s.fps))));
    ImGui::Separator();
    ImGui::SetNextItemWidth(em * 24.0f);
    ImGui::InputText("##path", st.exportPath, sizeof st.exportPath);
    ImGui::SameLine();
    if (ImGui::Button("Choose...")) {
        std::string out;
        const std::string ext = std::string(vs::formatExtension(s.format)).substr(1);
        if (ed::saveFile(out, fs::path(st.exportPath).parent_path().generic_string(), fs::path(st.exportPath).filename().string(),
                         vs::formatName(s.format), "*." + ext, ext))
            std::snprintf(st.exportPath, sizeof st.exportPath, "%s", out.c_str());
    }
    std::error_code ec;
    if (fs::exists(st.exportPath, ec)) ImGui::TextColored(ImVec4(1, 0.8f, 0.4f, 1), "There is a file of that name: it will be written over.");
    ImGui::Spacing();
    const bool can = (ff || !vs::formatNeedsFfmpeg(s.format)) && st.film.duration() > 0.0 && std::strlen(st.exportPath) > 0;
    if (!can) ImGui::BeginDisabled();
    if (ImGui::Button("Export", ImVec2(em * 8.0f, 0))) {
        startExport(st);
        ImGui::CloseCurrentPopup();
    }
    if (!can) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 8.0f, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void saveAsPopup(Panel::State& st) {
    if (st.askName) {
        ImGui::OpenPopup("Save the film as");
        st.askName = false;
    }
    if (!ImGui::BeginPopupModal("Save the film as", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextUnformatted("Into the project's content/films:");
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    const bool enter = ImGui::InputText("##name", st.nameBuf, sizeof st.nameBuf, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::Button("Save", ImVec2(ImGui::GetFontSize() * 7.0f, 0)) || enter) {
        const std::string name = safeName(st.nameBuf);
        st.film.name = name;
        saveTo(st, (fs::path(st.filmsDir()) / (name + ".json")).generic_string());
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 7.0f, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// --- Keys ------------------------------------------------------------------------------------------

void keys(Panel::State& st) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput || ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) return;
    const bool ctrl = io.KeyCtrl;
    if (ctrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) redoStep(st); else undoStep(st); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_Y, false)) { redoStep(st); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) { save(st); return; }
        if (ImGui::IsKeyPressed(ImGuiKey_E, false)) { st.exportDialog = true; return; }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) copyClips(st);
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) { copyClips(st); deleteClips(st, st.sel, false); }
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) pasteClips(st, st.playhead);
        if (ImGui::IsKeyPressed(ImGuiKey_D, false)) duplicateClips(st);
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) {
            st.sel.clear();
            for (const vs::Track& t : st.film.tracks)
                for (const vs::Clip& c : t.clips) st.sel.push_back(c.uid);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_K, false)) splitAt(st, st.playhead, st.sel);
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) { if (st.playing) st.stop(); else st.play(); }
    if (ImGui::IsKeyPressed(ImGuiKey_K, false)) st.stop();
    if (ImGui::IsKeyPressed(ImGuiKey_L, false)) { if (!st.playing) st.play(); }
    if (ImGui::IsKeyPressed(ImGuiKey_J, false)) st.seek(st.film.snap(st.playhead - 1.0));
    if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) st.seek(0.0);
    if (ImGui::IsKeyPressed(ImGuiKey_End, false)) st.seek(st.film.duration());
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) st.seek(st.film.snap(st.playhead - (io.KeyShift ? 1.0 : st.film.frame())));
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) st.seek(st.film.snap(st.playhead + (io.KeyShift ? 1.0 : st.film.frame())));
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) || ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        const auto pts = st.film.editPoints();
        const bool up = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
        double to = up ? 0.0 : st.film.duration();
        for (double p : pts) {
            if (up && p < st.playhead - 1e-6) to = p;
            if (!up && p > st.playhead + 1e-6) { to = p; break; }
        }
        st.seek(to);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_I, false)) {
        st.checkpoint();
        st.film.markIn = st.film.snap(st.playhead);
        if (st.film.markOut >= 0 && st.film.markOut <= st.film.markIn) st.film.markOut = -1;
        st.edited();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_O, false)) {
        st.checkpoint();
        st.film.markOut = st.film.snap(st.playhead);
        if (st.film.markIn >= st.film.markOut) st.film.markIn = -1;
        st.edited();
    }
    if (io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_X, false)) { st.checkpoint(); st.film.markIn = st.film.markOut = -1; st.edited(); }
    if (ImGui::IsKeyPressed(ImGuiKey_S, false)) splitAt(st, st.playhead, st.sel);
    if (ImGui::IsKeyPressed(ImGuiKey_T, false)) addMadeClip(st, vs::ClipKind::Text, -1, st.playhead);
    if (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))
        deleteClips(st, st.sel, io.KeyShift);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) st.sel.clear();
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) { st.pps = 0.0; st.follow = true; }
    if (ImGui::IsKeyPressed(ImGuiKey_N, false)) st.snap = !st.snap;
    if (ImGui::IsKeyPressed(ImGuiKey_Equal) || ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) st.pps *= 1.25;
    if (ImGui::IsKeyPressed(ImGuiKey_Minus) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) st.pps /= 1.25;
}

void takeDrops(Panel::State& st, const Host& h) {
    if (!h.drops || h.drops->empty()) return;
    const ImVec2 m = h.dropX && h.dropY ? ImVec2(*h.dropX, *h.dropY) : ImGui::GetIO().MousePos;
    const ImVec2 wa = ImGui::GetWindowPos(), wb(wa.x + ImGui::GetWindowSize().x, wa.y + ImGui::GetWindowSize().y);
    if (!inside(m, wa, wb)) return;
    std::vector<std::string> mine;
    for (auto it = h.drops->begin(); it != h.drops->end();) {
        if (vs::isVideoFile(*it) || vs::isImageFile(*it) || vs::isAudioFile(*it)) {
            mine.push_back(*it);
            it = h.drops->erase(it);
        } else {
            ++it;
        }
    }
    if (mine.empty()) return;
    // Onto the lanes: placed where they fell, one after another.
    if (inside(m, st.lanesA, st.lanesB) && st.pps > 0.0) {
        double at = st.film.snap(std::max(0.0, st.view0 + (m.x - st.lanesA.x) / st.pps));
        for (const std::string& p : mine) {
            bringIn(st, p);
            const int u = addMediaClip(st, p, st.selTrack, at);
            if (const vs::Clip* c = st.film.clip(u)) at = c->end();
        }
    } else {
        for (const std::string& p : mine) bringIn(st, p);
    }
    st.say("Brought in " + std::to_string(mine.size()) + (mine.size() == 1 ? " file." : " files."));
}

} // namespace

// --- The panel -------------------------------------------------------------------------------------

void install() {
    ImGuiSettingsHandler h;
    h.TypeName   = "FitzelFilms";
    h.TypeHash   = ImHashStr("FitzelFilms");
    h.ReadOpenFn = iniOpen;
    h.ReadLineFn = iniLine;
    h.WriteAllFn = iniWrite;
    ImGui::AddSettingsHandler(&h);
}

Panel::Panel() : m(std::make_unique<State>()) {
    m->lib.setResolver([st = m.get()](const std::string& f) { return st->absolute(f); });
    resetView(*m);
}

Panel::~Panel() {
    // The preview's thread and the export use the library: they go first.
    m->exporter.cancel();
    m->preview.reset();
    if (m->mixJob.valid()) m->mixJob.wait();
}

void Panel::draw(bool& show, const Host& h) {
    State& st = *m;
    st.host = &h;
    if (st.projectFile != h.projectFile) {
        if (!st.projectFile.empty()) projectChanged(h);
        st.projectFile = h.projectFile;
        std::error_code ec;
        const std::string dir = st.filmsDir();
        if (!dir.empty() && g_lastFilm.rfind(dir, 0) == 0 && fs::exists(g_lastFilm, ec) && st.path != g_lastFilm && !st.dirty)
            openFilm(st, g_lastFilm);
    }
    st.shown = show;
    // An export keeps going with the window closed; say when it is done.
    if (st.exportWatch && st.exporter.finished()) {
        st.exportWatch = false;
        st.say(st.exporter.message(), !st.exporter.ok());
        if (st.exporter.ok() && h.assetsChanged && !st.projectDir().empty() &&
            st.exporter.settings().path.rfind(st.projectDir(), 0) == 0)
            h.assetsChanged();
    }
    if (!show) {
        if (st.playing) st.stop();
        st.focused = false;
        return;
    }
    if (!st.preview) st.preview = std::make_unique<vs::Preview>(st.lib);
    stepSongs(st, h);
    stepSound(st, h);

    // The clock: the sound's when it plays, the wall's otherwise.
    if (st.playing) {
        double t = st.playFrom + (now() - st.playWall);
        if (st.player.isValid() && st.player.isPlaying() && st.mixedRev == st.rev) {
            const double ta = st.mixFrom + static_cast<double>(st.player.position()) / vs::kRate;
            if (std::abs(ta - t) > 0.06) {
                st.playFrom = ta;
                st.playWall = now();
                t = ta;
            }
        }
        const double endT = st.loop && st.film.markOut >= 0 ? st.film.markOut : st.film.duration();
        if (t >= endT) {
            if (st.loop && endT > 0.0) {
                st.playhead = st.film.markIn >= 0 ? st.film.markIn : 0.0;
                st.playFrom = st.playhead;
                st.playWall = now();
                if (st.player.isValid() && st.mixedRev == st.rev) st.player.play(static_cast<std::int64_t>((st.playhead - st.mixFrom) * vs::kRate));
            } else {
                st.playhead = endT;
                st.stop();
            }
        } else {
            st.playhead = t;
        }
    }

    ImGui::SetNextWindowSize(ImVec2(1400.0f, 820.0f), ImGuiCond_FirstUseEver);
    const std::string title = "Video Sequencer - " + st.filmName() + (st.dirty ? " *" : "") + "###Video Sequencer";
    if (!ImGui::Begin(title.c_str(), &show, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        st.focused = false;
        return;
    }
    st.focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (st.focused) st.focusFrame = ImGui::GetFrameCount();
    st.sel.erase(std::remove_if(st.sel.begin(), st.sel.end(), [&](int u) { return !st.film.clip(u); }), st.sel.end());
    if (!st.film.track(st.selTrack)) st.selTrack = st.film.tracks.empty() ? -1 : st.film.tracks.front().uid;

    menuBar(st, h);
    transportBar(st);

    const float em = ImGui::GetFontSize();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (st.bottomH <= 0.0f) st.bottomH = std::round(std::max(avail.y * 0.42f, std::min(avail.y * 0.55f, em * 18.0f)));
    st.bottomH = std::clamp(st.bottomH, em * 7.0f, std::max(em * 7.0f, avail.y - em * 10.0f));
    if (st.binW <= 0.0f) st.binW = std::round(std::max(em * 15.0f, avail.x * 0.2f));
    if (st.inspW <= 0.0f) st.inspW = std::round(std::max(em * 21.0f, avail.x * 0.22f));
    st.binW = std::clamp(st.binW, em * 10.0f, std::max(em * 10.0f, avail.x * 0.4f));
    st.inspW = std::clamp(st.inspW, em * 16.0f, std::max(em * 16.0f, avail.x * 0.45f));
    const float topH = std::max(em * 8.0f, avail.y - st.bottomH - 6.0f);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float viewW = std::max(em * 10.0f, avail.x - st.binW - st.inspW - 12.0f);

    bin(st, h, ImVec2(st.binW, topH));
    auto vsplit = [&](const char* id, float& w, float sign) {
        ImGui::SameLine(0, 0);
        ImGui::InvisibleButton(id, ImVec2(6.0f, topH));
        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (ImGui::IsItemActive()) w = std::max(em * 10.0f, w + sign * ImGui::GetIO().MouseDelta.x);
        ImGui::SameLine(0, 0);
    };
    vsplit("##binSplit", st.binW, 1.0f);
    viewer(st, h, ImVec2(viewW, topH));
    vsplit("##inspSplit", st.inspW, -1.0f);
    inspector(st, h, ImVec2(st.inspW, topH));

    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + topH));
    ImGui::InvisibleButton("##split", ImVec2(avail.x, 6.0f));
    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    if (ImGui::IsItemActive()) st.bottomH = std::max(em * 7.0f, st.bottomH - ImGui::GetIO().MouseDelta.y);
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + topH + 6.0f));
    timeline(st, h, ImVec2(avail.x, std::max(1.0f, avail.y - topH - 6.0f - em * 1.5f)));

    // The status line.
    ImGui::SetCursorScreenPos(ImVec2(at.x + 4.0f, at.y + avail.y - em * 1.3f));
    if (st.bounce) {
        ImGui::TextDisabled("Bouncing %s... %.0f%%", fs::path(st.bounceFile).stem().string().c_str(), st.bounce->progress() * 100.0f);
    } else if (!st.status.empty() && ImGui::GetTime() - st.statusAt < 12.0) {
        if (st.statusBad) ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s", st.status.c_str());
        else ImGui::TextDisabled("%s", st.status.c_str());
    } else if (st.lib.busy()) {
        ImGui::TextDisabled("Reading the media...");
    } else {
        ImGui::TextDisabled("%zu clips on %zu tracks%s", [&] { std::size_t n = 0; for (const auto& t : st.film.tracks) n += t.clips.size(); return n; }(),
                            st.film.tracks.size(), st.path.empty() ? "  (not saved yet)" : "");
    }

    takeDrops(st, h);
    exportDialog(st);
    saveAsPopup(st);
    if (st.focused) keys(st);
    ImGui::End();

    // The viewer's picture: asked for when the time, the film or the size moved
    // on; taken when it is done.
    if (st.snapRev != st.rev || !st.snapshot) {
        st.snapshot = std::make_shared<vs::Film>(st.film);
        st.snapRev = st.rev;
    }
    const double want = st.film.snap(st.playhead);
    if (std::abs(want - st.askedT) > 1e-9 || st.askedRev != st.rev || st.viewW != st.askedW || st.viewH != st.askedH) {
        st.preview->request(st.snapshot, st.rev, want, st.askedW, st.askedH, st.playing);
        st.askedT = want;
        st.askedRev = st.rev;
        st.viewW = st.askedW;
        st.viewH = st.askedH;
    }
    vs::Image img;
    double t = 0.0;
    std::uint64_t rev = 0;
    bool complete = false;
    if (st.preview->take(img, t, rev, complete)) {
        if (!st.viewTex.isValid() || st.viewTex.width() != img.w || st.viewTex.height() != img.h) st.viewTex = fitzel::Texture::blank(img.w, img.h);
        st.viewTex.update(img.px.data(), img.w, img.h, 4);
        st.shownT = t;
        st.shownRev = rev;
        st.shownComplete = complete;
        st.problems = st.preview->problems();
    }
    // Thumbnails made since: onto the GPU.
    for (auto& [file, tt] : st.thumbs)
        if (!tt.strip || (!tt.tex.isValid() && tt.strip->ready)) {
            tt.strip = st.lib.thumbs(file);
            if (tt.strip && tt.strip->ready && !tt.strip->strip.empty())
                tt.tex = fitzel::Texture::fromPixels(tt.strip->strip.px.data(), tt.strip->strip.w, tt.strip->strip.h, 4);
        }
}

bool Panel::wantsSmoothFrames() const {
    const State& st = *m;
    if (st.exporter.running() || st.exportWatch) return true;
    if (!st.shown) return false;
    const bool pictureComing = st.preview && st.preview->busy();
    return st.playing || st.drag.mode != Drag::None || pictureComing || st.bounce != nullptr || st.mixJob.valid() ||
           st.lib.busy() || (st.shownRev != st.rev);
}

bool Panel::hasKeyboard() const { return m->focused || ImGui::GetFrameCount() - m->focusFrame <= 1; }

void Panel::beforePlay(const Host& h) {
    State& st = *m;
    st.host = &h;
    st.stop();
    if (st.dirty && !st.path.empty()) saveTo(st, st.path);
}

void Panel::projectChanged(const Host& h) {
    State& st = *m;
    st.host = &h;
    if (st.dirty && !st.path.empty()) saveTo(st, st.path);
    newFilm(st);
    st.projectFile = h.projectFile;
    st.thumbs.clear();
    st.lib.clear();
    st.bounceFailed.clear();
}

bool Panel::open(const std::string& name, const Host& h) {
    State& st = *m;
    st.host = &h;
    if (st.projectFile != h.projectFile) {
        if (!st.projectFile.empty()) projectChanged(h);
        st.projectFile = h.projectFile;
    }
    if (name.empty() || st.filmsDir().empty()) return false;
    fs::path f = fs::path(name).is_absolute() ? fs::path(name) : fs::path(st.filmsDir()) / name;
    if (!f.has_extension()) f += ".json";
    if (f.generic_string() == st.path) return true;
    if (st.dirty && !st.path.empty()) saveTo(st, st.path);
    return openFilm(st, f.generic_string());
}

vseq::Film& Panel::film() { return m->film; }
void Panel::edited() { m->edited(); }
double Panel::playhead() const { return m->playhead; }
void Panel::seek(double t) { m->seek(t); }
bool Panel::playing() const { return m->playing; }
const std::vector<int>& Panel::selection() const { return m->sel; }
void Panel::select(const std::vector<int>& uids) { m->sel = uids; }
bool Panel::canUndo() const { return !m->undo.empty(); }
bool Panel::viewerSettled() const {
    const State& st = *m;
    return st.shownComplete && st.shownRev == st.rev && std::abs(st.shownT - st.film.snap(st.playhead)) < 1e-6 &&
           !(st.preview && st.preview->busy());
}
std::string Panel::status() const { return m->status; }
void Panel::importFiles(const std::vector<std::string>& paths, bool place) {
    State& st = *m;
    double at = st.playhead;
    for (const std::string& p : paths) {
        bringIn(st, p);
        if (place) {
            const int u = addMediaClip(st, p, st.selTrack, at);
            if (const vs::Clip* c = st.film.clip(u)) at = c->end();
        }
    }
}
bool Panel::exporting() const { return m->exporter.running() || m->exportPending || m->exportWatch; }
std::string Panel::exportMessage() const { return m->exporter.message(); }
void Panel::exportTo(const std::string& path, int format) {
    State& st = *m;
    st.exportSet.format = static_cast<vs::Format>(format);
    st.exportSet.width  = st.film.width;
    st.exportSet.height = st.film.height;
    st.exportSet.fps    = st.film.fps;
    st.exportRange = 0;
    std::snprintf(st.exportPath, sizeof st.exportPath, "%s", path.c_str());
    startExport(st);
}
void Panel::fitTimeline() { m->pps = 0.0; }

} // namespace vsequi
