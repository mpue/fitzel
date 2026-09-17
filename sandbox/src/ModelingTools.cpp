#include "ModelingTools.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "Component.hpp"
#include "SandboxMath.hpp"
#include "UiStyle.hpp"

namespace modeltools {

namespace {

// --- Colours -----------------------------------------------------------------
// Orange is "selected" everywhere in the editor; the pointer's candidate is a
// cool blue so the two can never be mistaken for each other; the preview is cyan
// for what an operation adds and red for what it takes away.
const ImU32 kWire      = IM_COL32(225, 232, 240, 120);
const ImU32 kWireBack  = IM_COL32(225, 232, 240, 30);
const ImU32 kVert      = IM_COL32(225, 230, 236, 210);
const ImU32 kVertBack  = IM_COL32(225, 230, 236, 55);
const ImU32 kSel       = IM_COL32(255, 175, 45, 255);
const ImU32 kSelFill   = IM_COL32(255, 170, 40, 55);
const ImU32 kSelEdge   = IM_COL32(255, 195, 70, 235);
const ImU32 kHover     = IM_COL32(120, 200, 255, 235);
const ImU32 kHoverFill = IM_COL32(110, 190, 255, 38);
const ImU32 kAdd       = IM_COL32(80, 228, 255, 235);
const ImU32 kAddFill   = IM_COL32(80, 228, 255, 45);
const ImU32 kRemove    = IM_COL32(245, 90, 80, 220);
const ImU32 kOutline   = IM_COL32(20, 20, 24, 230);

// Pick radii in pixels. Generous on purpose: the pointer of an unsteady hand
// lands near a corner, rarely on it.
// They grow with the UI font, which is how this editor knows the display scale.
float vertRadius() { return std::max(18.0f, ImGui::GetFontSize() * 0.9f); }
float edgeRadius() { return std::max(13.0f, ImGui::GetFontSize() * 0.65f); }
// Marker sizes on the same scale: 1.0 at a 17 px font.
float ui() { return std::max(1.0f, ImGui::GetFontSize() / 17.0f); }

// How long an edit's changed edges glow, and how long its name stays up.
constexpr double kFlashSecs = 1.2;
constexpr double kToastSecs = 1.6;

// --- State ---------------------------------------------------------------------
Amounts g_amounts;

Op g_preview;   // set by a hovered button, consumed by the next overlay

struct Flash {
    std::vector<std::pair<glm::vec3, glm::vec3>> segs;   // world space
    double      t0 = -1.0;
    std::string label;
} g_flash;

// Where the last edit's name is written: under the Modeling panel, which is the
// thing that was just pressed. Reported by the panel every frame it draws.
ImVec2 g_panelMin{0, 0}, g_panelMax{0, 0};
int    g_panelFrame = -10;

// --- Geometry helpers ------------------------------------------------------------
glm::vec3 xf(const glm::mat4& m, const glm::vec3& p) {
    return glm::vec3(m * glm::vec4(p, 1.0f));
}

ImVec2 toPx(const View& v, const glm::vec4& c) {
    const glm::vec3 n = glm::vec3(c) / c.w;
    return ImVec2(v.min.x + (n.x * 0.5f + 0.5f) * v.size.x,
                  v.min.y + (1.0f - (n.y * 0.5f + 0.5f)) * v.size.y);
}

// A world point on screen; false when it is behind the camera.
bool point(const View& v, const glm::vec3& w, ImVec2& out) {
    const glm::vec4 c = v.vp * glm::vec4(w, 1.0f);
    if (c.w <= 1e-3f) return false;
    out = toPx(v, c);
    return true;
}

// A world segment on screen, cut at the near plane so an edge running past the
// camera still draws the part in front of it.
bool segment(const View& v, const glm::vec3& wa, const glm::vec3& wb, ImVec2& pa, ImVec2& pb) {
    const float eps = 0.02f;
    glm::vec4 a = v.vp * glm::vec4(wa, 1.0f);
    glm::vec4 b = v.vp * glm::vec4(wb, 1.0f);
    if (a.w < eps && b.w < eps) return false;
    if (a.w < eps)      a = glm::mix(a, b, (eps - a.w) / (b.w - a.w));
    else if (b.w < eps) b = glm::mix(b, a, (eps - b.w) / (a.w - b.w));
    pa = toPx(v, a);
    pb = toPx(v, b);
    return true;
}

// The ray under a pixel, in world space.
void rayAt(const View& v, ImVec2 px, glm::vec3& ro, glm::vec3& rd) {
    const float nx = (px.x - v.min.x) / std::max(v.size.x, 1.0f) * 2.0f - 1.0f;
    const float ny = 1.0f - (px.y - v.min.y) / std::max(v.size.y, 1.0f) * 2.0f;
    const glm::mat4 inv = glm::inverse(v.vp);
    glm::vec4 pn = inv * glm::vec4(nx, ny, -1.0f, 1.0f); pn /= pn.w;
    glm::vec4 pf = inv * glm::vec4(nx, ny,  1.0f, 1.0f); pf /= pf.w;
    ro = glm::vec3(pn);
    rd = glm::normalize(glm::vec3(pf) - glm::vec3(pn));
}

std::vector<glm::vec3> worldVerts(const EditMesh& m, const glm::mat4& model) {
    std::vector<glm::vec3> w;
    w.reserve(m.verts.size());
    for (const glm::vec3& p : m.verts) w.push_back(xf(model, p));
    return w;
}

// Does face `f` turn towards the camera? Read off its winding on screen: faces
// are counter-clockwise seen from outside, and the pixel rows run downwards, so
// a face you look at from outside has a negative signed area there. A face
// reaching behind the camera counts as facing it -- it is right in front of you.
bool frontFacing(const EditMesh& m, const std::vector<glm::vec3>& w, int f, const View& v) {
    const std::vector<int>& fv = m.faces[f];
    if (fv.size() < 3) return false;
    float area = 0.0f;
    ImVec2 first, prev;
    for (std::size_t i = 0; i < fv.size(); ++i) {
        ImVec2 p;
        if (!point(v, w[fv[i]], p)) return true;
        if (i == 0) first = p;
        else area += prev.x * p.y - p.x * prev.y;
        prev = p;
    }
    area += prev.x * first.y - first.x * prev.y;
    return area < 0.0f;
}

// Is anything of the mesh between the camera and `p`? Faces `skip` says belong
// to the element itself are not in the way of it.
bool occluded(const EditMesh& m, const std::vector<glm::vec3>& w, const View& v,
              const glm::vec3& p, const std::function<bool(int)>& skip) {
    ImVec2 px;
    if (!point(v, p, px)) return true;
    glm::vec3 ro, rd;
    rayAt(v, px, ro, rd);
    const float dist = glm::length(p - ro);
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        if (skip(f)) continue;
        const std::vector<int>& fv = m.faces[f];
        for (std::size_t i = 1; i + 1 < fv.size(); ++i) {
            const float t = rayTriangle(ro, rd, w[fv[0]], w[fv[i]], w[fv[i + 1]]);
            if (t >= 0.0f && t < dist * 0.999f - 1e-3f) return true;
        }
    }
    return false;
}

bool faceHas(const EditMesh& m, int f, int vert) {
    const std::vector<int>& fv = m.faces[f];
    return std::find(fv.begin(), fv.end(), vert) != fv.end();
}

float distToSegment(ImVec2 p, ImVec2 a, ImVec2 b) {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float l2 = dx * dx + dy * dy;
    float t = l2 > 1e-6f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / l2 : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = a.x + t * dx - p.x, ey = a.y + t * dy - p.y;
    return std::sqrt(ex * ex + ey * ey);
}

// Edges compared by where they ARE, not by index: an operation renumbers freely,
// and what the eye sees as "the same edge" is the same two positions.
using PKey = std::array<std::int64_t, 3>;
using EKey = std::array<std::int64_t, 6>;
PKey pkey(const glm::vec3& p) {
    return {std::llround(p.x * 1e4), std::llround(p.y * 1e4), std::llround(p.z * 1e4)};
}
EKey ekey(const glm::vec3& a, const glm::vec3& b) {
    PKey ka = pkey(a), kb = pkey(b);
    if (kb < ka) std::swap(ka, kb);
    return {ka[0], ka[1], ka[2], kb[0], kb[1], kb[2]};
}

// The edges `after` has that `before` did not (added) and the reverse (removed),
// in mesh space.
void edgeDiff(const EditMesh& before, const EditMesh& after,
              std::vector<std::pair<glm::vec3, glm::vec3>>* added,
              std::vector<std::pair<glm::vec3, glm::vec3>>* removed) {
    std::set<EKey> eb, ea;
    for (const editmesh::EdgeInfo& e : editmesh::edges(before))
        eb.insert(ekey(before.verts[e.a], before.verts[e.b]));
    for (const editmesh::EdgeInfo& e : editmesh::edges(after)) {
        const glm::vec3& a = after.verts[e.a];
        const glm::vec3& b = after.verts[e.b];
        const EKey k = ekey(a, b);
        ea.insert(k);
        if (added && !eb.count(k)) added->push_back({a, b});
    }
    std::vector<std::pair<glm::vec3, glm::vec3>> gone;
    for (const editmesh::EdgeInfo& e : editmesh::edges(before)) {
        const glm::vec3& a = before.verts[e.a];
        const glm::vec3& b = before.verts[e.b];
        if (!ea.count(ekey(a, b))) gone.push_back({a, b});
    }

    // An edge that was only SPLIT is neither new nor gone: its halves lie on it.
    // Shown as cyan halves over a red whole it would drown the one thing the
    // eye should find -- the cut itself -- so both sides of a split drop out.
    auto onSeg = [](const glm::vec3& p, const std::pair<glm::vec3, glm::vec3>& s) {
        const glm::vec3 d = s.second - s.first;
        const float l2 = glm::dot(d, d);
        if (l2 < 1e-12f) return false;
        const float t = glm::dot(p - s.first, d) / l2;
        if (t < -1e-4f || t > 1.0f + 1e-4f) return false;
        return glm::length(s.first + d * t - p) < 1e-4f;
    };
    std::vector<char> split(gone.size(), 0);
    if (added) {
        std::vector<std::pair<glm::vec3, glm::vec3>> kept;
        for (const auto& n : *added) {
            bool half = false;
            for (std::size_t g = 0; g < gone.size(); ++g)
                if (onSeg(n.first, gone[g]) && onSeg(n.second, gone[g])) {
                    half = true;
                    split[g] = 1;
                }
            if (!half) kept.push_back(n);
        }
        *added = std::move(kept);
    }
    if (removed)
        for (std::size_t g = 0; g < gone.size(); ++g)
            if (!split[g]) removed->push_back(gone[g]);
}

// A face's outline and fill on screen (convex fan, like the GPU draws it).
void drawFace(ImDrawList* dl, const std::vector<glm::vec3>& w, const std::vector<int>& fv,
              const View& v, ImU32 fill, ImU32 line, float thick) {
    std::vector<ImVec2> pts;
    pts.reserve(fv.size());
    for (int i : fv) {
        ImVec2 p;
        if (!point(v, w[i], p)) return;
        pts.push_back(p);
    }
    if (pts.size() < 3) return;
    if (fill) dl->AddConvexPolyFilled(pts.data(), static_cast<int>(pts.size()), fill);
    if (line) dl->AddPolyline(pts.data(), static_cast<int>(pts.size()), line,
                              ImDrawFlags_Closed, thick);
}

bool hasEdge(const Selection& s, int a, int b) {
    const std::pair<int, int> k{std::min(a, b), std::max(a, b)};
    return std::find(s.edges.begin(), s.edges.end(), k) != s.edges.end();
}

} // namespace

