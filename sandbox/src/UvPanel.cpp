#include "UvPanel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <fitzel/ui/ImGuiTexture.hpp>

#include "Component.hpp"
#include "UiStyle.hpp"

namespace uvui {

namespace {

using uvops::SelectMode;

enum class Op { None, Move, Rotate, Scale };

// What the window keeps between frames.
struct Ui {
    uvops::Selection sel;
    SelectMode mode     = SelectMode::Vertex;
    bool       sticky   = true;    // welded corners come along (Blender: shared location)
    bool       additive = false;   // clicks add, without Shift
    bool       allFaces = false;   // every face, not only the ones picked in the viewport
    bool       showStretch = false;
    bool       showTexture = true;
    bool       eachIsland  = false;   // pivot: every island about its own middle
    int        owner = -1;

    // The view: the UV point at the middle of the canvas, and how far it is zoomed.
    glm::vec2 center{0.5f, 0.5f};
    float     zoom = 1.0f;
    bool      fit  = true;
    ImVec2    canvasMin{0, 0}, canvasSize{1, 1};
    float     ppu = 100.0f;           // pixels per UV unit

    // G / R / S in flight.
    Op          op = Op::None;
    bool        dragOp = false;       // started by dragging a selected element: letting go ends it
    ImVec2      opStart{0, 0};
    glm::vec2   pivot{0.0f};
    int         axis = -1;            // -1 free, 0 U only, 1 V only
    std::string typed;
    std::vector<int> opFaces;         // the faces shown when it began
    bool        anchor = false;       // started from a button: measured from where the pointer enters the canvas
    int         endReq = 0;           // +1 commit, -1 cancel: done once the frame is drawn

    // Box select: B arms it, a click sets a corner, the next one closes it;
    // dragging over empty canvas does both at once.
    bool      boxArmed = false, boxOn = false;
    glm::vec2 boxA{0.0f};
    // A press on the canvas, until it turns into a click or a drag.
    bool      press = false;
    ImVec2    pressAt{0, 0};
    bool      pressOnSel = false;

    // The amounts the operations take.
    float margin = 0.02f, smartAngle = 66.0f, cubeSize = 1.0f, sharpAngle = 30.0f, mergeDist = 0.002f;
    bool  packRotate = true;
    EditMesh::FaceUV place;           // "Project in metres"

    int         texW = 0, texH = 0;
    std::string msg;
    int         focusFrame = -10;
    std::function<void()> pending;    // an edit, run once the frame is drawn
};
Ui g;

const MaterialDef* findMaterial(const PanelState& s, const fitzel::AssetId& id) {
    if (!id.valid()) return nullptr;
    for (const MaterialDef& md : s.materials)
        if (md.assetId == id) return &md;
    return nullptr;
}

float em() { return ImGui::GetFontSize(); }

bool bigButton(const char* label, const char* tip = nullptr, float w = 0.0f) {
    const bool r = ImGui::Button(label, ImVec2(w, ImGui::GetFrameHeight() + em() * 0.35f));
    if (tip) ImGui::SetItemTooltip("%s", tip);
    return r;
}

bool toggle(const char* label, bool on, const char* tip = nullptr, float w = 0.0f) {
    if (on) {
        const ImVec4 c = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
        ImGui::PushStyleColor(ImGuiCol_Button, c);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, c);
    }
    const bool r = bigButton(label, tip, w);
    if (on) ImGui::PopStyleColor(2);
    return r;
}

// Buttons left to right, a new row when the next would not fit -- so a
// narrow window keeps every control on screen.
struct Flow {
    float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    bool  first = true;
    // Before an item `w` wide; `gap` is the space before it (a group's first
    // item gets more).
    void next(float w, float gap = 2.0f) {
        if (!first) {
            ImGui::SameLine(0.0f, gap);
            if (ImGui::GetCursorPosX() + w > right) ImGui::NewLine();
        }
        first = false;
    }
    static float width(const char* label) {
        return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    }
};

ImVec2 uvToScreen(glm::vec2 q) {
    return ImVec2(g.canvasMin.x + g.canvasSize.x * 0.5f + (q.x - g.center.x) * g.ppu,
                  g.canvasMin.y + g.canvasSize.y * 0.5f - (q.y - g.center.y) * g.ppu);
}

glm::vec2 screenToUv(ImVec2 p) {
    return glm::vec2(g.center.x + (p.x - g.canvasMin.x - g.canvasSize.x * 0.5f) / g.ppu,
                     g.center.y - (p.y - g.canvasMin.y - g.canvasSize.y * 0.5f) / g.ppu);
}

const char* modeName(SelectMode m) {
    switch (m) {
    case SelectMode::Vertex: return "Vertex";
    case SelectMode::Edge:   return "Edge";
    case SelectMode::Face:   return "Face";
    case SelectMode::Island: return "Island";
    }
    return "";
}

// The faces an operation works on: the ones wholly selected in the UV editor,
// or every face shown when none is. Wholly: a sticky selection reaches into
// the neighbours' corners, and unwrapping one face must not take them along.
std::vector<int> targets(const uvops::View& v) {
    std::vector<int> out;
    for (int f : uvops::facesOf(g.sel))
        if (uvops::faceSelected(g.sel, *v.mesh, f)) out.push_back(f);
    return out.empty() ? v.faces : out;
}

// Frame these UV bounds in the canvas.
void frame(glm::vec2 lo, glm::vec2 hi) {
    const glm::vec2 ext = glm::max(hi - lo, glm::vec2(0.05f));
    g.center = 0.5f * (lo + hi);
    const float base = std::min(g.canvasSize.x, g.canvasSize.y);
    g.zoom = std::clamp(0.85f / std::max(ext.x * base / std::max(g.canvasSize.x, 1.0f),
                                         ext.y * base / std::max(g.canvasSize.y, 1.0f)), 0.02f, 400.0f);
}

void frameAll(const uvops::View& v) {
    glm::vec2 lo(0.0f), hi(1.0f);
    for (const auto& f : v.uv)
        for (const glm::vec2& p : f) { lo = glm::min(lo, p); hi = glm::max(hi, p); }
    frame(lo, hi);
}

void frameSelected(const uvops::View& v) {
    glm::vec2 lo, hi;
    if (uvops::bounds(v, g.sel, lo, hi)) frame(lo, hi);
    else frameAll(v);
}

// --- The modal transform ------------------------------------------------------------

const char* opLabel(Op op) {
    return op == Op::Move ? "UV move" : op == Op::Rotate ? "UV rotate" : "UV scale";
}

void beginOp(const PanelState& s, const uvops::View& v, Op op, bool drag, bool fromButton = false) {
    if (g.sel.empty()) { g.msg = "Select something first: click a corner, an edge, a face -- or A for all."; return; }
    if (!s.live) return;
    glm::vec2 lo, hi;
    if (!uvops::bounds(v, g.sel, lo, hi)) return;
    g.op       = op;
    g.dragOp   = drag;
    g.opStart  = ImGui::GetIO().MousePos;
    g.pivot    = 0.5f * (lo + hi);
    g.axis     = -1;
    g.typed.clear();
    g.opFaces  = v.faces;
    g.anchor   = fromButton;
    g.endReq   = 0;
    g.boxArmed = g.boxOn = false;
    s.live(Live::Begin, nullptr, opLabel(op));
}

// Asked for now, done once the frame is drawn: cancelling puts the entity
// back, and nothing drawn this frame may point into the one it replaces.
void requestEnd(bool keep) { g.endReq = keep ? 1 : -1; }

// What the pointer (or the typed number) says the transform is now.
void opParams(glm::mat2& a, glm::vec2& t, std::string& hud) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool snap = io.KeyCtrl;
    const bool haveTyped = !g.typed.empty() && g.typed != "-" && g.typed != ".";
    const float typed = haveTyped ? std::strtof(g.typed.c_str(), nullptr) : 0.0f;
    a = glm::mat2(1.0f);
    t = glm::vec2(0.0f);
    char buf[160];
    const char* ax = g.axis == 0 ? "  along U" : g.axis == 1 ? "  along V" : "";
    if (g.op == Op::Move) {
        glm::vec2 d(( io.MousePos.x - g.opStart.x) / g.ppu, -(io.MousePos.y - g.opStart.y) / g.ppu);
        if (snap) {
            // A texel when there is a texture, an eighth of a tile otherwise.
            const glm::vec2 step = g.texW > 0 ? glm::vec2(1.0f / float(g.texW), 1.0f / float(g.texH)) : glm::vec2(0.125f);
            d = glm::round(d / step) * step;
        }
        if (haveTyped) d = g.axis == 1 ? glm::vec2(0.0f, typed) : glm::vec2(typed, 0.0f);
        if (g.axis == 0) d.y = 0.0f;
        if (g.axis == 1) d.x = 0.0f;
        t = d;
        std::snprintf(buf, sizeof buf, "Move  U %+.4f  V %+.4f%s%s", d.x, d.y, ax, haveTyped ? "  (typed)" : "");
    } else if (g.op == Op::Rotate) {
        const ImVec2 c = uvToScreen(g.pivot);
        const float a0 = std::atan2(-(g.opStart.y - c.y), g.opStart.x - c.x);
        const float a1 = std::atan2(-(io.MousePos.y - c.y), io.MousePos.x - c.x);
        float deg = glm::degrees(a1 - a0);
        deg = std::remainder(deg, 360.0f);
        if (snap) deg = std::round(deg / 5.0f) * 5.0f;
        if (haveTyped) deg = typed;
        const float r = glm::radians(deg);
        a = glm::mat2(std::cos(r), std::sin(r), -std::sin(r), std::cos(r));
        std::snprintf(buf, sizeof buf, "Rotate  %.1f deg%s", deg, haveTyped ? "  (typed)" : "");
    } else {
        const ImVec2 c = uvToScreen(g.pivot);
        const float d0 = std::max(4.0f, std::hypot(g.opStart.x - c.x, g.opStart.y - c.y));
        float k = std::hypot(io.MousePos.x - c.x, io.MousePos.y - c.y) / d0;
        if (snap) k = std::round(k * 10.0f) / 10.0f;
        if (haveTyped) k = typed;
        a = glm::mat2(g.axis == 1 ? 1.0f : k, 0.0f, 0.0f, g.axis == 0 ? 1.0f : k);
        std::snprintf(buf, sizeof buf, "Scale  %.3f%s%s", k, ax, haveTyped ? "  (typed)" : "");
    }
    hud = buf;
}

