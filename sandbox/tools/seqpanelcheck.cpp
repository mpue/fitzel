// seqpanelcheck: the Sequencer window (SequencerPanel.cpp), driven by real
// ImGui mouse and key events in a frame nobody draws -- the way a person uses
// it. The panel says where it drew each strip and lane (its probe), and the
// harness presses there:
//   - a strip dragged along its lane lands on the frame grid, and stops against
//     its neighbour instead of sliding under it;
//   - a strip dragged onto another camera track moves to that track;
//   - a strip's right edge dragged in trims it;
//   - a click on an empty lane moves the playhead there, poses the scene and
//     cuts to the shot under it -- and ending the preview gives both back;
//   - Space plays, S splits the selected strip at the playhead, Del deletes it.
//   build/release/bin/seqpanelcheck
//   build/release/bin/seqpanelcheck --png out.png [--size WxH] [--select music|shot|none]
// The second form draws the window through the editor's own Gui and theme --
// two camera shots, an animation, music and an effect on two audio tracks,
// previewing on the second shot -- and writes a picture of it.

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

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <fitzel/core/Window.hpp>
#include <fitzel/ui/Gui.hpp>
#include <imgui.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/AnimSystem.hpp"
#include "../src/Component.hpp"
#include "../src/SceneTypes.hpp"
#include "../src/Sequencer.hpp"
#include "../src/SequencerPanel.hpp"
#include "../src/UiStyle.hpp"

namespace fs = std::filesystem;

namespace {

int g_fails = 0;
void check(bool ok, const std::string& what) {
    std::printf(ok ? "  ok   %s\n" : "[FAIL] %s\n", what.c_str());
    if (!ok) ++g_fails;
}

// A mono 16-bit WAV: a few notes with a decaying envelope, so the waveform in
// the picture has something to show.
void writeWav(const fs::path& p, float seconds, float freq, bool hits) {
    const int rate = 22050;
    const int n = static_cast<int>(seconds * rate);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / rate;
        float env;
        if (hits) env = std::exp(-6.0f * std::fmod(t, 0.5f)) * (0.4f + 0.6f * std::fabs(std::sin(t * 0.7f)));
        else      env = 0.35f + 0.3f * std::sin(t * 1.3f) + 0.25f * std::sin(t * 4.1f) * std::sin(t * 0.37f);
        const float v = env * (std::sin(2.0f * 3.14159f * freq * t) + 0.3f * std::sin(2.0f * 3.14159f * freq * 2.01f * t));
        pcm[static_cast<std::size_t>(i)] = static_cast<std::int16_t>(std::clamp(v, -1.0f, 1.0f) * 30000.0f);
    }
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + n * 2); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    f.write("data", 4); u32(n * 2);
    f.write(reinterpret_cast<const char*>(pcm.data()), n * 2);
}

struct Scene {
    std::vector<Entity>        entities;
    std::vector<anim::Clip>    clips;
    std::vector<seq::Sequence> seqs;
    int  editSeq   = 0;
    int  activeCam = -1;
    bool show      = true;
    seq::Runner runner;
    int  camA = 0, camB = 0, door = 0, music = 0, boom = 0;   // strip uids
    int  camTrack = 0, camTrack2 = 0;
};

anim::Clip slide(const char* name, int entity, float len, float to) {
    anim::Clip c;
    c.name = name;
    anim::Track t;
    t.entityId = entity;
    t.key = "center";
    t.keys = {{0.0f, 0.0f}, {len * 0.4f, to * 0.5f}, {len, to}};
    c.tracks.push_back(t);
    return c;
}

