#include "ModelingKeys.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

#include "Component.hpp"

namespace modelkeys {

namespace {

using modeltools::Mode;
using modeltools::Selection;
using EdgeSel = std::pair<int, int>;

// =============================================================================
// State
// =============================================================================

enum class Kind { None, Grab, Rotate, Scale, Extrude, Inset, Bevel, LoopHover, LoopSlide };

// The operation in flight. Everything it computes, it computes from `base` and
// `M` -- the mesh and its transform as they were when it began -- because the
// host puts the entity back to exactly that before every Live::Set.
struct Modal {
    Kind        kind  = Kind::None;
    const char* label = "";
    EditMesh    base;
    glm::mat4   M{1.0f};

    // Transforms (G/R/S, and the move after an edge extrude or a duplicate).
    std::function<void(EditMesh&)> pre;   // topology made first, then moved
    std::vector<int> verts;               // the corners moved (indices after `pre`)
    glm::vec3 pivot{0.0f};                // world
    ImVec2    pivotPx{0, 0};
    int       axis     = -1;              // 0..2, -1 free
    int       axisMode = 0;               // 0 free, 1 global, 2 local
    bool      plane    = false;           // Shift+axis: everything BUT that axis
    float     angle    = 0.0f;            // rotate: accumulated, radians on screen

    // Face and edge operations.
    std::vector<int>     faces;
    std::vector<EdgeSel> edges;
    glm::vec3 normal{0.0f, 1.0f, 0.0f};  // extrude: world direction...
    float     normalScale = 1.0f;         // ...and world metres per mesh unit along it
    float     meshScale   = 1.0f;         // world metres per mesh unit, roughly
    int       segs        = 1;            // bevel
    float     lastWidth   = 0.0f;
    int       ea = -1, eb = -1;           // loop cut
    bool      mid = false;                // loop cut: cancelled slide = the middle

    // The pointer: where it started, and where the operation sees it -- Shift
    // slows it to a tenth, so the two part company.
    ImVec2 start{0, 0}, virt{0, 0}, prevVirt{0, 0};
    int    frame0 = 0;                    // the frame it began (its own click is not a confirm)

    std::string num;                      // a typed value
    std::string readout;                  // what the header says

