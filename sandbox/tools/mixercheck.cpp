// mixercheck -- draw the Mixer panel and write a picture of it.
//
// Same idea as viewcheck, one floor up: a panel is a thing you LOOK at, and the
// only way to look at this one used to be to start the editor, open a project,
// find the menu entry and press Play so the meters had something to show. That
// is four steps too many for "is the fader cap sitting on the right decibel",
// which is a question about eleven pixels.
//
// So it draws the real panel -- mixerui::drawPanel, the same function the editor
// calls, through the editor's own Gui and theme -- against a desk this file sets
// up, and writes a PNG. Nothing here is a mock-up of the panel: a picture of a
// second implementation would be exactly the wrong answer that looks like proof.
//
//   build/release/bin/mixercheck.exe [out.png] [--size WxH] [--frames N] [--zone W]
//
// --zone W adds a Reverb Zone's bus ("Room", SFX sent in) with the listener W
// (0..1) inside the zone (Desk::zoneWeight): at 0 the bus must stay silent, above
// it must hear the tone -- the zone's weight reaching the engine's send.
//
// --vst3 NAME puts that VST3 plugin after the EQ on SFX and opens its parameters
// under the desk instead of the EQ's; --vst3-path DIR adds a folder to look in
// (build/linux/bin/vst3fixture holds vst3check's "Fitzel Test Gain").
//
// The desk it draws is deliberately mid-mix and not at rest: one bus soloed
// would hide the other, everything at unity would put all three faders on the
// same line, and a silent desk shows no meters at all. What you should see:
//
//   * AMBIENT  at -6 dB, asking for about -9 dB, peak hanging above the bar
//   * SFX      at -12 dB and muted (the button reads MUTE, the bar is dark)
//   * MASTER   at -2.5 dB, metering what came up the buses, clip flag lit
//
// It exits non-zero only if it could not draw at all -- there is nothing to pass
// or fail here, there is a picture to look at.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/audio/Vst3.hpp>
#include <fitzel/core/Window.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/MixerPanel.hpp"
#include "../src/UiStyle.hpp"

