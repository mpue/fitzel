#include "AnimSystem.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "CameraPath.hpp"    // catmull<>, the curve the camera path already uses
#include "Component.hpp"
#include "PropertyMeta.hpp"
#include "SceneTypes.hpp"

namespace anim {

namespace {

// Two key times count as the same moment inside this. Every time the editor
// writes is snapped to the grid first, so this only has to survive the rounding
// of a float, not a near miss by hand.
constexpr float kSame = 1e-4f;

int insertPos(const std::vector<Key>& keys, float t) {
    int i = 0;
    while (i < static_cast<int>(keys.size()) && keys[i].t < t) ++i;
    return i;
}

} // namespace

// --- Track ------------------------------------------------------------------

int Track::keyAt(float time) const {
    for (std::size_t i = 0; i < keys.size(); ++i)
        if (std::fabs(keys[i].t - time) <= kSame) return static_cast<int>(i);
    return -1;
}

float Track::valueAt(float time) const {
    if (keys.empty()) return 0.0f;
    // CLAMPED AT BOTH ENDS, never extrapolated. A track holds a property that
    // exists before and after the clip, and a line continued past the last key
    // would send it somewhere nobody authored -- an object still drifting after
    // the animation has finished is the bug that reads as "the timeline broke
    // my scene".
    if (time <= keys.front().t) return keys.front().v;
    if (time >= keys.back().t)  return keys.back().v;

    std::size_t i = 0;
    while (i + 1 < keys.size() && keys[i + 1].t <= time) ++i;
    const Key& a = keys[i];
    const Key& b = keys[i + 1];
    if (interp == Interp::Step) return a.v;

    const float span = b.t - a.t;
    const float u    = span > 1e-6f ? (time - a.t) / span : 0.0f;
    if (interp == Interp::Linear) return a.v + (b.v - a.v) * u;

    // Smooth: the neighbours give the tangents, and at the ends the segment's
    // own key stands in for the one that is not there -- which flattens the
    // curve into the first and last key instead of overshooting out of them.
    const Key& p0 = keys[i > 0 ? i - 1 : i];
    const Key& p3 = keys[i + 2 < keys.size() ? i + 2 : i + 1];
    return catmull(p0.v, a.v, b.v, p3.v, u);
}

float Clip::lastKeyTime() const {
    float last = 0.0f;
    for (const Track& t : tracks)
        if (!t.keys.empty()) last = std::max(last, t.keys.back().t);
    return last;
}

// --- Property kinds ---------------------------------------------------------

bool isAnimatable(PropKind k) { return k != PropKind::Text; }

bool isDiscrete(PropKind k) {
    return k == PropKind::Bool || k == PropKind::Int || k == PropKind::EnumInt;
}

int componentCount(PropKind k) {
    return (k == PropKind::Vec3 || k == PropKind::Color) ? 3 : 1;
}

const char* channelSuffix(PropKind k, int index) {
    if (componentCount(k) == 1) return "";
    switch (index) {
        case 0: return k == PropKind::Color ? ".r" : ".x";
        case 1: return k == PropKind::Color ? ".g" : ".y";
        default: return k == PropKind::Color ? ".b" : ".z";
    }
}

float readValue(const Property& p, const void* owner, int index) {
    void* f = p.field(const_cast<void*>(owner));
    switch (p.kind) {
        case PropKind::Float:   return *static_cast<float*>(f);
        case PropKind::Vec3:
        case PropKind::Color:   return (&static_cast<glm::vec3*>(f)->x)[std::clamp(index, 0, 2)];
        case PropKind::Bool:    return *static_cast<bool*>(f) ? 1.0f : 0.0f;
        case PropKind::Int:
        case PropKind::EnumInt: return static_cast<float>(*static_cast<int*>(f));
        case PropKind::Text:    break;
    }
    return 0.0f;
}

void writeValue(const Property& p, void* owner, int index, float v) {
    void* f = p.field(owner);
    switch (p.kind) {
        case PropKind::Float:   *static_cast<float*>(f) = v; break;
        case PropKind::Vec3:
        case PropKind::Color:   (&static_cast<glm::vec3*>(f)->x)[std::clamp(index, 0, 2)] = v; break;
        case PropKind::Bool:    *static_cast<bool*>(f) = v >= 0.5f; break;
        case PropKind::Int:
        case PropKind::EnumInt: *static_cast<int*>(f) = static_cast<int>(std::lround(v)); break;
        case PropKind::Text:    break;
    }
}

// --- Binding ----------------------------------------------------------------

const std::vector<Property>* propsFor(const Entity& e, const std::string& comp) {
    if (comp.empty()) return &entityProperties();
    for (const auto& c : e.components.items)
        if (comp == c->typeId()) return &c->props();
    return nullptr;
}

Bound bind(const Track& t, std::vector<Entity>& entities) {
    for (Entity& e : entities) {
        if (e.id != t.entityId) continue;
        void* owner = &e;
        const std::vector<Property>* props = nullptr;
        if (t.comp.empty()) {
            props = &entityProperties();
        } else {
            for (auto& c : e.components.items)
                if (t.comp == c->typeId()) { owner = c.get(); props = &c->props(); break; }
        }
        if (!props) return {};
        for (const Property& p : *props)
            if (p.key == t.key) return {&p, owner};
        return {};
    }
    return {};
}

// --- Playing ----------------------------------------------------------------

void apply(const Clip& c, std::vector<Entity>& entities, float time) {
    for (const Track& t : c.tracks) {
        if (t.keys.empty()) continue;
        const Bound b = bind(t, entities);
        if (!b || !isAnimatable(b.prop->kind)) continue;
        writeValue(*b.prop, b.owner, t.index, t.valueAt(time));
    }
}

void beginPreview(const Clip& c, std::vector<Entity>& entities, Player& p) {
    p.restore.assign(c.tracks.size(), 0.0f);
    for (std::size_t i = 0; i < c.tracks.size(); ++i) {
        const Bound b = bind(c.tracks[i], entities);
        if (b) p.restore[i] = readValue(*b.prop, b.owner, c.tracks[i].index);
    }
    p.preview = true;
}

void endPreview(const Clip& c, std::vector<Entity>& entities, Player& p) {
    if (!p.preview) return;
    // Only as many as were captured: a track added mid-preview has no saved
    // value, and inventing one would write a zero into the scene.
    const std::size_t n = std::min(p.restore.size(), c.tracks.size());
    for (std::size_t i = 0; i < n; ++i) {
        const Bound b = bind(c.tracks[i], entities);
        if (b) writeValue(*b.prop, b.owner, c.tracks[i].index, p.restore[i]);
    }
    p.restore.clear();
    p.preview = false;
    p.playing = false;
}

bool advance(const Clip& c, std::vector<Entity>& entities, Player& p, float dt) {
    const float end = std::max(c.duration, c.lastKeyTime());
    p.time += dt * c.speed;
    bool running = true;
    if (p.time >= end) {
        if (c.loop && end > 1e-4f) {
            p.time = std::fmod(p.time, end);
        } else {
            p.time  = end;
            running = false;
        }
    }
    apply(c, entities, p.time);
    return running;
}

// --- Editing ----------------------------------------------------------------

float snap(const Clip& c, float t) {
    const float fps = c.fps > 0.1f ? c.fps : 10.0f;
    return std::max(0.0f, std::round(t * fps) / fps);
}

const Track* findTrack(const Clip& c, int entityId, const char* comp,
                       const std::string& key, int index) {
    for (const Track& t : c.tracks)
        if (t.entityId == entityId && t.comp == comp && t.key == key && t.index == index)
            return &t;
    return nullptr;
}

Track& track(Clip& c, int entityId, const char* comp, const Property& p, int index) {
    for (Track& t : c.tracks)
        if (t.entityId == entityId && t.comp == comp && t.key == p.key && t.index == index)
            return t;
    Track t;
    t.entityId = entityId;
    t.comp     = comp ? comp : "";
    t.key      = p.key;
    t.index    = index;
    t.interp   = isDiscrete(p.kind) ? Interp::Step : Interp::Linear;
    c.tracks.push_back(std::move(t));
    return c.tracks.back();
}

void keyProperty(Clip& c, int entityId, const char* comp, const Property& p,
                 const void* owner, float time) {
    if (!isAnimatable(p.kind)) return;
    const float t = snap(c, time);
    for (int i = 0; i < componentCount(p.kind); ++i) {
        Track& tr = track(c, entityId, comp, p, i);
        const float v  = readValue(p, owner, i);
        const int   at = tr.keyAt(t);
        if (at >= 0) tr.keys[at].v = v;
        else         tr.keys.insert(tr.keys.begin() + insertPos(tr.keys, t), Key{t, v});
    }
}

void unkeyProperty(Clip& c, int entityId, const char* comp, const Property& p, float time) {
    const float t = snap(c, time);
    for (int i = 0; i < componentCount(p.kind); ++i) {
        for (Track& tr : c.tracks) {
            if (tr.entityId != entityId || tr.comp != comp || tr.key != p.key || tr.index != i)
                continue;
            const int at = tr.keyAt(t);
            if (at >= 0) tr.keys.erase(tr.keys.begin() + at);
        }
    }
    // A track with nothing left in it is not an empty animation, it is no
    // animation: leaving it would keep the property's row on the timeline and
    // its diamond lit in the inspector, both saying something is animated when
    // nothing is.
    c.tracks.erase(std::remove_if(c.tracks.begin(), c.tracks.end(),
                                  [](const Track& t) { return t.keys.empty(); }),
                   c.tracks.end());
}

bool isAnimated(const Clip& c, int entityId, const char* comp, const Property& p) {
    for (int i = 0; i < componentCount(p.kind); ++i)
        if (const Track* t = findTrack(c, entityId, comp, p.key, i))
            if (!t->keys.empty()) return true;
    return false;
}

bool isKeyedAt(const Clip& c, int entityId, const char* comp, const Property& p, float time) {
    const float t = snap(c, time);
    for (int i = 0; i < componentCount(p.kind); ++i)
        if (const Track* tr = findTrack(c, entityId, comp, p.key, i))
            if (tr->keyAt(t) >= 0) return true;
    return false;
}

// --- Persistence ------------------------------------------------------------

int findClip(const std::vector<Clip>& clips, const std::string& name) {
    for (int i = 0; i < static_cast<int>(clips.size()); ++i)
        if (clips[i].name == name) return i;
    return -1;
}

namespace {

void writeClip(nlohmann::json& a, const Clip& c) {
    a["name"]        = c.name;
    a["duration"]    = c.duration;
    a["fps"]         = c.fps;
    a["speed"]       = c.speed;
    a["loop"]        = c.loop;
    a["playOnStart"] = c.playOnStart;
    nlohmann::json arr = nlohmann::json::array();
    for (const Track& t : c.tracks) {
        if (t.keys.empty()) continue;
        std::ostringstream ks;
        ks.precision(7);
        for (const Key& k : t.keys) ks << k.t << ' ' << k.v << ' ';
        arr.push_back({{"e", t.entityId}, {"c", t.comp}, {"k", t.key},
                       {"i", t.index}, {"p", static_cast<int>(t.interp)},
                       {"keys", ks.str()}});
    }
    a["tracks"] = std::move(arr);
}

void readClip(const nlohmann::json& a, Clip& c) {
    c = Clip{};
    c.name        = a.value("name", std::string{"Animation"});
    c.duration    = a.value("duration", 10.0f);
    c.fps         = a.value("fps", 10.0f);
    c.speed       = a.value("speed", 1.0f);
    c.loop        = a.value("loop", false);
    c.playOnStart = a.value("playOnStart", false);
    const auto ta = a.find("tracks");
    if (ta == a.end() || !ta->is_array()) return;
    for (const nlohmann::json& tj : *ta) {
        Track t;
        t.entityId = tj.value("e", 0);
        t.comp     = tj.value("c", std::string{});
        t.key      = tj.value("k", std::string{});
        t.index    = tj.value("i", 0);
        const int ip = tj.value("p", 0);
        t.interp = ip == 1 ? Interp::Smooth : (ip == 2 ? Interp::Step : Interp::Linear);
        std::istringstream ks(tj.value("keys", std::string{}));
        float kt = 0.0f, kv = 0.0f;
        while (ks >> kt >> kv) t.keys.push_back(Key{kt, kv});
        std::sort(t.keys.begin(), t.keys.end(),
                  [](const Key& x, const Key& y) { return x.t < y.t; });
        if (!t.key.empty() && !t.keys.empty()) c.tracks.push_back(std::move(t));
    }
}

} // namespace

void save(nlohmann::json& j, const std::vector<Clip>& clips) {
    nlohmann::json arr = nlohmann::json::array();
    for (const Clip& c : clips) {
        // An empty clip nobody named is not worth a line in the scene file; one
        // the author named is, even with nothing in it yet -- they made it on
        // purpose and expect to find it again.
        if (c.tracks.empty() && c.name == "Animation") continue;
        nlohmann::json a;
        writeClip(a, c);
        arr.push_back(std::move(a));
    }
    j["anim"] = nlohmann::json{{"clips", std::move(arr)}};
}

void load(const nlohmann::json& j, std::vector<Clip>& clips) {
    clips.clear();
    const auto it = j.find("anim");
    if (it == j.end() || !it->is_object()) return;   // a scene with no animation
    const auto ca = it->find("clips");
    if (ca != it->end() && ca->is_array()) {
        for (const nlohmann::json& cj : *ca) {
            Clip c;
            readClip(cj, c);
            clips.push_back(std::move(c));
        }
        return;
    }
    // The older shape: one unnamed clip written straight into "anim".
    if (it->contains("tracks")) {
        Clip c;
        readClip(*it, c);
        clips.push_back(std::move(c));
    }
}

} // namespace anim
