// songpanelcheck: the Song Sequencer window (SongPanel.cpp, SongPianoRoll.cpp),
// driven by real ImGui mouse and key events in a frame nobody draws -- the way
// a person uses it. The panel says where it drew its parts (its probe) and the
// harness presses there:
//   - a double-click on an empty instrument lane makes a one-bar clip on the
//     bar, and opens it in the piano roll;
//   - in the piano roll a double-click adds a note on the grid, a note dragged
//     a beat and two rows lands a beat later and two semitones up, its edge
//     dragged makes it longer, the pencil draws one as long as the drag, the
//     velocity lane paints the selected note's velocity, Del deletes it, and
//     Ctrl+Z / Ctrl+Y take that back and forth;
//   - in the arrangement a clip dragged a bar moves a bar, dragged a lane down
//     moves to that track, Ctrl-dragged leaves a copy, its edge dragged makes it
//     longer; S splits it at the playhead, a rubber band selects, Del deletes;
//   - the ruler: a drag in its top part sets the loop, a click below it moves
//     the playhead;
//   - the MIDI input reads notes out of a byte stream (running status, SysEx,
//     clock) through a real file descriptor -- a FIFO standing in for
//     /dev/snd/midiC*D*.
//   build/linux/bin/songpanelcheck
//   build/linux/bin/songpanelcheck --png out.png [--size WxH]
//   build/linux/bin/songpanelcheck --bench           (what a frame of the window costs)
//   build/linux/bin/songpanelcheck --demo <project>  (a demo song into a project)
// The second form draws the window through the editor's own Gui and theme --
// drums, bass, keys, a MIDI part and an audio track, a clip open in the piano
// roll -- and writes a picture of it.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <fitzel/audio/Song.hpp>
#include <fitzel/core/Window.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/ui/Gui.hpp>
#include <imgui.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/SongMidiIn.hpp"
#include "../src/SongPanel.hpp"
#include "../src/UiStyle.hpp"

namespace fs = std::filesystem;
namespace sg = fitzel::song;

namespace {

int g_fails = 0;
void check(bool ok, const std::string& what) {
    std::printf(ok ? "  ok   %s\n" : "[FAIL] %s\n", what.c_str());
    if (!ok) ++g_fails;
}

using Rects = std::map<std::string, std::pair<ImVec2, ImVec2>>;

void writeWav(const fs::path& p, float seconds) {
    const int rate = 22050;
    const int n = static_cast<int>(seconds * rate);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / rate;
        const float env = std::exp(-5.0f * std::fmod(t, 0.48f)) * 0.8f;
        pcm[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(env * std::sin(6.2831f * 110.0f * t * (1.0f + std::fmod(t, 0.48f))) * 30000.0f);
    }
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + n * 2); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    f.write("data", 4); u32(static_cast<std::uint32_t>(n * 2));
    f.write(reinterpret_cast<const char*>(pcm.data()), n * 2);
}

sg::Note note(std::int64_t tick, std::int64_t len, int pitch, int vel) {
    sg::Note n;
    n.tick = tick;
    n.length = len;
    n.pitch = pitch;
    n.velocity = vel;
    return n;
}