namespace {

struct Options {
    std::string out    = "mixer.png";
    int         width  = 1100;
    int         height = 980;
    int         frames = 12;     // ImGui needs a few: fonts, sizing, ballistics
    float       zone   = -1.0f;  // a Reverb Zone bus with this weight (< 0: none)
    std::string vst3;            // a VST3 plugin on SFX, by name
};

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) {
            int w = 0, h = 0;
            if (std::sscanf(argv[++i], "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                o.width = w;
                o.height = h;
            }
        } else if (a == "--zone" && i + 1 < argc) {
            o.zone = std::clamp(static_cast<float>(std::atof(argv[++i])), 0.0f, 1.0f);
        } else if (a == "--vst3" && i + 1 < argc) {
            o.vst3 = argv[++i];
        } else if (a == "--vst3-path" && i + 1 < argc) {
            setenv("VST3_PATH", argv[++i], 1);
        } else if (a == "--frames" && i + 1 < argc) {
            o.frames = std::max(2, std::atoi(argv[++i]));
        } else if (!a.empty() && a[0] != '-') {
            o.out = a;
        }
    }
    return o;
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);

    fitzel::Window window(fitzel::WindowConfig{
        .width = opt.width, .height = opt.height,
        .title = "mixercheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    fitzel::rhi::Device& gpu = window.device();
    ui::setBoldFont(gui.boldFont());
    // No imgui.ini: this has to come out the same on a machine that has
    // never run it, and a remembered window size is exactly the thing
    // that would stop it.
    ImGui::GetIO().IniFilename = nullptr;

    // A real engine and a real desk: a test tone into the SFX channel, an EQ
    // insert on it, a send to the Reverb bus, through the master. The meters
    // the panel draws are then the engine's own measurements, and the numbers
    // printed below say whether the signal came through each strip.
    fitzel::Audio audio;
    mixerui::Desk desk;
    mixerui::Strip* sfx = nullptr;
    mixerui::Strip* reverb = nullptr;
    for (mixerui::Strip& s : desk.strips) {
        if (s.name == "SFX") sfx = &s;
        if (s.name == "Reverb") reverb = &s;
    }
    const int sfxUid = sfx ? sfx->uid : -1, revUid = reverb ? reverb->uid : -1;
    if (sfx) {
        mixerui::Insert eq;
        eq.type = fitzel::mixfx::Type::Eq;
        for (const auto& p : fitzel::mixfx::params(eq.type)) eq.values.push_back(p.def);
        eq.values[1] = 6.0f;                       // low shelf +6 dB
        sfx->inserts.push_back(eq);
        sfx->level = mixerui::fromDb(-6.0f);
        sfx->pan   = -0.4f;
        if (reverb) sfx->sends[revUid] = {mixerui::fromDb(-3.0f), false};
    }
    // A Reverb Zone's bus: what ReverbZones.cpp makes, with the listener this far
    // inside the zone.
    int roomUid = -1;
    if (opt.zone >= 0.0f) {
        mixerui::Strip& room = desk.add(mixerui::Kind::Aux, "Room");
        mixerui::Insert rv;
        rv.type = fitzel::mixfx::Type::Reverb;
        rv.values = {0.75f, 0.4f, 1.0f};
        room.inserts.push_back(rv);
        roomUid = room.uid;
        for (mixerui::Strip& s : desk.strips)
            if (s.name == "SFX") s.sends[roomUid] = {mixerui::fromDb(-3.0f), false};
        desk.zoneWeight[roomUid] = opt.zone;
    }
    for (mixerui::Strip& s : desk.strips)
        if (s.name == "Ambient") s.mute = true;
    desk.master.level = mixerui::fromDb(-2.5f);
    // as if a project were open, so the panel draws its "Global" box enabled (only
    // a click on it would write anything, and nothing clicks here)
    desk.projectFolder = "(mixercheck)";

    // The tone: two seconds of 220 Hz at -12 dBFS, looped, written next to the
    // picture so nothing outside this run is needed.
    const std::string wav = opt.out + ".tone.wav";
    {
        const int rate = 48000, frames = rate * 2;
        std::vector<short> pcm(static_cast<std::size_t>(frames) * 2);
        for (int i = 0; i < frames; ++i) {
            const short v = static_cast<short>(std::sin(i * 2.0 * 3.14159265 * 220.0 / rate) * 32767 * 0.25);
            pcm[i * 2] = pcm[i * 2 + 1] = v;
        }
        FILE* f = std::fopen(wav.c_str(), "wb");
        if (f) {
            const int dataBytes = frames * 4, byteRate = rate * 4;
            const short fmtTag = 1, ch = 2, align = 4, bits = 16;
            const int fmtLen = 16, riffLen = 36 + dataBytes;
            std::fwrite("RIFF", 1, 4, f); std::fwrite(&riffLen, 4, 1, f);
            std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&fmtLen, 4, 1, f);
            std::fwrite(&fmtTag, 2, 1, f); std::fwrite(&ch, 2, 1, f);
            std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f);
            std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
            std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
            std::fwrite(pcm.data(), 2, pcm.size(), f);
            std::fclose(f);
        }
    }
    int selSlot = 0;   // the EQ's parameters under the desk ...
    if (sfx && !opt.vst3.empty()) {
        for (const fitzel::vst3::PluginInfo& p : fitzel::vst3::plugins())
            if (p.name == opt.vst3) {
                sfx->inserts.push_back(mixerui::makePluginInsert(p.uid, p.name));
                selSlot = static_cast<int>(sfx->inserts.size()) - 1;   // ... or the plugin's
            }
        if (selSlot == 0) std::printf("[mixercheck] no VST3 plugin called %s\n", opt.vst3.c_str());
    }
    desk.sync(audio.mixer());
    mixerui::selectInsert(sfxUid, selSlot);
    fitzel::Sound tone = fitzel::Sound::fromFile(audio, wav, true);
    tone.setOutput(audio.mixer(), desk.route("SFX", "SFX"));
    tone.play();

    // By strip uid, not by place: a strip removed halfway (below) moves every
    // later one up a place, and a peak kept by place would land on its neighbour.
    std::map<int, float> maxPeak;
    float masterPeak = 0.0f;
    auto record = [&] {
        for (const mixerui::Strip& s : desk.strips)
            maxPeak[s.uid] = std::max(maxPeak[s.uid], std::max(s.peak[0], s.peak[1]));
        masterPeak = std::max(masterPeak, std::max(desk.master.peak[0], desk.master.peak[1]));
    };
    const int frames = std::max(opt.frames, 90);
    bool removed = false;
    for (int frame = 0; frame < frames; ++frame) {
        window.pollEvents();
        desk.sync(audio.mixer());
        record();
        // Halfway: take a strip out under the running graph, the way the panel's
        // Remove does, and re-route the way main does on a new revision.
        if (frame == frames / 2 && !removed) {
            for (const mixerui::Strip& s : desk.strips)
                if (s.name == "Music") { desk.remove(s.uid); break; }
            removed = true;
            desk.sync(audio.mixer());
            tone.setOutput(audio.mixer(), desk.route("SFX", "SFX"));
        }

        gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
        gpu.clear(fitzel::rhi::ClearColor);
        gui.beginFrame();
        bool show = true;
        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
        mixerui::drawPanel({show, desk, /*dt=*/1.0f / 60.0f, audio.ok(), /*playing=*/true});
        gui.endFrame();
        if (frame + 1 < frames) {
            window.swapBuffers();
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    // The verdict, strip by strip: what came through, in dB.
    int fails = 0;
    for (const mixerui::Strip& s : desk.strips) {
        const float db = mixerui::toDb(maxPeak[s.uid]);
        const bool should = s.uid == sfxUid || s.uid == revUid || (s.uid == roomUid && opt.zone > 0.0f);
        const bool got = maxPeak[s.uid] > 0.001f;
        std::printf("[mixercheck] %-8s %-7s peak %7.1f dB  %s\n", s.name.c_str(),
                    s.kind == mixerui::Kind::Aux ? "aux" : "channel", db,
                    should ? (got ? "ok (signal expected)" : "FAIL (no signal)")
                           : (got ? "FAIL (signal leaked)" : "ok (silent)"));
        if (should != got) ++fails;
    }
    const bool mGot = masterPeak > 0.001f;
    std::printf("[mixercheck] Master   master  peak %7.1f dB  %s\n", mixerui::toDb(masterPeak),
                mGot ? "ok" : "FAIL (no signal)");
    if (!mGot) ++fails;
    if (!audio.ok()) std::printf("[mixercheck] no audio device: the levels mean nothing\n");
    std::remove(wav.c_str());

    // The global desk (mixer.json in the project folder): on writes it, a scene of
    // the project then gets it and keeps it across scene changes, off hands the
    // scenes their own desks back. A scratch project folder, removed afterwards.
    {
        namespace fs = std::filesystem;
        const fs::path proj = fs::temp_directory_path() / "mixercheck_project";
        std::error_code ec;
        fs::remove_all(proj, ec);
        fs::create_directories(proj, ec);
        const std::string folder = proj.generic_string();
        const auto has = [](const mixerui::Desk& d, const char* name) {
            for (const mixerui::Strip& s : d.strips) if (s.name == name) return true;
            return false;
        };
        const auto check = [&](bool ok, const char* what) {
            std::printf("[mixercheck] global: %-52s %s\n", what, ok ? "ok" : "FAIL");
            if (!ok) ++fails;
        };
        mixerui::Desk a;
        check(!a.useProjectDesk(folder), "no mixer.json -> the scene's own desk");
        a.add(mixerui::Kind::Channel, "GlobalTest");
        a.setGlobal(true);
        check(fs::exists(proj / "mixer.json"), "Global on writes mixer.json");
        mixerui::Desk b;
        check(b.useProjectDesk(folder) && has(b, "GlobalTest"), "first scene of the project reads it");
        b.add(mixerui::Kind::Channel, "EditedLive");
        check(b.useProjectDesk(folder) && has(b, "EditedLive"), "next scene keeps the desk as it is");
        b.saveProjectDesk();
        mixerui::Desk c;
        check(c.useProjectDesk(folder) && has(c, "EditedLive"), "saving the scene saves the global desk");
        c.setGlobal(false);
        mixerui::Desk e;
        check(!e.useProjectDesk(folder), "Global off -> scenes bring their own desk");
        e.setGlobal(true);
        check(e.global, "back on (file kept its desk)");
        mixerui::Desk f;
        check(f.useProjectDesk(folder) && has(f, "EditedLive") == false && has(f, "Music"),
              "on again writes the desk that is up now");
        fs::remove_all(proj, ec);
    }

    int w = 0, h = 0;
    window.framebufferSize(w, h);
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
    gpu.readPixels({}, 0, fitzel::rhi::Rect{0, 0, w, h}, fitzel::rhi::Format::RGBA8, px.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(opt.out.c_str(), w, h, 4, px.data(), w * 4) != 0;
    std::printf(wrote ? "[mixercheck] wrote %s (%dx%d)\n" : "[mixercheck] could not write %s\n",
                opt.out.c_str(), w, h);
    if (fails) std::printf("[mixercheck] %d FAIL(S)\n", fails);
    return wrote && fails == 0 ? 0 : 1;
}