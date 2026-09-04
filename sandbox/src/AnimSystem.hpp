#pragma once

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "Property.hpp"

struct Entity;

// Keyframe animation over ANY inspector-reachable property.
//
// WHY IT NEEDS NO PER-FIELD REGISTRATION. Every field the Inspector shows is
// already declared once as a Property (label, kind, and an accessor from an
// owner pointer to the field itself) -- for the entity's own fields and for
// every component alike. That declaration is a complete address for a number in
// the scene, so a track can be stored as "entity 7, component `light`, key
// `range`, sub-index 0" and resolved against the live scene whenever it is
// played. Nothing here knows what a light or a spin is, and a component added
// tomorrow is animatable the day it gets its property table.
//
// THE CLIP IS SCENE DATA, like the camera path and for the same reason: an
// animation belongs to the level, not to the editor that made it. It is saved
// into the .fitzel and played by the shipped player.
namespace anim {

// One keyframe: a value at a time. Both are plain floats -- a bool track holds
// 0/1, an enum holds its index -- because a single numeric channel is what makes
// the sampling, the storage and the timeline row one piece of code instead of
// seven.
struct Key {
    float t = 0.0f;   // seconds from the clip start
    float v = 0.0f;
};

enum class Interp { Linear, Smooth, Step };

// One animated number.
//
// The address is stored as ids and STRINGS rather than pointers: entities are a
// vector that reallocates, components are re-created on load, and a scene that
// is saved and reopened has to find the same field again. The entity id is
// stable by design (it survives deletion and reordering) and the property key is
// the same one the field serializes under, so a track outlives everything except
// deleting the object or the field itself -- and a track that cannot be resolved
// is skipped, not dropped. That is deliberate on both counts: a component
// detached for a moment does not throw its animation away, and neither does
// deleting an object, which is an UNDOABLE step -- forgetting the tracks there
// would mean the undo brought the object back stripped of its movement.
struct Track {
    int         entityId = 0;
    std::string comp;        // component typeId; "" = the entity's own fields
    std::string key;         // Property::key
    int         index = 0;   // which number of a Vec3/Color; 0 for scalars
    // How the value gets from one key to the next. Linear is the default
    // because it is what an author predicts; Smooth is what you reach for once a
    // move exists and reads as mechanical. Step is not a choice -- it is what a
    // bool or an enum IS, since there is no half-way between two of them, and
    // the track is created that way for those kinds.
    Interp      interp = Interp::Linear;
    std::vector<Key> keys;   // kept sorted by t

    float valueAt(float time) const;
    // Index of the key at `time` (within half a millisecond), or -1.
    int   keyAt(float time) const;
};

// One animation: a name and the tracks that make it up.
//
// A scene holds a LIST of these (see below) and an object chooses one with an
// Animator component, the way a Unity object picks a clip. Tracks still name
// their own objects by id, so a clip is not confined to the object the Animator
// hangs on -- one clip can move a door, its handle and the light over it, and
// the component only says which of the scene's animations runs.
struct Clip {
    std::string name = "Animation";
    std::vector<Track> tracks;
    float duration    = 10.0f;  // seconds; the timeline's extent
    float fps         = 10.0f;  // the snap grid -- see snap()
    float speed       = 1.0f;   // playback multiplier
    bool  loop        = false;
    bool  playOnStart = false;  // run it the moment the game starts

