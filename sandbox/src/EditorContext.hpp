#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "Command.hpp"
#include "Document.hpp"
#include "SceneTypes.hpp"
#include "Selection.hpp"

class ModelLibrary;
class MeshComponent;
namespace fitzel { class AssetDatabase; class Camera; }

// The editor's core, as a tool that lives outside main() sees it: the scene
// (the document and its entities), the selection, the undo history, the scene's
// material library and the asset database it is drawn from, the model library,
// the camera and the status line -- plus the few operations only main can do.
//
// main builds ONE of these and hands it to every tool, which then adds only its
// own state. Before it existed, each tool pulled out of main() needed its own
// context carrying the same fifteen references, and moving a tool out mostly
// moved that repetition around. Editor only.
struct EditorContext {
    Document&                 document;
    std::vector<Entity>&      entities;    // document.entities()
    Selection&                sel;
    CommandStack&             history;
    std::vector<MaterialDef>& materials;   // the scene's material library
    int&                      matSel;      // ...and the one the Materials panel shows
    fitzel::AssetDatabase&    assetDb;
    ModelLibrary&             models;
    fitzel::Camera&           camera;
    std::string&              status;      // the one-line message under the File menu
    // The modelling panel's face selection: which face, of whose mesh. Tools
    // that change a mesh's faces keep it honest (a split face is not the one
    // that was picked).
    int& meshFaceOwner;
    int& meshFaceSel;

    // What main still does itself.
    std::function<void(glm::vec3, int)>                addModelEntity;     // a loaded model, on the ground
    std::function<void(glm::vec3, const std::string&)> addModelHierarchy;  // ...one entity per node
    std::function<bool(const std::string&)>            isStructuredModel;  // import that file as a hierarchy?

    // Copies of these entities as they are now: an undo step's "before".
    std::vector<Entity> snapshot(const std::vector<int>& ids) const;
    // Bank the change from `before` to the same entities as they are now as one
    // undo step -- none when nothing changed. The edit itself is already made.
    void commitEdit(std::vector<Entity> before, const std::vector<int>& ids,
                    const char* label = "Transform");
};

// The scene viewport this frame, as a tool needs it: the camera's
// view-projection, the image's place and size on screen, the cursor over it,
// and the ground under a point of it.
struct ViewportFrame {
    glm::mat4 viewProj{1.0f};
    ImVec2    origin{0.0f, 0.0f};   // the image's top-left, screen pixels
    float     w = 1.0f, h = 1.0f;   // its size, pixels
    bool      hovered = false;      // the cursor is over it (and not over a widget on it)
    glm::vec2 mouseNdc{0.0f};       // the cursor in NDC [-1, 1]
    ImVec2    mousePos{0.0f, 0.0f}; // ...and in screen pixels
    // The terrain under a viewport NDC point (main's roadPickTerrain), and its height.
    std::function<bool(glm::vec2, const glm::mat4&, glm::vec3&)> pickTerrain;
    std::function<float(float, float)>                           groundAt;

    // The world ray under the cursor.
    void mouseRay(glm::vec3& origin, glm::vec3& dir) const;
    // A world point in screen pixels; false behind the camera.
    bool toScreen(const glm::vec3& p, ImVec2& out) const;
    // ...and false past the far plane as well: for marks that must not show
    // through from beyond what the view draws.
    bool project(const glm::vec3& p, ImVec2& out) const;
    // A box's twelve edges -- the corners lo..hi, through `model` -- into the
    // current window's draw list. An edge with a corner behind the camera or
    // past the far plane is left out.
    void wireBox(const glm::mat4& model, const glm::vec3& lo, const glm::vec3& hi,
                 ImU32 col, float thick) const;
};

// An entity's editable mesh in the world: the mesh is drawn stretched to the
// entity's half-extents, so mesh space -> world is its transform with the scale
// half/bounds.
glm::mat4 meshModelOf(const Entity& e, const MeshComponent& mc);
// One face's corners in world space (empty for a face that is not there).
std::vector<glm::vec3> meshFaceWorld(const Entity& e, const MeshComponent& mc, int face);
