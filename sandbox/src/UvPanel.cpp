#include "UvPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include <imgui.h>

#include "Component.hpp"
#include "UiStyle.hpp"

namespace uvui {

namespace {

// The placement being edited, and what it belongs to. The buffer exists because
// nothing here commits while a control is still under the hand: a slider dragged
// across its range would otherwise push one undo step per frame, and undoing a
// texture size would then take forty presses. The preview draws from the buffer,
// so the picture follows the slider; the mesh gets it -- once -- on release.
EditMesh::FaceUV g_uv;
int              g_face = -1;
std::uint64_t    g_rev  = 0;

// The drag inside the preview. Not an ImGui item, so its "is it still held"
// has to be tracked here.
bool g_dragging = false;

// The visible corner of the UV plane, frozen while anything is being dragged.
// Refitting it under a moving island would move the island back under the
// cursor, which is a picture that never responds.
glm::vec2 g_viewLo{0.0f}, g_viewHi{1.0f};

// Both size axes move together unless somebody says otherwise. A texture is
// nearly always square on the wall, and two sliders to say so is one too many.
bool g_linkSize = true;

// Full-width and tall enough to be aimed at rather than hit precisely.
bool bigButton(const char* label) {
    return ImGui::Button(label, ImVec2(-1.0f, 30.0f));
}

const MaterialDef* findMaterial(const PanelState& s, const fitzel::AssetId& id) {
    if (!id.valid()) return nullptr;
    for (const MaterialDef& md : s.materials)
        if (md.assetId == id) return &md;
    return nullptr;
}

// The texture the selected face is actually seen through: its own material's if
// it wears one, otherwise the object's.
const MaterialDef* faceMaterial(const PanelState& s, const EditMesh& m, int f) {
    const fitzel::AssetId own = m.faceMaterial(f);
    if (const MaterialDef* md = findMaterial(s, own)) return md;
    return findMaterial(s, s.objectMaterial);
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (!ImGui::Begin("UV", &s.show)) { ImGui::End(); return; }

    if (!s.mesh) {
        ui::sectionText("No editable mesh");
        if (!s.haveSelection) {
            ui::hint("Select an object first. This panel places the texture on\n"
                     "the faces of a mesh fitzel made -- a box you made editable.\n"
                     "An imported model keeps the UVs its author gave it.");
        } else if (s.canConvert) {
            ui::hint("This box can become an editable mesh. Then every face has\n"
                     "its own texture placement, and this panel sets it.");
            ImGui::Spacing();
            if (bigButton("Make editable") && s.convert) s.convert();
        } else {
            ui::hint("Only meshes fitzel made are placed from here. An imported\n"
                     "model arrives unwrapped and is left as its author made it.");
        }
        ImGui::End();
        return;
    }

    EditMesh& mesh = s.mesh->mesh;
    const int f    = s.faceSel;
    if (f < 0 || !mesh.validFace(f)) {
        ImGui::Text("%d faces", s.faceCount);
        ui::hint("Click a face in the viewport to place its texture.");
        ImGui::End();
        return;
    }

    // Reseed from the mesh whenever the panel is looking at something else than
    // it was -- another face, or the same face after an edit or an undo. Never
    // while a control is held, which is exactly when the buffer is ahead of the
    // mesh on purpose.
    const bool held = g_dragging || ImGui::IsAnyItemActive();
    if (!held && (g_face != f || g_rev != s.mesh->revision)) {
        g_uv   = mesh.faceUv(f);
        g_face = f;
        g_rev  = s.mesh->revision;
    }

    // One undo step with whatever the buffer holds now.
    auto commit = [&](const char* label) {
        if (!s.edit) return;
        const EditMesh::FaceUV u = g_uv;
        const int              k = f;
        s.edit([k, u](MeshComponent& mc) {
            mc.mesh.setFaceUv(k, u);
            return k;
        }, label);
    };

    const MaterialDef* md  = faceMaterial(s, mesh, f);
    const fitzel::Texture* tex = (md && md->tex) ? md->tex.get() : nullptr;

    // --- The preview ---------------------------------------------------------
    // The face's outline over the texture it is cut from. Drawn from the same
    // faceUvs() the renderer uploads, so what is framed here is what ends up on
    // the wall.
    const std::vector<glm::vec2> uv = editmesh::faceUvs(mesh, f, g_uv);

    const float w = std::max(120.0f, ImGui::GetContentRegionAvail().x);
    const float h = std::min(w, 300.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + w, p0.y + h);
    ImGui::InvisibleButton("uvview", ImVec2(w, h));
    const bool viewHeld = ImGui::IsItemActive();

    if (!held) {
        // Frame the tile the texture repeats over TOGETHER with the face, so the
        // two are always both on screen -- there is nothing to pan or zoom, and
        // therefore nothing to get lost in.
        glm::vec2 lo(0.0f), hi(1.0f);
        for (const glm::vec2& q : uv) { lo = glm::min(lo, q); hi = glm::max(hi, q); }
        const glm::vec2 c   = 0.5f * (lo + hi);
        // One scale for both axes: a UV square has to look square, or a rotation
        // reads as a shear.
        const float     ext = std::max({hi.x - lo.x, hi.y - lo.y, 0.2f}) * 0.6f + 0.08f;
        g_viewLo = c - glm::vec2(ext);
        g_viewHi = c + glm::vec2(ext);
    }
    const float span = std::max(1e-4f, g_viewHi.x - g_viewLo.x);
    const float ppu  = std::min(w, h) / span;             // pixels per UV unit
    const ImVec2 org(p0.x + 0.5f * w - (g_viewLo.x + g_viewHi.x) * 0.5f * ppu,
                     p0.y + 0.5f * h + (g_viewLo.y + g_viewHi.y) * 0.5f * ppu);
    // v runs UP the screen, the way it runs up the wall.
    auto toScreen = [&](const glm::vec2& q) {
        return ImVec2(org.x + q.x * ppu, org.y - q.y * ppu);
    };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 26, 255));