// =============================================================================
// Selection
// =============================================================================

Amounts& amounts() { return g_amounts; }

void validate(Selection& s, int ownerId, const EditMesh* mesh) {
    if (ownerId != s.owner || !mesh) {
        s.owner = ownerId;
        s.clear();
        if (!mesh) g_preview = nullptr;
        return;
    }
    const int nv = static_cast<int>(mesh->verts.size());
    s.verts.erase(std::remove_if(s.verts.begin(), s.verts.end(),
                                 [nv](int i) { return i < 0 || i >= nv; }),
                  s.verts.end());
    s.edges.erase(std::remove_if(s.edges.begin(), s.edges.end(),
                                 [nv](const std::pair<int, int>& e) {
                                     return e.first < 0 || e.second >= nv ||
                                            e.first == e.second;
                                 }),
                  s.edges.end());
    if (s.mode == Mode::Face) s.clear();
}

std::vector<int> activeVerts(const Selection& s, const EditMesh& m, int faceSel) {
    std::vector<int> out;
    if (s.mode == Mode::Face) {
        if (m.validFace(faceSel)) out = m.faces[faceSel];
    } else if (s.mode == Mode::Vertex) {
        out = s.verts;
    } else {
        for (const auto& e : s.edges) { out.push_back(e.first); out.push_back(e.second); }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](int i) { return i < 0 || i >= static_cast<int>(m.verts.size()); }),
              out.end());
    return out;
}