void applyOp(const PanelState& s) {
    if (g.anchor) {
        const ImVec2 p = ImGui::GetIO().MousePos;
        const bool inside = p.x >= g.canvasMin.x && p.y >= g.canvasMin.y && p.x <= g.canvasMin.x + g.canvasSize.x &&
                            p.y <= g.canvasMin.y + g.canvasSize.y;
        g.opStart = p;
        if (!inside) return;
        g.anchor = false;
    }
    glm::mat2 a;
    glm::vec2 t;
    std::string hud;
    opParams(a, t, hud);
    const uvops::Selection sel = g.sel;
    const glm::vec2 pivot = g.pivot;
    const bool each = g.eachIsland && g.op != Op::Move;
    const std::vector<int> faces = g.opFaces;
    s.live(Live::Set, [sel, a, t, pivot, each, faces](EditMesh& m) {
        if (each) uvops::transformEach(m, uvops::view(m, faces), sel, a, t);
        else uvops::transform(m, sel, a, pivot, t);
    }, opLabel(g.op));
}

void endOp(const PanelState& s, bool keep) {
    if (g.op == Op::None) return;
    if (s.live) s.live(keep ? Live::Commit : Live::Cancel, nullptr, opLabel(g.op));
    g.op = Op::None;
    g.dragOp = false;
    g.typed.clear();
}

// --- Running an edit -------------------------------------------------------------------

// One undo step: `fn` gets the mesh. Run after the frame is drawn, so nothing
// drawn this frame points at a mesh the edit replaced.
void run(const PanelState& s, const char* label, std::function<void(EditMesh&)> fn) {
    if (!s.edit) return;
    std::string lbl = label;
    g.pending = [&s, lbl, fn]() {
        const int keep = s.faceSel;
        s.edit([fn, keep](MeshComponent& mc) {
            fn(mc.mesh);
            return keep;
        }, lbl.c_str());
    };
}

// --- The menus and the bar ---------------------------------------------------------------

void stepRow(const char* label, const char* id, float& v, float step, float lo, float hi, const char* fmt) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(em() * 7.0f);
    ui::stepper(id, v, step, lo, hi, fmt, em() * 9.0f);
}

