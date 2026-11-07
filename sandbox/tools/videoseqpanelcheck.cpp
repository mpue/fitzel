// videoseqpanelcheck: the Video Sequencer window (VideoSeqPanel.cpp,
// VideoSeqTimeline.cpp) driven by real ImGui mouse and key events -- the way a
// person uses it. The panel says where it drew its parts (its probe) and the
// harness presses there:
//   - media brought in; one dragged from the bin onto a lane lands where it was
//     dropped, a double-click puts one at the playhead;
//   - a clip dragged along its lane moves by what the pointer moved (on a
//     frame), dragged a lane up goes onto that track, Alt-dragged leaves a
//     copy; its right edge pulled shortens it, its left edge moves its start and
//     in-point together, its top corner pulled sets a fade;
//   - S cuts at the playhead, Del deletes, Ctrl+Z / Ctrl+Y take it back and
//     forth, a rubber band picks clips, Shift+Del closes the gap;
//   - the ruler moves the playhead; I and O set the range;
//   - in the viewer the picked clip is dragged to a new place and pulled bigger
//     by a corner; a click on the picture picks the clip shown there;
//   - Space plays and stops; the film is saved into the project and opened
//     again; an export from the panel writes a video ffprobe reads.
//   build/linux/bin/videoseqpanelcheck            (needs ffmpeg and a display; exit 3 without ffmpeg)
//   build/linux/bin/videoseqpanelcheck --png out.png [--size WxH]   (a picture of the window)
//   build/linux/bin/videoseqpanelcheck --demo <project dir>          (a demo film into a project)

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <fitzel/core/Window.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/ui/Gui.hpp>
#include <imgui.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fitzel/audio/SampleData.hpp>

#include "../src/UiStyle.hpp"
#include "../src/VideoSeq.hpp"
#include "../src/VideoSeqMedia.hpp"
#include "../src/VideoSeqPanel.hpp"

namespace fs = std::filesystem;
namespace vs = vseq;

namespace {

int g_fails = 0;
void check(bool ok, const std::string& what) {
    std::printf(ok ? "  ok   %s\n" : "[FAIL] %s\n", what.c_str());
    if (!ok) ++g_fails;
}

using Rects = std::map<std::string, std::pair<ImVec2, ImVec2>>;

bool ffmpeg(const std::vector<std::string>& args) {
    vs::Process p;
    if (!p.start(vs::ffmpegExe(), args, false, false)) return false;
    return p.wait() == 0;
}

// The media a demo film is cut from: a moving test picture with a tone, a
// second shot in other colours, a picture, a sound.
void makeMedia(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!fs::exists(dir / "shot1.mp4", ec))
        ffmpeg({"-y", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=640x360:r=30:d=6", "-f", "lavfi", "-i",
                "sine=f=330:d=6:r=48000", "-c:v", "libx264", "-crf", "20", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest",
                (dir / "shot1.mp4").string()});
    if (!fs::exists(dir / "shot2.mp4", ec))
        ffmpeg({"-y", "-v", "error", "-f", "lavfi", "-i", "mandelbrot=s=640x360:r=30", "-t", "6", "-c:v", "libx264", "-crf",
                "20", "-pix_fmt", "yuv420p", (dir / "shot2.mp4").string()});
    if (!fs::exists(dir / "logo.png", ec)) {
        std::vector<unsigned char> px(256 * 128 * 4);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 256; ++x) {
                unsigned char* q = &px[(static_cast<std::size_t>(y) * 256 + x) * 4];
                const float d = std::hypot(x - 128.0f, (y - 64.0f) * 2.0f) / 128.0f;
                q[0] = static_cast<unsigned char>(240 - 120 * d);
                q[1] = static_cast<unsigned char>(150 + 60 * d);
                q[2] = 40;
                q[3] = d < 1.0f ? 255 : 0;
            }
        stbi_write_png((dir / "logo.png").string().c_str(), 256, 128, 4, px.data(), 256 * 4);
    }
    if (!fs::exists(dir / "music.wav", ec)) {
        fitzel::SampleData d;
        d.channels = 2;
        d.sampleRate = 48000;
        const int n = 48000 * 10;
        d.samples.resize(static_cast<std::size_t>(n) * 2);
        for (int i = 0; i < n; ++i) {
            const float t = i / 48000.0f;
            const float env = std::exp(-4.0f * std::fmod(t, 0.5f));
            const float v = 0.3f * env * std::sin(6.2831f * (110.0f + 55.0f * std::floor(std::fmod(t, 4.0f))) * t);
            d.samples[static_cast<std::size_t>(i) * 2] = d.samples[static_cast<std::size_t>(i) * 2 + 1] = v;
        }
        fitzel::saveWav((dir / "music.wav").string(), d, fitzel::WavFormat::Pcm16);
    }
}

