#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

struct Entity;
class SplineSystem;
namespace prefab { struct Prefab; }

// Placing OBJECTS along a spline path: copies of one object -- the one selected in
// the scene, or a prefab from the project -- every N metres, as real entities, in
// one undoable step. The path is usually a bare one (splinegen::Kind::Path) laid
// out for exactly this, but any path works: lamps along a wall, signals beside a
// track.
//
// Stamped, not derived, for the reason RoadPrefab.hpp gives: what is placed is an
// entity subtree whose scripts, lights and physics only mean anything as scene
// objects. So the copies are placed ONCE, are ordinary objects afterwards, and do
// not follow the path when it later moves. Ctrl+Z and place again is the way to
// try another spacing -- the viewport preview (SplineEdit's draw) is what makes
// that rarely necessary.
//
// Editor-only, like RoadPrefab: the shipped player only ever loads the result.
namespace splineplace {

// What gets copied.
enum class Source {
    Selection,   // the object selected in the scene, children and all
    Prefab,      // a .fprefab from the project's prefabs folder
};

// What the panel edits and main keeps. Metric, and every number is typed or
// stepped with +/- rather than dragged: "a lamp every 12 m, 3 m to the right"
// is a placement anybody can hit, dragging copies into line is not.
struct Settings {
    Source      source = Source::Selection;
    std::string path;              // .fprefab file, Source::Prefab ("" = none yet)
    std::string name;              // its display name, for the picker
    float spacing   = 5.0f;        // metres between copies
    float start     = 0.0f;        // metres along the path to the first copy
    float side      = 0.0f;        // metres to the path's right (negative = left)
    bool  bothSides = false;       // mirror each copy onto the other side as well
    bool  align     = true;        // turn each copy with the path
    float turn      = 0.0f;        // extra degrees about the upright axis
    float scale     = 1.0f;        // uniform, the whole subtree
    float height    = 0.0f;        // metres above where it would otherwise stand
    // Set by the panel while its placement section is open, so main draws the
    // preview markers only when they are what the author is looking at.
    bool  preview   = false;
};

// One copy's place: world position and heading. Heading 0 faces +Z, the same
// convention prefab::instantiate and every placed object here uses.
struct Spot {
    glm::vec3 pos{0.0f};
    float     yawDeg = 0.0f;
};

// The spacing actually used on path `path`. Settings::spacing on an open path;
// on a closed loop the nearest spacing that divides the loop evenly, so the last
// copy is a full step from the first instead of crowding it at the seam.
float spacingOn(const SplineSystem& sp, int path, const Settings& s);

// Where the copies would stand on path `path` -- the stations, pushed out to the
// side(s), put back on the ground there (keeping the path's own lift), raised by
// Settings::height. At most `maxCount`, so a spacing typo costs a short run
// instead of the session. Empty until the path has been built once.
std::vector<Spot> spots(const SplineSystem& sp, int path, const Settings& s,
                        int maxCount = 2000);

// A template from the scene: the subtree under `rootId`, copied AS IS -- its
// components and any prefab link it has included, since a copy of a prefab
// instance is still an instance of that prefab. Ids are made local (root = 0),
// the root's world transform becomes its own. An invalid GUID marks it as not a
// prefab file. Empty when `rootId` is not in the scene.
prefab::Prefab fromScene(const std::vector<Entity>& scene, int rootId);

// One copy of `t` per spot, ids minted from `entityCounter`, in parent-before-
// child order (ready for AddEntitiesCmd). Every copy's ROOT has parent -1, so
// the caller can put them all under one group. A template with a valid GUID (a
// prefab file) is instantiated as prefab instances; one from fromScene is copied
// plainly. Each root sits exactly on its spot, scaled by `scale`.
std::vector<Entity> stamp(const prefab::Prefab& t, const std::vector<Spot>& at,
                          float scale, int& entityCounter);

} // namespace splineplace
