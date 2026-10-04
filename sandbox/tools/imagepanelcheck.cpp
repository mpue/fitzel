// imagepanelcheck -- draws the Image editor window and writes a picture of it.
//
// mixercheck's trick applied to the image editor: the editor's own
// imageui::ImageEditor drawn through the editor's own Gui and theme, on a
// picture painted here through the same document API the tools use (layers, a
// gradient, shapes, a brush stroke, text, a selection). Run from the directory
// the shaders were copied to (build/release/bin).
//
// --interact drives the window the way a hand would instead: mouse and key
// events go into ImGui's input queue, frame by frame, and the picture is checked
// afterwards -- click-click selection, a dragged shape, a stabilised stroke,
// click-to-click lines, undo/redo by keyboard, a move, the wand, typed text.
//
//   imagepanelcheck [out.png] [--size WxH] [--tool n] [--open picture] [--welcome]
//                   [--filter n] [--zoom z] [--frames n] [--interact]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/ImageEditPanel.hpp"
#include "../src/UiStyle.hpp"

static int g_fail = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) { ++g_fail; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
                       std::printf(__VA_ARGS__); std::printf("\n"); }      \
    } while (0)

static void paintScene(img::Document& d) {
    const int W = d.width(), H = d.height();
    // Sky: a gradient on the background.
    d.beginEdit("Gradient");
    img::gradient(d, 0, 0, 0, float(H) * 0.7f, {0.35f, 0.55f, 0.9f, 1}, {0.95f, 0.85f, 0.7f, 1}, false, 1.0f);
    d.endEdit();
    // Hills on a layer of their own.
    d.addLayer("Hills");
    img::Stroke s;
    img::Brush b;
    s.begin(d, img::PaintMode::Paint, {0.25f, 0.45f, 0.2f, 1}, b, 0, 0, "Ellipse");
    s.shape(img::Stroke::Shape::EllipseFill, -0.2f * W, 0.55f * H, 0.7f * W, 1.4f * H, 1);
    s.end();
    s.begin(d, img::PaintMode::Paint, {0.18f, 0.36f, 0.16f, 1}, b, 0, 0, "Ellipse");
    s.shape(img::Stroke::Shape::EllipseFill, 0.4f * W, 0.62f * H, 1.3f * W, 1.5f * H, 1);
    s.end();
    // A sun, soft, screened over the sky.
    d.addLayer("Sun");
    b.radius = 38; b.hardness = 0.2f; b.opacity = 0.9f;
    s.begin(d, img::PaintMode::Paint, {1.0f, 0.9f, 0.5f, 1}, b);
    s.to(0.78f * W, 0.22f * H);
    s.end();
    d.setBlend(2, img::Blend::Screen);
    // A brush stroke and a frame.
    d.addLayer("Path");
    b.radius = 6; b.hardness = 0.7f; b.opacity = 1.0f;
    s.begin(d, img::PaintMode::Paint, {0.55f, 0.4f, 0.25f, 1}, b);
    for (int i = 0; i <= 40; ++i) {
        const float t = float(i) / 40.0f;
        s.to(0.2f * W + t * 0.45f * W, H - 10.0f - t * 0.32f * H + 18.0f * std::sin(t * 9.0f));
    }
    s.end();
    s.begin(d, img::PaintMode::Paint, {0.1f, 0.1f, 0.1f, 1}, b, 0, 0, "Rectangle");
    s.shape(img::Stroke::Shape::Rect, 8, 8, float(W - 8), float(H - 8), 4);
    s.end();
    // Text.
    const auto& fonts = img::systemFonts();
    std::string font;
    for (const auto& f : fonts) if (f.name == "Arial Bold") font = f.path;
    if (font.empty() && !fonts.empty()) font = fonts.front().path;
    img::Pixels t;
    int tw = 0, th = 0;
    if (img::renderText(font, "fitzel", 64, {1, 1, 1, 1}, t, tw, th)) {
        d.checkpoint("Text");
        d.insertLayer("fitzel", t, tw, th, 30, 20);
    }
    d.selectRect({int(0.55f * W), int(0.5f * H), int(0.9f * W), int(0.85f * H)}, img::SelOp::Replace, true);
}