void build(Scene& sc) {
    for (int i = 0; i < 2; ++i) {
        Entity cam;
        cam.type = EntityType::Empty;
        cam.id   = 10 + i;
        cam.name = i == 0 ? "Crane cam" : "Dolly cam";
        cam.components.items.push_back(std::make_unique<CameraComponent>());
        sc.entities.push_back(std::move(cam));
    }
    Entity box;
    box.type = EntityType::Box;
    box.id   = 20;
    box.name = "Gate";
    sc.entities.push_back(std::move(box));
    sc.clips = {slide("Crane rise", 10, 4.0f, 6.0f), slide("Dolly push", 11, 5.0f, 8.0f),
                slide("Gate opens", 20, 3.0f, 4.0f)};

    seq::Sequence s;
    s.name = "Opening";
    s.duration = 12.0f;
    s.fps = 30.0f;
    seq::Track& cams = s.addTrack(seq::Kind::Camera, "Cut");
    sc.camTrack = cams.uid;
    seq::Strip a; a.uid = s.newUid(); a.start = 0.0f; a.length = 4.0f; a.camera = 10; a.clip = "Crane rise";
    seq::Strip b; b.uid = s.newUid(); b.start = 4.0f; b.length = 5.0f; b.camera = 11; b.clip = "Dolly push";
    cams.strips = {a, b};
    sc.camA = a.uid; sc.camB = b.uid;
    seq::Track& cams2 = s.addTrack(seq::Kind::Camera, "Inserts");
    sc.camTrack2 = cams2.uid;
    seq::Track& an = s.addTrack(seq::Kind::Anim, "Set");
    seq::Strip d; d.uid = s.newUid(); d.start = 2.5f; d.length = 3.0f; d.clip = "Gate opens";
    an.strips = {d};
    sc.door = d.uid;
    seq::Track& mu = s.addTrack(seq::Kind::Audio, "Score");
    seq::Strip m; m.uid = s.newUid(); m.start = 0.0f; m.length = 11.0f; m.sound = "score.wav";
    m.fadeIn = 1.0f; m.fadeOut = 2.5f;
    mu.strips = {m};
    sc.music = m.uid;
    seq::Track& fx = s.addTrack(seq::Kind::Audio, "Effects");
    fx.channel = "SFX";
    seq::Strip x; x.uid = s.newUid(); x.start = 4.0f; x.length = 2.0f; x.sound = "boom.wav";
    x.fadeOut = 0.5f;
    fx.strips = {x};
    sc.boom = x.uid;
    sc.seqs.push_back(std::move(s));
}

sequi::Host hostFor(Scene& sc, const fs::path& soundDir,
                    std::map<std::string, std::pair<ImVec2, ImVec2>>* where) {
    sequi::Host h{sc.show, sc.seqs, sc.editSeq, sc.runner, sc.clips, sc.entities, sc.activeCam};
    h.soundNames   = [] { return std::vector<std::string>{"boom.wav", "score.wav"}; };
    h.resolveSound = [soundDir](const std::string& n) { return (soundDir / n).generic_string(); };
    h.channels     = [] { return std::vector<std::string>{"Ambient", "SFX", "Music"}; };
    h.markDirty    = [] {};
    if (where)
        h.probe = [where](const std::string& k, ImVec2 a, ImVec2 b) { (*where)[k] = {a, b}; };
    return h;
}

const Entity& byId(const std::vector<Entity>& es, int id) {
    for (const Entity& e : es) if (e.id == id) return e;
    return es.front();
}

