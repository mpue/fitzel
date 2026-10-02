#include "SceneOps.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>

#include <glm/gtc/quaternion.hpp>

#include "Command.hpp"
#include "Component.hpp"
#include "EditorContext.hpp"
#include "PrefabSystem.hpp"
#include "SceneGraph.hpp"

namespace sceneops {

namespace {

bool validIndex(const EditorContext& ed, int idx) {
    return idx >= 0 && idx < static_cast<int>(ed.entities.size());
}

// The Materials panel's pick, for a new solid: what "add" gives it to wear.
void dressLikeNew(const EditorContext& ed, Entity& e) {
    if (ed.materials.empty()) return;
    auto mc = std::make_unique<MaterialComponent>();
    mc->material = ed.materials[glm::clamp(ed.matSel, 0,
                                           static_cast<int>(ed.materials.size()) - 1)].assetId;
    e.components.items.push_back(std::move(mc));
}

// A child's world transform for the rest of this frame, from its local one; the
// scene-graph resolve takes it over from here (local is the source of truth).
void seedWorld(const Entity& parent, Entity& child) {
    glm::vec3 sc;
    scenegraph::decompose(scenegraph::worldOf(parent) *
                              scenegraph::compose(child.localCenter, child.localRotation,
                                                  glm::vec3(1.0f)),
                          child.center, child.rotation, sc);
}

} // namespace

bool isUnder(const std::vector<Entity>& entities, int a, int ancestorId) {
    for (int p = a; p >= 0; ) {
        if (p == ancestorId) return true;
        int nextIdx = -1;
        for (int i = 0; i < static_cast<int>(entities.size()); ++i)
            if (entities[i].id == p) { nextIdx = i; break; }
        p = (nextIdx >= 0) ? entities[nextIdx].parent : -1;
    }
    return false;
}

void deleteEntity(EditorContext& ed, int idx) {
    if (!validIndex(ed, idx)) return;
    if (ed.entities[idx].type == EntityType::Sun) return; // the sun is permanent
    // Delete the whole subtree: the entity plus every descendant, as one
    // undoable step (deleting a parent shouldn't orphan its child parts).
    std::vector<int> ids{ed.entities[idx].id};
    for (std::size_t k = 0; k < ids.size(); ++k)
        for (const Entity& e : ed.entities)
            if (e.parent == ids[k]) ids.push_back(e.id);
    ed.history.push(std::make_unique<DeleteEntitiesCmd>(ed.document, ids), ed.document);
    ed.sel.clear();
}

// It used to unparent the copy, and that moved it. localCenter is relative to
// the parent and is the source of truth; resolveHierarchy gives a ROOT the world
// position `center = localCenter`. So a child sitting at local (0, 0, -3) on a
// craft half a map away had its copy teleported to world (1.1, 0, -3) -- next to
// the origin. On a visible object you would watch it fly off; on an Empty there
// is nothing to see, so the copy was simply somewhere else, unclickable where
// you were looking. Duplicating a thruster mount is exactly that case.
//
// The offset is in the parent's frame, which is what "beside the original"
// means for a child. `center` is left alone: it is derived, and
// resolveHierarchy fills it from the parent this frame.
// A copy is never the main camera: exactly one camera starts Play (see
// setMainCamera), and a duplicated one that kept the flag would quietly
// take over from whichever comes first in the list.
static void notMainCamera(Entity& e) {
    if (auto* cc = e.components.get<CameraComponent>()) cc->activeOnStart = false;
}

void duplicateEntity(EditorContext& ed, int idx) {
    if (!validIndex(ed, idx)) return;
    if (ed.entities[idx].type == EntityType::Sun) return;
    Entity nb = ed.entities[idx];
    nb.localCenter.x += nb.half.x * 2.2f;
    nb.id     = ed.entityCounter++;
    nb.name  += " copy";
    notMainCamera(nb);
    ed.history.push(std::make_unique<AddEntityCmd>(nb), ed.document);
    ed.sel.select(nb.id);
}

void deleteSelection(EditorContext& ed) {
    const std::vector<int> chosen = ed.sel.ids();
    if (chosen.size() <= 1) { deleteEntity(ed, ed.sel.index()); return; }
    std::vector<int> ids;
    for (int rootId : chosen) {
        const Entity* e = ed.document.find(rootId);
        if (!e || e->type == EntityType::Sun) continue;
        for (int id : scenegraph::subtree(ed.entities, rootId))
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
    }
    if (ids.empty()) return;
    ed.history.push(std::make_unique<DeleteEntitiesCmd>(ed.document, ids), ed.document);
    ed.sel.clear();
}

// With one wrinkle a single copy cannot have: when a selected object's PARENT
// was copied too, the copy must hang off the copied parent rather than the
// original. Otherwise duplicating a craft and its thrusters together gives you
// a second craft whose thrusters are still bolted to the first one.
void duplicateSelection(EditorContext& ed) {
    const std::vector<int> chosen = ed.sel.ids();
    if (chosen.size() <= 1) { duplicateEntity(ed, ed.sel.index()); return; }
    std::vector<Entity> copies;
    std::vector<int>    newIds;
    std::unordered_map<int, int> remap;   // original id -> copy id
    for (int id : chosen) {
        const Entity* src = ed.document.find(id);
        if (!src || src->type == EntityType::Sun) continue;
        Entity nb = *src;
        nb.localCenter.x += nb.half.x * 2.2f;
        nb.id     = ed.entityCounter++;
        nb.name  += " copy";
        notMainCamera(nb);
        remap[id] = nb.id;
        newIds.push_back(nb.id);
        copies.push_back(std::move(nb));
    }
    for (Entity& c : copies) {
        const auto it = remap.find(c.parent);
        if (it != remap.end()) c.parent = it->second;
    }
    if (copies.empty()) return;
    ed.history.push(std::make_unique<AddEntitiesCmd>(std::move(copies), "Duplicate"),
                    ed.document);
    ed.sel.clear();
    ed.sel.addMany(newIds);
}

void unpackPrefab(EditorContext& ed, int id) {
    std::vector<int> ids;
    auto addInstance = [&](int eid) {
        for (int m : prefab::instanceMembers(ed.entities, eid))
            if (std::find(ids.begin(), ids.end(), m) == ids.end()) ids.push_back(m);
    };
    addInstance(id);
    if (ed.sel.contains(id))
        for (int sid : ed.sel.ids()) addInstance(sid);
    if (ids.empty()) return;
    std::vector<Entity> before = ed.snapshot(ids);
    for (int m : ids)
        if (Entity* e = ed.document.find(m)) prefab::unpack(*e);
    ed.history.pushApplied(std::make_unique<ModifyEntitiesCmd>(
        std::move(before), ed.snapshot(ids), "Unpack Prefab"));
}

// The view that Play (and the exported game) starts from: activeOnStart set on
// this camera and cleared on every other, so exactly one is the main camera.
// One undoable step over all camera entities.
void setMainCamera(EditorContext& ed, int entId) {
    if (entId >= 0) {
        const Entity* e = ed.document.find(entId);
        if (!e || !e->components.get<CameraComponent>()) return;
    }
    std::vector<int> camIds;
    for (const Entity& e : ed.entities)
        if (e.components.get<CameraComponent>()) camIds.push_back(e.id);
    if (camIds.empty()) return;
    std::vector<Entity> before = ed.snapshot(camIds);
    for (Entity& e : ed.entities)
        if (auto* cc = e.components.get<CameraComponent>())
            cc->activeOnStart = (e.id == entId);
    auto cmd = std::make_unique<ModifyEntitiesCmd>(before, ed.snapshot(camIds));
    if (!cmd->trivial()) ed.history.pushApplied(std::move(cmd));
}

// Mirrors the toolbar's new object (material, light) but builds it parented.
int spawnChild(EditorContext& ed, int parentId, EntityType type, const glm::vec3& wPos,
               const glm::vec3& wRot, const glm::vec3& newHalf) {
    Entity nb;
    nb.type = type;
    nb.half = (type == EntityType::Light) ? glm::vec3(0.3f)
            : (type == EntityType::Empty) ? glm::vec3(0.5f)
            : (type == EntityType::Plane) ? glm::vec3(newHalf.x, kPlaneHalfY, newHalf.z)
            : newHalf;
    if (type == EntityType::Light)
        nb.components.items.push_back(std::make_unique<LightComponent>());
    if (isSolidPrimitive(type)) dressLikeNew(ed, nb);
    nb.id     = ed.entityCounter++;
    nb.parent = parentId;
    nb.name   = std::string(entityTypeName(type)) + " " + std::to_string(nb.id);
    Entity* p = (parentId >= 0) ? ed.document.find(parentId) : nullptr;
    const glm::mat4 pw = p ? scenegraph::worldOf(*p) : glm::mat4(1.0f);
    scenegraph::setWorld(nb, wPos, wRot, p ? &pw : nullptr);
    ed.history.push(std::make_unique<AddEntityCmd>(nb), ed.document);
    return nb.id;
}

// Index-based, like the menu that calls them: the id is captured first, so the
// entity list may safely grow underneath.
void addEmptyChild(EditorContext& ed, int idx) {
    if (!validIndex(ed, idx)) return;
    const Entity& n = ed.entities[idx];
    const int id = spawnChild(ed, n.id, EntityType::Empty, n.center, n.rotation, glm::vec3(0.5f));
    ed.sel.select(id);
}

void addPrimitiveChild(EditorContext& ed, int idx, EntityType type, const glm::vec3& newHalf) {
    if (!validIndex(ed, idx)) return;
    const Entity& n = ed.entities[idx];
    const int id = spawnChild(ed, n.id, type, n.center, glm::vec3(0.0f), newHalf);
    ed.sel.select(id);
}

// A cloth already hung from the picked object: a thin box carrying a Soft Body
// whose pinning, size and weight say what it is. The picked object is what it
// hangs FROM -- a curtain rail, a flagpole -- so the cloth is put where it would
// hang off it, as a child, turned the way it is turned.
//
// Everything here is a starting point for the Inspector, not a mode: the sizes
// are a room's curtain and a flagpole's flag, and the weights are what those
// weigh -- a flag at the component's default 20 kg would hang off its pole like
// a wet towel however hard it blew.
void addClothChild(EditorContext& ed, int idx, int which) {
    if (!validIndex(ed, idx)) return;
    const Entity& n = ed.entities[idx];
    Entity cl;
    cl.type   = EntityType::Box;
    cl.id     = ed.entityCounter++;
    cl.parent = n.id;
    auto sb = std::make_unique<SoftBodyComponent>();
    sb->kind = SoftBodyComponent::Cloth;
    if (which == 1) {                       // flag: flies from the pole's +X side
        cl.name = "Flag " + std::to_string(cl.id);
        cl.half = glm::vec3(0.75f, 0.5f, 0.01f);
        cl.localCenter = glm::vec3(n.half.x + cl.half.x + 0.03f,
                                   n.half.y - cl.half.y - 0.1f, 0.0f);
        sb->pinning    = SoftBodyComponent::PinPole;
        sb->resolution = 5;
        sb->mass       = 0.5f;
        sb->softness   = 0.3f;
        sb->damping    = 0.05f;
        // A breeze out along the way the flag points, flat: so it flies as
        // placed, and turning the pole turns where it flies.
        glm::vec3 out = glm::quat(glm::radians(n.rotation)) * glm::vec3(1.0f, 0.0f, 0.0f);
        out.y = 0.0f;
        sb->wind = glm::length(out) > 1.0e-3f ? glm::normalize(out) * 6.0f
                                              : glm::vec3(6.0f, 0.0f, 0.0f);
    } else if (which == 2) {                // banner: two top corners, below
        cl.name = "Banner " + std::to_string(cl.id);
        cl.half = glm::vec3(0.5f, 1.0f, 0.01f);
        cl.localCenter = glm::vec3(0.0f, -(n.half.y + cl.half.y + 0.02f), 0.0f);
        sb->pinning    = SoftBodyComponent::PinTopCorners;
        sb->resolution = 5;
        sb->mass       = 1.0f;
        sb->softness   = 0.3f;
    } else {                                // curtain: on rings, pleated, below
        cl.name = "Curtain " + std::to_string(cl.id);
        cl.half = glm::vec3(1.0f, 1.25f, 0.02f);
        cl.localCenter = glm::vec3(0.0f, -(n.half.y + cl.half.y + 0.02f), 0.0f);
        sb->pinning    = SoftBodyComponent::PinRings;
        sb->rings      = 10;
        sb->folds      = 0.6f;
        sb->resolution = 6;
        sb->mass       = 3.0f;
        sb->softness   = 0.35f;
        sb->damping    = 0.15f;
    }
    cl.components.items.push_back(std::move(sb));
    dressLikeNew(ed, cl);
    seedWorld(n, cl);
    ed.history.push(std::make_unique<AddEntityCmd>(cl), ed.document);
    ed.sel.select(cl.id);
}

// A camera that SHOOTS the picked object: an Empty carrying a Camera in
// Multishot mode, aimed at that object by id (see MultiShot.hpp).
//
// It is deliberately NOT a child of its subject, which is the opposite of how a
// follow camera is made here. A multishot camera stands off the thing it films
// -- ahead of it, above it, planted in the road waiting for it -- and a camera
// parented to a moving car would be fighting that transform in every shot. So
// the subject is named instead, and this menu item is what saves the author
// from having to know that.
//
// Where it is placed hardly matters (the shots decide where the eye goes), but
// it is put a sensible framing distance off the subject anyway, so the gizmo's
// tether is short and readable rather than crossing the map.
void addShotCamera(EditorContext& ed, int idx) {
    if (!validIndex(ed, idx)) return;
    const Entity& n = ed.entities[idx];
    const float r = glm::max(glm::length(glm::vec2(n.half.x, n.half.z)), 0.4f);
    Entity cam;
    cam.type        = EntityType::Empty;
    cam.half        = glm::vec3(0.5f);
    cam.id          = ed.entityCounter++;
    cam.name        = n.name + " Cam";
    cam.localCenter = cam.center =
        n.center + glm::vec3(r * 2.6f + 1.5f, n.half.y + 1.0f, 0.0f);
    auto cc = std::make_unique<CameraComponent>();
    cc->mode       = CameraComponent::Multishot;
    cc->shotTarget = n.id;
    cam.components.items.push_back(std::move(cc));
    ed.history.push(std::make_unique<AddEntityCmd>(cam), ed.document);
    ed.sel.select(cam.id);
}

// A camera that SITS IN the picked object: a Camera child in Cockpit mode,
// seated at the front of its bounding box and TURNED TO FACE THE NOSE.
//
// That half turn is the whole reason this menu item exists. A camera looks down
// its own -Z; a craft's nose is its +Z. So a camera child left at zero rotation
// -- which is what dropping one in gives you -- looks out of the BACK of the
// craft, and the obvious conclusion is that the mode is broken rather than that
// it is facing the wrong way. The turn is not done inside the camera system,
// where it would make the frustum the gizmo draws a lie; it is done once, here,
// on a camera the author can then freely turn any way they like.
//
// Which end the nose is at, the craft already says: Vehicle and Glider both
// carry `forward` for models built the other way round.
void addCockpitCamera(EditorContext& ed, int idx) {
    if (!validIndex(ed, idx)) return;
    const Entity& n = ed.entities[idx];
    const auto* gc = n.components.get<GliderComponent>();
    const auto* vc = n.components.get<VehicleComponent>();
    const bool noseBack = (gc && gc->forward == 1) || (vc && vc->forward == 1);
    const float nose = noseBack ? -1.0f : 1.0f;

    Entity cam;
    cam.type   = EntityType::Empty;
    cam.half   = glm::vec3(0.5f);
    cam.id     = ed.entityCounter++;
    cam.parent = n.id;
    cam.name   = n.name + " Cockpit";
    // A seat, not a pose: forward of centre and above it, in fractions of the
    // craft's own size so it lands sensibly on a glider and on a lorry. The
    // author drags it to the actual canopy from there -- which is the one thing
    // only they can know.
    cam.localCenter   = glm::vec3(0.0f, n.half.y * 0.35f, nose * n.half.z * 0.35f);
    cam.localRotation = glm::vec3(0.0f, noseBack ? 0.0f : 180.0f, 0.0f);
    auto cc = std::make_unique<CameraComponent>();
    cc->mode = CameraComponent::Cockpit;
    cam.components.items.push_back(std::move(cc));
    seedWorld(n, cam);
    ed.history.push(std::make_unique<AddEntityCmd>(cam), ed.document);
    ed.sel.select(cam.id);
}

// Insert a new Empty between `idx` and its current parent, then reparent `idx`
// under it -- keeping the node put. Groups the node under a fresh pivot, like
// Unity's "Create Empty Parent".
void addEmptyParent(EditorContext& ed, int idx) {
    if (!validIndex(ed, idx)) return;
    if (ed.entities[idx].type == EntityType::Sun) return; // the sun stays root
    const int       nodeId      = ed.entities[idx].id;
    const int       grandparent = ed.entities[idx].parent;
    const glm::vec3 wPos        = ed.entities[idx].center;
    const glm::vec3 wRot        = ed.entities[idx].rotation;
    const int emptyId = spawnChild(ed, grandparent, EntityType::Empty, wPos, wRot,
                                   glm::vec3(0.5f));
    Entity* node = ed.document.find(nodeId);
    Entity* emp  = ed.document.find(emptyId);
    if (node && emp) {
        node->parent = emptyId;
        const glm::mat4 pw = scenegraph::worldOf(*emp);
        scenegraph::setWorld(*node, node->center, node->rotation, &pw); // keep it where it was
    }
    ed.sel.select(emptyId);
}

// Two forward spot headlights at the nose and two red point taillights (no
// shadows) at the tail, all parented so they move and steer with the car. One
// undoable step. Nothing without a Vehicle.
void addVehicleLights(EditorContext& ed, int idx) {
    if (!validIndex(ed, idx)) return;
    Entity& veh = ed.entities[idx];
    const auto* vc = veh.components.get<VehicleComponent>();
    if (!vc) return;
    const int vehId = veh.id;
    // Body extents: the larger of the model AABB and the chassis box.
    // frontSign maps the model's nose (native -Z when forward==1) to local Z.
    const glm::vec3 h = glm::max(veh.half, vc->chassisHalf);
    const float frontSign = (vc->forward == 1) ? -1.0f : 1.0f;
    const float zx  = h.z * 0.96f * frontSign; // nose Z (tail is -zx)
    const float xo  = h.x * 0.6f;              // left/right inset
    const float yo  = h.y * 0.1f;              // just above centre
    const float yaw = (vc->forward == 1) ? 180.0f : 0.0f; // spot faces the nose
    std::vector<Entity> batch;
    auto makeLight = [&](const char* name, glm::vec3 lpos, glm::vec3 lrot,
                         bool spot, glm::vec3 col, float inten, float rng) {
        Entity nb;
        nb.type   = EntityType::Light;
        nb.half   = glm::vec3(0.12f);
        nb.id     = ed.entityCounter++;
        nb.parent = vehId;
        nb.name   = name;
        nb.localCenter   = lpos;
        nb.localRotation = lrot;
        auto lc = std::make_unique<LightComponent>();
        lc->type = spot ? 1 : 0;
        lc->color = col; lc->intensity = inten; lc->range = rng;
        lc->castShadows = false;
        if (spot) { lc->spotAngle = 30.0f; lc->spotBlend = 0.25f; }
        nb.components.items.push_back(std::move(lc));
        seedWorld(veh, nb);   // resolveHierarchy refreshes it each frame
        batch.push_back(std::move(nb));
    };
    const glm::vec3 warm(1.0f, 0.96f, 0.85f);
    const glm::vec3 red (1.0f, 0.05f, 0.02f);
    makeLight("Headlight L", { xo, yo,  zx}, {0.0f, yaw, 0.0f}, true,  warm, 12.0f, 28.0f);
    makeLight("Headlight R", {-xo, yo,  zx}, {0.0f, yaw, 0.0f}, true,  warm, 12.0f, 28.0f);
    makeLight("Taillight L", { xo, yo, -zx}, {0.0f, 0.0f, 0.0f}, false, red,   3.0f,  4.0f);
    makeLight("Taillight R", {-xo, yo, -zx}, {0.0f, 0.0f, 0.0f}, false, red,   3.0f,  4.0f);
    ed.history.push(std::make_unique<AddEntitiesCmd>(std::move(batch), "Add headlights"),
                    ed.document);
    ed.sel.select(vehId);
}

} // namespace sceneops