void demoFilm(vs::Film& f, const fs::path& dir) {
    f = vs::Film{};
    f.name = "Trailer";
    f.width = 1920;
    f.height = 1080;
    const int v1 = f.addTrack(vs::TrackKind::Video, "V1");
    const int v2 = f.addTrack(vs::TrackKind::Video, "V2");
    const int v3 = f.addTrack(vs::TrackKind::Video, "Titles");
    const int a1 = f.addTrack(vs::TrackKind::Audio, "Music");
    auto file = [&](const char* n) { return (dir / n).generic_string(); };
    f.media = {file("shot1.mp4"), file("shot2.mp4"), file("logo.png"), file("music.wav")};
    vs::Clip c;
    c.kind = vs::ClipKind::Video;
    c.file = file("shot1.mp4");
    c.name = "shot1";
    c.start = 0.0;
    c.length = 5.0;
    c.fadeIn = 0.5f;
    f.addClip(v1, c);
    c.file = file("shot2.mp4");
    c.name = "shot2";
    c.start = 4.0;
    c.length = 5.0;
    c.in = 0.5;
    c.fadeIn = 0.0f;
    c.fadeOut = 1.0f;
    f.addClip(v1, c);
    vs::Clip logo;
    logo.kind = vs::ClipKind::Image;
    logo.file = file("logo.png");
    logo.name = "logo";
    logo.start = 1.0;
    logo.length = 3.5;
    logo.look.scale = 0.35f;
    logo.look.x = 0.3f;
    logo.look.y = -0.28f;
    logo.animate = true;
    logo.lookEnd = logo.look;
    logo.lookEnd.rotation = 20.0f;
    logo.fadeIn = logo.fadeOut = 0.4f;
    f.addClip(v2, logo);
    vs::Clip t;
    t.kind = vs::ClipKind::Text;
    t.name = "Title";
    t.text.text = "FITZEL PRESENTS\na film cut in the editor";
    t.text.size = 0.07f;
    t.text.outline = 0.04f;
    t.start = 0.5;
    t.length = 3.0;
    t.fadeIn = t.fadeOut = 0.5f;
    t.look.y = 0.25f;
    f.addClip(v3, t);
    vs::Clip m;
    m.kind = vs::ClipKind::Audio;
    m.file = file("music.wav");
    m.name = "music";
    m.start = 0.0;
    m.length = 9.0;
    m.fadeOut = 2.0f;
    m.volume = 0.8f;
    f.addClip(a1, m);
    f.markIn = 0.5;
    f.markOut = 8.0;
}