    bool empty() const { return tracks.empty(); }
    // Last keyed moment, which is what "the animation is this long" really
    // means; `duration` is only where the timeline stops drawing.
    float lastKeyTime() const;
};

// Where a clip is being played or scrubbed, and the state needed to put the
// scene back the way it was found. Editor and game share the type; only the
// editor uses `restore`.
struct Player {
    float time     = 0.0f;
    bool  playing  = false;
    // A preview started from the Timeline panel, as opposed to a game running.
    // A preview OWNS the values it animates and hands them back when it stops,
    // because scrubbing is looking, not editing -- stopping the playhead must
    // not leave the scene at whatever pose frame 137 happened to be.
    bool  preview  = false;
    std::vector<float> restore;   // one per track, captured when a preview starts
};

// --- Reading and writing one property as a number ---------------------------
// Text has no meaningful in-between value, so it is the one kind that cannot be
// keyed. Bool and the two int kinds can: they step rather than ramp.
bool isAnimatable(PropKind k);
bool isDiscrete(PropKind k);      // bool/int/enum: hold, don't ramp
// How many numbers a property contributes (3 for Vec3/Color, else 1).
int  componentCount(PropKind k);
// Sub-channel name for the timeline row ("x"/"y"/"z", or "" for a scalar).
const char* channelSuffix(PropKind k, int index);

float readValue(const Property& p, const void* owner, int index);
void  writeValue(const Property& p, void* owner, int index, float v);

// --- Resolving a track against the live scene -------------------------------
struct Bound {
    const Property* prop  = nullptr;
    void*           owner = nullptr;
    explicit operator bool() const { return prop != nullptr; }
};
Bound bind(const Track& t, std::vector<Entity>& entities);
// The properties of one owner, for the panel's "add track" menu.
const std::vector<Property>* propsFor(const Entity& e, const std::string& comp);

// --- Playing ----------------------------------------------------------------
// Write every resolvable track's value at `time` into the scene.
void apply(const Clip& c, std::vector<Entity>& entities, float time);

// Start/stop an editor preview: the first captures what the clip is about to
// overwrite, the second gives it back.
void beginPreview(const Clip& c, std::vector<Entity>& entities, Player& p);
void endPreview(const Clip& c, std::vector<Entity>& entities, Player& p);

// Advance a running clip and apply it. Returns false when a non-looping clip has
// run out, which is the caller's cue to stop.
bool advance(const Clip& c, std::vector<Entity>& entities, Player& p, float dt);

// --- Editing ----------------------------------------------------------------
// Round to the clip's grid. EVERY time the editor writes goes through this: a
// key that lands on 2.9971 seconds looks identical to one on 3.0 and behaves
// differently forever, and lining two of them up by hand is exactly the fine
// aiming this editor exists to avoid.
float snap(const Clip& c, float t);

// Find (or create) the track for one property channel.
Track& track(Clip& c, int entityId, const char* comp, const Property& p, int index);
const Track* findTrack(const Clip& c, int entityId, const char* comp,
                       const std::string& key, int index);

// Key the property's CURRENT value at `time`, replacing a key already there.
// Keys every channel of a Vec3 in one call: an author moving an object means the
// position, not the x of it, and three separate buttons would be three chances
// to key two of them.
void keyProperty(Clip& c, int entityId, const char* comp, const Property& p,
                 const void* owner, float time);
// Remove this property's keys at `time` (and any track left empty).
void unkeyProperty(Clip& c, int entityId, const char* comp, const Property& p,
                   float time);
// Is any channel of this property animated / keyed exactly here?
bool isAnimated(const Clip& c, int entityId, const char* comp, const Property& p);
bool isKeyedAt(const Clip& c, int entityId, const char* comp, const Property& p,
               float time);

// The scene's animations. Never empty in the editor: main keeps one clip in it
// the way the material library keeps a Default, so every path that keys a
// property has somewhere to put it without first asking the author to create an
// animation they did not know they needed.
int findClip(const std::vector<Clip>& clips, const std::string& name); // -1 if none

// --- Persistence ------------------------------------------------------------
// Written under one "anim" object in the scene, as a list of clips. The keys
// themselves are a compact space-separated blob per track (t v t v ...), the
// same scheme the camera path and the painted grass use -- pretty-printed JSON
// would otherwise spend a line on every number.
void save(nlohmann::json& j, const std::vector<Clip>& clips);
// Reads the list. A scene written before clips had names holds a single clip
// inline under "anim"; it loads as one clip called "Animation", so the first
// scenes anyone animated do not lose their work to the feature that came next.
void load(const nlohmann::json& j, std::vector<Clip>& clips);

} // namespace anim