void setMode(Selection& s, Mode mode, const EditMesh& m, int& faceSel) {
    if (s.mode == mode) return;
    const std::vector<int> corners = activeVerts(s, m, faceSel);
    s.clear();
    s.mode = mode;
    if (mode == Mode::Vertex) {
        s.verts = corners;
        faceSel = -1;
    } else if (mode == Mode::Edge) {
        // Every edge both of whose ends were selected.
        for (const editmesh::EdgeInfo& e : editmesh::edges(m))
            if (std::binary_search(corners.begin(), corners.end(), e.a) &&
                std::binary_search(corners.begin(), corners.end(), e.b))
                s.edges.push_back({std::min(e.a, e.b), std::max(e.a, e.b)});
        faceSel = -1;
    } else {
        // The first face all of whose corners were selected.
        faceSel = -1;
        for (int f = 0; f < static_cast<int>(m.faces.size()) && faceSel < 0 && !corners.empty(); ++f) {
            bool all = m.validFace(f);
            for (int i : m.faces[f])
                all = all && std::binary_search(corners.begin(), corners.end(), i);
            if (all) faceSel = f;
        }
    }
}

// =============================================================================
// Picking
// =============================================================================

Hit pick(const EditMesh& m, const View& v, ImVec2 mouse, Mode mode) {
    Hit h;
    if (m.verts.empty()) return h;
    const std::vector<glm::vec3> w = worldVerts(m, v.model);

    // The face under the pointer -- the same fan the GPU mesh is built from, so
    // what is picked is exactly what is drawn.
    glm::vec3 ro, rd;
    rayAt(v, mouse, ro, rd);
    float faceT = 1e30f;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        const std::vector<int>& fv = m.faces[f];
        for (std::size_t i = 1; i + 1 < fv.size(); ++i) {
            const float t = rayTriangle(ro, rd, w[fv[0]], w[fv[i]], w[fv[i + 1]]);
            if (t >= 0.0f && t < faceT) { faceT = t; h.face = f; }
        }
    }
    h.onMesh = h.face >= 0;
    if (mode == Mode::Face) return h;
    h.face = -1;

    if (mode == Mode::Vertex) {
        // Candidates within reach, nearest first; the first one not hidden
        // behind the mesh wins.
        std::vector<std::pair<float, int>> cand;
        for (int i = 0; i < static_cast<int>(w.size()); ++i) {
            ImVec2 p;
            if (!point(v, w[i], p)) continue;
            const float d = std::hypot(p.x - mouse.x, p.y - mouse.y);
            if (d <= vertRadius()) cand.push_back({d, i});
        }
        std::sort(cand.begin(), cand.end());
        for (std::size_t k = 0; k < cand.size() && k < 12; ++k) {
            const int i = cand[k].second;
            if (occluded(m, w, v, w[i], [&](int f) { return faceHas(m, f, i); })) continue;
            h.vert = i;
            break;
        }
        return h;
    }

    std::vector<std::pair<float, int>> cand;
    const std::vector<editmesh::EdgeInfo> es = editmesh::edges(m);
    for (int k = 0; k < static_cast<int>(es.size()); ++k) {
        ImVec2 pa, pb;
        if (!segment(v, w[es[k].a], w[es[k].b], pa, pb)) continue;
        const float d = distToSegment(mouse, pa, pb);
        if (d <= edgeRadius()) cand.push_back({d, k});
    }
    std::sort(cand.begin(), cand.end());
    for (std::size_t k = 0; k < cand.size() && k < 12; ++k) {
        const editmesh::EdgeInfo& e = es[cand[k].second];
        const glm::vec3 mid = 0.5f * (w[e.a] + w[e.b]);
        if (occluded(m, w, v, mid, [&](int f) { return f == e.f0 || f == e.f1; })) continue;
        h.ea = e.a;
        h.eb = e.b;
        break;
    }
    return h;
}

