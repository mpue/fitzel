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
#include "ViewportFrame.hpp" // the viewport, as every tool takes it

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

// An entity's editable mesh in the world: the mesh is drawn stretched to the
// entity's half-extents, so mesh space -> world is its transform with the scale
// half/bounds.
glm::mat4 meshModelOf(const Entity& e, const MeshComponent& mc);
// One face's corners in world space (empty for a face that is not there).
std::vector<glm::vec3> meshFaceWorld(const Entity& e, const MeshComponent& mc, int face);

// What every mesh edit ends with, whichever way it was made -- a panel button or
// a gizmo drag: re-centre the geometry on the object's origin, move the object
// by that same shift so nothing appears to jump, and take the new bounds as its
// half-extents. That invariant is what keeps the pick box, the gizmo and the
// collider describing the shape that is actually there -- an extruded tower
// whose AABB still claimed to be the original cube would be unpickable at the
// top and would collide with air at the bottom. `scale` is the one the entity
// applied to its mesh before the edit (editmesh::fitScale), kept across it.
void normalizeMeshEntity(const std::vector<Entity>& entities, Entity& e, MeshComponent& mc,
                         const glm::vec3& scale);