    Selection selBefore;
    int       faceSelBefore = -1;
    std::function<void(Selection&, int&)> after;   // the pick once confirmed
};
Modal g;

struct BoxDrag {
    bool   pressed = false, dragging = false;
    ImVec2 a{0, 0};
} g_box;

std::string g_msg;
double      g_msgT = -100.0;

void say(const std::string& s) { g_msg = s; g_msgT = ImGui::GetTime(); }

bool key(ImGuiKey k) { return ImGui::IsKeyPressed(k, false); }

EdgeSel ordered(int a, int b) { return {std::min(a, b), std::max(a, b)}; }

glm::vec3 xf(const glm::mat4& M, const glm::vec3& p) { return glm::vec3(M * glm::vec4(p, 1.0f)); }

// =============================================================================
// Geometry of the pointer
// =============================================================================

struct Ray { glm::vec3 o{0.0f}, d{0.0f, 0.0f, -1.0f}; };
Ray ray(const modeltools::View& v, ImVec2 px) {
    Ray r;
    modeltools::rayThrough(v, px, r.o, r.d);
    return r;
}

// Where along the line p + t*dir the ray passes closest.
bool lineParam(const glm::vec3& p, const glm::vec3& dir, const Ray& r, float& t) {
    const glm::vec3 w0 = p - r.o;
    const float a = glm::dot(dir, dir), b = glm::dot(dir, r.d), c = glm::dot(r.d, r.d);
    const float d = glm::dot(dir, w0), e = glm::dot(r.d, w0);
    const float den = a * c - b * b;
    if (std::fabs(den) < 1e-6f) return false;   // looking straight down the line
    t = (b * e - c * d) / den;
    return true;
}

bool planeHit(const glm::vec3& p, const glm::vec3& n, const Ray& r, glm::vec3& hit) {
    const float den = glm::dot(n, r.d);
    if (std::fabs(den) < 1e-6f) return false;
    hit = r.o + r.d * (glm::dot(p - r.o, n) / den);
    return true;
}

// World metres one pixel spans at the pivot.
float metresPerPixel(const modeltools::View& v) {
    const Ray r0 = ray(v, g.pivotPx), r1 = ray(v, ImVec2(g.pivotPx.x + 1.0f, g.pivotPx.y));
    glm::vec3 a, b;
    if (!planeHit(g.pivot, r0.d, r0, a) || !planeHit(g.pivot, r0.d, r1, b)) return 0.01f;
    return glm::length(b - a);
}

float snap(float v, float step) { return std::round(v / step) * step; }

bool typed(float& v) {
    if (g.num.empty() || g.num == "-" || g.num == "." || g.num == "-.") return false;
    v = std::strtof(g.num.c_str(), nullptr);
    return true;
}

glm::vec3 axisDir(int k) {
    if (g.axisMode == 2) {
        const glm::vec3 c(g.M[k]);
        if (glm::length(c) > 1e-6f) return glm::normalize(c);
    }
    glm::vec3 a(0.0f);
    a[k] = 1.0f;
    return a;
}

// =============================================================================
// What a selection amounts to
// =============================================================================

// Edge mode's edges, otherwise every edge both of whose ends are picked.
std::vector<EdgeSel> pickedEdges(const Selection& s, const EditMesh& m, int faceSel) {
    if (s.mode == Mode::Edge) return s.edges;
    const std::vector<int> vs = modeltools::activeVerts(s, m, faceSel);
    std::vector<EdgeSel> out;
    for (const editmesh::EdgeInfo& e : editmesh::edges(m))
        if (std::binary_search(vs.begin(), vs.end(), e.a) &&
            std::binary_search(vs.begin(), vs.end(), e.b))
            out.push_back(ordered(e.a, e.b));
    return out;
}

// Face mode's faces, otherwise every face all of whose corners are picked.
std::vector<int> pickedFaces(const Selection& s, const EditMesh& m, int faceSel) {
    if (s.mode == Mode::Face) return modeltools::selectedFaces(s, m, faceSel);
    const std::vector<int> vs = modeltools::activeVerts(s, m, faceSel);
    std::vector<int> out;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        if (!m.validFace(f)) continue;
        bool all = true;
        for (int v : m.faces[f]) all = all && std::binary_search(vs.begin(), vs.end(), v);
        if (all) out.push_back(f);
    }
    return out;
}

glm::vec3 centreOf(const EditMesh& m, const glm::mat4& M, const std::vector<int>& vs) {
    glm::vec3 c(0.0f);
    int n = 0;
    for (int v : vs)
        if (v >= 0 && v < static_cast<int>(m.verts.size())) { c += xf(M, m.verts[v]); ++n; }
    return n ? c / static_cast<float>(n) : xf(M, glm::vec3(0.0f));
}

std::vector<int> cornersOf(const EditMesh& m, const std::vector<int>& faces) {
    std::vector<int> out;
    for (int f : faces) out.insert(out.end(), m.faces[f].begin(), m.faces[f].end());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

// The new pick after an operation that made new elements: in the current mode,
// the corners / edges / faces it made.
std::function<void(Selection&, int&)> pickNew(Mode mode, std::vector<int> faces,
                                              std::vector<EdgeSel> edges) {
    return [mode, faces, edges](Selection& s, int& fs) {
        s.clear();
        fs = -1;
        if (mode == Mode::Face) {
            s.faces = faces;
            fs = faces.empty() ? -1 : faces.front();
        } else if (mode == Mode::Edge) {
            s.edges = edges;
        } else {
            for (const EdgeSel& e : edges) { s.verts.push_back(e.first); s.verts.push_back(e.second); }
            std::sort(s.verts.begin(), s.verts.end());
            s.verts.erase(std::unique(s.verts.begin(), s.verts.end()), s.verts.end());
        }
    };
}

// =============================================================================
// Starting an operation
// =============================================================================

// Everything but what the operation itself set up: the pointer, the pick to go
// back to, the entity's snapshot.
void begin(const Host& h) {
    const ImGuiIO& io = ImGui::GetIO();
    g.selBefore     = *h.sel;
    g.faceSelBefore = *h.faceSel;
    g.start = g.virt = g.prevVirt = io.MousePos;
    if (!modeltools::toScreen(h.view, g.pivot, g.pivotPx)) g.pivotPx = io.MousePos;
    g.frame0 = ImGui::GetFrameCount();
    const glm::mat3 L(g.M);
    g.meshScale = std::max((glm::length(L[0]) + glm::length(L[1]) + glm::length(L[2])) / 3.0f,
                           1e-6f);
    h.live(Live::Begin, nullptr, nullptr);
}

void fresh(const Host& h, Kind k, const char* label) {
    g = Modal{};
    g.kind  = k;
    g.label = label;
    g.base  = h.mesh->mesh;
    g.M     = h.view.model;
}

void startTransform(const Host& h, Kind k) {
    const EditMesh& m = h.mesh->mesh;
    const std::vector<int> vs = modeltools::activeVerts(*h.sel, m, *h.faceSel);
    if (vs.empty()) { say("Pick something first"); return; }
    fresh(h, k, k == Kind::Grab ? "Move" : k == Kind::Rotate ? "Rotate" : "Scale");
    g.verts = vs;
    g.pivot = centreOf(m, g.M, vs);
    begin(h);
}

void startExtrude(const Host& h) {
    const EditMesh& m = h.mesh->mesh;
    const Selection& s = *h.sel;
    if (s.mode == Mode::Face) {
        const std::vector<int> fs = modeltools::selectedFaces(s, m, *h.faceSel);
        if (fs.empty()) { say("Pick faces to extrude"); return; }
        fresh(h, Kind::Extrude, "Extrude");
        g.faces = fs;
        glm::vec3 n(0.0f);
        for (int f : fs) n += m.faceNormal(f);
        if (glm::length(n) < 1e-6f) n = m.faceNormal(fs[0]);
        const glm::vec3 wn = glm::mat3(g.M) * glm::normalize(n);
        g.normalScale = std::max(glm::length(wn), 1e-6f);
        g.normal      = wn / g.normalScale;
        g.pivot       = centreOf(m, g.M, cornersOf(m, fs));
        begin(h);
        return;
    }
    const std::vector<EdgeSel> es = pickedEdges(s, m, *h.faceSel);
    if (es.empty()) {
        say("Extrude needs edges or faces (a fitzel mesh has no loose corners)");
        return;
    }
    EditMesh tmp = m;
    const std::vector<EdgeSel> made = editmesh::extrudeEdges(tmp, es);
    if (made.empty()) { say("Nothing to extrude there"); return; }
    fresh(h, Kind::Grab, "Extrude");
    g.pre = [es](EditMesh& mm) { editmesh::extrudeEdges(mm, es); };
    std::vector<EdgeSel> newEdges;
    for (const EdgeSel& e : made) {
        g.verts.push_back(e.first);
        g.verts.push_back(e.second);
        newEdges.push_back(ordered(e.first, e.second));
    }
    std::sort(g.verts.begin(), g.verts.end());
    g.verts.erase(std::unique(g.verts.begin(), g.verts.end()), g.verts.end());
    g.pivot = centreOf(tmp, g.M, g.verts);
    g.after = pickNew(s.mode, {}, newEdges);
    begin(h);
}

void startDuplicate(const Host& h) {
    const EditMesh& m = h.mesh->mesh;
    const Selection& s = *h.sel;
    const std::vector<int> fs = pickedFaces(s, m, *h.faceSel);
    if (fs.empty()) { say("Duplicate copies faces: pick faces, or all the corners of some"); return; }
    EditMesh tmp = m;
    const std::vector<int> made = editmesh::duplicateFaces(tmp, fs);
    fresh(h, Kind::Grab, "Duplicate");
    g.pre   = [fs](EditMesh& mm) { editmesh::duplicateFaces(mm, fs); };
    g.verts = cornersOf(tmp, made);
    g.pivot = centreOf(tmp, g.M, g.verts);
    std::vector<EdgeSel> newEdges;
    for (const editmesh::EdgeInfo& e : editmesh::edges(tmp))
        if (std::binary_search(g.verts.begin(), g.verts.end(), e.a) &&
            std::binary_search(g.verts.begin(), g.verts.end(), e.b))
            newEdges.push_back(ordered(e.a, e.b));
    g.after = pickNew(s.mode, made, newEdges);
    begin(h);
}

void startInset(const Host& h) {
    const EditMesh& m = h.mesh->mesh;
    const std::vector<int> fs = pickedFaces(*h.sel, m, *h.faceSel);
    if (fs.empty()) { say("Pick faces to inset"); return; }
    fresh(h, Kind::Inset, "Inset");
    g.faces = fs;
    g.pivot = centreOf(m, g.M, cornersOf(m, fs));
    begin(h);
}

void startBevel(const Host& h) {
    const EditMesh& m = h.mesh->mesh;
    const std::vector<EdgeSel> es = pickedEdges(*h.sel, m, *h.faceSel);
    if (es.empty()) { say("Pick edges to bevel"); return; }
    fresh(h, Kind::Bevel, "Bevel");
    g.edges = es;
    g.segs  = std::max(1, static_cast<int>(std::lround(modeltools::amounts().bevelSegs)));
    std::vector<int> ends;
    for (const EdgeSel& e : es) { ends.push_back(e.first); ends.push_back(e.second); }
    g.pivot = centreOf(m, g.M, ends);
    g.after = [](Selection& s, int& fs) { s.clear(); fs = -1; };
    begin(h);
}

// =============================================================================
// Running it
// =============================================================================

void setAxis(int k, bool plane) {
    if (g.axis == k && g.plane == plane) {
        g.axisMode = (g.axisMode + 1) % 3;   // global -> local -> free
        if (g.axisMode == 0) { g.axis = -1; g.plane = false; }
    } else {
        g.axis = k;
        g.plane = plane;
        g.axisMode = 1;
    }
}

std::string axisText() {
    if (g.axis < 0) return "";
    static const char* kN[3] = {"X", "Y", "Z"};
    std::string s = g.plane ? std::string("  plane ") + kN[(g.axis + 1) % 3] + kN[(g.axis + 2) % 3]
                            : std::string("  along ") + kN[g.axis];
    return s + (g.axisMode == 2 ? " (local)" : " (global)");
}

std::string typedText() { return g.num.empty() ? "" : "  [" + g.num + "|]"; }

// The world-space transform G/R/S make of the pointer.
glm::mat4 transformNow(const Host& h) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl, fine = io.KeyShift;
    const glm::vec3 P = g.pivot;
    char buf[160];
    float v = 0.0f;

    if (g.kind == Kind::Grab) {
        glm::vec3 D(0.0f);
        if (typed(v)) {
            D = (g.axis >= 0 && !g.plane ? axisDir(g.axis) : glm::vec3(1, 0, 0)) * v;
        } else if (g.axis >= 0 && !g.plane) {
            const glm::vec3 A = axisDir(g.axis);
            float t0 = 0.0f, t1 = 0.0f;
            if (lineParam(P, A, ray(h.view, g.start), t0) && lineParam(P, A, ray(h.view, g.virt), t1)) {
                float d = t1 - t0;
                if (ctrl) d = snap(d, fine ? 0.01f : 0.1f);
                D = A * d;
            }
        } else {
            const glm::vec3 n = g.axis >= 0 ? axisDir(g.axis) : ray(h.view, g.pivotPx).d;
            glm::vec3 a, b;
            if (planeHit(P, n, ray(h.view, g.start), a) && planeHit(P, n, ray(h.view, g.virt), b)) {
                D = b - a;
                if (ctrl) {
                    const float st = fine ? 0.01f : 0.1f;
                    D = glm::vec3(snap(D.x, st), snap(D.y, st), snap(D.z, st));
                }
            }
        }
        std::snprintf(buf, sizeof buf, "Move  D: %.3f  %.3f  %.3f  (%.3f m)", D.x, D.y, D.z,
                      glm::length(D));
        g.readout = buf + axisText() + typedText();
        return glm::translate(glm::mat4(1.0f), D);
    }

    if (g.kind == Kind::Rotate) {
        const ImVec2 a(g.prevVirt.x - g.pivotPx.x, g.prevVirt.y - g.pivotPx.y);
        const ImVec2 b(g.virt.x - g.pivotPx.x, g.virt.y - g.pivotPx.y);
        if (std::hypot(a.x, a.y) > 2.0f && std::hypot(b.x, b.y) > 2.0f)
            g.angle += std::atan2(a.x * b.y - a.y * b.x, a.x * b.x + a.y * b.y);
        g.prevVirt = g.virt;
        // Clockwise on screen (pixel rows run down) is a positive turn about the
        // direction the camera looks in, which is the right-hand rule seen from
        // behind the axis: the object follows the pointer round.
        const glm::vec3 view = ray(h.view, g.pivotPx).d;
        glm::vec3 A = view;
        float sign = 1.0f;
        if (g.axis >= 0) {
            A    = axisDir(g.axis);
            sign = glm::dot(A, view) >= 0.0f ? 1.0f : -1.0f;
        }
        float deg = glm::degrees(g.angle) * sign;
        if (typed(v)) deg = v;
        else if (ctrl) deg = snap(deg, fine ? 1.0f : 5.0f);
        std::snprintf(buf, sizeof buf, "Rotate  %.1f deg", deg);
        g.readout = buf + axisText() + typedText();
        return glm::translate(glm::mat4(1.0f), P) *
               glm::rotate(glm::mat4(1.0f), glm::radians(deg), A) *
               glm::translate(glm::mat4(1.0f), -P);
    }

    // Scale: the pointer's distance from the pivot against where it started.
    const float d0 = std::max(std::hypot(g.start.x - g.pivotPx.x, g.start.y - g.pivotPx.y), 1.0f);
    const float d1 = std::hypot(g.virt.x - g.pivotPx.x, g.virt.y - g.pivotPx.y);
    float f = d1 / d0;
    if (typed(v)) f = v;
    else if (ctrl) f = snap(f, fine ? 0.01f : 0.1f);
    glm::mat3 S(f);
    if (g.axis >= 0) {
        const glm::vec3 A  = axisDir(g.axis);
        const glm::mat3 AA = glm::outerProduct(A, A);
        S = g.plane ? glm::mat3(f) + (1.0f - f) * AA : glm::mat3(1.0f) + (f - 1.0f) * AA;
    }
    std::snprintf(buf, sizeof buf, "Scale  %.3f", f);
    g.readout = buf + axisText() + typedText();
    return glm::translate(glm::mat4(1.0f), P) * glm::mat4(S) * glm::translate(glm::mat4(1.0f), -P);
}

// Compute this frame's result and hand it to the host.
void apply(const Host& h) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl, fine = io.KeyShift;
    char buf[160];
    float v = 0.0f;

