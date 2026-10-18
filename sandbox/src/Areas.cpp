#include "Areas.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include <imgui.h>
#include <imgui_internal.h>   // dock nodes, settings handlers, FindWindowByName

#include "AreaTree.hpp"
#include "ToolbarIcons.hpp"
#include "UiStyle.hpp"

namespace areas {

namespace {

// The three windows the default arrangement is built from. They are drawn
// every frame by main and have no show flag, so these titles are the only
// handle on them.
constexpr const char* kViewport  = "Scene";
constexpr const char* kHierarchy = "Hierarchy";
constexpr const char* kInspector = "Inspector";

struct State {
    Tree tree;
    // The area a newly opened panel goes to when no area is empty: the last
    // one clicked that showed a panel (or nothing). By area id, since node
    // indices move under splits and joins.
    int  toolArea = -1;
    bool loaded   = false;     // imgui.ini had a layout
    bool reset    = false;
    bool settled  = false;     // the first frame's clean-up has run
    // The area blown up to the whole screen (Ctrl+Space, or its header
    // button), by id; -1 = none. The others keep their windows docked,
    // hidden, until it is put back.
    int  maximized = -1;
    // Each panel's show flag as last seen, to tell "opened since" from "open".
    std::unordered_map<std::string, bool> lastShow;
};

State& st() {
    static State s;
    return s;
}

// The dock node area `areaId` keeps its editor in. A hash of the id rather
// than an ImGui::GetID(), so it does not depend on the ID stack it is asked
// from -- the docking below runs outside the host window.
ImGuiID nodeFor(int areaId) {
    return ImHashData(&areaId, sizeof(areaId), ImHashStr("FitzelArea"));
}

void dirty() { ImGui::MarkIniSettingsDirty(); }

const Editor* findEditor(const std::vector<Editor>& eds, const std::string& title) {
    for (const Editor& e : eds)
        if (title == e.window) return &e;
    return nullptr;
}

// Take `title` out of whatever area held it: off the screen, and out of the
// dock node -- which would otherwise hold it as a hidden second window of the
// next editor shown there.
void release(const std::vector<Editor>& eds, const std::string& title) {
    if (title.empty()) return;
    if (const Editor* e = findEditor(eds, title); e && e->show) {
        *e->show = false;
        st().lastShow[title] = false;
    }
    ImGui::DockBuilderDockWindow(title.c_str(), 0);
}

// Show `title` in `leaf`. If another area already shows it, the two swap: a
// window is one window and cannot be in two places, and swapping is what
// "show this here" means when it is already over there.
void place(const std::vector<Editor>& eds, int leaf, const std::string& title) {
    Tree& t = st().tree;
    const std::string old = t.editor(leaf);
    if (old == title) return;
    const int other = t.findEditor(title);
    if (other >= 0) t.setEditor(other, old);
    else            release(eds, old);
    t.setEditor(leaf, title);
    if (const Editor* e = findEditor(eds, title); e && e->show) {
        *e->show = true;
        st().lastShow[title] = true;
    }
    dirty();
}

// The default: Hierarchy | viewport | Inspector over an empty tool area -- the
// arrangement the dock layout had, with the tool tabs replaced by one area
// whose header picks the tool.
void buildDefault(const std::vector<Editor>& eds) {
    State& s = st();
    for (int n : s.tree.leaves()) release(eds, s.tree.editor(n));
    Tree t;
    const int root = t.root();
    t.setEditor(root, kHierarchy);
    const int centre = t.split(root, Cut::Columns, 0.18f);
    t.setEditor(centre, kViewport);
    const int right = t.split(centre, Cut::Columns, 0.70f);
    t.setEditor(right, kInspector);
    const int tools = t.split(right, Cut::Rows, 0.38f);
    s.tree     = t;
    s.toolArea = s.tree.id(tools);
    dirty();
}

// A 3D view for the new half of a split 3D view: the first one no area shows
// and that is not open elsewhere; -1 when all of them are in use.
const Editor* freeView(const std::vector<Editor>& eds) {
    const Tree& t = st().tree;
    for (const Editor& e : eds)
        if (e.view3d && t.findEditor(e.window) < 0 && (!e.show || !*e.show)) return &e;
    return nullptr;
}

// Where a panel opened from outside the areas (the View menu, the toolbar, a
// double-click on an asset) is put: an empty area if there is one, else the
// tool area. -1 if neither exists -- the panel then floats, as it always did.
int pickTarget(const std::vector<Editor>& eds) {
    const Tree& t = st().tree;
    for (int n : t.leaves())
        if (t.editor(n).empty()) return n;
    const int n = t.leafById(st().toolArea);
    if (n < 0) return -1;
    const Editor* e = findEditor(eds, t.editor(n));
    return (!e || e->show) ? n : -1;    // never over the viewport or the lists
}

// --- imgui.ini --------------------------------------------------------------

void* iniOpen(ImGuiContext*, ImGuiSettingsHandler*, const char* name) {
    return std::strcmp(name, "Layout") == 0 ? &st() : nullptr;
}

void iniLine(ImGuiContext*, ImGuiSettingsHandler*, void*, const char* line) {
    State& s = st();
    int v = 0;
    if (std::strncmp(line, "Tree=", 5) == 0) {
        if (s.tree.load(line + 5)) s.loaded = true;
    } else if (std::sscanf(line, "Tool=%d", &v) == 1) {
        s.toolArea = v;
    }
}

void iniWrite(ImGuiContext*, ImGuiSettingsHandler* h, ImGuiTextBuffer* buf) {
    const State& s = st();
    buf->appendf("[%s][Layout]\n", h->TypeName);
    buf->appendf("Tree=%s\n", s.tree.save().c_str());
    buf->appendf("Tool=%d\n\n", s.toolArea);
}

// --- Drawing ------------------------------------------------------------------

// The header's buttons: icon::button's blank, with a picture drawn on it.
enum class Glyph { Columns, Rows, Join, Maximize, Restore };

bool glyphButton(const char* id, Glyph g, float size, const char* tip, bool disabled) {
    ImVec2 c;
    const bool hit = icon::button(id, ImVec2(size, size), tip, disabled, c);
    ImDrawList* dl  = ImGui::GetWindowDrawList();
    const ImU32 col = disabled ? icon::kDim : icon::kOff;
    const float r   = std::floor(size * 0.27f);
    const ImVec2 a(c.x - r, c.y - r), b(c.x + r, c.y + r);
    switch (g) {
    case Glyph::Columns:
        dl->AddRect(a, b, col, 1.5f, 0, 1.5f);
        dl->AddLine({c.x, a.y}, {c.x, b.y}, col, 1.5f);
        break;
    case Glyph::Rows:
        dl->AddRect(a, b, col, 1.5f, 0, 1.5f);
        dl->AddLine({a.x, c.y}, {b.x, c.y}, col, 1.5f);
        break;
    case Glyph::Maximize:     // corners pushed outwards
    case Glyph::Restore: {    // ...and pulled in
        const float o = g == Glyph::Maximize ? r : r * 0.45f;
        const float l = r * 0.55f;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) {
                const ImVec2 k(c.x + sx * o, c.y + sy * o);
                const float  hx = g == Glyph::Maximize ? -sx * l : sx * l;
                const float  hy = g == Glyph::Maximize ? -sy * l : sy * l;
                dl->AddLine(k, ImVec2(k.x + hx, k.y), col, 1.5f);
                dl->AddLine(k, ImVec2(k.x, k.y + hy), col, 1.5f);
            }
        break;
    }
    case Glyph::Join: {
        const float k = r * 0.8f;
        dl->AddLine({c.x - k, c.y - k}, {c.x + k, c.y + k}, col, 1.5f);
        dl->AddLine({c.x - k, c.y + k}, {c.x + k, c.y - k}, col, 1.5f);
        break;
    }
    }
    return hit;
}

// The grip at the header's left end -- Blender's corner. Dragged into its own
// area it cuts it where it is let go (sideways: side by side, down: top and
// bottom); dragged over the neighbour the area was cut from, it takes that
// neighbour's space. Returns true while it is being dragged.
struct GripDrop {
    int leaf = -1;              // area the grip belongs to
    int join = -1;              // ...joins this one away, or
    Cut cut  = Cut::Columns;    // ...is cut this way
    float ratio = -1.0f;        // ...at this share (< 0: nothing to do)
};

void drawGrip(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col) {
    const float w = b.x - a.x, h = b.y - a.y;
    for (int i = 1; i <= 3; ++i) {
        const float k = 0.22f * static_cast<float>(i);
        dl->AddLine(ImVec2(a.x + w * 0.2f, a.y + h * (0.2f + k)),
                    ImVec2(a.x + w * (0.2f + k * w / h), a.y + h * 0.2f), col, 1.2f);
    }
}

// What letting the grip of `leaf` (drawn at `r`) go at `mouse` would do.
GripDrop gripDrop(const Tree& t, int leaf, const Rect& r, ImVec2 start, ImVec2 mouse,
                  float gap, float minA, float fs) {
    GripDrop d;
    d.leaf = leaf;
    const bool inside = mouse.x >= r.x && mouse.x < r.x + r.w &&
                        mouse.y >= r.y && mouse.y < r.y + r.h;
    if (inside) {
        const float dx = mouse.x - start.x, dy = mouse.y - start.y;
        if (std::max(std::fabs(dx), std::fabs(dy)) < fs) return d;   // not a drag yet
        const bool  cols = std::fabs(dx) >= std::fabs(dy);
        const float at   = cols ? mouse.x - r.x : mouse.y - r.y;
        const float ext  = cols ? r.w : r.h;
        if (at < minA || ext - at < minA) return d;                   // a half too thin
        d.cut   = cols ? Cut::Columns : Cut::Rows;
        d.ratio = (at - gap * 0.5f) / (ext - gap);
        return d;
    }
    // Over another area: only the one this area was cut from (its sibling)
    // can be joined over -- anything else would not leave a rectangle.
    const int p = t.parent(leaf);
    if (p < 0) return d;
    const int sib = t.first(p) == leaf ? t.second(p) : t.first(p);
    if (!t.isLeaf(sib)) return d;
    const Rect& o = t.rect(sib);
    if (mouse.x >= o.x && mouse.x < o.x + o.w && mouse.y >= o.y && mouse.y < o.y + o.h)
        d.join = sib;
    return d;
}

// While the grip is held: the cut it would make, or the area it would take
// over. On the foreground list -- the line runs over the editor's window.
void gripPreview(const Tree& t, const GripDrop& d, const Rect& r, float gap) {
    ImDrawList* fg  = ImGui::GetForegroundDrawList();
    const ImU32 acc = icon::on();
    if (d.join >= 0) {
        const Rect& o = t.rect(d.join);
        fg->AddRectFilled(ImVec2(o.x, o.y), ImVec2(o.x + o.w, o.y + o.h), IM_COL32(0, 0, 0, 110));
        fg->AddRect(ImVec2(r.x, r.y), ImVec2(o.x + o.w > r.x + r.w ? o.x + o.w : r.x + r.w,
                                             o.y + o.h > r.y + r.h ? o.y + o.h : r.y + r.h),
                    acc, 0.0f, 0, 2.0f);
        const char*  msg = "Join over this area";
        const ImVec2 ts  = ImGui::CalcTextSize(msg);
        fg->AddText(ImVec2(std::round(o.x + (o.w - ts.x) * 0.5f),
                           std::round(o.y + (o.h - ts.y) * 0.5f)), acc, msg);
        return;
    }
    if (d.ratio <= 0.0f) return;
    if (d.cut == Cut::Columns) {
        const float x = std::round(r.x + gap * 0.5f + d.ratio * (r.w - gap));
        fg->AddLine(ImVec2(x, r.y), ImVec2(x, r.y + r.h), acc, 2.0f);
    } else {
        const float y = std::round(r.y + gap * 0.5f + d.ratio * (r.h - gap));
        fg->AddLine(ImVec2(r.x, y), ImVec2(r.x + r.w, y), acc, 2.0f);
    }
}

// The editor menu: every editor under its group, the one shown ticked. Picking
// one shows it here (or swaps it over from where it was).
void editorMenu(const std::vector<Editor>& eds, int leaf, float width) {
    Tree& t = st().tree;
    const std::string& cur = t.editor(leaf);
    const Editor* shown = findEditor(eds, cur);
    ImGui::SetNextItemWidth(width);
    if (!ImGui::BeginCombo("##editor", shown ? shown->label : "(empty)",
                           ImGuiComboFlags_HeightLargest))
        return;
    if (ImGui::Selectable("(empty)", cur.empty()) && !cur.empty()) {
        release(eds, cur);
        t.setEditor(leaf, "");
        dirty();
    }
    const char* group = nullptr;
    for (const Editor& e : eds) {
        if (!group || std::strcmp(group, e.group) != 0) {
            group = e.group;
            ui::sectionText(group);
        }
        // Where it is now, if not here: picking it swaps the two areas.
        const int at = t.findEditor(e.window);
        if (ImGui::Selectable(e.label, at == leaf)) place(eds, leaf, e.window);
        if (at >= 0 && at != leaf && ImGui::IsItemHovered())
            ImGui::SetTooltip("Shown in another area -- picking it swaps the two");
    }
    ImGui::EndCombo();
}

} // namespace

