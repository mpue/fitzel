#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>

#include "Command.hpp"     // CommandStack -- the undo history, stashed with the scene
#include "ProjectIO.hpp"
#include "SceneTypes.hpp"

// Editing a prefab on its own, away from the scene it is used in.
//
// WHY A MODE RATHER THAN A PANEL. A prefab is a subtree, and the only tools that
// can shape one are the scene's own -- the viewport, the gizmo, the hierarchy,
// the inspector, the mesh tools. Building a second, smaller editor for prefabs
// would mean either a poorer set of tools or two of everything. So the prefab is
// brought to the tools instead: the scene steps aside for a moment and the
// prefab IS the scene, with everything that already works pointed at it.
//
// That is also the answer to "isolated". A prefab dropped into a finished track
// to be edited sits inside a hundred other objects: you cannot see it, clicking
// picks its neighbours, and framing it means flying through scenery. Here it is
// the only thing in the document.
//
// WHAT STEPS ASIDE, exactly: the entity list and the undo history, both stashed
// here and handed back untouched on close. The undo history has to travel with
// them -- an undo recorded against a prefab's stage would, back in the scene,
// try to undo an edit to objects that are not there. The terrain, the roads and
// the vegetation stay: they are not entities, and they make a floor to stand the
// thing on rather than a void.
//
// WHAT IS SAVED is the subtree rooted at the object the prefab was opened as.
// Anything else added to the stage is scaffolding -- a light to see by, a box for
// scale -- and is deliberately NOT written to the file; the banner says how many
// such objects it is leaving behind, so nothing is lost silently.
//
// WHILE A SESSION IS OPEN the document is not the scene, so nothing that writes
// the scene may run: saving the project, autosaving, entering Play, loading
// another scene. The caller owns those doors and this cannot close them -- it
// only says, through `active`, that they should be shut.
namespace prefabedit {

// One editing session. Default-constructed is "not editing".
struct Session {
    bool            active = false;
    std::string     name;    // the prefab's display name
    std::string     path;    // its .fprefab file
    fitzel::AssetId guid;    // its identity, kept across a save so instances still match
    int             rootId = -1;   // the edited root, in the stage's id space

    // --- The scene, while the stage is up ------------------------------------
    std::vector<Entity> scene;
    CommandStack        history;
    // ...and where the eye was standing in it.
    glm::vec3 camPos{0.0f};
    float     camYaw = 0.0f, camPitch = 0.0f;
    bool      entityEdit = false;   // the editor's entity-edit toggle
};

// Open the prefab at `path` on its own stage: stash `entities` and `history`,
// replace the document with the prefab's own entities, standing its root at
// `groundAt`. `status` gets a line for the status bar either way.
//
// Returns false and changes NOTHING if a session is already open or the file
// cannot be read -- a half-entered session would leave the scene stashed with no
// way back to it.
bool open(Session& s, projectio::Context& ctx, const std::string& path,
          std::vector<Entity>& entities, CommandStack& history, int& entityCounter,
          const glm::vec3& groundAt, std::string& status);

// Write the stage back to the prefab's own file, keeping its GUID and name so
// every instance in every scene still points at it. Objects outside the edited
// root are not written; `status` says how many were left behind.
bool saveBack(Session& s, projectio::Context& ctx,
              const std::vector<Entity>& entities, const std::string& dir,
              std::string& status);

// Put the scene back and end the session. Safe to call when none is open.
void close(Session& s, std::vector<Entity>& entities, CommandStack& history);

// Where an eye should stand to see the whole of what is being edited, given the
// stage's entities. False when there is nothing to look at.
bool frame(const Session& s, const std::vector<Entity>& entities, float fovDeg,
           glm::vec3& eye, glm::vec3& lookAt);

#ifndef FITZEL_PLAYER
// What the banner across the top of the editor was asked to do this frame.
enum class Action { None, Save, SaveClose, Discard };

// Draw that banner (nothing when no session is open). It is a viewport bar
// rather than a window on purpose: it takes a strip of the screen for as long as
// the document is not the scene, which is the one thing about this mode that
// must not be possible to lose behind a panel.
Action banner(const Session& s, const std::vector<Entity>& entities);
#endif

} // namespace prefabedit