    switch (g.kind) {
    case Kind::Grab: case Kind::Rotate: case Kind::Scale: {
        const glm::mat4 L = glm::inverse(g.M) * transformNow(h) * g.M;
        const std::function<void(EditMesh&)> pre = g.pre;
        const std::vector<int> vs = g.verts;
        h.live(Live::Set, [pre, vs, L](EditMesh& m) {
            if (pre) pre(m);
            editmesh::transformVerts(m, vs, L);
        }, nullptr);
        break;
    }
    case Kind::Extrude: {
        float d = 0.0f, t0 = 0.0f, t1 = 0.0f;
        if (typed(v)) d = v;
        else if (lineParam(g.pivot, g.normal, ray(h.view, g.start), t0) &&
                 lineParam(g.pivot, g.normal, ray(h.view, g.virt), t1)) {
            d = t1 - t0;
            if (ctrl) d = snap(d, fine ? 0.01f : 0.1f);
        }
        std::snprintf(buf, sizeof buf, "Extrude  %.3f m along the normal", d);
        g.readout = buf + typedText();
        const std::vector<int> fs = g.faces;
        const float dm = d / g.normalScale;
        h.live(Live::Set, [fs, dm](EditMesh& m) { editmesh::extrudeFaces(m, fs, dm); }, nullptr);
        break;
    }
    case Kind::Inset: case Kind::Bevel: {
        // Inset grows as the pointer comes IN towards the faces, a bevel as it
        // goes OUT from its edges -- the way each one looks as it happens.
        const float d0 = std::hypot(g.start.x - g.pivotPx.x, g.start.y - g.pivotPx.y);
        const float d1 = std::hypot(g.virt.x - g.pivotPx.x, g.virt.y - g.pivotPx.y);
        float w = std::max(0.0f, (g.kind == Kind::Inset ? d0 - d1 : d1 - d0)) * metresPerPixel(h.view);
        if (typed(v)) w = std::max(0.0f, v);
        else if (ctrl) w = snap(w, fine ? 0.005f : 0.05f);
        g.lastWidth = w;
        const float wm = w / g.meshScale;
        if (g.kind == Kind::Inset) {
            std::snprintf(buf, sizeof buf, "Inset  %.3f m", w);
            const std::vector<int> fs = g.faces;
            h.live(Live::Set, [fs, wm](EditMesh& m) { editmesh::insetFaces(m, fs, wm); }, nullptr);
        } else {
            std::snprintf(buf, sizeof buf, "Bevel  %.3f m   %d segment%s (wheel)", w, g.segs,
                          g.segs == 1 ? "" : "s");
            const std::vector<EdgeSel> es = g.edges;
            const int segs = g.segs;
            h.live(Live::Set, [es, wm, segs](EditMesh& m) {
                if (wm > 0.0f) editmesh::bevelEdges(m, es, wm, segs);
            }, nullptr);
        }
        g.readout = buf + typedText();
        break;
    }
    case Kind::LoopSlide: {
        // Where along the edge, on screen, the pointer is.
        ImVec2 pa, pb;
        float t = 0.5f;
        if (!g.mid && modeltools::toScreen(h.view, xf(g.M, g.base.verts[g.ea]), pa) &&
            modeltools::toScreen(h.view, xf(g.M, g.base.verts[g.eb]), pb)) {
            const float dx = pb.x - pa.x, dy = pb.y - pa.y, l2 = dx * dx + dy * dy;
            if (l2 > 1.0f) t = ((g.virt.x - pa.x) * dx + (g.virt.y - pa.y) * dy) / l2;
        }
        if (!g.mid && typed(v)) t = v;
        t = std::clamp(t, 0.02f, 0.98f);
        std::snprintf(buf, sizeof buf, "Loop cut  at %.2f along the edge", t);
        g.readout = buf + typedText();
        // loopCutEdge measures from the lower-numbered corner.
        const float tt = g.ea < g.eb ? t : 1.0f - t;
        const int a = g.ea, b = g.eb;
        h.live(Live::Set, [a, b, tt](EditMesh& m) { editmesh::loopCutEdge(m, a, b, tt); }, nullptr);
        break;
    }
    default: break;
    }
}

