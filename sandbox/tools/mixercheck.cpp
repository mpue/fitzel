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
//   build/release/bin/mixercheck.exe [out.png] [--size WxH] [--frames N]
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
#include <string>
#include <thread>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/core/Window.hpp>
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
    for (mixerui::Strip& s : desk.strips)
        if (s.name == "Ambient") s.mute = true;
    desk.master.level = mixerui::fromDb(-2.5f);

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
    desk.sync(audio.mixer());
    mixerui::selectInsert(sfxUid, 0);   // the EQ's parameters under the desk
    fitzel::Sound tone = fitzel::Sound::fromFile(audio, wav, true);
    tone.setOutput(audio.mixer(), desk.route("SFX", "SFX"));
    tone.play();

    float maxPeak[64] = {};
    auto record = [&] {
        int i = 0;
        for (const mixerui::Strip& s : desk.strips) {
            maxPeak[i] = std::max(maxPeak[i], std::max(s.peak[0], s.peak[1]));
            if (++i >= 63) break;
        }
        maxPeak[63] = std::max(maxPeak[63], std::max(desk.master.peak[0], desk.master.peak[1]));
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

        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
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
    int i = 0, fails = 0;
    for (const mixerui::Strip& s : desk.strips) {
        const float db = mixerui::toDb(maxPeak[i]);
        const bool should = s.uid == sfxUid || s.uid == revUid;
        const bool got = maxPeak[i] > 0.001f;
        std::printf("[mixercheck] %-8s %-7s peak %7.1f dB  %s\n", s.name.c_str(),
                    s.kind == mixerui::Kind::Aux ? "aux" : "channel", db,
                    should ? (got ? "ok (signal expected)" : "FAIL (no signal)")
                           : (got ? "FAIL (signal leaked)" : "ok (silent)"));
        if (should != got) ++fails;
        if (++i >= 63) break;
    }
    const bool mGot = maxPeak[63] > 0.001f;
    std::printf("[mixercheck] Master   master  peak %7.1f dB  %s\n", mixerui::toDb(maxPeak[63]),
                mGot ? "ok" : "FAIL (no signal)");
    if (!mGot) ++fails;
    if (!audio.ok()) std::printf("[mixercheck] no audio device: the levels mean nothing\n");
    std::remove(wav.c_str());

    int w = 0, h = 0;
    window.framebufferSize(w, h);
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(opt.out.c_str(), w, h, 4, px.data(), w * 4) != 0;
    std::printf(wrote ? "[mixercheck] wrote %s (%dx%d)\n" : "[mixercheck] could not write %s\n",
                opt.out.c_str(), w, h);
    if (fails) std::printf("[mixercheck] %d FAIL(S)\n", fails);
    return wrote && fails == 0 ? 0 : 1;
}