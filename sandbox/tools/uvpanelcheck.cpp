// uvpanelcheck -- the UV editor window, driven the way a hand drives it, and a
// picture of it.
//
// imagepanelcheck's trick applied to the UV editor: uvui::drawPanel drawn
// through the editor's own Gui and theme, on a mesh held here, with the host's
// callbacks (one undo step per edit, the live G/R/S, undo) as plain functions.
// Mouse and key events go into ImGui's queue frame by frame, and the mesh is
// checked afterwards: a click selects welded corners together, G follows the
// pointer to the next click, Esc puts it back, a typed number is exact, R and
// S turn and scale about the middle, B is click and click, Ctrl+Z undoes.
// Run from the directory the shaders were copied to (build/linux/bin).
//
// --extrude: no interaction; a box smart-projected, its top pulled out twice,
// for a picture of the walls folding out from the cap.
//
//   uvpanelcheck [out.png] [--size WxH] [--extrude]

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/Component.hpp"
#include "../src/UiStyle.hpp"
#include "../src/UvPanel.hpp"

static int g_fail = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) { ++g_fail; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
                       std::printf(__VA_ARGS__); std::printf("\n"); }      \
        else { std::printf("  ok   "); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

// A UV test grid: coloured squares, eight to the tile, so stretching shows.
static std::vector<unsigned char> testGrid(int n) {
    std::vector<unsigned char> px(std::size_t(n) * n * 4);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int cx = x * 8 / n, cy = y * 8 / n;
            const bool dark = ((cx + cy) & 1) != 0;
            unsigned char* p = &px[(std::size_t(y) * n + x) * 4];
            p[0] = (unsigned char)(dark ? 40 + cx * 18 : 120 + cx * 16);
            p[1] = (unsigned char)(dark ? 40 + cy * 18 : 120 + cy * 16);
            p[2] = (unsigned char)(dark ? 90 : 200);
            p[3] = 255;
            if (x % (n / 8) == 0 || y % (n / 8) == 0) p[0] = p[1] = p[2] = 20;
        }
    return px;
}