void confirm(const Host& h) {
    h.live(Live::Commit, nullptr, g.label);
    if (g.after) g.after(*h.sel, *h.faceSel);
    if (g.kind == Kind::Bevel) {
        // What worked is what the panel offers next.
        modeltools::amounts().bevel     = std::max(g.lastWidth, 0.01f);
        modeltools::amounts().bevelSegs = static_cast<float>(g.segs);
    }
    g.kind = Kind::None;
}

void abandon(const Host& h) {
    if (g.kind != Kind::LoopHover) {
        h.live(Live::Cancel, nullptr, nullptr);
        *h.sel     = g.selBefore;
        *h.faceSel = g.faceSelBefore;
    }
    g.kind = Kind::None;
}

void loopHover(const Host& h) {
    const ImGuiIO& io = ImGui::GetIO();
    const EditMesh& m = h.mesh->mesh;
    const modeltools::Hit hit = modeltools::pick(m, h.view, io.MousePos, Mode::Edge);
    g.ea = hit.ea;
    g.eb = hit.eb;
    g.readout = "Loop cut  point at an edge, click to place it, then slide";
    if (g.ea >= 0) {
        const int a = g.ea, b = g.eb;
        modeltools::preview([a, b](EditMesh& mm) { return editmesh::loopCutEdge(mm, a, b, 0.5f); });
    }
    if (key(ImGuiKey_Escape) || ImGui::IsMouseClicked(1)) { g.kind = Kind::None; return; }
    if (ImGui::IsMouseClicked(0) && ImGui::GetFrameCount() != g.frame0 && g.ea >= 0 && h.hovered) {
        const int  a = g.ea, b = g.eb;
        const Mode mode = h.sel->mode;
        // Afterwards the new ring is what is picked, as in Blender. Where the cut
        // falls does not change which corners it makes, so a trial cut says.
        const int nv0 = static_cast<int>(m.verts.size());
        EditMesh probe = m;
        editmesh::loopCutEdge(probe, a, b, 0.5f);
        std::vector<EdgeSel> ring;
        for (const editmesh::EdgeInfo& e : editmesh::edges(probe))
            if (e.a >= nv0 && e.b >= nv0) ring.push_back(ordered(e.a, e.b));

        fresh(h, Kind::LoopSlide, "Loop cut");
        g.ea    = a;
        g.eb    = b;
        g.pivot = xf(g.M, 0.5f * (m.verts[a] + m.verts[b]));
        g.after = pickNew(mode, {}, ring);
        begin(h);
    }
}