int main(int argc, char** argv) {
    std::string out = "imagepanel.png", openPath;
    int w = 1500, h = 900, frames = 8, tool = int(imageui::Tool::Brush), filter = -1;
    float zoom = 0.0f;
    bool welcome = false, interact = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (a == "--tool" && i + 1 < argc) tool = std::atoi(argv[++i]);
        else if (a == "--open" && i + 1 < argc) openPath = argv[++i];
        else if (a == "--filter" && i + 1 < argc) filter = std::atoi(argv[++i]);
        else if (a == "--zoom" && i + 1 < argc) zoom = float(std::atof(argv[++i]));
        else if (a == "--frames" && i + 1 < argc) frames = std::max(3, std::atoi(argv[++i]));
        else if (a == "--welcome") welcome = true;
        else if (a == "--interact") interact = true;
        else if (!a.empty() && a[0] != '-') out = a;
    }
    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "imagepanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;

    const std::string project;
    std::string status;
    imageui::ImageEditor ed({project, status});
    ed.setFirstSize(float(w), float(h));

    int frameNo = 0;
    auto frame = [&] {
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        gui.beginFrame();
        bool show = true;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        if (frameNo == 2 && filter >= 0) ed.startFilter(img::Filter(filter));
        if (frameNo == 2 && zoom > 0.0f) ed.setZoom(zoom);
        ed.panel(show);
        gui.endFrame();
        window.swapBuffers();
        ++frameNo;
    };

    if (interact) {
        // The pointer is ours: tell the backend it is inside the window, so it
        // does not overwrite our positions with the real cursor's.
        ImGui_ImplGlfw_CursorEnterCallback(window.nativeHandle(), 1);
        ImGuiIO& io = ImGui::GetIO();
        ed.newImage(400, 300, {1, 1, 1, 1});
        ed.setColors({0, 0, 0, 1}, {1, 1, 1, 1});
        for (int i = 0; i < 3; ++i) frame();
        ed.setZoom(1.0f);
        frame();
        img::Document& d = *ed.document();
        auto at = [&](float x, float y) { const ImVec2 s = ed.toScreen(ImVec2(x, y)); io.AddMousePosEvent(s.x, s.y); frame(); };
        auto down = [&] { io.AddMouseButtonEvent(0, true); frame(); };
        auto up = [&] { io.AddMouseButtonEvent(0, false); frame(); };
        auto click = [&](float x, float y) { at(x, y); down(); up(); };
        auto drag = [&](float x0, float y0, float x1, float y1, int steps) {
            at(x0, y0); down();
            for (int i = 1; i <= steps; ++i) at(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps);
            up();
        };
        auto key = [&](ImGuiKey k, bool ctrl) {
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(k, true); frame();
            io.AddKeyEvent(k, false);
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
            frame();
        };
        auto px = [&](int layer, int x, int y) { return d.pixels(layer).data() + (std::size_t(y) * d.width() + x) * 4; };

        // Select: click one corner, click the other.
        ed.setTool(imageui::Tool::Select);
        click(50, 50);
        CHECK(!d.hasSelection(), "one click only anchors");
        at(150, 120);
        click(150, 120);
        const img::Rect b = d.selectionBounds();
        CHECK(d.hasSelection() && b.x0 == 50 && b.y0 == 50 && b.x1 == 150 && b.y1 == 120,
              "click-click selects the box (%d %d %d %d)", b.x0, b.y0, b.x1, b.y1);
        key(ImGuiKey_D, true);
        CHECK(!d.hasSelection(), "Ctrl+D deselects");
        // Select by dragging.
        drag(20, 20, 60, 40, 5);
        CHECK(d.hasSelection() && d.selectionBounds().x1 == 60, "a drag selects too (x1 %d)", d.selectionBounds().x1);
        key(ImGuiKey_D, true);

        // A dragged filled rectangle.
        ed.setTool(imageui::Tool::Shape);
        ed.setShape(2);
        drag(200, 40, 300, 140, 6);
        CHECK(px(0, 250, 90)[0] < 10 && px(0, 190, 90)[0] == 255, "dragged filled rectangle (%d / %d)",
              px(0, 250, 90)[0], px(0, 190, 90)[0]);
        CHECK(d.undoLabel() == "Rectangle", "one undo step named Rectangle (%s)", d.undoLabel().c_str());

        // A stabilised stroke: the brush trails the pointer by the string.
        ed.setTool(imageui::Tool::Brush);
        drag(40, 200, 160, 200, 12);
        CHECK(px(0, 100, 200)[0] < 30, "the stroke painted along the way (%d)", px(0, 100, 200)[0]);
        CHECK(px(0, 158, 200)[0] > 200, "...and stopped a string's length short of the pointer (%d)", px(0, 158, 200)[0]);
        key(ImGuiKey_Z, true);
        CHECK(px(0, 100, 200)[0] == 255, "Ctrl+Z took it back");
        key(ImGuiKey_Y, true);
        CHECK(px(0, 100, 200)[0] < 30, "Ctrl+Y brought it back");

        // Lines from click to click, no drag.
        ed.setLineMode(true);
        click(40, 260);
        click(360, 260);
        CHECK(px(0, 200, 260)[0] < 30, "a straight line between two clicks (%d)", px(0, 200, 260)[0]);
        ed.setLineMode(false);

        // Move the whole layer 10 px right by dragging.
        ed.setTool(imageui::Tool::Move);
        drag(100, 100, 110, 100, 4);
        CHECK(px(0, 255, 90)[0] < 10 && px(0, 205, 90)[3] == 255 && px(0, 205, 90)[0] == 255,
              "the layer moved 10 px right (%d, %d)", px(0, 255, 90)[0], px(0, 205, 90)[0]);
        CHECK(d.undoLabel() == "Move", "one Move step (%s)", d.undoLabel().c_str());

        // The wand takes the white around the rectangle.
        ed.setTool(imageui::Tool::Wand);
        click(380, 10);
        CHECK(d.hasSelection() && d.selAt(std::size_t(90) * 400 + 250) == 0, "wand selects the white, not the box");
        key(ImGuiKey_D, true);

        // Text: click, type, done.
        ed.setTool(imageui::Tool::Text);
        const int before = d.layerCount();
        click(20, 150);
        io.AddInputCharactersUTF8("Hi");
        frame(); frame();
        ed.setTool(imageui::Tool::Brush);   // ends the text, keeps it
        frame();
        int ink = 0;
        if (d.layerCount() == before + 1)
            for (int y = 140; y < 200; ++y)
                for (int x = 15; x < 120; ++x) ink += px(d.active(), x, y)[3] > 128;
        CHECK(d.layerCount() == before + 1 && ink > 50, "typed text became a layer (%d layers, %d inked)", d.layerCount(), ink);
        CHECK(d.layer(d.active()).name == "Hi", "typing replaced the last text and named the layer (%s)",
              d.layer(d.active()).name.c_str());

        // Right click picks a colour.
        ed.setColors({1, 0, 0, 1}, {1, 1, 1, 1});
        at(250, 90);
        io.AddMouseButtonEvent(1, true); frame();
        io.AddMouseButtonEvent(1, false); frame();
        img::Color fg{};
        // (read back through a fill: the bucket paints with the picked colour)
        ed.setTool(imageui::Tool::Fill);
        d.setActive(0);
        click(10, 290);
        fg = d.sample(10, 290, false);
        CHECK(fg.r < 0.05f && fg.g < 0.05f, "right click picked the black under it (%.2f %.2f %.2f)", fg.r, fg.g, fg.b);

        frame();
        std::printf("[imagepanelcheck] interact: %s\n", g_fail ? "FAILED" : "all passed");
    } else {
        if (!welcome) {
            if (!openPath.empty()) {
                if (!ed.open(openPath)) { std::printf("[imagepanelcheck] %s\n", status.c_str()); return 1; }
            } else {
                ed.newImage(900, 560, {1, 1, 1, 1});
                paintScene(*ed.document());
            }
            ed.setTool(imageui::Tool(std::clamp(tool, 0, int(imageui::Tool::Count) - 1)));
            ed.setColors({0.85f, 0.25f, 0.2f, 1}, {1, 1, 1, 1});
        }
        for (int i = 0; i < frames; ++i) frame();
    }

    // The last frame went to the front buffer; draw it once more to read it back.
    frameNo = 100;
    window.pollEvents();
    int fw = 0, fh = 0;
    window.framebufferSize(fw, fh);
    glViewport(0, 0, fw, fh);
    glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    gui.beginFrame();
    bool show = true;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ed.panel(show);
    gui.endFrame();
    std::vector<unsigned char> pixels(static_cast<std::size_t>(fw) * fh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(out.c_str(), fw, fh, 4, pixels.data(), fw * 4) != 0;
    std::printf(wrote ? "[imagepanelcheck] wrote %s (%dx%d)\n" : "[imagepanelcheck] could not write %s\n",
                out.c_str(), fw, fh);
    if (!status.empty()) std::printf("[imagepanelcheck] status: %s\n", status.c_str());
    return wrote && !g_fail ? 0 : 1;
}