void unwrapItems(const PanelState& s, const uvops::View& v) {
    const std::vector<int> fs = targets(v);
    if (ImGui::MenuItem("Unwrap", "U")) {
        const float mg = g.margin;
        run(s, "Unwrap", [fs, mg](EditMesh& m) {
            int closed = 0;
            const int n = uvops::unwrap(m, fs, mg, &closed);
            g.msg = "Unwrapped: " + std::to_string(n) + (n == 1 ? " chart" : " charts") + " along the seams.";
            if (closed)
                g.msg += " " + std::to_string(closed) + (closed == 1 ? " has" : " have") +
                         " no seam to open along and came out crumpled -- mark seams first (edge mode, or Seams from sharp edges).";
        });
    }
    ImGui::SetItemTooltip("Cut open along the seams and flattened with its angles kept (Blender's Conformal).\n"
                          "Pinned corners hold their place; charts without pins are packed into the tile.");
    if (ImGui::BeginMenu("Smart UV Project")) {
        stepRow("Angle limit", "sang", g.smartAngle, 5.0f, 5.0f, 85.0f, "%.0f deg");
        stepRow("Island margin", "smar", g.margin, 0.005f, 0.0f, 0.1f, "%.3f");
        if (bigButton("Project", nullptr, -1.0f)) {
            const float a = g.smartAngle, mg = g.margin;
            run(s, "Smart UV project", [fs, a, mg](EditMesh& m) {
                const int n = uvops::smartProject(m, fs, a, mg);
                g.msg = "Smart UV project: " + std::to_string(n) + " islands.";
            });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }
    ImGui::SetItemTooltip("No seams needed: faces facing the same way (within the angle) become one island each.");
    if (ImGui::BeginMenu("Cube Projection")) {
        stepRow("Cube size", "csz", g.cubeSize, 0.25f, 0.05f, 50.0f, "%.2f m");
        if (bigButton("Project", nullptr, -1.0f)) {
            const float sz = g.cubeSize;
            run(s, "Cube projection", [fs, sz](EditMesh& m) { uvops::cubeProject(m, fs, sz); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Cylinder Projection"))
        run(s, "Cylinder projection", [fs](EditMesh& m) { uvops::cylinderProject(m, fs); });
    if (ImGui::MenuItem("Sphere Projection"))
        run(s, "Sphere projection", [fs](EditMesh& m) { uvops::sphereProject(m, fs); });
    if (ImGui::BeginMenu("Project in metres")) {
        ui::hint("The texture in metres, along an axis -- one\nbrick course across every wall that shares it.");
        static const char* axes[4] = {"Auto", "X", "Y", "Z"};
        for (int i = 0; i < 4; ++i) {
            if (i) ImGui::SameLine();
            if (toggle(axes[i], g.place.axis == i, nullptr, em() * 3.6f)) g.place.axis = i;
        }
        float sz = g.place.size.x;
        stepRow("Tile size", "psz", sz, sz < 1.0f ? 0.05f : 0.25f, 0.05f, 50.0f, "%.2f m");
        g.place.size = glm::vec2(sz);
        stepRow("Rotation", "prot", g.place.rotate, 15.0f, -180.0f, 180.0f, "%.0f deg");
        if (bigButton("Project", nullptr, -1.0f)) {
            const EditMesh::FaceUV p = g.place;
            run(s, "Project in metres", [fs, p](EditMesh& m) { uvops::projectWorld(m, fs, p); });
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Reset"))
        run(s, "Reset UVs", [fs](EditMesh& m) { uvops::resetUvs(m, fs); });
    ImGui::SetItemTooltip("Every face fills the whole tile.");
}

void menuBar(const PanelState& s, const uvops::View& v) {
    if (!ImGui::BeginMenuBar()) return;
    const std::vector<int> fs = targets(v);
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Frame all", "Home")) frameAll(v);
        if (ImGui::MenuItem("Frame selected", "F")) frameSelected(v);
        if (ImGui::MenuItem("Zoom in", "+")) g.zoom *= 1.25f;
        if (ImGui::MenuItem("Zoom out", "-")) g.zoom /= 1.25f;
        ImGui::Separator();
        ImGui::MenuItem("All faces", nullptr, &g.allFaces);
        ImGui::SetItemTooltip("Off: the faces picked in the viewport (all of them while none is).");
        ImGui::MenuItem("Texture", nullptr, &g.showTexture);
        ImGui::MenuItem("Stretch", nullptr, &g.showStretch);
        ImGui::SetItemTooltip("Faces coloured by texture density: blue gets less of the texture than the\n"
                              "average, red more. Even is what an unwrap wants.");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Select")) {
        if (ImGui::MenuItem("All", "A")) uvops::selectAll(v, g.sel);
        if (ImGui::MenuItem("None", "Alt+A")) g.sel.clear();
        if (ImGui::MenuItem("Invert", "Ctrl+I")) uvops::invert(v, g.sel);
        if (ImGui::MenuItem("Box", "B")) { g.boxArmed = true; g.boxOn = false; }
        if (ImGui::MenuItem("Linked", "Ctrl+L")) uvops::selectLinked(v, g.sel);
        if (ImGui::MenuItem("Pinned")) uvops::selectPinned(v, g.sel);
        ImGui::Separator();
        for (SelectMode m : {SelectMode::Vertex, SelectMode::Edge, SelectMode::Face, SelectMode::Island}) {
            char key[4];
            std::snprintf(key, sizeof key, "%d", int(m) + 1);
            if (ImGui::MenuItem(modeName(m), key, g.mode == m)) g.mode = m;
        }
        ImGui::Separator();
        ImGui::MenuItem("Sticky (shared location)", nullptr, &g.sticky);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("UV")) {
        unwrapItems(s, v);
        ImGui::Separator();
        if (ImGui::MenuItem("Seams from islands"))
            run(s, "Seams from islands", [fs](EditMesh& m) {
                g.msg = std::to_string(uvops::seamsFromIslands(m, fs)) + " edges marked as seams where the islands end.";
            });
        if (ImGui::BeginMenu("Seams from sharp edges")) {
            stepRow("Sharper than", "shp", g.sharpAngle, 5.0f, 1.0f, 179.0f, "%.0f deg");
            if (bigButton("Mark", nullptr, -1.0f)) {
                const float a = g.sharpAngle;
                run(s, "Seams from sharp edges", [fs, a](EditMesh& m) {
                    g.msg = std::to_string(uvops::seamsFromSharp(m, fs, a)) + " sharp edges marked as seams.";
                });
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Clear seams"))
            run(s, "Clear seams", [fs](EditMesh& m) {
                for (int f : fs) {
                    const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
                    for (std::size_t k = 0; k < fv.size(); ++k) m.setSeam(fv[k], fv[(k + 1) % fv.size()], false);
                }
                g.msg = "Seams cleared.";
            });
        ImGui::Separator();
        if (ImGui::BeginMenu("Pack Islands")) {
            stepRow("Margin", "pmar", g.margin, 0.005f, 0.0f, 0.1f, "%.3f");
            ImGui::Checkbox("Rotate to fit", &g.packRotate);
            if (bigButton("Pack", nullptr, -1.0f)) {
                const float mg = g.margin;
                const bool rot = g.packRotate;
                run(s, "Pack islands", [fs, mg, rot](EditMesh& m) { uvops::pack(m, fs, mg, rot); });
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Average Islands Scale"))
            run(s, "Average islands scale", [fs](EditMesh& m) { uvops::averageScale(m, fs); });
        ImGui::Separator();
        const bool any = !g.sel.empty();
        const uvops::Selection sel = g.sel;
        glm::vec2 lo, hi;
        const glm::vec2 c = uvops::bounds(v, g.sel, lo, hi) ? 0.5f * (lo + hi) : glm::vec2(0.5f);
        auto affine = [&](const char* label, glm::mat2 a) {
            if (ImGui::MenuItem(label, nullptr, false, any))
                run(s, label, [sel, a, c](EditMesh& m) { uvops::transform(m, sel, a, c, glm::vec2(0.0f)); });
        };
        affine("Mirror U", glm::mat2(-1.0f, 0.0f, 0.0f, 1.0f));
        affine("Mirror V", glm::mat2(1.0f, 0.0f, 0.0f, -1.0f));
        affine("Rotate +90", glm::mat2(0.0f, 1.0f, -1.0f, 0.0f));
        affine("Rotate -90", glm::mat2(0.0f, -1.0f, 1.0f, 0.0f));
        const std::vector<int> shown = v.faces;
        if (ImGui::MenuItem("Align U", nullptr, false, any))
            run(s, "Align U", [sel, shown](EditMesh& m) { uvops::align(m, uvops::view(m, shown), sel, 0); });
        if (ImGui::MenuItem("Align V", nullptr, false, any))
            run(s, "Align V", [sel, shown](EditMesh& m) { uvops::align(m, uvops::view(m, shown), sel, 1); });
        ImGui::Separator();
        if (ImGui::MenuItem("Merge at Center", "M", false, any))
            run(s, "Merge UVs", [sel, shown](EditMesh& m) { uvops::mergeAtCenter(m, uvops::view(m, shown), sel); });
        if (ImGui::BeginMenu("Merge by Distance", any)) {
            stepRow("Distance", "mdist", g.mergeDist, 0.001f, 0.0f, 0.1f, "%.3f");
            if (bigButton("Merge", nullptr, -1.0f)) {
                const float d = g.mergeDist;
                run(s, "Merge UVs by distance", [sel, shown, d](EditMesh& m) {
                    g.msg = std::to_string(uvops::mergeByDistance(m, uvops::view(m, shown), sel, d)) + " corners merged.";
                });
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndMenu();
        }
        const int tw = g.texW, th = g.texH;
        if (ImGui::MenuItem("Snap to Pixels", nullptr, false, any && tw > 0))
            run(s, "Snap UVs to pixels", [sel, tw, th](EditMesh& m) { uvops::snapToPixels(m, sel, tw, th); });
        ImGui::Separator();
        if (ImGui::MenuItem("Pin", "P", false, any))
            run(s, "Pin UVs", [sel](EditMesh& m) { uvops::setPin(m, sel, true); });
        if (ImGui::MenuItem("Unpin", "Alt+P", false, any))
            run(s, "Unpin UVs", [sel](EditMesh& m) { uvops::setPin(m, sel, false); });
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void toolBar(const PanelState& s, const uvops::View& v) {
    Flow fl;
    const float group = em();
    auto tog = [&](const char* label, bool on, const char* tip, float gap = 2.0f) {
        fl.next(Flow::width(label), gap);
        return toggle(label, on, tip);
    };
    auto btn = [&](const char* label, const char* tip, float gap = 2.0f) {
        fl.next(Flow::width(label), gap);
        return bigButton(label, tip);
    };
    static const SelectMode modes[4] = {SelectMode::Vertex, SelectMode::Edge, SelectMode::Face, SelectMode::Island};
    static const char* tips[4] = {"Select corners  (1)", "Select edges  (2)", "Select faces  (3)", "Select whole islands  (4)"};
    for (int i = 0; i < 4; ++i)
        if (tog(modeName(modes[i]), g.mode == modes[i], tips[i])) g.mode = modes[i];
    if (tog("Sticky", g.sticky, "Corners welded to a picked one come with it, so an island stays in one piece.\n"
                                "Off: only what is picked moves -- the way to cut a face free.", group))
        g.sticky = !g.sticky;
    if (tog("Add", g.additive, "Clicks add to the selection (and take away what is picked already),\n"
                               "without holding Shift."))
        g.additive = !g.additive;
    if (tog("Move", g.op == Op::Move, "Move  (G): follows the pointer; click to place, right click to cancel.\n"
                                      "X or Y keeps it on one axis, Ctrl snaps, a typed number is exact.", group))
        if (g.op == Op::None) beginOp(s, v, Op::Move, false, true);
    if (tog("Rotate", g.op == Op::Rotate, "Rotate  (R): round the middle of the selection. Ctrl: 5 degree steps."))
        if (g.op == Op::None) beginOp(s, v, Op::Rotate, false, true);
    if (tog("Scale", g.op == Op::Scale, "Scale  (S): from the middle of the selection. X or Y: one axis only."))
        if (g.op == Op::None) beginOp(s, v, Op::Scale, false, true);
    if (tog("Each island", g.eachIsland, "Rotate and scale every island about its own middle\n"
                                         "(Blender's pivot: individual origins)."))
        g.eachIsland = !g.eachIsland;
    if (btn("Unwrap", "Unwrap  (U): along the seams, angles kept. More in the UV menu.", group)) {
        const std::vector<int> fs = targets(v);
        const float mg = g.margin;
        run(s, "Unwrap", [fs, mg](EditMesh& m) {
            int closed = 0;
            const int n = uvops::unwrap(m, fs, mg, &closed);
            g.msg = "Unwrapped: " + std::to_string(n) + (n == 1 ? " chart" : " charts") + " along the seams.";
            if (closed)
                g.msg += " " + std::to_string(closed) + (closed == 1 ? " has" : " have") +
                         " no seam to open along and came out crumpled -- mark seams first (edge mode, or Seams from sharp edges).";
        });
    }
    if (btn("Pack", "Pack the islands into the tile, apart.")) {
        const std::vector<int> fs = targets(v);
        const float mg = g.margin;
        const bool rot = g.packRotate;
        run(s, "Pack islands", [fs, mg, rot](EditMesh& m) { uvops::pack(m, fs, mg, rot); });
    }
    if (btn(" - ", "Zoom out  (-, or the mouse wheel)", group)) g.zoom /= 1.25f;
    if (btn(" + ", "Zoom in  (+)")) g.zoom *= 1.25f;
    if (btn("All", "Frame everything  (Home)")) frameAll(v);
    if (btn("Sel", "Frame the selection  (F)")) frameSelected(v);
}

// --- Keys --------------------------------------------------------------------------------

void keys(const PanelState& s, const uvops::View& v, bool hovered) {
    ImGuiIO& io = ImGui::GetIO();
    auto key = [](ImGuiKey k) { return ImGui::IsKeyPressed(k, false); };
    if (g.op != Op::None) {
        if (key(ImGuiKey_Escape)) { requestEnd(false); return; }
        if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter)) { requestEnd(true); return; }
        if (key(ImGuiKey_X)) g.axis = g.axis == 0 ? -1 : 0;
        if (key(ImGuiKey_Y)) g.axis = g.axis == 1 ? -1 : 1;
        for (int d = 0; d <= 9; ++d)
            if (key(ImGuiKey(ImGuiKey_0 + d)) || key(ImGuiKey(ImGuiKey_Keypad0 + d))) g.typed += char('0' + d);
        if (key(ImGuiKey_Period) || key(ImGuiKey_KeypadDecimal)) g.typed += '.';
        if (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) {
            if (!g.typed.empty() && g.typed[0] == '-') g.typed.erase(0, 1);
            else g.typed.insert(0, "-");
        }
        if (key(ImGuiKey_Backspace) && !g.typed.empty()) g.typed.pop_back();
        // Another transform key switches to that transform, from where it began.
        const Op to = key(ImGuiKey_G) ? Op::Move : key(ImGuiKey_R) ? Op::Rotate : key(ImGuiKey_S) ? Op::Scale : Op::None;
        if (to != Op::None && to != g.op) {
            g.op = to;
            g.opStart = io.MousePos;
            g.axis = -1;
            g.typed.clear();
        }
        return;
    }
    if (io.KeyCtrl) {
        // Undone once the frame is drawn: it replaces the entity.
        if (key(ImGuiKey_Z) && s.undo) { const bool redo = io.KeyShift; g.pending = [&s, redo] { s.undo(redo); }; }
        if (key(ImGuiKey_Y) && s.undo) g.pending = [&s] { s.undo(true); };
        if (key(ImGuiKey_I)) uvops::invert(v, g.sel);
        if (key(ImGuiKey_L)) uvops::selectLinked(v, g.sel);
        return;
    }
    if (io.KeyAlt) {
        if (key(ImGuiKey_A)) g.sel.clear();
        if (key(ImGuiKey_P) && !g.sel.empty()) {
            const uvops::Selection sel = g.sel;
            run(s, "Unpin UVs", [sel](EditMesh& m) { uvops::setPin(m, sel, false); });
        }
        return;
    }
    if (key(ImGuiKey_1)) g.mode = SelectMode::Vertex;
    if (key(ImGuiKey_2)) g.mode = SelectMode::Edge;
    if (key(ImGuiKey_3)) g.mode = SelectMode::Face;
    if (key(ImGuiKey_4)) g.mode = SelectMode::Island;
    if (key(ImGuiKey_A)) uvops::selectAll(v, g.sel);
    if (key(ImGuiKey_B)) { g.boxArmed = true; g.boxOn = false; }
    if (key(ImGuiKey_G)) beginOp(s, v, Op::Move, false);
    if (key(ImGuiKey_R)) beginOp(s, v, Op::Rotate, false);
    if (key(ImGuiKey_S)) beginOp(s, v, Op::Scale, false);
    if (key(ImGuiKey_U)) ImGui::OpenPopup("##unwrapmenu");
    if (key(ImGuiKey_M) && !g.sel.empty()) {
        const uvops::Selection sel = g.sel;
        const std::vector<int> shown = v.faces;
        run(s, "Merge UVs", [sel, shown](EditMesh& m) { uvops::mergeAtCenter(m, uvops::view(m, shown), sel); });
    }
    if (key(ImGuiKey_P) && !g.sel.empty()) {
        const uvops::Selection sel = g.sel;
        run(s, "Pin UVs", [sel](EditMesh& m) { uvops::setPin(m, sel, true); });
    }
    if (key(ImGuiKey_L) && hovered) {
        // The island under the pointer, added.
        uvops::Selection one;
        if (uvops::click(v, one, SelectMode::Island, screenToUv(io.MousePos), 10.0f / g.ppu, false, true))
            g.sel.insert(one.begin(), one.end());
    }
    if (key(ImGuiKey_Home)) frameAll(v);
    if (key(ImGuiKey_F) || key(ImGuiKey_KeypadDecimal)) frameSelected(v);
    if (key(ImGuiKey_Equal) || key(ImGuiKey_KeypadAdd)) g.zoom *= 1.25f;
    if (key(ImGuiKey_Minus) || key(ImGuiKey_KeypadSubtract)) g.zoom /= 1.25f;
    if (key(ImGuiKey_Escape)) g.boxArmed = g.boxOn = false;
}

// --- The canvas --------------------------------------------------------------------------

ImU32 stretchColor(float s) {
    // Blue below the average density, red above, green on it.
    const float t = std::clamp(s / 2.0f, -1.0f, 1.0f);
    const float r = t > 0 ? 0.25f + 0.75f * t : 0.25f * (1.0f + t);
    const float gr = 0.7f * (1.0f - std::abs(t)) + 0.15f;
    const float b = t < 0 ? 0.25f - 0.75f * t : 0.25f * (1.0f - t);
    return IM_COL32(int(r * 255), int(gr * 255), int(b * 255), 120);
}

void canvas(const PanelState& s, const uvops::View& v, const fitzel::Texture* tex) {
    ImGuiIO& io = ImGui::GetIO();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 sz = ImGui::GetContentRegionAvail();
    // Room for the rows under it: the selection's numbers (two rows when the
    // window is narrow) and the status line.
    const float rows = ImGui::GetContentRegionAvail().x < em() * 46.0f ? 3.2f : 2.2f;
    sz.y -= (ImGui::GetFrameHeight() + em() * 0.35f + ImGui::GetStyle().ItemSpacing.y) * rows;
    sz.x = std::max(sz.x, 120.0f);
    sz.y = std::max(sz.y, 120.0f);
    g.canvasMin = p0;
    g.canvasSize = sz;
    ImGui::InvisibleButton("##uvcanvas", sz, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                                                 ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    if (g.fit) { frameAll(v); g.fit = false; }
    g.ppu = std::min(sz.x, sz.y) * 0.9f * g.zoom;

    // Pan with the middle button, zoom with the wheel about the pointer.
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
        g.center.x -= io.MouseDelta.x / g.ppu;
        g.center.y += io.MouseDelta.y / g.ppu;
    }
    if (hovered && io.MouseWheel != 0.0f) {
        const glm::vec2 before = screenToUv(io.MousePos);
        g.zoom = std::clamp(g.zoom * std::pow(1.2f, io.MouseWheel), 0.02f, 400.0f);
        g.ppu = std::min(sz.x, sz.y) * 0.9f * g.zoom;
        const glm::vec2 after = screenToUv(io.MousePos);
        g.center += before - after;
    }
    const glm::vec2 mouseUv = screenToUv(io.MousePos);
    const float pick = 10.0f / g.ppu;

    // --- Pointer -------------------------------------------------------------------------
    if (g.op != Op::None) {
        if (g.dragOp && !io.MouseDown[0]) requestEnd(true);
        else if (!g.dragOp && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) requestEnd(true);
        else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) requestEnd(false);
    } else if (hovered || g.press) {
        const bool add = io.KeyShift || g.additive;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && hovered) {
            if (g.boxArmed && !g.boxOn) {
                g.boxOn = true;
                g.boxA = mouseUv;
            } else if (g.boxArmed && g.boxOn) {
                if (!add && !io.KeyCtrl) g.sel.clear();
                uvops::selectBox(v, g.sel, g.mode, g.boxA, mouseUv, io.KeyCtrl);
                g.boxArmed = g.boxOn = false;
            } else {
                g.press = true;
                g.pressAt = io.MousePos;
                // On something selected: a drag moves it (Blender's tweak).
                uvops::Selection under;
                g.pressOnSel = uvops::click(v, under, g.mode, mouseUv, pick, false, g.sticky) &&
                               std::any_of(under.begin(), under.end(), [](const uvops::Loop& l) { return g.sel.count(l) > 0; });
            }
        }
        if (g.press && io.MouseDown[0]) {
            const float moved = std::hypot(io.MousePos.x - g.pressAt.x, io.MousePos.y - g.pressAt.y);
            if (moved > 5.0f) {
                g.press = false;
                if (g.pressOnSel) {
                    beginOp(s, v, Op::Move, true);
                    g.opStart = g.pressAt;
                } else {
                    g.boxOn = true;
                    g.boxArmed = true;
                    g.boxA = screenToUv(g.pressAt);
                }
            }
        }
        if (g.press && !io.MouseDown[0]) {
            g.press = false;
            if (!uvops::click(v, g.sel, g.mode, mouseUv, pick, add, g.sticky) && !add) g.sel.clear();
        }
        // A box being dragged closes when the button comes up.
        if (g.boxOn && g.boxArmed && !io.MouseDown[0] && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            std::hypot(io.MousePos.x - uvToScreen(g.boxA).x, io.MousePos.y - uvToScreen(g.boxA).y) > 5.0f) {
            if (!add && !io.KeyCtrl) g.sel.clear();
            uvops::selectBox(v, g.sel, g.mode, g.boxA, mouseUv, io.KeyCtrl);
            g.boxArmed = g.boxOn = false;
        }
    }

    // --- Drawing ---------------------------------------------------------------------------
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p1(p0.x + sz.x, p0.y + sz.y);
    dl->PushClipRect(p0, p1, true);
    dl->AddRectFilled(p0, p1, IM_COL32(28, 30, 36, 255));
    const glm::vec2 uvLo = screenToUv(ImVec2(p0.x, p1.y)), uvHi = screenToUv(ImVec2(p1.x, p0.y));
    const int u0 = int(std::floor(uvLo.x)), u1 = int(std::ceil(uvHi.x));
    const int v0 = int(std::floor(uvLo.y)), v1 = int(std::ceil(uvHi.y));
    const bool sane = (u1 - u0) <= 16 && (v1 - v0) <= 16;
    for (int iv = v0; sane && iv < v1; ++iv)
        for (int iu = u0; iu < u1; ++iu) {
            const ImVec2 a = uvToScreen(glm::vec2(iu, iv + 1)), b = uvToScreen(glm::vec2(iu + 1, iv));
            const bool home = iu == 0 && iv == 0;
            if (tex && g.showTexture) {
                // Texture assets are loaded bottom-up (OpenGL's way), so v = 1
                // is the picture's top: (0,1)..(1,0) stands it the right way up.
                dl->AddImage(fitzel::imTexture(tex->id()), a, b, ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f),
                             home ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 70));
            } else {
                // A checker, eight squares to the tile, so stretching shows.
                const int n = 8;
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x) {
                        const ImVec2 c0 = uvToScreen(glm::vec2(iu + float(x) / n, iv + float(y + 1) / n));
                        const ImVec2 c1 = uvToScreen(glm::vec2(iu + float(x + 1) / n, iv + float(y) / n));
                        const bool dark = ((x + y) & 1) != 0;
                        const int l = dark ? 52 : 66;
                        dl->AddRectFilled(c0, c1, home ? IM_COL32(l, l + 3, l + 10, 255) : IM_COL32(l - 14, l - 12, l - 8, 255));
                    }
            }
            dl->AddRect(a, b, IM_COL32(255, 255, 255, 30));
        }
    dl->AddRect(uvToScreen(glm::vec2(0, 1)), uvToScreen(glm::vec2(1, 0)), IM_COL32(255, 205, 70, 170), 0.0f, 0, 1.5f);

    const EditMesh& m = *v.mesh;
    const std::vector<float> st = g.showStretch ? uvops::stretch(v) : std::vector<float>{};
    const ImU32 kEdge = IM_COL32(205, 210, 220, 200), kSel = IM_COL32(255, 150, 40, 255), kSeam = IM_COL32(235, 60, 50, 255);
    // Faces: filled when selected (or by stretch), outlined.
    for (std::size_t i = 0; i < v.faces.size(); ++i) {
        const int f = v.faces[i];
        std::vector<ImVec2> pts;
        for (const glm::vec2& q : v.uv[i]) pts.push_back(uvToScreen(q));
        const bool fsel = uvops::faceSelected(g.sel, m, f);
        if (g.showStretch) dl->AddConcavePolyFilled(pts.data(), int(pts.size()), stretchColor(st[i]));
        if (fsel) dl->AddConcavePolyFilled(pts.data(), int(pts.size()), IM_COL32(255, 150, 40, 60));
        const std::vector<int>& fv = m.faces[static_cast<std::size_t>(f)];
        for (std::size_t k = 0; k < pts.size(); ++k) {
            const std::size_t j = (k + 1) % pts.size();
            const bool seam = m.isSeam(fv[k], fv[j]);
            const bool esel = uvops::edgeSelected(g.sel, m, f, int(k));
            dl->AddLine(pts[k], pts[j], esel ? kSel : seam ? kSeam : kEdge, esel || seam ? 2.2f : 1.2f);
        }
        if (g.mode == SelectMode::Face || g.mode == SelectMode::Island) {
            ImVec2 c(0, 0);
            for (const ImVec2& p : pts) { c.x += p.x; c.y += p.y; }
            c.x /= float(pts.size());
            c.y /= float(pts.size());
            dl->AddRectFilled(ImVec2(c.x - 2.5f, c.y - 2.5f), ImVec2(c.x + 2.5f, c.y + 2.5f), fsel ? kSel : IM_COL32(20, 20, 20, 220));
        }
    }
    // Corners: every one in vertex mode, the selected ones always; pinned in red.
    for (std::size_t i = 0; i < v.faces.size(); ++i) {
        const int f = v.faces[i];
        for (std::size_t k = 0; k < v.uv[i].size(); ++k) {
            const ImVec2 p = uvToScreen(v.uv[i][k]);
            const bool on = g.sel.count({f, int(k)}) > 0;
            const bool pin = m.hasCornerUvs(f) && m.faceUV[static_cast<std::size_t>(f)].corners[k].pin;
            if (pin) dl->AddRectFilled(ImVec2(p.x - 3.5f, p.y - 3.5f), ImVec2(p.x + 3.5f, p.y + 3.5f), IM_COL32(230, 40, 40, 255));
            if (on) dl->AddCircleFilled(p, 3.2f, kSel);
            else if (g.mode == SelectMode::Vertex) dl->AddCircleFilled(p, 2.4f, IM_COL32(15, 15, 15, 230));
        }
    }
    // What a click would take.
    if (hovered && g.op == Op::None && !g.boxOn) {
        const ImU32 kHover = IM_COL32(120, 210, 255, 255);
        if (g.mode == SelectMode::Vertex) {
            const uvops::Loop l = uvops::nearestLoop(v, mouseUv, pick);
            if (l.valid()) dl->AddCircle(uvToScreen(v.at(l)), 6.0f, kHover, 0, 2.0f);
        } else if (g.mode == SelectMode::Edge) {
            const uvops::Loop e = uvops::nearestEdge(v, mouseUv, pick);
            if (e.valid()) {
                const int n = int(m.faces[static_cast<std::size_t>(e.f)].size());
                dl->AddLine(uvToScreen(v.at(e)), uvToScreen(v.at({e.f, (e.k + 1) % n})), kHover, 3.0f);
            }
        } else {
            const int f = uvops::faceAt(v, mouseUv, pick);
            if (f >= 0) {
                std::vector<ImVec2> pts;
                for (const glm::vec2& q : v.uv[static_cast<std::size_t>(v.slot[static_cast<std::size_t>(f)])]) pts.push_back(uvToScreen(q));
                dl->AddPolyline(pts.data(), int(pts.size()), kHover, ImDrawFlags_Closed, 2.4f);
            }
        }
    }
    // The box.
    if (g.boxOn) {
        const ImVec2 a = uvToScreen(g.boxA);
        dl->AddRectFilled(ImVec2(std::min(a.x, io.MousePos.x), std::min(a.y, io.MousePos.y)),
                          ImVec2(std::max(a.x, io.MousePos.x), std::max(a.y, io.MousePos.y)), IM_COL32(120, 180, 255, 40));
        dl->AddRect(ImVec2(std::min(a.x, io.MousePos.x), std::min(a.y, io.MousePos.y)),
                    ImVec2(std::max(a.x, io.MousePos.x), std::max(a.y, io.MousePos.y)), IM_COL32(120, 180, 255, 220), 0.0f, 0, 1.5f);
    }
    // The transform: its pivot, an axis line, and what it is doing.
    if (g.op != Op::None) {
        const ImVec2 c = uvToScreen(g.pivot);
        if (g.axis == 0) dl->AddLine(ImVec2(p0.x, c.y), ImVec2(p1.x, c.y), IM_COL32(230, 70, 70, 200), 1.5f);
        if (g.axis == 1) dl->AddLine(ImVec2(c.x, p0.y), ImVec2(c.x, p1.y), IM_COL32(90, 200, 90, 200), 1.5f);
        if (g.op != Op::Move) dl->AddLine(c, io.MousePos, IM_COL32(255, 255, 255, 120), 1.0f);
        dl->AddCircle(c, 5.0f, IM_COL32(255, 255, 255, 220), 0, 1.5f);
        glm::mat2 a;
        glm::vec2 t;
        std::string hud;
        opParams(a, t, hud);
        hud += "     click or Enter: done   right click or Esc: cancel   X / Y: one axis   Ctrl: snap   type a number";
        dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y + em() * 1.7f), IM_COL32(0, 0, 0, 170));
        dl->AddText(ImVec2(p0.x + em() * 0.5f, p0.y + em() * 0.3f), IM_COL32(255, 255, 255, 255), hud.c_str());
    } else if (g.boxArmed) {
        const char* t = g.boxOn ? "Box: click the other corner  (Shift adds, Ctrl takes away, Esc stops)"
                                : "Box: click one corner";
        dl->AddRectFilled(ImVec2(p0.x, p0.y), ImVec2(p1.x, p0.y + em() * 1.7f), IM_COL32(0, 0, 0, 170));
        dl->AddText(ImVec2(p0.x + em() * 0.5f, p0.y + em() * 0.3f), IM_COL32(255, 255, 255, 255), t);
    }
    dl->PopClipRect();
}

// Under the canvas: where the selection is, as numbers you can step.
void selectionRow(const PanelState& s, const uvops::View& v) {
    glm::vec2 lo, hi;
    const bool any = uvops::bounds(v, g.sel, lo, hi);
    ImGui::BeginDisabled(!any || g.op != Op::None);
    glm::vec2 c = any ? 0.5f * (lo + hi) : glm::vec2(0.0f);
    const glm::vec2 was = c;
    const char* fmt = "%.4f";
    const float sw = em() * 9.0f;
    Flow fl;
    fl.next(Flow::width("Selection  U") + sw);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Selection  U");
    ImGui::SameLine();
    bool moved = ui::stepper("selu", c.x, g.texW > 0 ? 1.0f / float(g.texW) : 0.01f, -100.0f, 100.0f, fmt, sw);
    ImGui::SetItemTooltip("Its middle, in UV; one step is %s.", g.texW > 0 ? "one texel" : "a hundredth of the tile");
    fl.next(Flow::width("V") + sw, em() * 0.5f);
    ImGui::TextUnformatted("V");
    ImGui::SameLine();
    moved |= ui::stepper("selv", c.y, g.texH > 0 ? 1.0f / float(g.texH) : 0.01f, -100.0f, 100.0f, fmt, sw);
    if (moved) {
        const uvops::Selection sel = g.sel;
        const glm::vec2 d = c - was;
        run(s, "UV move", [sel, d](EditMesh& m) { uvops::transform(m, sel, glm::mat2(1.0f), glm::vec2(0.0f), d); });
    }
    const uvops::Selection sel = g.sel;
    const glm::vec2 mid = c;
    float gap = em();
    auto turn = [&](const char* label, const char* tip, glm::mat2 a) {
        fl.next(Flow::width(label), gap);
        gap = 2.0f;
        if (bigButton(label, tip)) run(s, tip, [sel, a, mid](EditMesh& m) { uvops::transform(m, sel, a, mid, glm::vec2(0.0f)); });
    };
    turn("-90", "Rotate -90", glm::mat2(0.0f, -1.0f, 1.0f, 0.0f));
    turn("+90", "Rotate +90", glm::mat2(0.0f, 1.0f, -1.0f, 0.0f));
    turn("Mirror U", "Mirror U", glm::mat2(-1.0f, 0.0f, 0.0f, 1.0f));
    turn("Mirror V", "Mirror V", glm::mat2(1.0f, 0.0f, 0.0f, -1.0f));
    fl.next(Flow::width("Pin"), em());
    if (bigButton("Pin", "Pin  (P): held in place when unwrapping again")) run(s, "Pin UVs", [sel](EditMesh& m) { uvops::setPin(m, sel, true); });
    fl.next(Flow::width("Unpin"));
    if (bigButton("Unpin", "Unpin  (Alt+P)")) run(s, "Unpin UVs", [sel](EditMesh& m) { uvops::setPin(m, sel, false); });
    ImGui::EndDisabled();
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) {
        if (g.op != Op::None) endOp(s, false);
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(em() * 46.0f, em() * 44.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UV editor###UV", &s.show, ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoScrollbar |
                                                     ImGuiWindowFlags_NoScrollWithMouse)) {
        if (g.op != Op::None) endOp(s, false);
        ImGui::End();
        return;
    }
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    if (!s.mesh) {
        if (g.op != Op::None) endOp(s, false);
        ui::sectionText("No editable mesh");
        if (!s.haveSelection) {
            ui::hint("Select an object first. The UV editor unwraps meshes fitzel made --\n"
                     "a shape you made editable. An imported model keeps the UVs its author gave it.");
        } else if (s.canConvert) {
            ui::hint("This shape can become an editable mesh; then it can be unwrapped here.");
            ImGui::Spacing();
            if (bigButton("Make editable", nullptr, -1.0f) && s.convert) s.convert();
        } else {
            ui::hint("Only meshes fitzel made are unwrapped here. An imported model\n"
                     "arrives unwrapped and is left as its author made it.");
        }
        ImGui::End();
        return;
    }
    if (s.ownerId != g.owner) {
        g.owner = s.ownerId;
        g.sel.clear();
        g.fit = true;
        g.msg.clear();
    }

    // A transform in flight: the mesh as the pointer says, before anything is
    // drawn from it.
    MeshComponent* mc = s.mesh;
    if (g.op != Op::None) {
        applyOp(s);
        if (s.meshNow) mc = s.meshNow();
        if (!mc) { g.op = Op::None; ImGui::End(); return; }
    }
    const EditMesh& m = mc->mesh;
    std::vector<int> shown = s.picked;
    if (g.allFaces || shown.empty()) {
        shown.resize(m.faces.size());
        for (std::size_t i = 0; i < shown.size(); ++i) shown[i] = static_cast<int>(i);
    }
    const uvops::View v = uvops::view(m, g.op != Op::None ? g.opFaces : shown);
    uvops::validate(v, g.sel);

    // The texture under the faces: the active face's material, else the object's.
    const MaterialDef* md = nullptr;
    if (m.validFace(s.faceSel)) md = findMaterial(s, m.faceMaterial(s.faceSel));
    if (!md) md = findMaterial(s, s.objectMaterial);
    const fitzel::Texture* tex = md && md->tex ? md->tex.get() : nullptr;
    g.texW = tex ? tex->width() : 0;
    g.texH = tex ? tex->height() : 0;

    menuBar(s, v);
    ImGui::BeginDisabled(g.op != Op::None && !ImGui::IsAnyItemActive());
    toolBar(s, v);
    ImGui::EndDisabled();
    const bool canvasWasHovered = ImGui::IsMouseHoveringRect(g.canvasMin, ImVec2(g.canvasMin.x + g.canvasSize.x,
                                                                                    g.canvasMin.y + g.canvasSize.y));
    if ((focused || canvasWasHovered || g.op != Op::None) && !ImGui::GetIO().WantTextInput && !ImGui::IsAnyItemActive())
        keys(s, v, canvasWasHovered);
    if (ImGui::BeginPopup("##unwrapmenu")) {
        ui::sectionText("Unwrap");
        unwrapItems(s, v);
        ImGui::EndPopup();
    }
    canvas(s, v, tex);
    selectionRow(s, v);

    // The status line.
    int selFaces = 0;
    for (int f : v.faces) selFaces += uvops::faceSelected(g.sel, m, f);
    char line[256];
    std::snprintf(line, sizeof line, "%d of %d faces shown%s  |  %d corners selected (%d faces)  |  %s mode",
                  int(v.faces.size()), int(m.faces.size()), (g.allFaces || s.picked.empty()) ? "" : " (the picked ones)",
                  int(g.sel.size()), selFaces, modeName(g.mode));
    ImGui::AlignTextToFramePadding();
    ui::hint("%s%s%s", line, g.msg.empty() ? "" : "  |  ", g.msg.c_str());

    if (focused) g.focusFrame = ImGui::GetFrameCount();
    ImGui::End();
    // The keys are this window's while it has them (1 2 3 4 change the view
    // elsewhere, X the gizmo's frame): claimed the way a text field does.
    if (focused || canvasWasHovered || g.op != Op::None) ImGui::GetIO().WantTextInput = true;

    if (g.endReq != 0) {
        const bool keep = g.endReq > 0;
        g.endReq = 0;
        endOp(s, keep);
    }
    if (g.pending) {
        std::function<void()> p = std::move(g.pending);
        g.pending = nullptr;
        if (g.op != Op::None) endOp(s, true);
        p();
    }
}

bool hasKeyboard() { return g.focusFrame >= ImGui::GetFrameCount() - 1 || g.op != Op::None; }

uvops::Selection&  selection() { return g.sel; }
uvops::SelectMode& selectMode() { return g.mode; }
ImVec2 toScreen(glm::vec2 uv) { return uvToScreen(uv); }
const std::string& message() { return g.msg; }
bool transforming() { return g.op != Op::None; }


} // namespace uvui