int main(int argc, char** argv) {
    std::string out = "uvpanel.png";
    int w = 1500, h = 1000;
    bool extrudeShot = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--size" && i + 1 < argc) std::sscanf(argv[++i], "%dx%d", &w, &h);
        else if (a == "--extrude") extrudeShot = true;
        else if (!a.empty() && a[0] != '-') out = a;
    }
    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "uvpanelcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    fitzel::rhi::Device& gpu = window.device();
    ui::setBoldFont(gui.boldFont());
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplGlfw_CursorEnterCallback(window.nativeHandle(), 1);

    // The mesh: a cylinder cut open down one side and round both rims.
    MeshComponent mc;
    mc.mesh = EditMesh::cylinder(glm::vec3(0.5f, 1.0f, 0.5f), 12);
    for (int f = 0; f < static_cast<int>(mc.mesh.faces.size()); ++f) {
        if (std::abs(mc.mesh.faceNormal(f).y) < 0.5f) continue;
        const std::vector<int>& fv = mc.mesh.faces[static_cast<std::size_t>(f)];
        for (std::size_t k = 0; k < fv.size(); ++k) mc.mesh.setSeam(fv[k], fv[(k + 1) % fv.size()], true);
    }
    {
        const std::vector<int>& fv = mc.mesh.faces[0];
        for (std::size_t k = 0; k < fv.size(); ++k) {
            const glm::vec3 d = mc.mesh.verts[fv[(k + 1) % fv.size()]] - mc.mesh.verts[fv[k]];
            if (std::abs(d.y) > 0.5f) { mc.mesh.setSeam(fv[k], fv[(k + 1) % fv.size()], true); break; }
        }
    }
    std::vector<int> all(mc.mesh.faces.size());
    for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<int>(i);
    uvops::unwrap(mc.mesh, all, 0.02f);
    std::vector<int> shotFaces;
    if (extrudeShot) {
        mc.mesh = EditMesh::box(glm::vec3(0.6f, 0.4f, 0.5f));
        std::vector<int> six{0, 1, 2, 3, 4, 5};
        uvops::smartProject(mc.mesh, six, 66.0f, 0.04f);
        int top = 0;
        for (int f = 0; f < 6; ++f) if (mc.mesh.faceNormal(f).y > 0.9f) top = f;
        // Shrink the islands so the walls folding out have room in the tile.
        uvops::Selection sAll;
        uvops::selectAll(uvops::view(mc.mesh, six), sAll);
        uvops::transform(mc.mesh, sAll, glm::mat2(0.6f), glm::vec2(0.5f), glm::vec2(0.0f));
        editmesh::extrude(mc.mesh, top, 0.3f);
        editmesh::extrude(mc.mesh, top, 0.2f);
        all.resize(mc.mesh.faces.size());
        for (std::size_t i = 0; i < all.size(); ++i) all[i] = static_cast<int>(i);
        // The top and the eight walls grown from it, as if picked in the viewport.
        shotFaces.push_back(top);
        for (int f = 6; f < static_cast<int>(mc.mesh.faces.size()); ++f) shotFaces.push_back(f);
    }

    std::vector<MaterialDef> mats(1);
    mats[0].assetId = fitzel::AssetId::generate();
    const std::vector<unsigned char> grid = testGrid(256);
    mats[0].tex = std::make_shared<fitzel::Texture>(fitzel::Texture::fromPixels(grid.data(), 256, 256, 4));

    // The host: undo steps as snapshots, the live edit as base + op.
    std::vector<EditMesh> undo, redo;
    EditMesh liveBase;
    int edits = 0;
    std::string lastLabel;
    bool show = true;
    int faceSel = 0;
    uvui::PanelState st{show, &mc, faceSel, 7, shotFaces, mats};
    st.objectMaterial = mats[0].assetId;
    st.haveSelection = true;
    st.edit = [&](const std::function<int(MeshComponent&)>& op, const char* label) {
        undo.push_back(mc.mesh);
        redo.clear();
        faceSel = op(mc);
        mc.touch();
        ++edits;
        lastLabel = label;
    };
    st.live = [&](uvui::Live ph, const std::function<void(EditMesh&)>& op, const char* label) {
        if (ph == uvui::Live::Begin) liveBase = mc.mesh;
        else if (ph == uvui::Live::Set) { mc.mesh = liveBase; if (op) op(mc.mesh); }
        else if (ph == uvui::Live::Cancel) mc.mesh = liveBase;
        else { undo.push_back(liveBase); redo.clear(); ++edits; lastLabel = label; }
    };
    st.meshNow = [&] { return &mc; };
    st.undo = [&](bool r) {
        std::vector<EditMesh>& from = r ? redo : undo;
        std::vector<EditMesh>& to = r ? undo : redo;
        if (from.empty()) return;
        to.push_back(mc.mesh);
        mc.mesh = from.back();
        from.pop_back();
    };

    int frameNo = 0;
    auto frame = [&] {
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        gpu.setViewport(0, 0, fw, fh);
        gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
        gpu.clear(fitzel::rhi::ClearColor);
        gui.beginFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(float(w), float(h)), ImGuiCond_Always);
        st.mesh = &mc;
        uvui::drawPanel(st);
        gui.endFrame();
        window.swapBuffers();
        ++frameNo;
    };
    ImGuiIO& io = ImGui::GetIO();
    auto at = [&](ImVec2 p) { io.AddMousePosEvent(p.x, p.y); frame(); };
    auto atUv = [&](glm::vec2 q) { at(uvui::toScreen(q)); };
    auto click = [&](int b) { io.AddMouseButtonEvent(b, true); frame(); io.AddMouseButtonEvent(b, false); frame(); };
    auto key = [&](ImGuiKey k) { io.AddKeyEvent(k, true); frame(); io.AddKeyEvent(k, false); frame(); };
    auto chord = [&](ImGuiKey mod, ImGuiKey k) {
        io.AddKeyEvent(mod, true); frame();
        key(k);
        io.AddKeyEvent(mod, false); frame();
    };
    auto uvOf = [&](int f, int k) { return editmesh::faceUvs(mc.mesh, f)[static_cast<std::size_t>(k)]; };

    for (int i = 0; i < 4; ++i) frame();
    if (extrudeShot) goto picture;
    // Focus the window: a click on empty canvas, right of the tile.
    atUv({1.08f, 0.5f});
    click(0);

    std::printf("[uvpanelcheck] selecting\n");
    {
        // The corner welded to the most others (inside the side, off the seams).
        uvops::View v = uvops::view(mc.mesh, all);
        int f = 0, k = 0;
        std::size_t welded = 0;
        for (int ff : all)
            for (int kk = 0; kk < static_cast<int>(mc.mesh.faces[static_cast<std::size_t>(ff)].size()); ++kk) {
                const std::size_t n = uvops::coincident(v, {ff, kk}).size();
                if (n > welded) { welded = n; f = ff; k = kk; }
            }
        const glm::vec2 p = uvOf(f, k);
        atUv(p);
        click(0);
        CHECK(uvui::selection().size() == welded && welded >= 2, "a click on a corner takes the %zu welded loops there", welded);
        key(ImGuiKey_A);
        std::size_t loops = 0;
        for (const auto& fv : mc.mesh.faces) loops += fv.size();
        CHECK(uvui::selection().size() == loops, "A selects every loop (%zu)", uvui::selection().size());
        chord(ImGuiMod_Alt, ImGuiKey_A);
        CHECK(uvui::selection().empty(), "Alt+A: nothing");
        // B: click one corner, click the other.
        glm::vec2 lo(1e30f), hi(-1e30f);
        for (const glm::vec2& q : editmesh::faceUvs(mc.mesh, 0)) { lo = glm::min(lo, q); hi = glm::max(hi, q); }
        atUv(glm::vec2(1.08f, 0.5f));
        key(ImGuiKey_B);
        atUv(lo - glm::vec2(0.003f));
        click(0);
        atUv(hi + glm::vec2(0.003f));
        click(0);
        CHECK(uvui::selection().size() >= 4 && uvops::faceSelected(uvui::selection(), mc.mesh, 0),
              "B, click, click: the box takes face 0 (%zu loops)", uvui::selection().size());
    }

    std::printf("[uvpanelcheck] G, R, S\n");
    {
        // Face 0 and what is welded to it, moved with G: follows the pointer, a click places it.
        const glm::vec2 before = uvOf(0, 0);
        const int e0 = edits;
        atUv({1.08f, 0.5f});
        key(ImGuiKey_G);
        CHECK(uvui::transforming(), "G starts a move");
        const ImVec2 s0 = uvui::toScreen({1.08f, 0.5f});
        at(ImVec2(s0.x + 60.0f, s0.y));
        at(ImVec2(s0.x + 120.0f, s0.y));
        const glm::vec2 live = uvOf(0, 0);
        CHECK(live.x > before.x + 1e-3f && std::abs(live.y - before.y) < 1e-4f, "while moving, the mesh follows (live)");
        click(0);
        frame();
        const glm::vec2 placed = uvOf(0, 0);
        CHECK(!uvui::transforming() && edits == e0 + 1 && std::abs(placed.x - live.x) < 1e-4f,
              "a click places it: one undo step (%s)", lastLabel.c_str());
        // Esc puts a move back.
        key(ImGuiKey_G);
        at(ImVec2(s0.x + 200.0f, s0.y + 80.0f));
        key(ImGuiKey_Escape);
        frame();
        CHECK(glm::length(uvOf(0, 0) - placed) < 1e-6f && edits == e0 + 1, "Esc puts it back, no undo step");
        // A typed number along V: exact.
        key(ImGuiKey_G);
        key(ImGuiKey_Y);
        key(ImGuiKey_0); key(ImGuiKey_Period); key(ImGuiKey_2); key(ImGuiKey_5);
        key(ImGuiKey_Enter);
        frame();
        CHECK(std::abs(uvOf(0, 0).y - (placed.y + 0.25f)) < 1e-5f && std::abs(uvOf(0, 0).x - placed.x) < 1e-5f,
              "G Y 0.25 Enter: exactly a quarter tile up");
        // S 2 Enter: twice the size about the middle of the selection.
        glm::vec2 l0, h0;
        uvops::bounds(uvops::view(mc.mesh, all), uvui::selection(), l0, h0);
        key(ImGuiKey_S);
        key(ImGuiKey_2);
        key(ImGuiKey_Enter);
        frame();
        glm::vec2 l1, h1;
        uvops::bounds(uvops::view(mc.mesh, all), uvui::selection(), l1, h1);
        CHECK(std::abs((h1.x - l1.x) - 2.0f * (h0.x - l0.x)) < 1e-4f && glm::length((l1 + h1) - (l0 + h0)) < 1e-4f,
              "S 2 Enter: twice the size, about its middle");
        // R 90 Enter: width and height swap.
        key(ImGuiKey_R);
        key(ImGuiKey_9); key(ImGuiKey_0);
        key(ImGuiKey_Enter);
        frame();
        glm::vec2 l2, h2;
        uvops::bounds(uvops::view(mc.mesh, all), uvui::selection(), l2, h2);
        CHECK(std::abs((h2.x - l2.x) - (h1.y - l1.y)) < 1e-4f && std::abs((h2.y - l2.y) - (h1.x - l1.x)) < 1e-4f,
              "R 90 Enter: a quarter turn");
        // Ctrl+Z three times: back to where the move left it.
        chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        chord(ImGuiMod_Ctrl, ImGuiKey_Z);
        frame();
        CHECK(glm::length(uvOf(0, 0) - placed) < 1e-5f, "Ctrl+Z three times: back to the placed move");
    }

    std::printf("[uvpanelcheck] modes\n");
    {
        key(ImGuiKey_3);
        CHECK(uvui::selectMode() == uvops::SelectMode::Face, "3: face mode");
        key(ImGuiKey_4);
        atUv(uvOf(0, 0) + 0.25f * (uvOf(0, 2) - uvOf(0, 0)));
        click(0);
        const std::vector<int> fs = uvops::facesOf(uvui::selection());
        CHECK(fs.size() > 3, "4: an island click takes the whole side (%zu faces)", fs.size());
        key(ImGuiKey_1);
    }

    // The picture: a selection on the unwrapped cylinder.
picture:
    for (int i = 0; i < 4; ++i) frame();
    int fw = 0, fh = 0;
    window.pollEvents();
    window.framebufferSize(fw, fh);
    gpu.setViewport(0, 0, fw, fh);
    gpu.clear(fitzel::rhi::ClearColor);
    gui.beginFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(float(w), float(h)), ImGuiCond_Always);
    uvui::drawPanel(st);
    gui.endFrame();
    std::vector<unsigned char> pixels(static_cast<std::size_t>(fw) * fh * 4);
    gpu.readPixels({}, 0, fitzel::rhi::Rect{0, 0, fw, fh}, fitzel::rhi::Format::RGBA8, pixels.data());
    stbi_flip_vertically_on_write(1);
    const bool wrote = stbi_write_png(out.c_str(), fw, fh, 4, pixels.data(), fw * 4) != 0;
    std::printf(wrote ? "[uvpanelcheck] wrote %s (%dx%d)\n" : "[uvpanelcheck] could not write %s\n", out.c_str(), fw, fh);
    std::printf("[uvpanelcheck] %s\n", g_fail ? "FAILED" : "all passed");
    return wrote && !g_fail ? 0 : 1;
}