void runModal(const Host& h) {
    ImGuiIO& io = ImGui::GetIO();
    const float k = io.KeyShift ? 0.1f : 1.0f;
    g.virt.x += io.MouseDelta.x * k;
    g.virt.y += io.MouseDelta.y * k;

    if (g.kind == Kind::LoopHover) { loopHover(h); return; }

    const bool first = ImGui::GetFrameCount() == g.frame0;
    if (key(ImGuiKey_Escape) || (!first && ImGui::IsMouseClicked(1))) {
        if (g.kind == Kind::LoopSlide) {   // Blender: a cancelled slide leaves the cut centred
            g.mid = true;
            apply(h);
            confirm(h);
        } else {
            abandon(h);
        }
        return;
    }
    if (g.kind == Kind::Grab || g.kind == Kind::Rotate || g.kind == Kind::Scale) {
        if (key(ImGuiKey_X)) setAxis(0, io.KeyShift);
        if (key(ImGuiKey_Y)) setAxis(1, io.KeyShift);
        if (key(ImGuiKey_Z)) setAxis(2, io.KeyShift);
    }
    // A typed value: digits and a point (or a comma, for the keyboards that have
    // one there); minus flips the sign wherever it is typed.
    for (int i = 0; i < io.InputQueueCharacters.Size; ++i) {
        const ImWchar c = io.InputQueueCharacters[i];
        if ((c >= '0' && c <= '9') || c == '.') g.num += static_cast<char>(c);
        else if (c == ',') g.num += '.';
        else if (c == '-') g.num = (!g.num.empty() && g.num[0] == '-') ? g.num.substr(1) : "-" + g.num;
    }
    if (key(ImGuiKey_Backspace) && !g.num.empty()) g.num.pop_back();
    if (g.kind == Kind::Bevel && io.MouseWheel != 0.0f)
        g.segs = std::clamp(g.segs + (io.MouseWheel > 0.0f ? 1 : -1), 1, 12);

    apply(h);
    if (key(ImGuiKey_Enter) || key(ImGuiKey_KeypadEnter) || key(ImGuiKey_Space) ||
        (!first && ImGui::IsMouseClicked(0)))
        confirm(h);
}

// =============================================================================
// One-shot operations and menus
// =============================================================================

void makeFace(const Host& h) {
    const Selection& s = *h.sel;
    const EditMesh&  m = h.mesh->mesh;
    int made = -1;
    if (s.mode == Mode::Edge && s.edges.size() == 1) {
        const EdgeSel e = s.edges[0];
        h.edit([e, &made](MeshComponent& mc) { made = editmesh::fillHole(mc.mesh, e.first, e.second); return -1; },
               "Fill hole");
        if (made < 0) say("F on one edge fills the hole it borders -- this one borders none");
    } else if (s.mode == Mode::Face) {
        say("F joins corners or edges into a face: press 1 or 2 first");
        return;
    } else {
        std::vector<int> vs = modeltools::activeVerts(s, m, *h.faceSel);
        if (vs.size() == 2) {
            say("Two corners would make a loose edge, which fitzel meshes do not have -- J connects them across a face");
            return;
        }
        if (vs.size() < 3) { say("Pick three or more corners (or two or more edges)"); return; }
        h.edit([vs, &made](MeshComponent& mc) { made = editmesh::makeFace(mc.mesh, vs); return -1; },
               "Make face");
        if (made < 0) say("No face: the corners are in a line, or that face is there already");
    }
}