// --png: the window as the editor draws it.
int picture(const std::string& out, int w, int h, int selectUid) {
    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "seqpanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;

    const fs::path dir = fs::temp_directory_path() / "fitzel-seqpanelcheck";
    std::error_code ec;
    fs::create_directories(dir, ec);
    writeWav(dir / "score.wav", 14.0f, 220.0f, false);
    writeWav(dir / "boom.wav", 2.5f, 70.0f, true);

    Scene sc;
    build(sc);
    sequi::Host host = hostFor(sc, dir, nullptr);

    for (int frame = 0; frame < 90; ++frame) {
        window.pollEvents();
        if (frame == 2) {
            sc.runner.beginPreview(0, sc.seqs, sc.clips, sc.entities, sc.activeCam);
            sc.runner.seek(5.6f);
        }
        sc.runner.tick(sc.seqs, sc.clips, sc.entities, 0.0f, sc.activeCam);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        gui.beginFrame();
        // The panel sets its own first size; the picture's is the harness's.
        if (frame > 0) {
            ImGui::SetWindowPos("Sequencer", ImVec2(8.0f, 8.0f));
            ImGui::SetWindowSize("Sequencer", ImVec2(static_cast<float>(w) - 16.0f, static_cast<float>(h) - 16.0f));
        }
        if (frame == 40) sequi::selectStrip(selectUid >= 0 ? (selectUid == 0 ? sc.music : sc.camB) : -1);
        sequi::drawPanel(host);
        gui.endFrame();
        // The waveforms are read on a worker: give it the time a person would.
        if (frame < 60) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (frame + 1 < 90) window.swapBuffers();
    }
    int fw = 0, fh = 0;
    window.framebufferSize(fw, fh);
    std::vector<unsigned char> px(static_cast<std::size_t>(fw) * fh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(out.c_str(), fw, fh, 4, px.data(), fw * 4) != 0;
    std::printf(wrote ? "[seqpanelcheck] wrote %s (%dx%d)\n" : "[seqpanelcheck] could not write %s\n",
                out.c_str(), fw, fh);
    return wrote ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 2 && std::string(argv[1]) == "--png") {
        int w = 1560, h = 640, sel = 0;   // 0 = the music strip, 1 = a shot, -1 = nothing
        for (int i = 3; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--size") std::sscanf(argv[i + 1], "%dx%d", &w, &h);
            if (std::string(argv[i]) == "--select")
                sel = std::string(argv[i + 1]) == "none" ? -1 : std::string(argv[i + 1]) == "shot" ? 1 : 0;
        }
        return picture(argv[2], w, h, sel);
    }

    Scene sc;
    build(sc);
    std::map<std::string, std::pair<ImVec2, ImVec2>> where;
    sequi::Host host = hostFor(sc, fs::temp_directory_path(), &where);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.DeltaTime   = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigInputTrickleEventQueue = false;

    ImVec2 mouse(1590.0f, 890.0f);
    auto frame = [&](bool lmb) {
        where.clear();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
        sc.runner.tick(sc.seqs, sc.clips, sc.entities, io.DeltaTime, sc.activeCam);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1600.0f, 900.0f), ImGuiCond_Always);
        sequi::drawPanel(host);
        ImGui::Render();
    };
    auto rect = [&](const std::string& k, ImVec2& a, ImVec2& b) {
        auto it = where.find(k);
        if (it == where.end()) { check(false, "the panel drew " + k); return false; }
        a = it->second.first; b = it->second.second;
        return true;
    };
    // Press at `from`, move to `to` over a few frames, let go.
    auto drag = [&](ImVec2 from, ImVec2 to) {
        mouse = from; frame(false); frame(true);
        for (int i = 1; i <= 6; ++i) {
            mouse = ImVec2(from.x + (to.x - from.x) * i / 6.0f, from.y + (to.y - from.y) * i / 6.0f);
            frame(true);
        }
        frame(false); frame(false);
    };
    auto key = [&](ImGuiKey k) {
        io.AddKeyEvent(k, true);  frame(false);
        io.AddKeyEvent(k, false); frame(false);
    };
    auto strip = [&](int uid) { return sc.seqs[0].stripByUid(uid); };

    for (int i = 0; i < 3; ++i) frame(false);
    ImVec2 a, b;
    check(rect("strip:" + std::to_string(sc.camA), a, b) && rect("strip:" + std::to_string(sc.camB), a, b),
          "both camera shots are drawn");
    const float pps = (b.x - a.x) / 5.0f;
    check(pps > 10.0f, "the lanes are wide enough to aim at (" + std::to_string(pps) + " px/s)");

    std::printf("move\n");
    {
        rect("strip:" + std::to_string(sc.camB), a, b);
        const ImVec2 grab(a.x + (b.x - a.x) * 0.5f, (a.y + b.y) * 0.5f);
        drag(grab, ImVec2(grab.x + pps * 1.013f, grab.y));
        const float st = strip(sc.camB)->start;
        const float frames = st * 30.0f;
        check(std::fabs(st - 5.0f) < 0.05f, "a shot dragged a second on lands a second on (" + std::to_string(st) + ")");
        check(std::fabs(frames - std::round(frames)) < 1e-3f, "...on a whole frame");
        rect("strip:" + std::to_string(sc.camA), a, b);
        const ImVec2 g2(a.x + (b.x - a.x) * 0.5f, (a.y + b.y) * 0.5f);
        drag(g2, ImVec2(g2.x + pps * 3.0f, g2.y));
        check(strip(sc.camA)->end() <= strip(sc.camB)->start + 1e-3f,
              "a strip pushed into its neighbour stops against it");
    }

    std::printf("move between tracks\n");
    {
        rect("strip:" + std::to_string(sc.camB), a, b);
        ImVec2 la, lb;
        rect("lane:" + std::to_string(sc.camTrack2), la, lb);
        const ImVec2 grab(a.x + (b.x - a.x) * 0.5f, (a.y + b.y) * 0.5f);
        drag(grab, ImVec2(grab.x, (la.y + lb.y) * 0.5f));
        seq::Track* owner = nullptr;
        sc.seqs[0].stripByUid(sc.camB, &owner);
        check(owner && owner->uid == sc.camTrack2, "a shot dragged onto the other camera track moves there");
        // ...and back, for what follows.
        frame(false);
        rect("strip:" + std::to_string(sc.camB), a, b);
        ImVec2 ma, mb;
        rect("lane:" + std::to_string(sc.camTrack), ma, mb);
        drag(ImVec2(a.x + (b.x - a.x) * 0.5f, (a.y + b.y) * 0.5f),
             ImVec2(a.x + (b.x - a.x) * 0.5f, (ma.y + mb.y) * 0.5f));
        sc.seqs[0].stripByUid(sc.camB, &owner);
        check(owner && owner->uid == sc.camTrack, "...and back again");
    }

    std::printf("trim\n");
    {
        rect("strip:" + std::to_string(sc.camB), a, b);
        const float len0 = strip(sc.camB)->length;
        drag(ImVec2(b.x - 2.0f, b.y - 6.0f), ImVec2(b.x - 2.0f - pps * 1.0f, b.y - 6.0f));
        check(std::fabs(strip(sc.camB)->length - (len0 - 1.0f)) < 0.05f,
              "the right edge dragged in a second trims a second (" + std::to_string(strip(sc.camB)->length) + ")");
    }

    std::printf("playhead, preview, cut\n");
    {
        const float x0 = sc.entities[2].localCenter.x;
        ImVec2 la, lb;
        rect("lane:" + std::to_string(sc.camTrack2), la, lb);
        // An empty lane, at about 6.2 s (the Gate opens strip runs 2.5..5.5).
        rect("strip:" + std::to_string(sc.camB), a, b);
        const float t5 = a.x - strip(sc.camB)->start * pps;   // x of t = 0
        mouse = ImVec2(t5 + 3.5f * pps, (la.y + lb.y) * 0.5f);
        frame(false); frame(true); frame(false); frame(false);
        check(sc.runner.preview, "a click on a lane starts the preview");
        check(std::fabs(sc.runner.time - 3.5f) < 0.05f, "...and puts the playhead there (" + std::to_string(sc.runner.time) + ")");
        check(sc.activeCam == 10, "the view cuts to the shot under the playhead (" + std::to_string(sc.activeCam) + ")");
        check(std::fabs(byId(sc.entities, 20).localCenter.x - x0) > 0.1f, "the animation strip under it poses the scene");
        mouse = ImVec2(t5 + 5.5f * pps, (la.y + lb.y) * 0.5f);
        frame(false); frame(true); frame(false); frame(false);
        check(sc.activeCam == 11, "past the cut, the other camera (" + std::to_string(sc.activeCam) + ")");
        sc.runner.endPreview(sc.clips, sc.entities, sc.activeCam);
        check(sc.activeCam == -1, "ending the preview gives the view back");
        check(std::fabs(byId(sc.entities, 20).localCenter.x - x0) < 1e-4f, "...and the scene");
    }

    std::printf("keys\n");
    {
        // Select the shot (a click on it) with the playhead inside it, then S.
        rect("strip:" + std::to_string(sc.camB), a, b);
        mouse = ImVec2(a.x + (b.x - a.x) * 0.5f, (a.y + b.y) * 0.5f);
        frame(false); frame(true); frame(false); frame(false);
        sc.runner.seek(strip(sc.camB)->start + 1.0f);
        std::size_t before = 0;
        for (const seq::Track& t : sc.seqs[0].tracks) before += t.strips.size();
        key(ImGuiKey_S);
        std::size_t after = 0;
        for (const seq::Track& t : sc.seqs[0].tracks) after += t.strips.size();
        check(after == before + 1, "S splits the selected strip at the playhead");
        key(ImGuiKey_Delete);
        std::size_t gone = 0;
        for (const seq::Track& t : sc.seqs[0].tracks) gone += t.strips.size();
        check(gone == after - 1, "Del deletes the selected strip");
        key(ImGuiKey_Space);
        check(sc.runner.playing, "Space plays");
        key(ImGuiKey_Space);
        check(!sc.runner.playing, "...and pauses");
    }

    ImGui::DestroyContext();
    std::printf(g_fails ? "\n%d check(s) FAILED\n" : "\nall sequencer panel checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