// The song the picture shows.
void demoSong(sg::Song& s) {
    using sg::kPpq;
    s = sg::Song{};
    s.name = "Night Drive";
    s.bpm = 118.0;
    s.loop = true;
    s.loopStart = 0;
    s.loopEnd = s.ticksPerBar() * 4;
    const std::int64_t bar = s.ticksPerBar();
    sg::Track& d = s.addTrack(sg::TrackKind::Instrument, "Drums");
    d.instrument.kind = sg::InstrumentKind::Drums;
    sg::Clip beat;
    beat.uid = s.newUid();
    beat.name = "Beat";
    beat.length = bar * 8;
    beat.loop = bar;
    for (int i = 0; i < 4; ++i) beat.notes.push_back(note(i * kPpq, 120, 36, 118));
    for (int i = 0; i < 2; ++i) beat.notes.push_back(note(kPpq + i * kPpq * 2, 120, 38, 104));
    for (int i = 0; i < 8; ++i) beat.notes.push_back(note(i * kPpq / 2, 60, 42, i % 2 ? 64 : 96));
    beat.sortNotes();
    d.clips.push_back(beat);
    sg::Track& b = s.addTrack(sg::TrackKind::Instrument, "Bass");
    b.instrument.patch = "builtin:Bass";
    sg::Clip bl;
    bl.uid = s.newUid();
    bl.name = "Bassline";
    bl.start = bar * 2;
    bl.length = bar * 6;
    const int roots[6] = {33, 33, 36, 31, 33, 29};
    for (int k = 0; k < 6; ++k)
        for (int i = 0; i < 8; ++i)
            bl.notes.push_back(note(k * bar + i * kPpq / 2, kPpq / 2 - 40, roots[k] + (i % 4 == 3 ? 12 : 0), 100 - (i % 2) * 18));
    b.clips.push_back(bl);
    sg::Track& k = s.addTrack(sg::TrackKind::Instrument, "Keys");
    k.instrument.patch = "builtin:Keys";
    sg::Clip ch;
    ch.uid = s.newUid();
    ch.name = "Chords";
    ch.start = bar * 2;
    ch.length = bar * 4;
    const int chords[4][4] = {{57, 60, 64, 67}, {53, 57, 60, 64}, {55, 59, 62, 67}, {52, 55, 59, 64}};
    for (int c = 0; c < 4; ++c)
        for (int n : chords[c]) {
            ch.notes.push_back(note(c * bar, kPpq * 3 - 60, n, 90));
            ch.notes.push_back(note(c * bar + kPpq * 3, kPpq - 60, n + 12 * (n == chords[c][3]), 70));
        }
    ch.sortNotes();
    k.clips.push_back(ch);
    const int keysUid = k.uid;
    sg::Track& m = s.addTrack(sg::TrackKind::Midi, "Keys melody");
    m.target = keysUid;
    m.channel = 1;
    sg::Clip mel;
    mel.uid = s.newUid();
    mel.name = "Hook";
    mel.start = bar * 4;
    mel.length = bar * 2;
    const int line[8] = {76, 74, 72, 74, 76, 79, 76, 72};
    for (int i = 0; i < 8; ++i) mel.notes.push_back(note(i * kPpq, kPpq - 120, line[i], 96));
    m.clips.push_back(mel);
    sg::Track& a = s.addTrack(sg::TrackKind::Audio, "Riser");
    sg::Clip ac;
    ac.uid = s.newUid();
    ac.file = "riser.wav";
    ac.name = "riser";
    ac.start = bar * 3;
    ac.length = bar * 3;
    ac.fadeIn = 0.6f;
    ac.fadeOut = 1.0f;
    a.clips.push_back(ac);
}

