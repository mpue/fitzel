#pragma once

#include <functional>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include "SceneTypes.hpp"

struct ViewportFrame;

// The editor's "Add" menu: the basic objects of a scene -- shapes, lights,
// cameras, the player's start, triggers, sounds, effects -- by category, behind
// one filter field. Each one is an ordinary entity carrying the component that
// makes it what it is (a Point light is a Light entity with a Light component, a
// Player start an Empty with a Player Start component), set up the way it is
// first wanted: a spot light up high and pointing down, a door a door's size.
//
// It opens in three places, always the same list:
//   - the 3D viewport: a right-CLICK (a right-drag still looks around), or
//     Shift+A as in Blender -- the object lands on the ground under the pointer;
//   - the Hierarchy's empty space: a new root, at the 3D cursor or in view;
//   - a Hierarchy row, "Add child": hung under that row, at its centre.
//
// The menu knows the catalogue and draws it; ADDING is the caller's (main owns
// the undo history and the scene graph), handed in as Actions. Editor only.
namespace addmenu {

enum class Special {
    None,
    CameraFromView,   // a camera where the viewport's eye is (sceneops::addCameraFromView)
    Terrain,          // the scene's ground (one per scene)
};

struct Item {
    const char* category;
    const char* label;
    const char* hint;        // tooltip: what it is and what to do with it
    const char* keywords;    // more words the filter finds it by
    EntityType  type      = EntityType::Empty;
    glm::vec3   half{0.5f};  // half-extents (ignored with newSize)
    bool        newSize   = false;  // the size the toolbar gives new primitives
    float       lift      = 0.0f;   // its base this far above the ground
    const char* component = nullptr;          // type id, made by components::create
    void      (*setup)(Entity&) = nullptr;    // anything past the defaults
    Special     special   = Special::None;
};

const std::vector<Item>& catalog();

// Does the filter find this item (label, category, keywords; any case)?
bool matches(const Item& it, const char* filter);

// Where a new object goes.
struct Target {
    glm::vec3 at{0.0f};
    bool      onGround = true;   // `at` is the ground it stands on (else its centre)
    int       parentId = -1;     // hang it under this entity
};

// The entity for an item: id and name set, component attached and set up,
// standing on (or centred at) `at` in WORLD space. Parenting is the caller's.
Entity make(const Item& it, int id, const Target& t, const glm::vec3& newHalf);

struct Actions {
    std::function<void(const Item&, const Target&)> add;
    bool cameraFromView = true;   // offer the specials (false: hidden)
    bool terrain        = true;
};

// The menu's contents, inside a popup or menu the caller has begun: the
// filter (focused when it appears) and the catalogue in a scrolling list.
// Enter adds the first match. Returns true once something was added -- the
// popup is closed by then.
bool body(const Actions& a, const Target& t);

// The viewport's menu. `rightClick`, when not null, is where a right-click
// that did not turn the view went down (main tells a click from a look); Shift+A
// over the viewport opens it at the pointer too.
void viewport(const ViewportFrame& view, bool editing, const ImVec2* rightClick,
              const Actions& a);

} // namespace addmenu