void connect(const Host& h) {
    const Selection& s = *h.sel;
    if (s.mode != Mode::Vertex || s.verts.size() != 2) { say("J connects two picked corners"); return; }
    const int a = s.verts[0], b = s.verts[1];
    int r = -1;
    h.edit([a, b, &r](MeshComponent& mc) { r = editmesh::connectVerts(mc.mesh, a, b); return -1; },
           "Connect");
    if (r < 0) say("Those two corners are not across one face");
}

void frameSelection(const Host& h) {
    const EditMesh& m = h.mesh->mesh;
    std::vector<int> vs = modeltools::activeVerts(*h.sel, m, *h.faceSel);
    if (vs.empty())
        for (int i = 0; i < static_cast<int>(m.verts.size()); ++i) vs.push_back(i);
    if (vs.empty() || !h.frame) return;
    const glm::vec3 c = centreOf(m, h.view.model, vs);
    float r = 0.25f;
    for (int v : vs) r = std::max(r, glm::length(xf(h.view.model, m.verts[v]) - c));
    h.frame(c, r);
}

void menus(const Host& h) {
    Selection& s  = *h.sel;
    int&       fs = *h.faceSel;
    const EditMesh& m = h.mesh->mesh;
    const bool vertexish = s.mode != Mode::Face;

    if (ImGui::BeginPopup("##mk_merge")) {
        ImGui::TextDisabled("Merge");
        ImGui::Separator();
        const std::vector<int> vs = modeltools::activeVerts(s, m, fs);
        if (ImGui::MenuItem("At Center", nullptr, false, vs.size() >= 2)) {
            int r = -1;
            h.edit([vs, &r](MeshComponent& mc) { r = editmesh::mergeVerts(mc.mesh, vs); return -1; },
                   "Merge at center");
            const Mode mode = s.mode;
            s.clear();
            fs = -1;
            if (mode == Mode::Vertex && r >= 0) s.verts = {r};
        }
        if (ImGui::MenuItem("By Distance")) {
            std::vector<int> all = vs;
            if (all.size() < 2)
                for (int i = 0; i < static_cast<int>(m.verts.size()); ++i) all.push_back(i);
            const float d = modeltools::amounts().weld;
            int n = 0;
            h.edit([all, d, &n](MeshComponent& mc) { n = editmesh::weldVerts(mc.mesh, all, d); return -1; },
                   "Merge by distance");
            s.clear();
            fs = -1;
            say("Removed " + std::to_string(n) + " corner" + (n == 1 ? "" : "s"));
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##mk_delete")) {
        ImGui::TextDisabled("Delete");
        ImGui::Separator();
        const std::vector<int>     vs = modeltools::activeVerts(s, m, fs);
        const std::vector<EdgeSel> es = pickedEdges(s, m, fs);
        const std::vector<int>     pf = pickedFaces(s, m, fs);
        bool done = false;
        if (ImGui::MenuItem("Vertices", nullptr, false, !vs.empty())) {
            h.edit([vs](MeshComponent& mc) { editmesh::deleteVerts(mc.mesh, vs); return -1; },
                   "Delete vertices");
            done = true;
        }
        if (ImGui::MenuItem("Edges", nullptr, false, !es.empty())) {
            // Every face that uses a picked edge; corners left unused go too.
            std::vector<int> hit;
            for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
                const std::vector<int>& fv = m.faces[f];
                for (std::size_t i = 0; i < fv.size(); ++i)
                    if (std::find(es.begin(), es.end(), ordered(fv[i], fv[(i + 1) % fv.size()])) != es.end()) {
                        hit.push_back(f);
                        break;
                    }
            }
            h.edit([hit](MeshComponent& mc) { editmesh::deleteFaces(mc.mesh, hit); return -1; },
                   "Delete edges");
            done = true;
        }
        if (ImGui::MenuItem("Faces", nullptr, false, !pf.empty())) {
            h.edit([pf](MeshComponent& mc) { editmesh::deleteFaces(mc.mesh, pf); return -1; },
                   "Delete faces");
            done = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Dissolve Edges", nullptr, false, !es.empty() && vertexish)) {
            // Found again by where their ends ARE: dissolving one edge can
            // renumber the corners the next one is named by.
            std::vector<std::pair<glm::vec3, glm::vec3>> at;
            for (const EdgeSel& e : es) at.push_back({m.verts[e.first], m.verts[e.second]});
            h.edit([at](MeshComponent& mc) {
                for (const auto& p : at) {
                    int a = -1, b = -1;
                    for (int i = 0; i < static_cast<int>(mc.mesh.verts.size()); ++i) {
                        if (glm::length(mc.mesh.verts[i] - p.first) < 1e-6f) a = i;
                        if (glm::length(mc.mesh.verts[i] - p.second) < 1e-6f) b = i;
                    }
                    if (a >= 0 && b >= 0) editmesh::dissolveEdge(mc.mesh, a, b);
                }
                return -1;
            }, "Dissolve edges");
            done = true;
        }
        if (done) { s.clear(); fs = -1; }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##mk_normals")) {
        ImGui::TextDisabled("Normals");
        ImGui::Separator();
        const std::vector<int> pf = pickedFaces(s, m, fs);
        const int keep = fs;
        if (ImGui::MenuItem("Flip", nullptr, false, !pf.empty()))
            h.edit([pf, keep](MeshComponent& mc) { editmesh::flipFaces(mc.mesh, pf); return keep; },
                   "Flip normals");
        if (ImGui::MenuItem("Recalculate Outside", "Shift+N")) {
            int n = 0;
            h.edit([pf, keep, &n](MeshComponent& mc) { n = editmesh::recalcNormals(mc.mesh, pf); return keep; },
                   "Recalculate normals");
            say(std::to_string(n) + " face" + (n == 1 ? "" : "s") + " turned");
        }
        ImGui::EndPopup();
    }
}