bool click(Selection& s, int& faceSel, const Hit& h, bool additive) {
    if (s.mode == Mode::Face) {
        faceSel = h.face;
        return h.face >= 0;
    }
    if (s.mode == Mode::Vertex && h.vert >= 0) {
        auto it = std::find(s.verts.begin(), s.verts.end(), h.vert);
        if (!additive) s.verts = {h.vert};
        else if (it != s.verts.end()) s.verts.erase(it);   // a second click lets go
        else s.verts.push_back(h.vert);
        return true;
    }
    if (s.mode == Mode::Edge && h.ea >= 0) {
        const std::pair<int, int> k{std::min(h.ea, h.eb), std::max(h.ea, h.eb)};
        auto it = std::find(s.edges.begin(), s.edges.end(), k);
        if (!additive) s.edges = {k};
        else if (it != s.edges.end()) s.edges.erase(it);
        else s.edges.push_back(k);
        return true;
    }
    // Missed every element. On the mesh itself that lets go of a plain selection
    // (and keeps an additive one: a stray click must not cost the hand ten
    // careful ones); past the mesh it hands the click back to object picking.
    if (!additive || !h.onMesh) s.clear();
    return h.onMesh;
}

// =============================================================================
// Feedback
// =============================================================================

void preview(Op op) { g_preview = std::move(op); }