int picture(const std::string& out, int w, int h) {
    const fs::path dir = fs::temp_directory_path() / "fitzel-videoseqpanelcheck";
    makeMedia(dir);
    fitzel::Window window(fitzel::WindowConfig{.width = w, .height = h, .title = "videoseqpanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    fitzel::rhi::Device& gpu = window.device();
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;
    vsequi::Panel panel;
    vsequi::Host host;
    bool show = true;
    demoFilm(panel.film(), dir);
    panel.edited();
    panel.seek(1.9);
    int title = -1;
    for (const auto& t : panel.film().tracks)
        for (const auto& c : t.clips)
            if (c.kind == vs::ClipKind::Image) title = c.uid;
    const auto t0 = std::chrono::steady_clock::now();
    int settledFrames = 0;
    for (int frame = 0; frame < 2000; ++frame) {
        window.pollEvents();
        gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
        gpu.clear(fitzel::rhi::ClearColor);
        gui.beginFrame();
        if (frame > 0) {
            ImGui::SetWindowPos("###Video Sequencer", ImVec2(8.0f, 8.0f));
            ImGui::SetWindowSize("###Video Sequencer", ImVec2(static_cast<float>(w) - 16.0f, static_cast<float>(h) - 16.0f));
        }
        if (frame == 2) panel.select({title});
        panel.draw(show, host);
        gui.endFrame();
        // Until the picture, the thumbnails and the waveforms are all in.
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (frame > 30 && panel.viewerSettled() && !panel.exporting()) ++settledFrames;
        if ((settledFrames > 40 && secs > 4.0) || secs > 40.0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        window.swapBuffers();
    }
    int fw = 0, fh = 0;
    window.framebufferSize(fw, fh);
    std::vector<unsigned char> px(static_cast<std::size_t>(fw) * fh * 4);
    gpu.readPixels({}, 0, fitzel::rhi::Rect{0, 0, fw, fh}, fitzel::rhi::Format::RGBA8, px.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(out.c_str(), fw, fh, 4, px.data(), fw * 4) != 0;
    std::printf(wrote ? "[videoseqpanelcheck] wrote %s (%dx%d)\n" : "[videoseqpanelcheck] could not write %s\n", out.c_str(), fw, fh);
    return wrote ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (!vs::ffmpegReady()) {
        std::printf("[videoseqpanelcheck] no ffmpeg -- %s\n", vs::ffmpegInstallHint());
        return 3;
    }
    if (argc > 2 && std::string(argv[1]) == "--demo") {
        const fs::path proj = argv[2];
        const fs::path media = proj / "recordings" / "demo";
        makeMedia(media);
        vs::Film f;
        demoFilm(f, media);
        // Inside the project, the film names its sources relative to it.
        for (auto& t : f.tracks)
            for (auto& c : t.clips)
                if (!c.file.empty()) c.file = fs::relative(c.file, proj).generic_string();
        for (auto& m : f.media) m = fs::relative(m, proj).generic_string();
        const std::string out = (proj / "content" / "films" / "Trailer.json").generic_string();
        std::printf(f.save(out) ? "wrote %s\n" : "could not write %s\n", out.c_str());
        return 0;
    }
    if (argc > 2 && std::string(argv[1]) == "--png") {
        int w = 1700, h = 950;
        for (int i = 3; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--size") std::sscanf(argv[i + 1], "%dx%d", &w, &h);
        return picture(argv[2], w, h);
    }

    const fs::path dir = fs::temp_directory_path() / "fitzel-videoseqpanelcheck";
    makeMedia(dir);
    const fs::path proj = dir / "project";
    std::error_code ec;
    fs::remove_all(proj / "content" / "films", ec);
    fs::create_directories(proj, ec);
    const std::string projFile = (proj / "game.fitzel").generic_string();

    // A GL context for the textures (thumbnails, the viewer), no window shown.
    // ImGui is driven here, not through the GLFW backend: only these events.
    fitzel::Window window(fitzel::WindowConfig{.width = 640, .height = 360, .title = "videoseqpanelcheck", .vsync = false, .maximized = false, .visible = false});
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1600.0f, 900.0f);
    io.DeltaTime   = 1.0f / 60.0f;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.ConfigInputTrickleEventQueue = false;

    vsequi::Panel panel;
    vsequi::Host host;
    host.projectFile = projFile;
    Rects where;
    host.probe = [&](const std::string& k, ImVec2 a, ImVec2 b) { where[k] = {a, b}; };
    bool show = true;

    ImVec2 mouse(1590.0f, 890.0f);
    auto frame = [&](bool lmb, bool rmb = false) {
        where.clear();
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, lmb);
        io.AddMouseButtonEvent(ImGuiMouseButton_Right, rmb);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1600.0f, 900.0f), ImGuiCond_Always);
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
    auto mods = [&](bool ctrl, bool shift, bool alt = false) {
        io.AddKeyEvent(ImGuiMod_Ctrl, ctrl);
        io.AddKeyEvent(ImGuiMod_Shift, shift);
        io.AddKeyEvent(ImGuiMod_Alt, alt);
    };
    auto click = [&](ImVec2 p) {
        mouse = p;
        frame(false); frame(true); frame(false); frame(false);
    };
    auto dblclick = [&](ImVec2 p) {
        mouse = p;
        frame(false); frame(true); frame(false); frame(true); frame(false); frame(false);
    };
    auto drag = [&](ImVec2 from, ImVec2 to) {
        mouse = from;
        frame(false);
        frame(true);
        for (int i = 1; i <= 8; ++i) {
            mouse = ImVec2(from.x + (to.x - from.x) * i / 8.0f, from.y + (to.y - from.y) * i / 8.0f);
            frame(true);
        }
        frame(true);
        frame(false);
        frame(false);
    };
    auto key = [&](ImGuiKey k) {
        io.AddKeyEvent(k, true);
        frame(false);
        io.AddKeyEvent(k, false);
        frame(false);
    };
    auto settle = [&](double secs) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(secs);
        while (std::chrono::steady_clock::now() < until) {
            frame(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    auto clipOf = [&](const char* name) -> vs::Clip* {
        for (auto& t : panel.film().tracks)
            for (auto& c : t.clips)
                if (c.name == name) return &c;
        return nullptr;
    };
    auto trackOfName = [&](const char* name) -> const vs::Track* {
        for (auto& t : panel.film().tracks)
            for (auto& c : t.clips)
                if (c.name == name) return &t;
        return nullptr;
    };
    auto pps = [&](int uid) {
        ImVec2 a, b;
        const vs::Clip* c = panel.film().clip(uid);
        if (!c || !rect("clip:" + std::to_string(uid), a, b)) return 1.0;
        return (b.x - a.x) / c->length;
    };

    frame(false);
    frame(false);
    std::printf("-- media in, clips placed\n");
    panel.importFiles({(dir / "shot1.mp4").string(), (dir / "shot2.mp4").string(), (dir / "logo.png").string(), (dir / "music.wav").string()}, false);
    settle(1.5);
    check(panel.film().media.size() == 4, "four files in the bin");
    ImVec2 a, b, la, lb;
    const int v1 = panel.film().tracks[0].uid, a1 = panel.film().tracks[1].uid;
    // From the bin onto V1 (the timeline fits ~10 s across the lanes).
    if (rect("bin:0", a, b) && rect("lane:" + std::to_string(v1), la, lb)) {
        const float x = la.x + (lb.x - la.x) * 0.1f;
        drag(ImVec2(a.x + 40.0f, (a.y + b.y) * 0.5f), ImVec2(x, (la.y + lb.y) * 0.5f));
        const vs::Clip* c = clipOf("shot1");
        check(c && panel.film().trackOf(c->uid)->uid == v1, "a video dragged from the bin is on V1");
        if (c) {
            frame(false);
            const double expect = (x - la.x) / pps(c->uid);
            check(std::abs(c->start - expect) < 0.1 && std::abs(c->length - 6.0) < 0.05,
                  "...where it was dropped (" + std::to_string(c->start) + " ~ " + std::to_string(expect) + "), 6 s long");
        }
    }
    panel.seek(0.0);
    frame(false);
    if (rect("bin:3", a, b)) dblclick(ImVec2(a.x + 40.0f, (a.y + b.y) * 0.5f));
    {
        const vs::Clip* m = clipOf("music");
        check(m && panel.film().trackOf(m->uid)->uid == a1 && m->start == 0.0, "a double-clicked sound is on A1 at the playhead");
    }

    std::printf("-- dragging clips\n");
    vs::Clip* shot = clipOf("shot1");
    const int su = shot ? shot->uid : -1;
    if (shot && rect("clip:" + std::to_string(su), a, b)) {
        const double before = shot->start;
        const double k = pps(su);
        const float dx = static_cast<float>(1.0 * k);   // a second to the right
        drag(ImVec2(a.x + (b.x - a.x) * 0.5f, (a.y + b.y) * 0.5f), ImVec2(a.x + (b.x - a.x) * 0.5f + dx, (a.y + b.y) * 0.5f));
        shot = panel.film().clip(su);
        const double moved = shot->start - before;
        check(std::abs(moved - 1.0) < 0.06, "dragged a second right: it starts a second later (" + std::to_string(moved) + ")");
        check(std::abs(shot->start * 30.0 - std::round(shot->start * 30.0)) < 1e-6, "...on a frame");
        check(panel.canUndo(), "...and that can be undone");
        // Ctrl+Z, then Ctrl+Y.
        mods(true, false);
        key(ImGuiKey_Z);
        mods(false, false);
        check(std::abs(panel.film().clip(su)->start - before) < 1e-9, "Ctrl+Z: back where it was");
        mods(true, false);
        key(ImGuiKey_Y);
        mods(false, false);
        check(std::abs(panel.film().clip(su)->start - before - moved) < 1e-9, "Ctrl+Y: moved again");
    }
    // A second video track; the clip dragged up onto it.
    {
        const int v2 = panel.film().addTrack(vs::TrackKind::Video, "V2");
        panel.edited();
        frame(false);
        frame(false);
        ImVec2 ta, tb;
        if (rect("clip:" + std::to_string(su), a, b) && rect("lane:" + std::to_string(v2), ta, tb)) {
            const float cx = a.x + (b.x - a.x) * 0.5f;
            drag(ImVec2(cx, (a.y + b.y) * 0.5f), ImVec2(cx, (ta.y + tb.y) * 0.5f));
            check(panel.film().trackOf(su)->uid == v2, "dragged a lane up: on V2");
        }
        // Alt-drag: a copy back down on V1, the original stays.
        if (rect("clip:" + std::to_string(su), a, b) && rect("lane:" + std::to_string(v1), ta, tb)) {
            const std::size_t n = panel.film().tracks[static_cast<std::size_t>(panel.film().rowOf(v1))].clips.size();
            mods(false, false, true);
            const float cx = a.x + (b.x - a.x) * 0.5f;
            drag(ImVec2(cx, (a.y + b.y) * 0.5f), ImVec2(cx, (ta.y + tb.y) * 0.5f));
            mods(false, false, false);
            const auto& v1clips = panel.film().tracks[static_cast<std::size_t>(panel.film().rowOf(v1))].clips;
            check(panel.film().trackOf(su)->uid == v2 && v1clips.size() == n + 1, "Alt-dragged: a copy on V1, the original still on V2");
            // The copy: away, so the next steps have one clip to work on.
            if (v1clips.size() == n + 1) {
                panel.select({v1clips.back().uid});
                frame(false);
                key(ImGuiKey_Delete);
            }
        }
    }
    std::printf("-- trimming and fading\n");
    shot = panel.film().clip(su);
    if (shot && rect("clip:" + std::to_string(su), a, b)) {
        const double k = pps(su);
        const double len0 = shot->length, start0 = shot->start, in0 = shot->in;
        drag(ImVec2(b.x - 2.0f, (a.y + b.y) * 0.5f + 6.0f), ImVec2(b.x - 2.0f - static_cast<float>(2.0 * k), (a.y + b.y) * 0.5f + 6.0f));
        shot = panel.film().clip(su);
        check(std::abs(shot->length - (len0 - 2.0)) < 0.06 && shot->start == start0, "the right edge pulled 2 s in: 2 s shorter (" + std::to_string(shot->length) + ")");
        frame(false);
        rect("clip:" + std::to_string(su), a, b);
        const double len1 = shot->length;
        drag(ImVec2(a.x + 2.0f, (a.y + b.y) * 0.5f + 6.0f), ImVec2(a.x + 2.0f + static_cast<float>(1.0 * k), (a.y + b.y) * 0.5f + 6.0f));
        shot = panel.film().clip(su);
        check(std::abs(shot->start - (start0 + 1.0)) < 0.06 && std::abs(shot->in - (in0 + 1.0)) < 0.06 && std::abs(shot->length - (len1 - 1.0)) < 0.06,
              "the left edge pulled 1 s in: starts later, a second further into the source");
        // Pulled out past the source's start: it stops there.
        frame(false);
        rect("clip:" + std::to_string(su), a, b);
        drag(ImVec2(a.x + 2.0f, (a.y + b.y) * 0.5f + 6.0f), ImVec2(a.x + 2.0f - static_cast<float>(3.0 * k), (a.y + b.y) * 0.5f + 6.0f));
        shot = panel.film().clip(su);
        check(shot->in == 0.0 && std::abs(shot->start - start0) < 0.06, "...pulled out past the source's start: it stops at frame 0");
        // The fade handle in the top left corner.
        frame(false);
        rect("clip:" + std::to_string(su), a, b);
        mouse = ImVec2(a.x + 12.0f, (a.y + b.y) * 0.5f);   // hovered first: the handles show
        frame(false);
        drag(ImVec2(a.x + 4.0f, a.y + 4.0f), ImVec2(a.x + 4.0f + static_cast<float>(0.5 * k), a.y + 4.0f));
        shot = panel.film().clip(su);
        check(std::abs(shot->fadeIn - 0.5f) < 0.06f, "the top corner pulled half a second: a fade in of 0.5 s (" + std::to_string(shot->fadeIn) + ")");
    }

    std::printf("-- cutting, deleting, the playhead\n");
    shot = panel.film().clip(su);
    if (shot && rect("ruler", a, b)) {
        // A click on the ruler, a second into the clip.
        const double k = pps(su);
        ImVec2 ca, cb;
        rect("clip:" + std::to_string(su), ca, cb);
        click(ImVec2(ca.x + static_cast<float>(1.0 * k), (a.y + b.y) * 0.5f));
        check(std::abs(panel.playhead() - (shot->start + 1.0)) < 0.06, "a click on the ruler moves the playhead there (" + std::to_string(panel.playhead()) + ")");
        panel.select({su});
        frame(false);
        key(ImGuiKey_S);
        const vs::Track* tr = panel.film().trackOf(su);
        check(tr && tr->clips.size() == 2, "S cuts the clip at the playhead in two");
        if (tr && tr->clips.size() == 2) {
            const vs::Clip& l = tr->clips[0];
            const vs::Clip& r = tr->clips[1];
            check(std::abs(l.end() - r.start) < 1e-9 && std::abs(r.in - (l.in + l.length)) < 1e-6, "...the halves meet, the second where the first stopped in the source");
            const int right = r.uid;
            panel.select({right});
            frame(false);
            key(ImGuiKey_Delete);
            check(!panel.film().clip(right), "Del deletes the picked half");
            mods(true, false);
            key(ImGuiKey_Z);
            mods(false, false);
            check(panel.film().clip(right) != nullptr, "Ctrl+Z brings it back");
        }
    }
    key(ImGuiKey_I);
    const double in = panel.playhead();
    panel.seek(in + 2.0);
    frame(false);
    key(ImGuiKey_O);
    check(std::abs(panel.film().markIn - in) < 1e-6 && std::abs(panel.film().markOut - (in + 2.0)) < 1e-6, "I and O set the range");
    // A rubber band over V2: both halves picked.
    {
        const vs::Track* tr = panel.film().trackOf(su);
        ImVec2 ta, tb;
        if (tr && rect("lane:" + std::to_string(tr->uid), ta, tb)) {
            ImVec2 l0a, l0b, l1a, l1b;
            rect("clip:" + std::to_string(tr->clips[0].uid), l0a, l0b);
            rect("clip:" + std::to_string(tr->clips[1].uid), l1a, l1b);
            panel.select({});
            drag(ImVec2(l0a.x - 10.0f, ta.y + 2.0f), ImVec2(l1b.x + 10.0f, tb.y - 2.0f));
            check(panel.selection().size() == 2, "a rubber band across V2 picks both halves (" + std::to_string(panel.selection().size()) + ")");
        }
    }

    std::printf("-- the viewer\n");
    {
        // A picture on the top track, picked and dragged in the viewer.
        panel.seek(0.5);
        frame(false);
        const int v2 = panel.film().tracks[0].uid;
        vs::Clip pic;
        pic.kind = vs::ClipKind::Image;
        pic.file = (dir / "logo.png").generic_string();
        pic.name = "logo";
        pic.start = 0.0;
        pic.length = 4.0;
        pic.look.scale = 0.5f;
        const int pu = panel.film().addClip(v2, pic);
        panel.edited();
        panel.select({});
        settle(0.8);
        if (rect("viewer", a, b)) {
            const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            click(c);
            check(panel.selection().size() == 1 && panel.selection()[0] == pu, "a click on the picture picks the clip shown there");
            const float vw = b.x - a.x;
            drag(c, ImVec2(c.x + vw * 0.1f, c.y));
            const vs::Clip* p = panel.film().clip(pu);
            check(p && std::abs(p->look.x - 0.1f) < 0.01f && std::abs(p->look.y) < 0.01f,
                  "dragged a tenth of the viewer right: x is 10% (" + std::to_string(p ? p->look.x : -1) + ")");
            // A corner, pulled out to twice the distance from the middle: the
            // 256x128 logo fitted to 1920x1080 is 1920x960, at half size 960x480.
            frame(false);
            const float k = vw / 1920.0f;
            const ImVec2 cc(c.x + vw * 0.1f, c.y);
            const float halfW = 480.0f * k, halfH = 240.0f * k;
            const ImVec2 corner(cc.x + halfW, cc.y + halfH);
            drag(corner, ImVec2(cc.x + halfW * 2.0f, cc.y + halfH * 2.0f));
            p = panel.film().clip(pu);
            check(p && std::abs(p->look.scale - 1.0f) < 0.03f, "a corner pulled out to twice the distance: twice the size (" + std::to_string(p ? p->look.scale : -1) + ")");
        }
    }

    std::printf("-- playing\n");
    {
        panel.seek(0.0);
        frame(false);
        key(ImGuiKey_Space);
        check(panel.playing(), "Space plays");
        settle(0.6);
        const double t = panel.playhead();
        check(t > 0.3 && t < 1.2, "...and the playhead runs (" + std::to_string(t) + " s after 0.6 s)");
        key(ImGuiKey_Space);
        check(!panel.playing(), "Space again stops");
        // The picture catches up with the playhead.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!panel.viewerSettled() && std::chrono::steady_clock::now() < until) {
            frame(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(panel.viewerSettled(), "the viewer shows the frame at the playhead, all of it decoded");
        // And then it rests: nothing playing, nothing coming -- the editor may
        // drop to its idle frame rate.
        const auto quiet = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (panel.wantsSmoothFrames() && std::chrono::steady_clock::now() < quiet) {
            frame(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        check(!panel.wantsSmoothFrames(), "idle, it asks for no smooth frames");
    }

    std::printf("-- files and export\n");
    {
        panel.film().name = "Check";
        mods(true, false);
        key(ImGuiKey_S);
        mods(false, false);
        // The first save asks for a name (the field has the keyboard a frame
        // after it opens): Enter takes the film's.
        frame(false);
        frame(false);
        io.AddKeyEvent(ImGuiKey_Enter, true);
        frame(false);
        io.AddKeyEvent(ImGuiKey_Enter, false);
        frame(false);
        frame(false);
        const fs::path saved = proj / "content" / "films" / "Check.json";
        check(fs::exists(saved, ec), "Ctrl+S saves the film into content/films (" + panel.status() + ")");
        const std::string json = panel.film().toJson();
        vsequi::Panel again;
        bool show2 = true;
        vsequi::Host h2;
        h2.projectFile = projFile;
        check(again.open("Check", h2) && again.film().toJson() == json, "...and it opens again as it was");
        (void)show2;

        const fs::path out = dir / "panel-export.mp4";
        fs::remove(out, ec);
        panel.exportTo(out.string(), 0);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        frame(false);
        while (panel.exporting() && std::chrono::steady_clock::now() < until) {
            frame(false);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        frame(false);
        const vs::MediaInfo mi = vs::probe(out.string());
        check(mi.ok && mi.video && mi.width == 1920 && std::abs(mi.duration - panel.film().duration()) < 0.15,
              "an export from the panel: 1920 wide, as long as the film (" + panel.exportMessage() + ")");
    }

    ImGui::DestroyContext();
    std::printf(g_fails ? "[videoseqpanelcheck] %d FAILED\n" : "[videoseqpanelcheck] all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