int picture(const std::string& out, int w, int h) {
    fitzel::Window window(fitzel::WindowConfig{.width = w, .height = h, .title = "songpanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    fitzel::rhi::Device& gpu = window.device();
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;

    const fs::path dir = fs::temp_directory_path() / "fitzel-songpanelcheck";
    std::error_code ec;
    fs::create_directories(dir, ec);
    writeWav(dir / "riser.wav", 8.0f);

    songui::Panel panel;
    demoSong(panel.song());
    panel.edited();
    songui::Host host;
    host.resolveSound = [dir](const std::string& n) { return (dir / n).generic_string(); };
    bool show = true;
    int chords = -1;
    for (const auto& t : panel.song().tracks)
        for (const auto& c : t.clips)
            if (c.name == "Chords") chords = c.uid;

    for (int frame = 0; frame < 90; ++frame) {
        window.pollEvents();
        gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
        gpu.clear(fitzel::rhi::ClearColor);
        gui.beginFrame();
        if (frame > 0) {
            ImGui::SetWindowPos("###Song Sequencer", ImVec2(8.0f, 8.0f));
            ImGui::SetWindowSize("###Song Sequencer", ImVec2(static_cast<float>(w) - 16.0f, static_cast<float>(h) - 16.0f));
        }
        if (frame == 3) {
            panel.editClip(chords);
            for (auto& t : panel.song().tracks)
                for (auto& c : t.clips)
                    if (c.uid == chords)
                        for (std::size_t i = 4; i < 8; ++i) c.notes[i].selected = true;
        }
        panel.draw(show, host);
        gui.endFrame();
        if (frame < 60) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (frame + 1 < 90) window.swapBuffers();
    }
    int fw = 0, fh = 0;
    window.framebufferSize(fw, fh);
    std::vector<unsigned char> px(static_cast<std::size_t>(fw) * fh * 4);
    gpu.readPixels({}, 0, fitzel::rhi::Rect{0, 0, fw, fh}, fitzel::rhi::Format::RGBA8, px.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(out.c_str(), fw, fh, 4, px.data(), fw * 4) != 0;
    std::printf(wrote ? "[songpanelcheck] wrote %s (%dx%d)\n" : "[songpanelcheck] could not write %s\n", out.c_str(), fw, fh);
    return wrote ? 0 : 1;
}

} // namespace

// --bench: what a frame of the window costs, headless -- the picture's song,
// open in the piano roll, playhead moving.
int bench() {
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    io.DeltaTime   = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    const fs::path dir = fs::temp_directory_path() / "fitzel-songpanelcheck";
    std::error_code ec;
    fs::create_directories(dir, ec);
    writeWav(dir / "riser.wav", 8.0f);
    songui::Panel panel;
    demoSong(panel.song());
    panel.edited();
    songui::Host host;
    host.resolveSound = [dir](const std::string& n) { return (dir / n).generic_string(); };
    bool show = true;
    int chords = -1;
    for (const auto& t : panel.song().tracks)
        for (const auto& c : t.clips)
            if (c.name == "Chords") chords = c.uid;
    double total = 0.0, worst = 0.0;
    const int frames = 300;
    for (int f = 0; f < frames + 20; ++f) {
        io.AddMousePosEvent(900.0f, 300.0f);
        const auto t0 = std::chrono::steady_clock::now();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1900, 1060));
        if (f == 2) panel.editClip(chords);
        panel.draw(show, host);
        ImGui::Render();
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        if (f >= 20) { total += us; worst = std::max(worst, us); }
        if (f < 20) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("panel frame: mean %.0f us, worst %.0f us, %d draw vertices\n", total / frames, worst,
                ImGui::GetDrawData()->TotalVtxCount);
    ImGui::DestroyContext();
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--bench") return bench();
    // --demo <project dir>: the picture's song into a project, to open in the editor.
    if (argc > 2 && std::string(argv[1]) == "--demo") {
        const fs::path proj = argv[2];
        std::error_code ec;
        fs::create_directories(proj / "content" / "songs", ec);
        fs::create_directories(proj / "sounds", ec);
        writeWav(proj / "sounds" / "riser.wav", 8.0f);
        sg::Song s;
        demoSong(s);
        const std::string out = (proj / "content" / "songs" / "Night Drive.json").generic_string();
        std::printf(s.save(out) ? "wrote %s\n" : "could not write %s\n", out.c_str());
        return 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--png") {
        int w = 1600, h = 900;
        for (int i = 3; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--size") std::sscanf(argv[i + 1], "%dx%d", &w, &h);
        return picture(argv[2], w, h);
    }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.DeltaTime   = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigInputTrickleEventQueue = false;

    songui::Panel panel;
    songui::Host host;
    Rects where;
    host.probe = [&](const std::string& k, ImVec2 a, ImVec2 b) { where[k] = {a, b}; };
    bool show = true;
    sg::Song& s = panel.song();
    const std::int64_t bar = s.ticksPerBar(), beat = s.ticksPerBeat();

    ImVec2 mouse(1590.0f, 890.0f);
    auto frame = [&](bool lmb, bool rmb = false) {
        where.clear();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, rmb);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        panel.draw(show, host);
        ImGui::Render();
    };
    auto rect = [&](const std::string& k, ImVec2& a, ImVec2& b) {
        auto it = where.find(k);
        if (it == where.end()) { check(false, "the panel drew " + k); return false; }
        a = it->second.first;
        b = it->second.second;
        return true;
    };
    auto mods = [&](bool ctrl, bool shift) {
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
    };
    auto click = [&](ImVec2 p) {
        mouse = p;
        frame(false); frame(true); frame(false); frame(false);
    };
    auto dblclick = [&](ImVec2 p) {
        mouse = p;
        frame(false); frame(true); frame(false); frame(true); frame(false); frame(false);
    };
    auto drag = [&](ImVec2 from, ImVec2 to, bool ctrl = false) {
        mods(ctrl, false);
        mouse = from; frame(false); frame(true);
        for (int i = 1; i <= 8; ++i) {
            mouse = ImVec2(from.x + (to.x - from.x) * i / 8.0f, from.y + (to.y - from.y) * i / 8.0f);
            frame(true);
        }
        frame(false); frame(false);
        mods(false, false);
        frame(false);
    };
    auto key = [&](ImGuiKey k, bool ctrl = false) {
        mods(ctrl, false);
        io.AddKeyEvent(k, true); frame(false);
        io.AddKeyEvent(k, false); frame(false);
        mods(false, false);
        frame(false);
    };
    auto mid = [](ImVec2 a, ImVec2 b) { return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f); };
    auto track = [&](const char* name) -> sg::Track* {
        for (sg::Track& t : s.tracks) if (t.name == name) return &t;
        return nullptr;
    };
    auto clipCount = [&] { std::size_t n = 0; for (const sg::Track& t : s.tracks) n += t.clips.size(); return n; };

    for (int i = 0; i < 4; ++i) frame(false);
    check(s.tracks.size() == 3 && track("Drums") && track("Keys") && track("Bass"), "a new song has drums, keys and bass");
    ImVec2 a, b, ra, rb;
    if (!rect("ruler", ra, rb)) return 1;
    // An empty song is fitted to eight bars and a bit.
    const double ppt = (rb.x - ra.x) / (static_cast<double>(bar) * 8.0 * 1.08);
    auto xOf = [&](double tick) { return ra.x + static_cast<float>(tick * ppt); };
    const int keysUid = track("Keys")->uid, bassUid = track("Bass")->uid;

    std::printf("a clip\n");
    rect("lane:" + std::to_string(keysUid), a, b);
    dblclick(ImVec2(xOf(bar * 2.5), mid(a, b).y));
    sg::Clip* clip = track("Keys")->clips.empty() ? nullptr : &track("Keys")->clips.front();
    check(clip && clip->start == bar * 2 && clip->length == bar, "a double-click on an empty lane makes a bar-long clip on the bar");
    if (!clip) return 1;
    const int clipUid = clip->uid;
    check(panel.editedClip() == clipUid && panel.selection().size() == 1, "...selected, and open in the piano roll");

    std::printf("notes\n");
    frame(false);
    ImVec2 ga, gb;
    rect("pr", ga, gb);
    // The roll is fitted to the clip: content 0..bar over 106% of its width.
    const double span = static_cast<double>(bar) * 1.06;
    const double prppt = (gb.x - ga.x) / span;
    const double prv0 = -span * 0.01;
    auto px = [&](double ct) { return ga.x + static_cast<float>((ct - prv0) * prppt); };
    dblclick(ImVec2(px(beat + 100), ga.y + (gb.y - ga.y) * 0.45f));
    clip = s.clip(clipUid);
    check(clip->notes.size() == 1 && clip->notes[0].tick == beat && clip->notes[0].length == sg::kPpq / 4,
          "a double-click in the roll adds a sixteenth on the sixteenth (" + std::to_string(clip->notes.empty() ? -1 : clip->notes[0].tick) + ")");
    if (clip->notes.empty()) return 1;
    const int p0 = clip->notes[0].pitch;
    rect("pr:note:0", a, b);
    const float rowH = b.y - a.y + 1.0f;
    drag(mid(a, b), ImVec2(mid(a, b).x + static_cast<float>(beat * prppt), mid(a, b).y - 2.0f * rowH));
    clip = s.clip(clipUid);
    check(clip->notes[0].tick == beat * 2 && clip->notes[0].pitch == p0 + 2,
          "a note dragged a beat and two rows lands a beat later, two semitones up (" + std::to_string(clip->notes[0].tick) + ", " +
              std::to_string(clip->notes[0].pitch - p0) + ")");
    rect("pr:note:0", a, b);
    drag(ImVec2(b.x - 1.5f, mid(a, b).y), ImVec2(b.x - 1.5f + static_cast<float>(sg::kPpq / 4 * prppt), mid(a, b).y));
    clip = s.clip(clipUid);
    check(clip->notes[0].length == sg::kPpq / 2, "its right edge dragged a sixteenth makes it an eighth (" + std::to_string(clip->notes[0].length) + ")");

    key(ImGuiKey_2);   // the pencil
    drag(ImVec2(px(10), mid(a, b).y + 3.0f * rowH), ImVec2(px(sg::kPpq * 3 / 4 + 10), mid(a, b).y + 3.0f * rowH));
    clip = s.clip(clipUid);
    bool drawn = false;
    for (const sg::Note& n : clip->notes) drawn = drawn || (n.tick == 0 && n.length == sg::kPpq * 3 / 4 && n.pitch == p0 - 1);
    check(clip->notes.size() == 2 && drawn, "the pencil draws a note as long as the drag (a dotted eighth)");
    key(ImGuiKey_1);

    // Select the first note again, paint its velocity high.
    int idx = -1;
    for (std::size_t i = 0; i < clip->notes.size(); ++i) if (clip->notes[i].tick == beat * 2) idx = static_cast<int>(i);
    rect("pr:note:" + std::to_string(idx), a, b);
    click(mid(a, b));
    ImVec2 va, vb;
    rect("pr:vel", va, vb);
    drag(ImVec2(a.x + 1.0f, vb.y - 4.0f), ImVec2(a.x + 1.0f, va.y + (vb.y - va.y) * 0.1f));
    clip = s.clip(clipUid);
    int otherVel = 0;
    for (const sg::Note& n : clip->notes) if (n.tick == 0) otherVel = n.velocity;
    check(clip->notes[static_cast<std::size_t>(idx)].velocity > 110 && otherVel == 100,
          "the velocity lane paints the selected note's velocity (" + std::to_string(clip->notes[static_cast<std::size_t>(idx)].velocity) + ")");

    key(ImGuiKey_Delete);
    check(s.clip(clipUid)->notes.size() == 1, "Del deletes the selected note");
    key(ImGuiKey_Z, true);
    check(s.clip(clipUid)->notes.size() == 2, "Ctrl+Z brings it back");
    key(ImGuiKey_Y, true);
    check(s.clip(clipUid)->notes.size() == 1, "Ctrl+Y deletes it again");
    key(ImGuiKey_Z, true);

    std::printf("clips\n");
    frame(false);
    rect("clip:" + std::to_string(clipUid), a, b);
    drag(mid(a, b), ImVec2(mid(a, b).x + static_cast<float>(bar * ppt * 1.1), mid(a, b).y));
    check(s.clip(clipUid)->start == bar * 3, "a clip dragged a bar (and a bit) moves a bar (" + std::to_string(s.clip(clipUid)->start / bar) + ")");
    rect("clip:" + std::to_string(clipUid), a, b);
    ImVec2 la, lb;
    rect("lane:" + std::to_string(bassUid), la, lb);
    drag(mid(a, b), ImVec2(mid(a, b).x, mid(la, lb).y));
    const sg::Track* owner = nullptr;
    s.clip(clipUid, &owner);
    check(owner && owner->uid == bassUid && s.clip(clipUid)->start == bar * 3, "...dragged a lane down, it moves to that track");

    rect("clip:" + std::to_string(clipUid), a, b);
    drag(mid(a, b), ImVec2(mid(a, b).x + static_cast<float>(bar * ppt * 2), mid(a, b).y), true);
    check(clipCount() == 2 && s.clip(clipUid)->start == bar * 3, "a Ctrl-drag leaves the clip and makes a copy");
    int copyUid = -1;
    for (const sg::Clip& c : track("Bass")->clips) if (c.uid != clipUid) copyUid = c.uid;
    check(copyUid >= 0 && s.clip(copyUid)->start == bar * 5 && s.clip(copyUid)->notes.size() == 2, "...two bars on, with the notes");

    rect("clip:" + std::to_string(copyUid), a, b);
    drag(ImVec2(b.x - 2.0f, mid(a, b).y), ImVec2(b.x - 2.0f + static_cast<float>(bar * ppt), mid(a, b).y));
    check(s.clip(copyUid)->length == bar * 2, "its right edge dragged a bar makes it two bars long");

    std::printf("ruler\n");
    frame(false);
    rect("ruler", ra, rb);
    drag(ImVec2(xOf(bar * 1.0), ra.y + 4.0f), ImVec2(xOf(bar * 3.0), ra.y + 4.0f));
    check(s.loop && s.loopStart == bar && s.loopEnd == bar * 3, "a drag in the ruler's top sets the loop, on the bars");
    // On now: its middle moves it, its edge resizes it.
    drag(ImVec2(xOf(bar * 2.0), ra.y + 4.0f), ImVec2(xOf(bar * 3.0), ra.y + 4.0f));
    check(s.loopStart == bar * 2 && s.loopEnd == bar * 4, "...dragged by its middle it moves a bar");
    drag(ImVec2(xOf(bar * 4.0) - 1.0f, ra.y + 4.0f), ImVec2(xOf(bar * 6.0), ra.y + 4.0f));
    check(s.loopStart == bar * 2 && s.loopEnd == bar * 6, "...by its end it gets longer");
    click(ImVec2(xOf(bar * 6.2), rb.y - 4.0f));
    check(std::fabs(panel.playhead() - static_cast<double>(bar * 6)) < 1.0, "a click below it puts the playhead on the bar");

    std::printf("keys\n");
    rect("clip:" + std::to_string(copyUid), a, b);
    click(mid(a, b));
    const std::size_t n0 = clipCount();
    key(ImGuiKey_S);
    check(clipCount() == n0 + 1 && s.clip(copyUid)->length == bar, "S splits the selected clip at the playhead");
    // A rubber band round both of the Bass track's first two clips.
    frame(false);
    rect("lane:" + std::to_string(bassUid), la, lb);
    drag(ImVec2(xOf(bar * 2.6), la.y + 3.0f), ImVec2(xOf(bar * 6.4), lb.y - 3.0f));
    check(panel.selection().size() == 3, "a rubber band selects what it touches (" + std::to_string(panel.selection().size()) + ")");
    key(ImGuiKey_Delete);
    check(clipCount() == 0, "Del deletes the selection");
    key(ImGuiKey_Z, true);
    check(clipCount() == 3 && panel.selection().size() == 3, "...and Ctrl+Z brings it back, selected");
    key(ImGuiKey_D, true);
    check(clipCount() == 6, "Ctrl+D duplicates the selection");

    std::printf("tracks\n");
    rect("add:audio", a, b);
    click(mid(a, b));
    check(s.tracks.size() == 4 && s.tracks.back().kind == sg::TrackKind::Audio, "+ Audio adds an audio track");
    rect("add:midi", a, b);
    click(mid(a, b));
    check(s.tracks.size() == 5 && s.tracks.back().kind == sg::TrackKind::Midi && s.tracks.back().target == track("Drums")->uid,
          "+ MIDI adds a MIDI track, playing the first instrument");

#ifdef __linux__
    std::printf("MIDI input\n");
    {
        const fs::path fifo = fs::temp_directory_path() / "fitzel-songpanelcheck.midi";
        std::error_code ec;
        fs::remove(fifo, ec);
        check(::mkfifo(fifo.c_str(), 0600) == 0, "a FIFO stands in for the device");
        songui::MidiIn in;
        // The panel sounds a note right here, on the reader's thread.
        std::atomic<int> heard{0};
        const auto self = std::this_thread::get_id();
        std::atomic<bool> offThread{true};
        in.onEvent = [&](songui::MidiIn::Ev& e) {
            ++heard;
            if (std::this_thread::get_id() == self) offThread = false;
            e.track = 42;
        };
        check(in.open(fifo.string()), "the input opens it");
        const int w = ::open(fifo.c_str(), O_WRONLY);
        // C on, E on by running status, a clock tick in between, SysEx, C off,
        // E off as a note-on of velocity 0.
        const unsigned char bytes[] = {0x90, 60, 100, 0xF8, 64, 90, 0xF0, 0x7E, 0x01, 0xF7,
                                       0x80, 60, 0, 0x90, 64, 0, 0xB0, 64, 127};
        const bool wrote = w >= 0 && ::write(w, bytes, sizeof bytes) == static_cast<ssize_t>(sizeof bytes);
        std::vector<songui::MidiIn::Ev> got;
        for (int i = 0; i < 100 && got.size() < 4; ++i) {
            for (const auto& e : in.take()) got.push_back(e);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (w >= 0) ::close(w);
        in.close();
        fs::remove(fifo, ec);
        check(wrote && got.size() == 4 && got[0].on && got[0].pitch == 60 && got[0].vel == 100 && got[1].on &&
                  got[1].pitch == 64 && !got[2].on && got[2].pitch == 60 && !got[3].on && got[3].pitch == 64,
              "notes come out in order: running status kept, clock and SysEx skipped, velocity 0 is an off (" +
                  std::to_string(got.size()) + " events)");
        check(heard == 4 && offThread && !got.empty() && got[0].track == 42,
              "each note reaches the panel on the reader's thread as it arrives, not on the next frame");
    }
#endif

    ImGui::DestroyContext();
    std::printf(g_fails ? "\n%d check(s) FAILED\n" : "\nall song panel checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