void flash(const EditMesh& before, const EditMesh& after, const glm::mat4& model,
           const char* label) {
    std::vector<std::pair<glm::vec3, glm::vec3>> added;
    edgeDiff(before, after, &added, nullptr);
    g_flash.segs.clear();
    for (const auto& s : added) g_flash.segs.push_back({xf(model, s.first), xf(model, s.second)});
    g_flash.t0    = ImGui::GetTime();
    g_flash.label = label ? label : "";
}

void panelRect(ImVec2 mn, ImVec2 mx) {
    g_panelMin   = mn;
    g_panelMax   = mx;
    g_panelFrame = ImGui::GetFrameCount();
}

void readout(ImDrawList* dl, ImVec2 at, const char* text) {
    const ImVec2 ts  = ImGui::CalcTextSize(text);
    const ImVec2 pad(8.0f, 4.0f);
    const ImVec2 p0(at.x, at.y), p1(at.x + ts.x + pad.x * 2.0f, at.y + ts.y + pad.y * 2.0f);
    dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 24, 225), 6.0f);
    dl->AddRect(p0, p1, IM_COL32(255, 195, 70, 200), 6.0f, 0, 1.2f);
    dl->AddText(ImVec2(p0.x + pad.x, p0.y + pad.y), IM_COL32(255, 235, 200, 255), text);
}

