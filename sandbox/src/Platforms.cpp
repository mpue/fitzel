#include "Platforms.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <fitzel/physics/Physics.hpp>

#include "Component.hpp"   // MoverComponent, SpinnerComponent, PhysicsComponent
#include "SceneGraph.hpp"

namespace platforms {
namespace {

bool moves(const Entity& e) {
    return e.components.get<MoverComponent>() || e.components.get<SpinnerComponent>() ||
           isHull(e);
}

// The empty Model a structured import puts its parts under (PhysicsShapes'
// isModelGroup, which this file does not link).
bool modelGroup(const Entity& e) {
    return e.type == EntityType::Model && !e.components.get<ModelComponent>();
}

// Does this part stand in the way: something with a static collider? Not a
// model group's root -- its parts collide (groupPart) -- and not a sail, which
// is cloth.
bool solid(const Entity& e) {
    const auto* pc = e.components.get<PhysicsComponent>();
    if (!pc || pc->dynamic || !e.activeInHierarchy) return false;
    if (modelGroup(e) || e.components.get<SailComponent>()) return false;
    return e.type != EntityType::Empty && e.type != EntityType::Light && e.type != EntityType::Sun;
}

using Index = std::unordered_map<int, const Entity*>;

Index indexOf(const std::vector<Entity>& entities) {
    Index idx;
    idx.reserve(entities.size());
    for (const Entity& e : entities) idx[e.id] = &e;
    return idx;
}

// A part of a model group with static Physics that has none of its own: it
// collides as its own triangles, as addGroupBodies has it for the static world
// (a part with Physics of its own, and whatever hangs below that, is its own
// business; a swinging part brings its own boxes; a sail is cloth).
bool groupPart(const Index& idx, const Entity& e) {
    if (!e.activeInHierarchy || !e.components.get<ModelComponent>() ||
        e.components.get<PhysicsComponent>() || e.components.get<SailComponent>() ||
        e.components.get<SwingComponent>())
        return false;
    const Entity* cur = &e;
    for (int guard = 0; cur->parent >= 0 && guard < 256; ++guard) {
        auto it = idx.find(cur->parent);
        if (it == idx.end()) return false;
        cur = it->second;
        if (cur->components.get<SwingComponent>()) return false;
        if (const auto* pc = cur->components.get<PhysicsComponent>())
            return !pc->dynamic && modelGroup(*cur);
    }
    return false;
}

// Is there anything of this part to float: geometry, not a light or a group?
bool hasBody(const Entity& e) {
    if (!e.activeInHierarchy || e.components.get<SailComponent>()) return false;
    if (e.type == EntityType::Empty || e.type == EntityType::Light || e.type == EntityType::Sun)
        return false;
    return !modelGroup(e);
}

// The nearest moving object at or above `e` (itself included), or -1. The
// guard ends the walk on a parent chain a scene file claims is a loop.
int moverOf(const Index& idx, const Entity& e) {
    const Entity* cur = &e;
    for (int guard = 0; cur && guard < 256; ++guard) {
        if (moves(*cur)) return cur->id;
        if (cur->parent < 0) break;
        auto it = idx.find(cur->parent);
        cur = it != idx.end() ? it->second : nullptr;
    }
    return -1;
}

float ease(float x) {
    return 0.5f - 0.5f * std::cos(3.14159265f * std::clamp(x, 0.0f, 1.0f));
}

// The object's local transform `t` seconds into Play: the Mover's travel from
// where it started, then the Spinner's turn about its own axis.
glm::mat4 localAt(const Entity& e, const glm::vec3& homePos, const glm::vec3& homeRot, double t) {
    glm::vec3 pos = homePos;
    if (const auto* mv = e.components.get<MoverComponent>())
        pos += mv->offset * moverAt(*mv, t);
    glm::mat4 m = scenegraph::compose(pos, homeRot, glm::vec3(1.0f));
    if (const auto* sp = e.components.get<SpinnerComponent>()) {
        const float len = glm::length(sp->axis);
        if (len > 1e-6f)
            m = m * glm::rotate(glm::mat4(1.0f), glm::radians(spinnerAt(*sp, t)), sp->axis / len);
    }
    return m;
}

glm::quat rotationOf(const glm::mat4& w) {
    return glm::normalize(glm::quat_cast(glm::mat3(w)));
}

PhysicsBodyId addCollider(fitzel::PhysicsWorld& physics, const Entity& e, const glm::mat4& w,
                          const World* world) {
    if (e.type == EntityType::Model && world && world->modelCollider)
        if (const PhysicsBodyId id = world->modelCollider(e, w)) return id;
    if (e.type == EntityType::Cylinder)
        return physics.addKinematicCylinder(e.half.x, e.half.y, glm::vec3(w[3]), rotationOf(w));
    return physics.addKinematicBox(glm::max(e.half, glm::vec3(0.01f)), glm::vec3(w[3]), rotationOf(w));
}

} // namespace

// Where a hull's waterline is, how wide and long the water under it, how deep
// it sits: from the surface of everything it carries, at rest. The waterplane
// is what lies at or below the still water (half a metre over, for a hull
// whose sides flare): the masts, the yards and the sails above stay out of it,
// or a ship would ride the sea as wide as its rigging.
void System::shapeHull(Item& it, const std::vector<Entity>& entities, const World* world) {
    const Index idx = indexOf(entities);
    ocean::HullRide& r = it.ride;
    const glm::mat4 toLocal = glm::inverse(it.home);
    const float level = world ? world->seaLevel : -1000.0f;
    const glm::vec3 at(it.home[3]);
    r.waterline = level > -500.0f ? glm::vec3(toLocal * glm::vec4(at.x, level, at.z, 1.0f)).y : 0.0f;

    std::vector<glm::vec3> pts, mine;
    for (const Entity& e : entities) {
        if (!hasBody(e) || moverOf(idx, e) != it.id) continue;
        mine.clear();
        if (world && world->surfacePoints) world->surfacePoints(e, mine);
        if (mine.empty()) {
            const glm::mat4 w = scenegraph::worldOf(e);
            for (int k = 0; k < 8; ++k)
                mine.push_back(glm::vec3(w * glm::vec4((k & 1) ? e.half.x : -e.half.x,
                                                       (k & 2) ? e.half.y : -e.half.y,
                                                       (k & 4) ? e.half.z : -e.half.z, 1.0f)));
        }
        for (const glm::vec3& p : mine) pts.push_back(glm::vec3(toLocal * glm::vec4(p, 1.0f)));
    }
    if (pts.empty()) return;   // nothing to float: it keeps the defaults
    glm::vec2 lo(1e30f), hi(-1e30f);
    float bottom = 1e30f;
    for (const glm::vec3& p : pts) {
        bottom = std::min(bottom, p.y);
        if (p.y > r.waterline + 0.5f) continue;
        lo = glm::min(lo, glm::vec2(p.x, p.z));
        hi = glm::max(hi, glm::vec2(p.x, p.z));
    }
    if (lo.x > hi.x)   // all of it above the water: the whole of it, then
        for (const glm::vec3& p : pts) {
            lo = glm::min(lo, glm::vec2(p.x, p.z));
            hi = glm::max(hi, glm::vec2(p.x, p.z));
        }
    r.center = 0.5f * (lo + hi);
    r.half   = glm::max(0.5f * (hi - lo), glm::vec2(0.25f));
    r.draft  = std::max(r.waterline - bottom, 0.1f);
}

bool isHull(const Entity& e) {
    if (!e.components.get<BuoyancyComponent>()) return false;
    const auto* pc = e.components.get<PhysicsComponent>();
    return !pc || !pc->dynamic;
}

std::unordered_set<int> carriedIds(const std::vector<Entity>& entities) {
    std::unordered_set<int> out;
    if (std::none_of(entities.begin(), entities.end(), moves)) return out;
    const Index idx = indexOf(entities);
    for (const Entity& e : entities)
        if (moverOf(idx, e) >= 0) out.insert(e.id);
    return out;
}

float moverAt(const MoverComponent& mv, double t) {
    const double move  = std::max(mv.duration, 0.05f) * 0.5;   // each way
    const double rest  = std::max(mv.pause, 0.0f);
    const double cycle = 2.0 * (move + rest);
    double u = std::fmod(std::max(t, 0.0) + std::clamp(mv.startPhase, 0.0f, 1.0f) * cycle, cycle);
    if (u < move) return ease(static_cast<float>(u / move));
    u -= move;
    if (u < rest) return 1.0f;
    u -= rest;
    if (u < move) return 1.0f - ease(static_cast<float>(u / move));
    return 0.0f;
}

float spinnerAt(const SpinnerComponent& sp, double t) {
    if (sp.swing > 0.0f) {
        const double per = std::max(sp.period, 0.05f);
        return sp.swing * static_cast<float>(std::sin(6.283185307179586 * (t / per + sp.startPhase)));
    }
    return static_cast<float>(std::fmod(sp.speed * t + sp.startPhase * 360.0, 360.0));
}

void System::begin(const std::vector<Entity>& entities, fitzel::PhysicsWorld& physics,
                   std::map<int, PhysicsBodyId>& bodies, const World* world) {
    clear();
    const Index idx = indexOf(entities);
    std::unordered_map<int, size_t> slot;
    for (const Entity& e : entities) {
        if (!moves(e)) continue;
        Item it;
        it.id      = e.id;
        it.homePos = e.localCenter;
        it.homeRot = e.localRotation;
        it.hull    = isHull(e);
        it.home    = scenegraph::worldOf(e);
        // Inner ones are placed after the ones they ride on.
        const Entity* cur = &e;
        for (int guard = 0; cur->parent >= 0 && guard < 256; ++guard) {
            auto p = idx.find(cur->parent);
            if (p == idx.end()) break;
            cur = p->second;
            if (moves(*cur)) ++it.depth;
        }
        slot[e.id] = m_items.size();
        m_known.insert(e.id);
        m_items.push_back(std::move(it));
    }
    if (m_items.empty()) return;
    // Every solid part goes with the moving object nearest above it. A model
    // group's part is its triangles or nothing (leaves and decals have none);
    // without a way to make triangles it is its box, as an own part is.
    for (const Entity& e : entities) {
        const bool own = solid(e);
        if (!own && !groupPart(idx, e)) continue;
        const int owner = moverOf(idx, e);
        auto s = slot.find(owner);
        if (s == slot.end()) continue;
        const glm::mat4 w = scenegraph::worldOf(e);
        Part part;
        part.id   = e.id;
        part.rel  = glm::inverse(scenegraph::worldOf(*idx.at(owner))) * w;
        if (!own && world && world->modelCollider) {
            part.body = world->modelCollider(e, w);
            if (!part.body) continue;
        } else {
            part.body = addCollider(physics, e, w, world);
        }
        if (part.body) bodies[e.id] = part.body;
        m_items[s->second].parts.push_back(part);
    }
    for (Item& it : m_items)
        if (it.hull) shapeHull(it, entities, world);
    std::stable_sort(m_items.begin(), m_items.end(),
                     [](const Item& a, const Item& b) { return a.depth < b.depth; });
}

void System::update(std::vector<Entity>& entities, float dt, fitzel::PhysicsWorld* physics,
                    const World* world) {
    for (Entity& e : entities)
        if (moves(e) && m_known.insert(e.id).second) {
            Item it;
            it.id      = e.id;
            it.homePos = e.localCenter;
            it.homeRot = e.localRotation;
            it.hull    = isHull(e);
            it.home    = scenegraph::worldOf(e);
            if (it.hull) shapeHull(it, entities, world);
            m_items.push_back(std::move(it));
        }
    if (m_items.empty()) return;

    std::unordered_map<int, Entity*> idx;
    idx.reserve(entities.size());
    for (Entity& e : entities) idx[e.id] = &e;

    for (Item& it : m_items) {
        auto f = idx.find(it.id);
        if (f == idx.end()) continue;   // destroyed during Play
        Entity& e = *f->second;
        it.time += dt;
        glm::vec3 scale;
        glm::mat4 now;   // its world this frame
        if (it.hull) {
            // Led over the sea from where it was placed; the local transform
            // follows from the world one, not the other way round.
            if (world && world->seaAt && world->seaLevel > -500.0f)
                it.ride.step(world->seaAt, world->seaLevel, it.home, dt);
            now = it.ride.pose(it.home);
            glm::mat4 parentW(1.0f);
            if (e.parent >= 0)
                if (auto p = idx.find(e.parent); p != idx.end())
                    parentW = scenegraph::worldOf(*p->second);
            scenegraph::decompose(glm::inverse(parentW) * now, e.localCenter, e.localRotation, scale);
        } else {
            const glm::mat4 local = localAt(e, it.homePos, it.homeRot, it.time);
            scenegraph::decompose(local, e.localCenter, e.localRotation, scale);
            // Its world now, under a parent that may itself have just moved (outer
            // objects come first) -- so the inner ones and the colliders are placed
            // from where it is this frame, not where it was.
            now = local;
            if (e.parent >= 0)
                if (auto p = idx.find(e.parent); p != idx.end())
                    now = scenegraph::worldOf(*p->second) * local;
        }
        scenegraph::decompose(now, e.center, e.rotation, scale);
        for (const Part& part : it.parts) {
            const glm::mat4 w = now * part.rel;
            // The part's world this frame too, not after the next resolve: a
            // script asking where the floor under it is gets where it is.
            if (part.id != it.id)
                if (auto pe = idx.find(part.id); pe != idx.end())
                    scenegraph::decompose(w, pe->second->center, pe->second->rotation, scale);
            if (physics && part.body)
                physics->setKinematicTarget(part.body, glm::vec3(w[3]), rotationOf(w), dt);
        }
    }
}

const ocean::HullRide* System::hullRide(int id) const {
    for (const Item& it : m_items)
        if (it.id == id && it.hull) return &it.ride;
    return nullptr;
}

} // namespace platforms
