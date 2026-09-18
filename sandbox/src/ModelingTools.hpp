#pragma once

#include <functional>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "EditMesh.hpp"

class MeshComponent;

// The viewport side of modelling: what is selected on the mesh (corners, edges
// or a face), what the pointer is over, and how all of that is drawn over the
// scene. The buttons that act on it are the floating Modeling panel's
// (ModelingPanel.hpp), which shares the selection and the amounts below.
//
// The panel shows what a button WOULD do while the pointer rests on it (a ghost
// of the result over the mesh), and the overlay flashes what an edit DID right
// after it happened -- so an operation is never judged only after the fact.
//
// Tremor-friendly by the rules of the editor: nothing here needs a drag, the
// pick radius is generous, and adding to a selection is a sticky toggle on the
// panel rather than a modifier chord (Shift works too).
namespace modeltools {

enum class Mode { Vertex, Edge, Face };

struct Selection {
    Mode                             mode = Mode::Face;
    std::vector<int>                 verts;   // vertex mode
    std::vector<std::pair<int, int>> edges;   // edge mode, (lower, higher) corner
    // Face mode: every picked face. The ACTIVE one -- the one the material, the
    // UV panel and the loop cut act on -- is the host's faceSel, which is always
    // in here too (validate() sees to that).
    std::vector<int>                 faces;
    bool                             additive = false;   // clicks add instead of replace
    int                              owner    = -1;      // entity the indices belong to

    bool any() const { return !verts.empty() || !edges.empty() || !faces.empty(); }
    void clear() { verts.clear(); edges.clear(); faces.clear(); }
};

// The amounts the operations apply, shared by the panel and the toolbar. They
// live between invocations on purpose: pressing the same button five times with
// the same step is a legitimate way to build a staircase.
struct Amounts {
    float extrude = 0.5f;
    float move    = 0.25f;
    float inset   = 0.1f;
    float scale   = 0.75f;
    float loopAt  = 0.5f;    // where a loop cut falls, 0..1
    int   loopDir = 0;       // which of the two bands through a quad
    float nudge   = 0.25f;   // metres per click of the nudge arrows
    float bevel   = 0.1f;    // bevel width, metres
    float bevelSegs = 1.0f;  // 1 = a flat chamfer, more = rounded (a whole number)
    float weld    = 0.01f;   // corners closer than this merge
};
Amounts& amounts();

// Drop indices that no longer mean anything: the selection moved to another
// object (`ownerId`), or an undo left the mesh smaller than the indices. With
// `faceSel` it also keeps the picked faces and the active one agreeing: no
// active face means none picked, and an active face is always among them.
void validate(Selection& s, int ownerId, const EditMesh* mesh, int* faceSel = nullptr);

// The picked faces, the active one first. What every face operation is handed.
std::vector<int> selectedFaces(const Selection& s, const EditMesh& m, int faceSel);

// Everything, in the current mode (every corner / edge / face).
void selectAll(Selection& s, const EditMesh& m, int& faceSel);
// One ring more: the faces sharing an edge with a picked face, the corners one
// edge from a picked corner, the edges touching a picked edge. Pressed a few
// times it picks a whole side without aiming at every piece of it.
void grow(Selection& s, const EditMesh& m, int& faceSel);

// Switch between vertex, edge and face selection, carrying what is selected
// across (a face becomes its corners, corners become the face they close...).
void setMode(Selection& s, Mode mode, const EditMesh& m, int& faceSel);

// The corners the gizmo and the nudge arrows move: the selected face's, the
// selected corners, or both ends of every selected edge. Unique.
std::vector<int> activeVerts(const Selection& s, const EditMesh& m, int faceSel);

// How the mesh lands on the screen.
struct View {
    glm::mat4 model{1.0f};   // mesh space -> world
    glm::mat4 vp{1.0f};      // world -> clip
    ImVec2    min{0, 0};     // the viewport image's rect
    ImVec2    size{1, 1};
};

// What the pointer is over, in the current mode. `onMesh` is true whenever the
// ray meets the mesh at all, so a click that misses every corner but lands on
// the object does not select something behind it.
struct Hit {
    int  vert = -1;
    int  ea = -1, eb = -1;
    int  face = -1;
    bool onMesh = false;
};
Hit pick(const EditMesh& m, const View& v, ImVec2 mouse, Mode mode);

// A click at `h`. Returns true when the mesh took it (the caller then does not
// go on to select objects).
bool click(Selection& s, int& faceSel, const Hit& h, bool additive);

// --- Blender-style picking (the modelling mode, ModelingKeys.cpp) ------------

// Everything in the screen rectangle a..b that is in view (not hidden behind the
// mesh): corners inside it, edges with both ends inside, faces turned towards
// the camera whose centre is inside. Replaces, adds to or takes from the pick.
enum class BoxOp { Set, Add, Subtract };
void boxSelect(Selection& s, int& faceSel, const EditMesh& m, const View& v,
               ImVec2 a, ImVec2 b, BoxOp op);

// Alt+click: the loop through what is under the pointer -- the edge loop in edge
// and vertex mode, the ring of faces across the nearest edge in face mode.
// Returns false when there was nothing there.
bool loopSelect(Selection& s, int& faceSel, const EditMesh& m, const View& v,
                ImVec2 mouse, bool add);

// Everything joined to the pointer's element (L; `under` the pointer's hit) or
// to what is picked already (Ctrl+L; `under` null), added to the pick.
void selectLinked(Selection& s, int& faceSel, const EditMesh& m, const Hit* under);
// Ctrl+I, and Ctrl+Numpad minus: the pick turned inside out, one ring less.
void invert(Selection& s, int& faceSel, const EditMesh& m);
void shrink(Selection& s, int& faceSel, const EditMesh& m);

// A world point on the viewport (false behind the camera), and the ray under a
// pixel -- the two conversions every pointer-driven operation is made of.
bool toScreen(const View& v, const glm::vec3& world, ImVec2& out);
void rayThrough(const View& v, ImVec2 px, glm::vec3& ro, glm::vec3& rd);

// An operation as the preview runs it: on a copy of the mesh.
using Op = std::function<int(EditMesh&)>;
// Offer `op`'s result as a ghost over the mesh (call while its button is under
// the pointer; it lasts until the next overlay is drawn).
void preview(Op op);

// Remember what an edit changed so the overlay can flash it: `before` and
// `after` in mesh space, `model` the mesh's transform when the edit ran.
void flash(const EditMesh& before, const EditMesh& after, const glm::mat4& model,
           const char* label);

// Wireframe, corners, hover, selection, preview ghost and the flash, drawn into
// `dl` over the viewport. `hover` may be null.
void drawOverlay(ImDrawList* dl, const EditMesh& m, const View& v,
                 const Selection& s, int faceSel, const Hit* hover);

// A small rounded label, for a number next to the pointer during a drag.
void readout(ImDrawList* dl, ImVec2 at, const char* text);

// Where the Modeling panel is this frame, so the name of the last edit can be
// written just under it. The panel reports this while it draws.
void panelRect(ImVec2 mn, ImVec2 mx);

} // namespace modeltools
