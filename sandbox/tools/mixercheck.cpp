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

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/MixerPanel.hpp"
#include "../src/UiStyle.hpp"

namespace {

struct Options {
    std::string out    = "mixer.png";
    int         width  = 420;
    int         height = 640;
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

    // A desk mid-mix. The asks are what the editor feeds in from the routing:
    // the loudest voice on each bus, after that bus's own fader.
    mixerui::Desk desk;
    desk.ambient.level = mixerui::fromDb(-6.0f);
    desk.sfx.level     = mixerui::fromDb(-12.0f);
    desk.sfx.mute      = true;
    desk.master.level  = mixerui::fromDb(-2.5f);

    for (int frame = 0; frame < opt.frames; ++frame) {
        window.pollEvents();
        // Enough of a signal to fill the meters and, on one frame, to trip the
        // clip flag -- which is the only way to see that it latches.
        desk.ambient.ask = mixerui::fromDb(frame == 3 ? 0.5f : -9.0f)
                         * (desk.ambientGain() > 0.0f ? 1.0f : 0.0f);
        desk.sfx.ask     = mixerui::fromDb(-24.0f)
                         * (desk.sfxGain() > 0.0f ? 1.0f : 0.0f);

        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        gui.beginFrame();
        bool show = true;
        ImGui::SetNextWindowPos(ImVec2(8.0f, 8.0f), ImGuiCond_Always);
        mixerui::drawPanel({show, desk, /*dt=*/1.0f / 60.0f,
                            /*audioOk=*/true, /*playing=*/true});
        gui.endFrame();

        if (frame + 1 < opt.frames) window.swapBuffers();
    }

    // The finished frame is still in the back buffer -- read it before the swap
    // so what lands in the file is the frame this harness drew and not whatever
    // the driver left in the front one.
    int w = 0, h = 0;
    window.framebufferSize(w, h);
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);          // GL counts rows from the bottom
    const bool wrote = stbi_write_png(opt.out.c_str(), w, h, 4, px.data(), w * 4) != 0;
    std::printf(wrote ? "[mixercheck] wrote %s (%dx%d)\n"
                      : "[mixercheck] could not write %s\n",
                opt.out.c_str(), w, h);
    return wrote ? 0 : 1;
}
