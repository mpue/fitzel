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
// --prompt shows the "New image from prompt" window beside it; --comfy URL has
// that ComfyUI paint a picture through it (into a tab, then onto a layer) and
// checks both arrived. Exit 3: no ComfyUI answered there.
//
//   imagepanelcheck [out.png] [--size WxH] [--tool n] [--open picture] [--welcome]
//                   [--filter n] [--curves] [--transform] [--zoom z] [--frames n] [--interact]
//                   [--prompt] [--comfy http://host:8188 [--start "systemctl start comfyui"]]
//                   [--save-asset --project some/dir/game.fitzel] [--normal-map --project ...]
//
// --save-asset: File > Save in assets into that project (it need not exist
// beyond its folder): a new picture lands in textures/, one of the same name
// replaces it, and the dialog is left open on the replace warning.
// --normal-map: Image > Generate normal map on a project texture with a bright
// square: the slopes point the right way, Esc takes the tab away again, OK
// keeps it, and Save in assets offers the texture's folder. Left open.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/ComfyUi.hpp"
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
    bool welcome = false, interact = false, curves = false, transform = false, style = false, range = false, mask = false;
    bool prompt = false, saveAsset = false, normalMap = false;
    std::string comfyUrl, project;
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
        else if (a == "--curves") curves = true;
        else if (a == "--transform") transform = true;
        else if (a == "--style") style = true;
        else if (a == "--range") range = true;
        else if (a == "--mask") mask = true;
        else if (a == "--prompt") prompt = true;
        else if (a == "--save-asset") saveAsset = true;
        else if (a == "--normal-map") normalMap = true;
        else if (a == "--project" && i + 1 < argc) project = argv[++i];
        else if (a == "--comfy" && i + 1 < argc) comfyUrl = argv[++i];
        else if (a == "--start" && i + 1 < argc) comfy::client().settings.startCommand = argv[++i];
        else if (!a.empty() && a[0] != '-') out = a;
    }
    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "imagepanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    fitzel::rhi::Device& gpu = window.device();
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;

    std::string status;
    // The user's own comfyui.json stays as it is, whatever the window is told.
    comfy::client().persist = false;
    if (!comfyUrl.empty()) comfy::client().settings.url = comfyUrl;
    imageui::ImageEditor ed({project, status});
    ed.setFirstSize(float(w) * (prompt || !comfyUrl.empty() ? 0.64f : 1.0f), float(h));
    if (prompt || !comfyUrl.empty()) ed.openPrompt();

    int frameNo = 0;
    auto frame = [&] {
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        gpu.setViewport(0, 0, fw, fh);
        gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
        gpu.clear(fitzel::rhi::ClearColor);
        gui.beginFrame();
        bool show = true;
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        if (frameNo == 2 && filter >= 0) ed.startFilter(img::Filter(filter));
        if (frameNo == 2 && curves) ed.startCurves();
        if (frameNo == 2 && transform) {
            ed.document()->setActive(3);   // the path
            ed.document()->deselect();
            ed.beginTransform();
        }
        if (frameNo == 2 && zoom > 0.0f) ed.setZoom(zoom);
        if (frameNo == 2 && (style || mask)) {
            img::Document& d = *ed.document();
            // The hills behind a soft mask; the text with a sticker style.
            d.setActive(1);
            d.selectRect({0, 0, d.width() / 2, d.height()}, img::SelOp::Replace, true);
            d.modifySelection(img::SelModify::Feather, 60);
            d.addMask(img::MaskInit::RevealSelection);
            d.setActive(4);
            img::LayerStyle s;
            s.fx[int(img::Fx::Stroke)] = img::defaultEffect(img::Fx::Stroke);
            s.fx[int(img::Fx::Stroke)].color = {1, 1, 1, 1};
            s.fx[int(img::Fx::Stroke)].size = 5;
            s.fx[int(img::Fx::DropShadow)] = img::defaultEffect(img::Fx::DropShadow);
            s.fx[int(img::Fx::DropShadow)].distance = 6;
            s.fx[int(img::Fx::DropShadow)].size = 8;
            d.setStyle(4, s);
            d.deselect();
            if (style) ed.startStyle(int(img::Fx::DropShadow));
        }
        if (frameNo == 2 && range) {
            ed.startColorRange();
            img::ColorRange cr;
            cr.add = {{0.25f, 0.45f, 0.2f, 1}, {0.18f, 0.36f, 0.16f, 1}};   // the two greens of the hills
            ed.setColorRange(cr);
        }
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
        frame();   // the type field sits in the options bar, drawn before the canvas: it takes the keys a frame later
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

        // --- The Photoshop-style tools, on a fresh grey picture ---------------------------
        ed.newImage(400, 300, {0.5f, 0.5f, 0.5f, 1});
        for (int i = 0; i < 3; ++i) frame();
        ed.setZoom(1.0f);
        frame();
        img::Document& e = *ed.document();
        auto ep = [&](int layer, int x, int y) { return e.pixels(layer).data() + (std::size_t(y) * e.width() + x) * 4; };
        auto sel = [&](int x, int y) { return e.selAt(std::size_t(y) * e.width() + x); };
        auto mods = [&](ImGuiKey mod, bool on) { io.AddKeyEvent(mod, on); };

        // Polygonal lasso: corner by corner, closed by clicking the first point again.
        ed.setTool(imageui::Tool::Lasso);
        ed.setPolyLasso(true);
        click(50, 50); click(150, 50); click(100, 130);
        CHECK(!e.hasSelection(), "three corners are not a selection yet");
        click(50, 50);
        CHECK(e.hasSelection() && sel(100, 70) == 255 && sel(140, 120) == 0, "polygonal lasso: a triangle (%d %d)",
              sel(100, 70), sel(140, 120));
        key(ImGuiKey_D, true);
        // Freehand: a drag round a box.
        ed.setPolyLasso(false);
        at(200, 150); down();
        for (int i = 0; i <= 8; ++i) at(200 + i * 10.0f, 150);
        for (int i = 0; i <= 8; ++i) at(280, 150 + i * 10.0f);
        for (int i = 0; i <= 8; ++i) at(280 - i * 10.0f, 230);
        up();
        CHECK(e.hasSelection() && sel(240, 190) == 255 && sel(240, 260) == 0, "freehand lasso");
        // Shift adds to it.
        ed.setTool(imageui::Tool::Select);
        mods(ImGuiMod_Shift, true);
        click(20, 20); click(40, 40);
        mods(ImGuiMod_Shift, false);
        frame();
        CHECK(sel(30, 30) == 255 && sel(240, 190) == 255, "Shift adds a rectangle to the lasso");
        key(ImGuiKey_D, true);

        // A drag inside the selection moves its outline, not the pixels.
        click(50, 50); click(100, 100);
        drag(70, 70, 90, 80, 5);
        CHECK(e.selectionBounds().x0 == 70 && e.selectionBounds().y0 == 60 && e.undoLabel() == "Move selection",
              "dragging the marquee (%d %d, %s)", e.selectionBounds().x0, e.selectionBounds().y0, e.undoLabel().c_str());
        key(ImGuiKey_D, true);

        // Free transform: Ctrl+T, drag inside the box, Enter.
        e.addLayer("Red");
        e.selectRect({100, 100, 140, 140}, img::SelOp::Replace, false);
        e.fillSelection({1, 0, 0, 1});
        e.deselect();
        ed.setTool(imageui::Tool::Move);
        key(ImGuiKey_T, true);
        CHECK(ed.transforming(), "Ctrl+T starts a free transform");
        drag(120, 120, 170, 120, 6);
        key(ImGuiKey_Enter, false);
        CHECK(!ed.transforming() && ep(1, 110, 120)[3] == 0 && ep(1, 175, 120)[3] == 255 && e.undoLabel() == "Free transform",
              "the transform moved the square and Enter kept it (%d %d, %s)", ep(1, 110, 120)[3], ep(1, 175, 120)[3],
              e.undoLabel().c_str());
        // Scaling by a corner handle keeps the opposite corner where it is.
        key(ImGuiKey_T, true);
        drag(190, 140, 230, 180, 6);
        key(ImGuiKey_Enter, false);
        CHECK(ep(1, 152, 102)[3] == 255 && ep(1, 225, 175)[3] == 255 && ep(1, 148, 98)[3] == 0,
              "the corner handle scaled about the opposite corner (%d %d %d)", ep(1, 152, 102)[3], ep(1, 225, 175)[3],
              ep(1, 148, 98)[3]);
        // Esc throws a transform away.
        key(ImGuiKey_T, true);
        drag(190, 140, 300, 250, 6);
        key(ImGuiKey_Escape, false);
        CHECK(!ed.transforming() && ep(1, 225, 175)[3] == 255 && ep(1, 290, 240)[3] == 0, "Esc cancels the transform");

        // Ctrl+J: the selection onto a layer of its own.
        e.selectRect({160, 110, 180, 130}, img::SelOp::Replace, false);
        const int layersBefore = e.layerCount();
        key(ImGuiKey_J, true);
        CHECK(e.layerCount() == layersBefore + 1 && ep(e.active(), 170, 120)[3] == 255 && ep(e.active(), 200, 150)[3] == 0,
              "Ctrl+J copies the selection to a new layer");
        key(ImGuiKey_D, true);

        // Burn darkens the grey; Shift+click paints a straight line from the last stroke.
        e.setActive(0);
        ed.setTool(imageui::Tool::Dodge);
        ed.setTone(1);
        drag(20, 250, 60, 250, 6);
        CHECK(ep(0, 30, 250)[0] < 110, "burn darkens (%d)", ep(0, 30, 250)[0]);
        ed.setTool(imageui::Tool::Brush);
        ed.setColors({0, 0, 1, 1}, {1, 1, 1, 1});
        click(20, 280);
        mods(ImGuiMod_Shift, true);
        click(380, 280);
        mods(ImGuiMod_Shift, false);
        frame();
        CHECK(ep(0, 200, 280)[2] > 200 && ep(0, 200, 280)[0] < 30, "Shift+click: a line from the last stroke");

        // The zoom tool zooms in where it is clicked.
        const float span0 = ed.toScreen(ImVec2(100, 0)).x - ed.toScreen(ImVec2(0, 0)).x;
        ed.setTool(imageui::Tool::Zoom);
        click(200, 150);
        frame();
        const float span1 = ed.toScreen(ImVec2(100, 0)).x - ed.toScreen(ImVec2(0, 0)).x;
        CHECK(span1 > span0 * 1.2f, "the zoom tool zoomed in (%.0f -> %.0f px)", span0, span1);
        ed.setZoom(1.0f);
        frame();

        // Dialogs: a filter cancelled with Esc leaves nothing; Curves applied with Enter is one step.
        const std::string beforeLabel = e.undoLabel();
        ed.startFilter(img::Filter::Blur);
        frame(); frame();
        key(ImGuiKey_Escape, false);
        CHECK(e.undoLabel() == beforeLabel && !e.editing(), "Esc in the filter dialog leaves no step (%s)", e.undoLabel().c_str());
        ed.startCurves();
        frame(); frame();
        key(ImGuiKey_Enter, false);
        CHECK(e.undoLabel() == "Curves" && !e.editing(), "Enter applies Curves (%s)", e.undoLabel().c_str());

        // Select > Color range: a click on the picture picks, Enter keeps it as one step.
        ed.setTool(imageui::Tool::Brush);
        e.setActive(0);
        ed.startColorRange();
        frame(); frame();
        CHECK(ed.colorRangeOpen() && e.editing(), "Color range opens");
        click(200, 280);   // the blue line
        CHECK(sel(200, 280) == 255 && sel(30, 30) == 0, "a click on the picture picks its colour (%d %d)", sel(200, 280), sel(30, 30));
        CHECK(e.layer(0).visible && ep(0, 200, 280)[2] > 200, "picking paints nothing");
        key(ImGuiKey_Enter, false);
        CHECK(!ed.colorRangeOpen() && !e.editing() && e.undoLabel() == "Color range" && sel(200, 280) == 255,
              "Enter keeps the selection (%s)", e.undoLabel().c_str());
        key(ImGuiKey_D, true);
        ed.startColorRange();
        frame(); frame();
        click(30, 30);
        key(ImGuiKey_Escape, false);
        CHECK(!ed.colorRangeOpen() && !e.hasSelection() && e.undoLabel() == "Deselect",
              "Esc leaves no selection and no step (%s)", e.undoLabel().c_str());

        // Layer style: shown live, Enter keeps it as one step, Esc takes it back.
        e.setActive(1);
        ed.startStyle(int(img::Fx::DropShadow));
        frame(); frame();
        CHECK(e.layer(1).style.fx[int(img::Fx::DropShadow)].on, "the dialog shows the shadow live");
        key(ImGuiKey_Enter, false);
        CHECK(e.undoLabel() == "Layer style" && e.layer(1).style.fx[int(img::Fx::DropShadow)].on,
              "Enter keeps the style (%s)", e.undoLabel().c_str());
        ed.startStyle(int(img::Fx::Stroke));
        frame(); frame();
        CHECK(e.layer(1).style.fx[int(img::Fx::Stroke)].on, "the stroke shows while the dialog is open");
        key(ImGuiKey_Escape, false);
        CHECK(!e.layer(1).style.fx[int(img::Fx::Stroke)].on && e.layer(1).style.fx[int(img::Fx::DropShadow)].on &&
              e.undoLabel() == "Layer style", "Esc takes the stroke back, the shadow stays");

        // A layer mask: the brush paints on it, the layer's own pixels stay.
        e.addMask(img::MaskInit::RevealAll);
        ed.setColors({0, 0, 0, 1}, {1, 1, 1, 1});
        const std::uint8_t alphaBefore = ep(1, 190, 150)[3];
        drag(160, 150, 225, 150, 8);
        const std::uint8_t mv = (*e.layer(1).mask)[(std::size_t(150) * e.width() + 190) * 4];
        CHECK(ep(1, 190, 150)[3] == alphaBefore && mv < 40 && e.undoLabel() == "Brush",
              "the brush painted black on the mask, not on the layer (mask %d)", mv);
        key(ImGuiKey_Backslash, true);
        CHECK(!e.editingMask(), "Ctrl+\\ goes back to the layer");

        // Crop: two clicks, Enter.
        ed.setTool(imageui::Tool::Crop);
        click(20, 30); click(220, 130);
        CHECK(ed.cropping(), "two clicks make a crop box");
        key(ImGuiKey_Enter, false);
        CHECK(e.width() == 200 && e.height() == 100 && !ed.cropping(), "Enter crops (%dx%d)", e.width(), e.height());

        frame();
        std::printf("[imagepanelcheck] interact: %s\n", g_fail ? "FAILED" : "all passed");
    } else if (normalMap) {
        if (project.empty()) { std::printf("[imagepanelcheck] --normal-map needs --project\n"); return 2; }
        const std::filesystem::path dir = std::filesystem::path(project).parent_path();
        ImGuiIO& io = ImGui::GetIO();
        auto press = [&](ImGuiKey k) { io.AddKeyEvent(k, true); frame(); io.AddKeyEvent(k, false); frame(); };
        // A dark picture with a bright square in it: a raised block.
        ed.newImage(256, 256, {0.1f, 0.1f, 0.1f, 1});
        img::Document& src = *ed.document();
        src.selectRect({96, 96, 160, 160}, img::SelOp::Replace, false);
        src.fillSelection({0.9f, 0.9f, 0.9f, 1});
        src.deselect();
        std::error_code ec;
        std::filesystem::create_directories(dir / "art" / "walls", ec);
        std::string err;
        CHECK(src.save((dir / "art" / "walls" / "block.png").string(), err), "the texture saved (%s)", err.c_str());
        for (int i = 0; i < 3; ++i) frame();
        ed.startNormalMap();
        for (int i = 0; i < 3; ++i) frame();
        img::Document* nm = ed.document();
        CHECK(ed.normalMapOpen() && ed.pictureCount() == 2 && nm != &src && nm->title() == "block_normal",
              "a new tab block_normal with the dialog open (%d tabs, %s)", ed.pictureCount(), nm->title().c_str());
        auto at = [&](int x, int y) { return &nm->pixels(0)[(std::size_t(y) * 256 + x) * 4]; };
        CHECK(std::abs(at(30, 30)[0] - 128) < 3 && std::abs(at(30, 30)[1] - 128) < 3 && at(30, 30)[2] > 250,
              "flat is (128, 128, 255): %d %d %d", at(30, 30)[0], at(30, 30)[1], at(30, 30)[2]);
        CHECK(at(96, 128)[0] < 100 && at(159, 128)[0] > 156, "the left side faces left, the right side right (%d, %d)",
              at(96, 128)[0], at(159, 128)[0]);
        CHECK(at(128, 96)[1] > 156 && at(128, 159)[1] < 100, "the top faces up, green up (%d, %d)", at(128, 96)[1], at(128, 159)[1]);
        CHECK(src.layerCount() == 1 && src.pixels(0)[(std::size_t(128) * 256 + 128) * 4] > 200, "the texture stayed as it was");
        press(ImGuiKey_Escape);
        CHECK(!ed.normalMapOpen() && ed.pictureCount() == 1 && ed.document() == &src, "Esc took the tab away (%d)", ed.pictureCount());
        ed.startNormalMap();
        for (int i = 0; i < 3; ++i) frame();
        press(ImGuiKey_Enter);
        CHECK(!ed.normalMapOpen() && ed.pictureCount() == 2 && ed.document()->modified(), "Enter kept it, unsaved (%s)", status.c_str());
        ed.startSaveAsset();
        for (int i = 0; i < 3; ++i) frame();
        const std::string want = (dir / "art" / "walls" / "block_normal.png").generic_string();
        CHECK(ed.saveAssetOpen() && ed.saveAssetPath() == want, "Save in assets offers %s (%s)", want.c_str(), ed.saveAssetPath().c_str());
        press(ImGuiKey_Escape);
        // DirectX turns green over; dark-is-high turns the block into a pit.
        img::NormalMapOptions o;
        img::Pixels dx, inv;
        o.directX = true;
        img::normalMap(src.pixels(0), 256, 256, o, dx);
        o.directX = false;
        o.invert = true;
        img::normalMap(src.pixels(0), 256, 256, o, inv);
        CHECK(dx[(std::size_t(96) * 256 + 128) * 4 + 1] < 100, "DirectX: the top's green goes down");
        CHECK(inv[(std::size_t(128) * 256 + 96) * 4] > 156, "inverted: the left side of a pit faces right");
        // Left open, from the texture, for the picture.
        ed.selectPicture(0);
        frame();
        ed.startNormalMap();
        for (int i = 0; i < 4; ++i) frame();
        std::printf("[imagepanelcheck] normal-map: %s\n", g_fail ? "FAILED" : "all passed");
    } else if (saveAsset) {
        const std::filesystem::path dir = std::filesystem::path(project).parent_path();
        if (project.empty()) { std::printf("[imagepanelcheck] --save-asset needs --project\n"); return 2; }
        const std::filesystem::path file = dir / "textures" / "sky_test.png";
        std::error_code ec;
        std::filesystem::remove(file, ec);
        ImGuiIO& io = ImGui::GetIO();
        auto enter = [&] { io.AddKeyEvent(ImGuiKey_Enter, true); frame(); io.AddKeyEvent(ImGuiKey_Enter, false); frame(); };
        auto saveAs = [&](const char* name) {
            ed.startSaveAsset();
            for (int i = 0; i < 3; ++i) frame();
            CHECK(ed.saveAssetOpen(), "the dialog opened (%s)", status.c_str());
            // Typed into the name field, which has the keyboard: select all, then the name.
            io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(ImGuiKey_A, true);
            frame();
            io.AddKeyEvent(ImGuiKey_A, false);
            io.AddKeyEvent(ImGuiMod_Ctrl, false);
            frame();
            io.AddInputCharactersUTF8(name);
            frame();
            frame();
        };
        // A new picture, named as a generated one is: offered under that name, in textures/.
        ed.newImage(320, 200, {0.2f, 0.5f, 0.8f, 1});
        ed.document()->setName("sky_test");
        saveAs("sky_test");
        enter();
        img::Document& d = *ed.document();
        CHECK(!ed.saveAssetOpen() && std::filesystem::exists(file), "saved to %s (%s)", file.string().c_str(), status.c_str());
        CHECK(std::filesystem::path(d.path()) == file && !d.modified() && d.title() == "sky_test.png",
              "the tab is that file now (%s, %s)", d.path().c_str(), d.title().c_str());
        // Another one of the same name replaces it.
        ed.newImage(320, 200, {0.9f, 0.1f, 0.1f, 1});
        saveAs("sky_test");
        enter();
        img::Document back;
        std::string err;
        CHECK(back.load(file.string(), err) && back.pixels(0)[0] > 200 && back.pixels(0)[2] < 40,
              "the red one replaced it (%s)", status.c_str());
        // And once more, left open on the warning for the picture.
        ed.newImage(320, 200, {0.3f, 0.7f, 0.3f, 1});
        ed.document()->addLayer("Leaf");
        saveAs("sky_test");
        for (int i = 0; i < 3; ++i) frame();
        std::printf("[imagepanelcheck] save-asset: %s\n", g_fail ? "FAILED" : "all passed");
    } else if (!comfyUrl.empty()) {
        // A picture from a prompt, through the window as its button makes it.
        comfy::Settings& cs = comfy::client().settings;
        cs.workflow.clear();
        cs.width = 768;
        cs.height = 512;
        cs.count = 1;
        cs.steps = 0;
        cs.randomSeed = false;
        cs.seed = 4711;
        for (int i = 0; i < 3; ++i) frame();
        auto paint = [&](const char* what) {
            ed.generate(what);
            const auto t0 = std::chrono::steady_clock::now();
            while (ed.generating() &&
                   std::chrono::steady_clock::now() - t0 < std::chrono::minutes(10)) {
                frame();
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
            frame();
            std::printf("[imagepanelcheck] comfy: %s\n", ed.promptMessage().c_str());
        };
        paint("a weathered brass compass on an old sea chart, warm window light, photograph");
        if (ed.pictureCount() == 0 && ed.promptMessage().find("No ComfyUI answers") != std::string::npos) {
            std::printf("[imagepanelcheck] no ComfyUI at %s\n", comfyUrl.c_str());
            return 3;
        }
        CHECK(ed.pictureCount() == 1, "the picture opened in a tab (%d tabs: %s)", ed.pictureCount(), ed.promptMessage().c_str());
        if (img::Document* d = ed.document()) {
            CHECK(d->width() == 768 && d->height() == 512, "768 x 512 as asked (%d x %d)", d->width(), d->height());
            CHECK(d->modified() && d->title() == "a_weathered_brass_compass_on_an",
                  "unsaved, named after the prompt (%s)", d->title().c_str());
            // Not one flat colour: something was painted.
            const img::Pixels& px = d->pixels(0);
            int lo = 255, hi = 0;
            for (std::size_t i = 0; i < px.size(); i += 4 * 97) { lo = std::min<int>(lo, px[i + 1]); hi = std::max<int>(hi, px[i + 1]); }
            CHECK(hi - lo > 60, "the picture has contrast (%d..%d)", lo, hi);
            // Again, as a layer of the open picture -- same seed, other words.
            ed.setPromptAsLayer(true);
            paint("the same compass, at night, lit by a candle");
            CHECK(ed.pictureCount() == 1 && d->layerCount() == 2 && d->layer(1).name == "the_same_compass_at_night_lit",
                  "the second landed on a layer (%d tabs, %d layers)", ed.pictureCount(), d->layerCount());
        }
        // A model the server lacks: said in a sentence, nothing opens.
        comfy::Workflow wf;
        std::string werr;
        nlohmann::json g = comfy::builtinWorkflow().graph;
        g["1"]["inputs"]["unet_name"] = "no_such_model.safetensors";
        CHECK(comfy::parseWorkflow(g, wf, werr), "an exported graph parses (%s)", werr.c_str());
        CHECK(wf.prompt.size() == 1 && wf.prompt[0].node == "5" && wf.width.size() == 1 && wf.width[0].node == "7" &&
              wf.seed.size() == 1 && wf.steps.size() == 1 && wf.count.size() == 1 && wf.negative.empty(),
              "its slots are found by what the nodes are");
        auto wait = [](const comfy::JobPtr& j) {
            while (!j->finished()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::printf("[imagepanelcheck] comfy: %s\n", (j->ok ? j->info : j->error).c_str());
        };
        auto job = comfy::client().generate(wf, comfy::Request{"x", "", 512, 512, 0, 1, 1});
        wait(job);
        CHECK(!job->ok && job->error.find("no_such_model.safetensors") != std::string::npos,
              "a missing model is named (%s)", job->error.c_str());
        // Test: what the server is, and that it lacks the file.
        job = comfy::client().probe(comfy::builtinWorkflow());
        wait(job);
        CHECK(job->ok && job->info.rfind("ComfyUI ", 0) == 0 && job->info.find("Has every model file") != std::string::npos,
              "the server has the built-in workflow's files");
        job = comfy::client().probe(wf);
        wait(job);
        CHECK(job->ok && job->info.find("no_such_model.safetensors") != std::string::npos, "and lacks the made-up one");
        // Cancel while it paints: the job ends, the server's queue empties.
        job = comfy::client().generate(comfy::builtinWorkflow(), comfy::Request{"a lighthouse in a storm", "", 1536, 1536, 30, 1, 7});
        for (int i = 0; i < 600; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard<std::mutex> g(job->lock);
            if (job->step >= 2) break;
        }
        {
            std::lock_guard<std::mutex> g(job->lock);
            CHECK(job->steps == 30 && job->previewVersion > 0 && job->preview.w > 0,
                  "step %d of %d, with the server's preview (%u, %dx%d)", job->step, job->steps, job->previewVersion,
                  job->preview.w, job->preview.h);
        }
        comfy::client().cancel(job);
        wait(job);
        CHECK(!job->ok && job->error == "cancelled", "cancelled (%s)", job->error.c_str());
        std::this_thread::sleep_for(std::chrono::seconds(2));
        job = comfy::client().probe(comfy::Workflow{});   // the server still answers ...
        wait(job);
        CHECK(job->ok, "the server is fine after the cancel");
        std::printf("[imagepanelcheck] comfy: %s\n", g_fail ? "FAILED" : "all passed");
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
    gpu.setViewport(0, 0, fw, fh);
    gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
    gpu.clear(fitzel::rhi::ClearColor);
    gui.beginFrame();
    bool show = true;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ed.panel(show);
    gui.endFrame();
    std::vector<unsigned char> pixels(static_cast<std::size_t>(fw) * fh * 4);
    gpu.readPixels({}, 0, fitzel::rhi::Rect{0, 0, fw, fh}, fitzel::rhi::Format::RGBA8, pixels.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(out.c_str(), fw, fh, 4, pixels.data(), fw * 4) != 0;
    std::printf(wrote ? "[imagepanelcheck] wrote %s (%dx%d)\n" : "[imagepanelcheck] could not write %s\n",
                out.c_str(), fw, fh);
    if (!status.empty()) std::printf("[imagepanelcheck] status: %s\n", status.c_str());
    return wrote && !g_fail ? 0 : 1;
}