    // The texture, one image per tile. Tiled by drawing it repeatedly rather
    // than by running the UVs past 1: a texture asset may or may not be set to
    // repeat, and a preview that depends on which would be right on some
    // materials and blank on others.
    const int u0 = static_cast<int>(std::floor(g_viewLo.x));
    const int u1 = static_cast<int>(std::ceil(g_viewHi.x));
    const int v0 = static_cast<int>(std::floor(g_viewLo.y));
    const int v1 = static_cast<int>(std::ceil(g_viewHi.y));
    const bool sane = (u1 - u0) <= 24 && (v1 - v0) <= 24;
    for (int iv = v0; sane && iv < v1; ++iv) {
        for (int iu = u0; iu < u1; ++iu) {
            const ImVec2 a = toScreen(glm::vec2(iu, iv + 1));      // top-left
            const ImVec2 b = toScreen(glm::vec2(iu + 1, iv));      // bottom-right
            if (tex) {
                // uv0 goes with `a`, the tile's TOP corner -- and the top of a
                // tile is v = 1. Texture assets are loaded bottom-up (the GL
                // convention, TextureImport::flipVertically), so v = 1 is the
                // top of the picture: pass (0,1)..(1,0) and the preview stands
                // the same way up as the wall does.
                dl->AddImage(static_cast<ImTextureID>(
                                 static_cast<std::intptr_t>(tex->id())),
                             a, b, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f),
                             IM_COL32(255, 255, 255, 210));
            } else {
                const bool dark = ((iu + iv) & 1) != 0;
                dl->AddRectFilled(a, b, dark ? IM_COL32(44, 47, 56, 255)
                                             : IM_COL32(56, 60, 70, 255));
            }
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 28));
        }
    }
    // The 0..1 tile, drawn brightest: it is the one square the numbers are in.
    dl->AddRect(toScreen(glm::vec2(0, 1)), toScreen(glm::vec2(1, 0)),
                IM_COL32(255, 205, 70, 160), 0.0f, 0, 1.5f);

    // The face itself.
    if (uv.size() >= 3) {
        std::vector<ImVec2> pts;
        pts.reserve(uv.size());
        for (const glm::vec2& q : uv) pts.push_back(toScreen(q));
        dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()),
                                IM_COL32(90, 214, 255, 60));
        dl->AddPolyline(pts.data(), static_cast<int>(pts.size()),
                        IM_COL32(90, 214, 255, 235), ImDrawFlags_Closed, 2.0f);
        // The first corner marked, so a rotation or a flip is readable as
        // something that happened to THIS shape rather than a new one.
        dl->AddCircleFilled(pts[0], 4.0f, IM_COL32(255, 255, 255, 235));
    }
    dl->PopClipRect();

    // Drag the face across the texture. The whole picture is the handle -- there
    // are no points to hit, which is the point. One undo step on release, like
    // every other control here.
    if (viewHeld) {
        g_dragging = true;
        const ImVec2 d = ImGui::GetIO().MouseDelta;
        g_uv.offset.x += d.x / ppu;
        g_uv.offset.y -= d.y / ppu;
    } else if (g_dragging) {
        g_dragging = false;
        commit("Move texture");
    }
    ui::hint("Drag inside the picture to slide the texture across the face.");

    // --- The numbers ---------------------------------------------------------
    ui::sectionText("Projection");
    ui::hint("Which way the texture is cast onto the face. Auto lets each face\n"
             "choose; naming an axis is how several faces of one wall share a\n"
             "single course instead of each starting over.");
    {
        const char* names[4] = {"Auto", "X", "Y", "Z"};
        const float bw = (ImGui::GetContentRegionAvail().x -
                          3.0f * ImGui::GetStyle().ItemSpacing.x) / 4.0f;
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            const bool on = (g_uv.axis == i);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button,
                                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
            if (ImGui::Button(names[i], ImVec2(bw, 28.0f)) && !on) {
                g_uv.axis = i;
                commit("Texture projection");
            }
            if (on) ImGui::PopStyleColor();
        }
    }

    ui::sectionText("Size");
    ImGui::Checkbox("Same in both directions", &g_linkSize);
    bool sizeDone = false;
    if (g_linkSize) {
        float m = g_uv.size.x;
        ImGui::SliderFloat("Tile##uvsize", &m, 0.05f, 20.0f, "%.2f m",
                           ImGuiSliderFlags_Logarithmic);
        sizeDone = ImGui::IsItemDeactivatedAfterEdit();
        g_uv.size = glm::vec2(m, m);
    } else {
        ImGui::SliderFloat("Across##uvsizeu", &g_uv.size.x, 0.05f, 20.0f, "%.2f m",
                           ImGuiSliderFlags_Logarithmic);
        sizeDone = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::SliderFloat("Down##uvsizev", &g_uv.size.y, 0.05f, 20.0f, "%.2f m",
                           ImGuiSliderFlags_Logarithmic);
        sizeDone = sizeDone || ImGui::IsItemDeactivatedAfterEdit();
    }
    if (sizeDone) commit("Texture size");
    ui::hint("How many metres one tile of the texture covers. This is the\n"
             "number a brick or a plank actually has, so it is the one to set.");

    ui::sectionText("Rotation");
    ImGui::SliderFloat("##uvrot", &g_uv.rotate, -180.0f, 180.0f, "%.0f deg");
    if (ImGui::IsItemDeactivatedAfterEdit()) commit("Texture rotation");
    {
        const float bw = (ImGui::GetContentRegionAvail().x -
                          2.0f * ImGui::GetStyle().ItemSpacing.x) / 3.0f;
        // The three turns anybody actually asks for, as buttons: a quarter turn
        // is a thing you want exactly, and a slider can only be aimed at it.
        if (ImGui::Button("-90 deg", ImVec2(bw, 28.0f))) {
            g_uv.rotate = std::remainder(g_uv.rotate - 90.0f, 360.0f);
            commit("Texture rotation");
        }
        ImGui::SameLine();
        if (ImGui::Button("+90 deg", ImVec2(bw, 28.0f))) {
            g_uv.rotate = std::remainder(g_uv.rotate + 90.0f, 360.0f);
            commit("Texture rotation");
        }
        ImGui::SameLine();
        if (ImGui::Button("Upright", ImVec2(bw, 28.0f))) {
            g_uv.rotate = 0.0f;
            commit("Texture rotation");
        }
    }

    ui::sectionText("Offset");
    ImGui::SliderFloat("Across##uvoffu", &g_uv.offset.x, -4.0f, 4.0f, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit()) commit("Texture offset");
    ImGui::SliderFloat("Down##uvoffv", &g_uv.offset.y, -4.0f, 4.0f, "%.3f");
    if (ImGui::IsItemDeactivatedAfterEdit()) commit("Texture offset");
    {
        const float bw = (ImGui::GetContentRegionAvail().x -
                          ImGui::GetStyle().ItemSpacing.x) / 2.0f;
        if (ImGui::Button(g_uv.flipU ? "Mirrored across" : "Mirror across",
                          ImVec2(bw, 28.0f))) {
            g_uv.flipU = !g_uv.flipU;
            commit("Mirror texture");
        }
        ImGui::SameLine();
        if (ImGui::Button(g_uv.flipV ? "Mirrored down" : "Mirror down",
                          ImVec2(bw, 28.0f))) {
            g_uv.flipV = !g_uv.flipV;
            commit("Mirror texture");
        }
    }

    ui::sectionText("Fit and spread");
    // "Fit" is the one operation that reads the geometry rather than setting a
    // number: it puts the whole face inside the 0..1 tile, which is what you
    // want for a sign, a poster or a door -- anything whose texture is a picture
    // rather than a pattern.
    if (bigButton("Fit the face into one tile")) {
        // Measure the face at the CURRENT rotation and projection, with the
        // size and offset neutral, so fitting after a rotation fits what you see.
        EditMesh::FaceUV probe = g_uv;
        probe.size   = glm::vec2(1.0f);
        probe.offset = glm::vec2(0.0f);
        const std::vector<glm::vec2> p = editmesh::faceUvs(mesh, f, probe);
        if (p.size() >= 3) {
            glm::vec2 lo = p[0], hi = p[0];
            for (const glm::vec2& q : p) { lo = glm::min(lo, q); hi = glm::max(hi, q); }
            const glm::vec2 ext = glm::max(hi - lo, glm::vec2(1e-3f));
            g_uv.size   = g_linkSize ? glm::vec2(std::max(ext.x, ext.y)) : ext;
            g_uv.offset = -lo / g_uv.size;
            // A linked fit leaves the shorter axis short of the tile; centre it,
            // or a wide sign sits along the bottom edge of its own texture.
            if (g_linkSize) g_uv.offset += 0.5f * (glm::vec2(1.0f) - ext / g_uv.size);
            commit("Fit texture");
        }
    }
    if (bigButton("Reset this face")) {
        g_uv = EditMesh::FaceUV{};
        commit("Reset texture");
    }
    ImGui::Spacing();
    // Applying one face's placement to the rest is what makes this panel worth
    // opening on a building: you get the brick size right once, on the face you
    // can see, and the other forty-one take it.
    if (bigButton("Give every face this placement") && s.edit) {
        const EditMesh::FaceUV u = g_uv;
        const int              k = f;
        s.edit([k, u](MeshComponent& mc) {
            mc.mesh.syncFaceUv();
            for (EditMesh::FaceUV& e : mc.mesh.faceUV) e = u;
            return k;
        }, "Texture on every face");
    }
    if (bigButton("...and to faces wearing the same material") && s.edit) {
        const EditMesh::FaceUV u   = g_uv;
        const int              k   = f;
        const fitzel::AssetId  mat = mesh.faceMaterial(f);
        s.edit([k, u, mat](MeshComponent& mc) {
            mc.mesh.syncFaceUv();
            for (std::size_t i = 0; i < mc.mesh.faceUV.size(); ++i)
                if (mc.mesh.faceMaterial(static_cast<int>(i)) == mat)
                    mc.mesh.faceUV[i] = u;
            return k;
        }, "Texture on matching faces");
    }

    if (!tex)
        ui::hint("This face's material has no texture, so the picture above is a\n"
                 "grid. The numbers still apply -- they are what a texture will\n"
                 "land on when one is assigned.");

    ImGui::End();
}

} // namespace uvui