void keys(const Host& h) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool ctrl = io.KeyCtrl, shift = io.KeyShift, alt = io.KeyAlt;
    const bool plain = !ctrl && !shift && !alt;
    EditMesh&  m  = h.mesh->mesh;
    Selection& s  = *h.sel;
    int&       fs = *h.faceSel;

    if (plain && key(ImGuiKey_1))      modeltools::setMode(s, Mode::Vertex, m, fs);
    else if (plain && key(ImGuiKey_2)) modeltools::setMode(s, Mode::Edge, m, fs);
    else if (plain && key(ImGuiKey_3)) modeltools::setMode(s, Mode::Face, m, fs);
    else if (!ctrl && !shift && key(ImGuiKey_A)) {
        if (alt) { s.clear(); fs = -1; }
        else     modeltools::selectAll(s, m, fs);
    }
    else if (ctrl && !shift && !alt && key(ImGuiKey_I)) modeltools::invert(s, fs, m);
    else if (ctrl && (key(ImGuiKey_KeypadAdd) || key(ImGuiKey_Equal)))      modeltools::grow(s, m, fs);
    else if (ctrl && (key(ImGuiKey_KeypadSubtract) || key(ImGuiKey_Minus))) modeltools::shrink(s, fs, m);
    else if (!alt && !shift && key(ImGuiKey_L)) {
        if (ctrl) {
            modeltools::selectLinked(s, fs, m, nullptr);
        } else {
            modeltools::Hit hit = modeltools::pick(m, h.view, io.MousePos, s.mode);
            if (hit.vert < 0 && hit.ea < 0 && hit.face < 0)
                hit = modeltools::pick(m, h.view, io.MousePos, Mode::Face);
            if (hit.vert >= 0 || hit.ea >= 0 || hit.face >= 0)
                modeltools::selectLinked(s, fs, m, &hit);
        }
    }
    else if (plain && key(ImGuiKey_G)) startTransform(h, Kind::Grab);
    else if (plain && key(ImGuiKey_R)) startTransform(h, Kind::Rotate);
    else if (plain && key(ImGuiKey_S)) startTransform(h, Kind::Scale);
    else if (plain && key(ImGuiKey_E)) startExtrude(h);
    else if (plain && key(ImGuiKey_I)) startInset(h);
    else if (ctrl && !shift && !alt && key(ImGuiKey_B)) startBevel(h);
    else if (ctrl && !shift && !alt && key(ImGuiKey_R)) {
        g = Modal{};
        g.kind   = Kind::LoopHover;
        g.label  = "Loop cut";
        g.frame0 = ImGui::GetFrameCount();
    }
    else if (shift && !ctrl && !alt && key(ImGuiKey_D)) startDuplicate(h);
    else if (plain && key(ImGuiKey_F)) makeFace(h);
    else if (plain && key(ImGuiKey_J)) connect(h);
    else if (plain && key(ImGuiKey_M)) ImGui::OpenPopup("##mk_merge");
    else if ((plain && key(ImGuiKey_X)) || key(ImGuiKey_Delete)) ImGui::OpenPopup("##mk_delete");
    else if (shift && !ctrl && !alt && key(ImGuiKey_N)) {
        const std::vector<int> pf = pickedFaces(s, m, fs);
        const int keep = fs;
        int n = 0;
        h.edit([pf, keep, &n](MeshComponent& mc) { n = editmesh::recalcNormals(mc.mesh, pf); return keep; },
               "Recalculate normals");
        say(std::to_string(n) + " face" + (n == 1 ? "" : "s") + " turned outward");
    }
    else if (alt && !ctrl && !shift && key(ImGuiKey_N)) ImGui::OpenPopup("##mk_normals");
    else if (key(ImGuiKey_KeypadDecimal)) frameSelection(h);
}

void mouse(const Host& h) {
    const ImGuiIO& io = ImGui::GetIO();
    if (!g_box.pressed) {
        if (ImGui::IsMouseClicked(0) && h.hovered && !h.gizmoOver) {
            g_box.pressed  = true;
            g_box.dragging = false;
            g_box.a        = io.MousePos;
        }
        return;
    }
    if (ImGui::IsMouseDown(0)) {
        const float d = std::hypot(io.MousePos.x - g_box.a.x, io.MousePos.y - g_box.a.y);
        if (d > std::max(io.MouseDragThreshold, 6.0f)) g_box.dragging = true;
        return;
    }
    // Let go: a drag was a box, anything else a click where it went down.
    g_box.pressed = false;
    const EditMesh& m  = h.mesh->mesh;
    Selection&      s  = *h.sel;
    int&            fs = *h.faceSel;
    if (g_box.dragging) {
        g_box.dragging = false;
        modeltools::boxSelect(s, fs, m, h.view, g_box.a, io.MousePos,
                              io.KeyCtrl  ? modeltools::BoxOp::Subtract
                              : io.KeyShift ? modeltools::BoxOp::Add
                                            : modeltools::BoxOp::Set);
        return;
    }
    if (io.KeyAlt) {
        if (!modeltools::loopSelect(s, fs, m, h.view, g_box.a, io.KeyShift) && !io.KeyShift) {
            s.clear();
            fs = -1;
        }
        return;
    }
    const modeltools::Hit hit = modeltools::pick(m, h.view, g_box.a, s.mode);
    const bool add = io.KeyShift || s.additive;
    if (!modeltools::click(s, fs, hit, add) && !add) {
        s.clear();   // Blender: a click on nothing lets go of everything
        fs = -1;
    }
}