void install() {
    ImGuiSettingsHandler h;
    h.TypeName   = "FitzelAreas";
    h.TypeHash   = ImHashStr("FitzelAreas");
    h.ReadOpenFn = iniOpen;
    h.ReadLineFn = iniLine;
    h.WriteAllFn = iniWrite;
    ImGui::AddSettingsHandler(&h);
}

void requestReset() { st().reset = true; }

void frame(const std::vector<Editor>& eds) {
    State& s = st();
    Tree&  t = s.tree;

    if (!s.loaded || s.reset) {
        buildDefault(eds);
        s.loaded = true;
        s.reset  = false;
    }
    // A title that names no editor any more (an older layout, a renamed
    // panel) leaves its area empty rather than naming a window nobody opens.
    for (int n : t.leaves())
        if (!t.editor(n).empty() && !findEditor(eds, t.editor(n))) {
            t.setEditor(n, "");
            dirty();
        }
    // First frame: every window no area shows is taken out of whatever dock
    // node imgui.ini left it in -- the old fixed dock layout's, or an area's
    // the layout no longer agrees with.
    if (!s.settled) {
        s.settled = true;
        for (const Editor& e : eds) {
            if (t.findEditor(e.window) < 0) ImGui::DockBuilderDockWindow(e.window, 0);
            if (e.show) s.lastShow[e.window] = *e.show;
        }
    }

    // --- The panels' own flags --------------------------------------------
    // A flag turned off since last frame (the View menu, "Close all panels",
    // the toolbar) empties its area; one turned on puts the panel in one.
    for (const Editor& e : eds) {
        if (!e.show) continue;
        bool&     was  = s.lastShow[e.window];
        const int leaf = t.findEditor(e.window);
        if (leaf >= 0) {
            if (was && !*e.show) {
                t.setEditor(leaf, "");
                ImGui::DockBuilderDockWindow(e.window, 0);
                dirty();
            } else {
                *e.show = true;
            }
        } else if (*e.show && !was) {
            if (const int to = pickTarget(eds); to >= 0) place(eds, to, e.window);
        }
        was = *e.show;
    }

    // --- Layout ----------------------------------------------------------------
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float fs     = ImGui::GetFontSize();
    const float pad    = std::max(2.0f, std::round(fs * 0.15f));
    const float gap    = std::max(3.0f, std::round(fs * 0.25f));
    const float btn    = ImGui::GetFrameHeight();
    const float headH  = btn + pad * 2.0f;
    const float minA   = headH * 2.0f;
    const Rect  screen{vp->WorkPos.x, vp->WorkPos.y, vp->WorkSize.x, vp->WorkSize.y};
    t.layout(screen, gap, minA);
    if (s.maximized >= 0 && t.leafById(s.maximized) < 0) s.maximized = -1;
    const int maxLeaf = s.maximized >= 0 ? t.leafById(s.maximized) : -1;
    // Where an area is drawn: its own rect, or the whole screen when it is
    // the maximized one.
    auto areaRect = [&](int leaf) { return leaf == maxLeaf ? screen : t.rect(leaf); };

    // One borderless host over the work area, behind every floating window --
    // the backdrop the dock space used to be. It draws the headers and the
    // dividers; the editors sit in dock spaces inside it.
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##Areas", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                 ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGuiWindow* host = ImGui::GetCurrentWindow();
    ImDrawList*  dl   = ImGui::GetWindowDrawList();

    // The gaps between areas read as seams: a shade darker than a panel.
    ImVec4 seam = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
    seam.x *= 0.6f; seam.y *= 0.6f; seam.z *= 0.6f; seam.w = 1.0f;
    dl->AddRectFilled(vp->WorkPos, ImVec2(vp->WorkPos.x + vp->WorkSize.x,
                                          vp->WorkPos.y + vp->WorkSize.y),
                      ImGui::GetColorU32(seam));

    // --- Dividers ------------------------------------------------------------
    // Dragged before the areas are drawn, so they follow the pointer this frame.
    // None while one area has the screen.
    for (int n : maxLeaf >= 0 ? std::vector<int>{} : t.cuts()) {
        const Rect d = t.divider(n);
        if (d.w < 1.0f || d.h < 1.0f) continue;
        const bool cols = t.cut(n) == Cut::Columns;
        ImGui::SetCursorScreenPos(ImVec2(d.x, d.y));
        ImGui::PushID(n);
        ImGui::InvisibleButton("##cut", ImVec2(d.w, d.h));
        ImGui::PopID();
        const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (hot) ImGui::SetMouseCursor(cols ? ImGuiMouseCursor_ResizeEW
                                            : ImGuiMouseCursor_ResizeNS);
        if (ImGui::IsItemActive()) {
            const Rect&  c   = t.rect(n);
            const ImVec2 m   = ImGui::GetIO().MousePos;
            const float  ext = (cols ? c.w : c.h) - gap;
            if (ext > 1.0f) {
                const float at = cols ? m.x - c.x : m.y - c.y;
                t.setRatio(n, (at - gap * 0.5f) / ext);
                t.layout(screen, gap, minA);
            }
        }
        if (ImGui::IsItemDeactivated()) dirty();
        if (hot) {
            const Rect dn = t.divider(n);
            dl->AddRectFilled(ImVec2(dn.x, dn.y), ImVec2(dn.x + dn.w, dn.y + dn.h),
                              ImGui::GetColorU32(ImGui::IsItemActive()
                                                     ? ImGuiCol_SeparatorActive
                                                     : ImGuiCol_SeparatorHovered));
        }
    }

    // --- Areas -----------------------------------------------------------------
    // Clicks are read before any header button changes the tree.
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                         ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    const bool overUs  = g.HoveredWindow && g.HoveredWindow->RootWindowDockTree == host;

    // Ctrl+Space over an area: it takes the screen, or gives it back. Not
    // while a field has the keyboard -- there Ctrl+Space is completion.
    const ImGuiIO& io = ImGui::GetIO();
    if (overUs && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Space, false) &&
        !io.WantTextInput && !ImGui::IsAnyItemActive()) {
        if (maxLeaf >= 0) {
            s.maximized = -1;
        } else {
            for (int leaf : t.leaves()) {
                const Rect r = t.rect(leaf);
                if (ImGui::IsMouseHoveringRect(ImVec2(r.x, r.y),
                                               ImVec2(r.x + r.w, r.y + r.h), false))
                    s.maximized = t.id(leaf);
            }
        }
    }

    int joinLeaf = -1, splitLeaf = -1;
    Cut splitHow = Cut::Columns;
    float splitRatio = 0.5f;
    bool toggleMax = false;
    int  toggleMaxId = -1;
    const bool lastArea = t.areaCount() <= 1;
    const ImGuiDockNodeFlags dockFlags =
        // Passthru: an empty area shows the host (and its hint) instead of
        // the dock's empty fill.
        static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_PassthruCentralNode) |
        ImGuiDockNodeFlags_NoUndocking | ImGuiDockNodeFlags_NoResize |
        // (the private half of the flags is a second enum)
        static_cast<ImGuiDockNodeFlags>(ImGuiDockNodeFlags_NoTabBar | ImGuiDockNodeFlags_NoDocking |
                                        ImGuiDockNodeFlags_NoWindowMenuButton |
                                        ImGuiDockNodeFlags_NoCloseButton);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    for (int leaf : t.leaves()) {
        const int areaId = t.id(leaf);
        // Behind a maximized area the others only keep their windows docked
        // (and hidden), so putting it back finds everything where it was.
        if (maxLeaf >= 0 && leaf != maxLeaf) {
            ImGui::DockSpace(nodeFor(areaId), ImVec2(0.0f, 0.0f),
                             dockFlags | ImGuiDockNodeFlags_KeepAliveOnly);
            continue;
        }
        const Rect r = areaRect(leaf);
        if (r.w < 1.0f || r.h < 1.0f) continue;
        ImGui::PushID(areaId);

        const ImVec2 p0(r.x, r.y), p1(r.x + r.w, r.y + r.h);
        const float  hy = std::min(r.y + headH, p1.y);
        if (clicked && overUs && ImGui::IsMouseHoveringRect(p0, p1, false)) {
            const Editor* e = findEditor(eds, t.editor(leaf));
            if (!e || e->show) s.toolArea = areaId;
        }

        // The header strip.
        dl->AddRectFilled(p0, ImVec2(p1.x, hy), ImGui::GetColorU32(ImGuiCol_MenuBarBg));
        dl->AddRectFilled(ImVec2(r.x, hy), p1, ImGui::GetColorU32(ImGuiCol_WindowBg));
        ImGui::PushClipRect(p0, ImVec2(p1.x, hy), true);
        const bool  isMax = leaf == maxLeaf;
        // icon::button keeps them on one line, ItemSpacing apart.
        const float sp      = ImGui::GetStyle().ItemSpacing.x;
        const float buttons = btn * 4.0f + sp * 3.0f;
        const float gripW   = std::round(btn * 0.6f);

        // The grip, Blender's corner: drag it to cut or join (see GripDrop).
        ImGui::SetCursorScreenPos(ImVec2(r.x, r.y));
        ImGui::BeginDisabled(isMax);
        ImGui::InvisibleButton("##grip", ImVec2(gripW + pad, headH));
        ImGui::EndDisabled();
        const bool gripHot = ImGui::IsItemHovered() || ImGui::IsItemActive();
        if (ImGui::IsItemHovered() && !ImGui::IsItemActive())
            ImGui::SetTooltip("Drag into the area to split it,\n"
                              "onto its neighbour to join over it");
        if (gripHot) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        drawGrip(dl, ImVec2(r.x + pad, r.y + pad), ImVec2(r.x + pad + gripW, r.y + pad + btn),
                 ImGui::GetColorU32(gripHot ? ImGuiCol_Text : ImGuiCol_TextDisabled));
        const bool gripActive = ImGui::IsItemActive(), gripReleased = ImGui::IsItemDeactivated();
        if (gripActive || gripReleased) {
            const GripDrop d = gripDrop(t, leaf, r, io.MouseClickedPos[0], io.MousePos,
                                        gap, minA, fs);
            if (gripActive) gripPreview(t, d, r, gap);
            else if (d.join >= 0) joinLeaf = d.join;
            else if (d.ratio > 0.0f) {
                splitLeaf  = leaf;
                splitHow   = d.cut;
                splitRatio = d.ratio;
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(r.x + pad + gripW + pad, r.y + pad));
        editorMenu(eds, leaf, std::max(btn * 2.0f,
                                       std::min(fs * 12.0f,
                                                r.w - buttons - gripW - pad * 4.0f)));
        ImGui::SetCursorScreenPos(ImVec2(std::max(r.x + pad, p1.x - pad - buttons), r.y + pad));
        ImGui::BeginDisabled(isMax);
        if (glyphButton("cols", Glyph::Columns, btn, "Split side by side", false)) {
            splitLeaf = leaf;
            splitHow  = Cut::Columns;
        }
        if (glyphButton("rows", Glyph::Rows, btn, "Split top and bottom", false)) {
            splitLeaf = leaf;
            splitHow  = Cut::Rows;
        }
        ImGui::EndDisabled();
        if (glyphButton("max", isMax ? Glyph::Restore : Glyph::Maximize, btn,
                        isMax ? "Back to the other areas (Ctrl+Space)"
                              : "Take the whole screen (Ctrl+Space)", false)) {
            toggleMax   = true;
            toggleMaxId = areaId;
        }
        if (glyphButton("join", Glyph::Join, btn,
                        lastArea ? "The last area cannot be joined away"
                        : isMax  ? "Put the area back first"
                                 : "Join -- give this area back to its neighbour",
                        lastArea || isMax))
            joinLeaf = leaf;
        ImGui::NewLine();
        ImGui::PopClipRect();

        // The editor's dock space: one window, no tab bar, nothing docks in by
        // dragging and nothing drags out -- the header is the only way in.
        const float ch = p1.y - hy;
        if (ch >= 1.0f) {
            ImGui::SetCursorScreenPos(ImVec2(r.x, hy));
            ImGui::DockSpace(nodeFor(areaId), ImVec2(r.w, ch), dockFlags);
            if (t.editor(leaf).empty()) {
                const char*  msg = "Empty -- pick an editor in the header";
                const ImVec2 ts  = ImGui::CalcTextSize(msg);
                if (ts.x < r.w && ts.y < ch)
                    dl->AddText(ImVec2(std::round(r.x + (r.w - ts.x) * 0.5f),
                                       std::round(hy + (ch - ts.y) * 0.5f)),
                                ImGui::GetColorU32(ImGuiCol_TextDisabled), msg);
            }
        }
        ImGui::PopID();
    }
    ImGui::PopStyleColor();
    ImGui::End();

    // Structural changes after the loop, which walked the old tree.
    if (toggleMax) {
        s.maximized = s.maximized == toggleMaxId ? -1 : toggleMaxId;
    } else if (splitLeaf >= 0) {
        const Editor* src  = findEditor(eds, t.editor(splitLeaf));
        const int     half = t.split(splitLeaf, splitHow, splitRatio);
        if (src && src->view3d)
            if (const Editor* v = freeView(eds)) place(eds, half, v->window);
        dirty();
    } else if (joinLeaf >= 0) {
        std::string dropped;
        if (t.join(joinLeaf, &dropped)) {
            release(eds, dropped);
            dirty();
        }
    }

    // --- Windows into their areas ----------------------------------------------
    // Before any editor's Begin, so a move lands this frame. Docking by title
    // reaches a window main has not created yet just the same (it goes into
    // the window's settings).
    for (const Editor& e : eds) {
        const int leaf = t.findEditor(e.window);
        ImGuiWindow* w = ImGui::FindWindowByName(e.window);
        if (leaf >= 0) {
            const ImGuiID node = nodeFor(t.id(leaf));
            if (!w || w->DockId != node) ImGui::DockBuilderDockWindow(e.window, node);
        } else if (!e.show && w) {
            // Drawn every frame, shown nowhere: hidden. Its Begin returns false
            // (2, because Begin counts it down before looking).
            w->HiddenFramesCanSkipItems = 2;
        }
    }
}

bool showing(const std::string& title) {
    const State& s = st();
    const int leaf = s.tree.findEditor(title);
    if (leaf < 0) return false;
    return s.maximized < 0 || s.tree.id(leaf) == s.maximized;
}

bool showInstead(const std::string& title, const std::string& instead, bool* insteadKept) {
    State& s = st();
    const int leaf = s.tree.findEditor(instead);
    if (leaf < 0) return false;
    // Wherever `title` was shown, it moves here: one window, one area.
    const int old = s.tree.findEditor(title);
    if (old >= 0) s.tree.setEditor(old, instead);
    s.tree.setEditor(leaf, title);
    // Out of the area's dock node: with `title` docked there next frame, the
    // two windows would otherwise share it. (A window with a show flag that is
    // still on would come back somewhere; the caller closes it, see startPlay.)
    if (old < 0) ImGui::DockBuilderDockWindow(instead.c_str(), 0);
    if (insteadKept) *insteadKept = old >= 0;
    dirty();
    return true;
}

} // namespace areas