void drawOverlay(ImDrawList* dl, const EditMesh& m, const View& v,
                 const Selection& s, int faceSel, const Hit* hover) {
    dl->PushClipRect(v.min, ImVec2(v.min.x + v.size.x, v.min.y + v.size.y), true);
    const std::vector<glm::vec3> w = worldVerts(m, v.model);

    std::vector<char> front(m.faces.size(), 0);
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f)
        front[f] = frontFacing(m, w, f, v) ? 1 : 0;
    const std::vector<editmesh::EdgeInfo> es = editmesh::edges(m);

    // --- The whole mesh as a wireframe: what there is to pick ----------------
    // Edges round the far side stay faintly visible -- that is where the band of
    // a loop cut goes on, and where a corner you cannot see yet is.
    std::vector<char> vertFront(m.verts.size(), 0);
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f)
        if (front[f])
            for (int i : m.faces[f]) vertFront[i] = 1;
    for (const editmesh::EdgeInfo& e : es) {
        ImVec2 pa, pb;
        if (!segment(v, w[e.a], w[e.b], pa, pb)) continue;
        const bool vis = (e.f0 >= 0 && front[e.f0]) || (e.f1 >= 0 && front[e.f1]) || e.f1 < 0;
        dl->AddLine(pa, pb, vis ? kWire : kWireBack, vis ? 1.3f : 1.0f);
    }

    // --- Face mode -----------------------------------------------------------
    if (s.mode == Mode::Face) {
        if (hover && hover->face >= 0 && hover->face != faceSel)
            drawFace(dl, w, m.faces[hover->face], v, kHoverFill, kHover, 1.8f);
        if (m.validFace(faceSel)) {
            drawFace(dl, w, m.faces[faceSel], v, kSelFill, kSelEdge, 2.4f);
            // Which way is "out": extrude and move go along this.
            glm::vec3 c(0.0f);
            for (int i : m.faces[faceSel]) c += w[i];
            c /= static_cast<float>(m.faces[faceSel].size());
            const glm::vec3 wn  = glm::normalize(glm::vec3(
                glm::transpose(glm::inverse(glm::mat3(v.model))) * m.faceNormal(faceSel)));
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (int i : m.faces[faceSel]) { mn = glm::min(mn, w[i]); mx = glm::max(mx, w[i]); }
            const float len = std::clamp(glm::length(mx - mn) * 0.3f, 0.15f, 3.0f);
            ImVec2 pa, pb;
            if (segment(v, c, c + wn * len, pa, pb)) {
                dl->AddLine(pa, pb, kSelEdge, 2.0f);
                dl->AddCircleFilled(pb, 3.5f, kSel);
            }
        }
    }

    // --- Edge mode -----------------------------------------------------------
    if (s.mode == Mode::Edge) {
        if (hover && hover->ea >= 0 && !hasEdge(s, hover->ea, hover->eb)) {
            ImVec2 pa, pb;
            if (segment(v, w[hover->ea], w[hover->eb], pa, pb)) {
                dl->AddLine(pa, pb, kOutline, 6.0f);
                dl->AddLine(pa, pb, kHover, 3.5f);
            }
        }
        for (const auto& e : s.edges) {
            ImVec2 pa, pb;
            if (!segment(v, w[e.first], w[e.second], pa, pb)) continue;
            dl->AddLine(pa, pb, kOutline, 6.5f);
            dl->AddLine(pa, pb, kSel, 4.0f);
        }
    }

    // --- Vertex mode ---------------------------------------------------------
    if (s.mode == Mode::Vertex) {
        const float k = ui();
        for (int i = 0; i < static_cast<int>(w.size()); ++i) {
            ImVec2 p;
            if (!point(v, w[i], p)) continue;
            if (vertFront[i]) {
                dl->AddCircleFilled(p, 4.0f * k, kOutline);
                dl->AddCircleFilled(p, 3.0f * k, kVert);
            } else {
                dl->AddCircleFilled(p, 2.2f * k, kVertBack);
            }
        }
        for (int i : s.verts) {
            ImVec2 p;
            if (!point(v, w[i], p)) continue;
            dl->AddCircleFilled(p, 7.5f * k, kOutline);
            dl->AddCircleFilled(p, 6.0f * k, kSel);
        }
        if (hover && hover->vert >= 0) {
            ImVec2 p;
            if (point(v, w[hover->vert], p)) {
                dl->AddCircle(p, 11.0f * k, kOutline, 24, 4.0f * k);
                dl->AddCircle(p, 11.0f * k, kHover, 24, 2.2f * k);
            }
        }
    }

    // --- Preview: what the button under the pointer would do -----------------
    if (g_preview) {
        EditMesh after = m;
        const int res = g_preview(after);
        std::vector<std::pair<glm::vec3, glm::vec3>> added, removed;
        edgeDiff(m, after, &added, &removed);
        const std::vector<glm::vec3> wa = worldVerts(after, v.model);
        if (s.mode == Mode::Face && after.validFace(res))
            drawFace(dl, wa, after.faces[res], v, kAddFill, 0, 0.0f);
        for (const auto& r : removed) {
            ImVec2 pa, pb;
            if (segment(v, xf(v.model, r.first), xf(v.model, r.second), pa, pb))
                dl->AddLine(pa, pb, kRemove, 2.5f);
        }
        for (const auto& a : added) {
            ImVec2 pa, pb;
            if (!segment(v, xf(v.model, a.first), xf(v.model, a.second), pa, pb)) continue;
            dl->AddLine(pa, pb, kOutline, 4.5f);
            dl->AddLine(pa, pb, kAdd, 2.4f);
        }
        g_preview = nullptr;
    }

    // --- Flash: what the last edit changed -----------------------------------
    const double now = ImGui::GetTime();
    if (g_flash.t0 >= 0.0 && now - g_flash.t0 < kFlashSecs) {
        const float k = 1.0f - static_cast<float>((now - g_flash.t0) / kFlashSecs);
        const ImU32 col = IM_COL32(255, 240, 150, static_cast<int>(255 * k));
        for (const auto& sg : g_flash.segs) {
            ImVec2 pa, pb;
            if (segment(v, sg.first, sg.second, pa, pb))
                dl->AddLine(pa, pb, col, 1.5f + 4.0f * k);
        }
    }
    dl->PopClipRect();

    // The operation's name under the toolbar, so a click on a small button is
    // answered in words as well as in lines.
    if (g_flash.t0 >= 0.0 && now - g_flash.t0 < kToastSecs && !g_flash.label.empty() &&
        ImGui::GetFrameCount() - g_panelFrame <= 1 && g_panelMax.y > g_panelMin.y) {
        const float k = std::min(1.0f, 3.0f * (1.0f - static_cast<float>((now - g_flash.t0) / kToastSecs)));
        const ImVec2 ts = ImGui::CalcTextSize(g_flash.label.c_str());
        const float cx = 0.5f * (g_panelMin.x + g_panelMax.x);
        const ImVec2 p0(cx - ts.x * 0.5f - 12.0f, g_panelMax.y + 8.0f);
        const ImVec2 p1(cx + ts.x * 0.5f + 12.0f, g_panelMax.y + 8.0f + ts.y + 10.0f);
        dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 24, static_cast<int>(215 * k)), 8.0f);
        dl->AddText(ImVec2(p0.x + 12.0f, p0.y + 5.0f),
                    IM_COL32(255, 225, 150, static_cast<int>(255 * k)), g_flash.label.c_str());
    }
}

} // namespace modeltools