// --- Drawing -----------------------------------------------------------------

void label(ImDrawList* dl, ImVec2 at, const std::string& text, ImU32 col, bool centred) {
    const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
    const ImVec2 p0(centred ? at.x - ts.x * 0.5f - 10.0f : at.x, at.y);
    const ImVec2 p1(p0.x + ts.x + 20.0f, p0.y + ts.y + 10.0f);
    dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 24, 215), 6.0f);
    dl->AddText(ImVec2(p0.x + 10.0f, p0.y + 5.0f), col, text.c_str());
}

} // namespace

// =============================================================================
// Public
// =============================================================================

bool busy() { return g.kind != Kind::None || g_box.dragging; }

void cancel(const Host& h) {
    if (g.kind != Kind::None) abandon(h);
    g_box = BoxDrag{};
}

void update(const Host& h) {
    if (!h.mesh || !h.sel || !h.faceSel || !h.edit || !h.live) return;
    if (g.kind != Kind::None) { runModal(h); return; }
    menus(h);
    if (g_box.pressed) { mouse(h); return; }   // finish a drag wherever the pointer went
    if (!h.hovered || !h.keysFree ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
        return;
    keys(h);
    if (g.kind == Kind::None) mouse(h);
}

void drawHud(ImDrawList* dl, const Host& h) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 mn = h.view.min;
    const ImVec2 mx(mn.x + h.view.size.x, mn.y + h.view.size.y);
    dl->PushClipRect(mn, mx, true);

    if (g_box.dragging) {
        dl->AddRectFilled(g_box.a, io.MousePos, IM_COL32(255, 255, 255, 18));
        dl->AddRect(g_box.a, io.MousePos, IM_COL32(235, 235, 240, 200), 0.0f, 0, 1.2f);
    }

    const bool transforming = g.kind == Kind::Grab || g.kind == Kind::Rotate || g.kind == Kind::Scale;
    if (g.kind != Kind::None && g.kind != Kind::LoopHover) {
        // The locked axis (or the two of a plane) through the pivot, in its colour.
        static const ImU32 kAxisCol[3] = {IM_COL32(230, 80, 80, 220), IM_COL32(120, 210, 90, 220),
                                          IM_COL32(90, 140, 240, 220)};
        auto guide = [&](const glm::vec3& dir, ImU32 col) {
            ImVec2 a, b;
            const float len = std::max(glm::length(glm::vec3(g.M[0])), 1.0f) * 200.0f;
            if (modeltools::toScreen(h.view, g.pivot - dir * len, a) &&
                modeltools::toScreen(h.view, g.pivot + dir * len, b))
                dl->AddLine(a, b, col, 1.6f);
        };
        if (transforming && g.axis >= 0) {
            if (g.plane) {
                for (int k = 0; k < 3; ++k)
                    if (k != g.axis) guide(axisDir(k), kAxisCol[k]);
            } else {
                guide(axisDir(g.axis), kAxisCol[g.axis]);
            }
        }
        if (g.kind == Kind::Extrude) guide(g.normal, IM_COL32(255, 195, 70, 200));
        if (g.kind == Kind::Rotate || g.kind == Kind::Scale || g.kind == Kind::Inset ||
            g.kind == Kind::Bevel)
            dl->AddLine(g.pivotPx, io.MousePos, IM_COL32(20, 20, 24, 200), 1.2f);
    }

    const float  cx = 0.5f * (mn.x + mx.x);
    const ImU32  text = IM_COL32(235, 238, 245, 240), amber = IM_COL32(255, 205, 120, 255);
    const float  lh = ImGui::GetTextLineHeightWithSpacing() + 12.0f;

    // Which mode this is: the one thing a Blender hand needs to know before
    // pressing G -- here it moves corners, outside it flies a glider.
    label(dl, ImVec2(mx.x - ImGui::CalcTextSize("EDIT MODE  (Tab)").x - 34.0f, mn.y + 10.0f),
          "EDIT MODE  (Tab)", amber, false);

    if (g.kind != Kind::None) {
        label(dl, ImVec2(cx, mn.y + 40.0f), g.readout, amber, true);
        const char* hint =
            g.kind == Kind::LoopHover ? "click an edge to cut across it   Esc / right-click: cancel"
            : g.kind == Kind::LoopSlide
                ? "slide, click to place   type a value 0..1   Esc: leave it in the middle"
            : transforming
                ? "X Y Z lock an axis (again: local, again: free)   Shift+X Y Z plane   type a value   "
                  "Ctrl snap   Shift fine   Enter / click: done   Esc / right-click: cancel"
            : g.kind == Kind::Bevel
                ? "wheel: segments   type a value   Ctrl snap   Shift fine   Enter / click: done   Esc: cancel"
                : "type a value   Ctrl snap   Shift fine   Enter / click: done   Esc / right-click: cancel";
        label(dl, ImVec2(cx, mn.y + 40.0f + lh), hint, text, true);
    } else {
        label(dl, ImVec2(cx, mx.y - 2.0f * lh - 6.0f),
              "1 2 3 corner / edge / face    click  Shift+click  drag: box  Alt+click: loop    "
              "A all   Alt+A none   Ctrl+I invert   L linked",
              text, true);
        label(dl, ImVec2(cx, mx.y - lh - 4.0f),
              "G R S move rotate scale   E extrude   I inset   Ctrl+B bevel   Ctrl+R loop cut   "
              "Shift+D duplicate   F face   J connect   M merge   X delete   Alt+N normals",
              text, true);
    }
    if (ImGui::GetTime() - g_msgT < 2.5 && !g_msg.empty())
        label(dl, ImVec2(cx, mn.y + 40.0f + (g.kind != Kind::None ? 2.0f * lh : 0.0f)), g_msg,
              amber, true);
    dl->PopClipRect();
}

} // namespace modelkeys
