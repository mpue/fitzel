#include "Swing.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/physics/Physics.hpp>

#include "Component.hpp"   // SwingComponent, MeshComponent, ModelComponent
#include "SceneGraph.hpp"

namespace swing {
namespace {

constexpr float kG = 9.81f;
constexpr float kStep = 1.0f / 240.0f;   // the pendulum's own clock, steady whatever the frame

const Entity* findEntity(const std::vector<Entity>& entities, int id) {
    for (const Entity& e : entities)
        if (e.id == id) return &e;
    return nullptr;
}

// Does this part draw something -- and so need a box to be hit by?
bool solid(const Entity& e) {
    if (!e.activeInHierarchy) return false;
    if (e.type == EntityType::Empty || e.type == EntityType::Light || e.type == EntityType::Sun) return false;
    return e.components.get<MeshComponent>() || e.components.get<ModelComponent>() || isSolidPrimitive(e.type);
}

} // namespace

void step(State& s, const glm::vec3& arm0, float damping, float maxAngleDeg, float dt) {
    const float len2 = glm::dot(arm0, arm0);
    if (len2 < 1e-8f || dt <= 0.0f) return;
    const glm::vec3 down0 = glm::normalize(arm0);
    const float cosMax = std::cos(glm::radians(std::clamp(maxAngleDeg, 1.0f, 179.0f)));
    const int n = std::max(1, static_cast<int>(std::ceil(dt / kStep)));
    const float h = dt / static_cast<float>(n);
    const float keep = std::exp(-std::max(damping, 0.0f) * h);
    for (int i = 0; i < n; ++i) {
        const glm::vec3 a = s.q * arm0;
        // Gravity on the middle, about the pivot, over the moment of a point mass
        // there: the pendulum's angular acceleration.
        s.w += glm::cross(a, glm::vec3(0.0f, -kG, 0.0f)) / len2 * h;
        s.w *= keep;
        // Turn by w for h (world angular velocity: dq = 1/2 w q).
        const glm::quat dq(0.0f, s.w.x, s.w.y, s.w.z);
        s.q = glm::normalize(s.q + 0.5f * h * (dq * s.q));
        // No further out than the limit: back onto its edge, the outward part of
        // the motion taken away (the chain is taut).
        const glm::vec3 dir = glm::normalize(s.q * arm0);
        const float c = glm::dot(dir, down0);
        if (c < cosMax) {
            const glm::vec3 axis = glm::cross(down0, dir);
            if (glm::length(axis) > 1e-6f) {
                const glm::vec3 ax = glm::normalize(axis);
                const float over = std::acos(std::clamp(c, -1.0f, 1.0f)) - std::acos(cosMax);
                s.q = glm::normalize(glm::angleAxis(-over, ax) * s.q);
                const float out = glm::dot(s.w, ax);
                if (out > 0.0f) s.w -= ax * out;
            }
        }
    }
}

void kick(State& s, const glm::vec3& arm0, float mass, const glm::vec3& r, const glm::vec3& j) {
    const glm::vec3 a = s.q * arm0;
    const float inertia = std::max(mass, 0.01f) * std::max(glm::dot(a, a), 0.01f);
    s.w += glm::cross(r, j) / inertia;
    // A shot is a push, not a launch: no faster than a few turns a second.
    const float speed = glm::length(s.w);
    if (speed > 20.0f) s.w *= 20.0f / speed;
}

glm::mat4 turnAbout(const glm::vec3& pivot, const glm::quat& q) {
    return glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(q) * glm::translate(glm::mat4(1.0f), -pivot);
}

void System::begin(const std::vector<Entity>& entities, fitzel::PhysicsWorld& physics,
                   std::map<int, PhysicsBodyId>& bodies) {
    m_items.clear();
    for (const Entity& e : entities) {
        const auto* sc = e.components.get<SwingComponent>();
        if (!sc || !e.activeInHierarchy) continue;
        // Something that already swings is carried by it, not a second pendulum.
        bool inside = false;
        for (int p = e.parent; p >= 0 && !inside;) {
            const Entity* pe = findEntity(entities, p);
            if (!pe) break;
            if (pe->components.get<SwingComponent>()) inside = true;
            p = pe->parent;
        }
        if (inside) continue;
        Item it;
        it.rest = scenegraph::worldOf(e);
        // What hangs: its parts' world box, and their middle weighted by size --
        // the chain and the hook on it, not the chain alone. A group (an empty,
        // an import's node) has no box of its own worth asking.
        glm::vec3 lo(1e30f), hi(-1e30f), mid(0.0f);
        float weight = 0.0f;
        for (int id : scenegraph::subtree(entities, e.id)) {
            const Entity* pe = findEntity(entities, id);
            if (!pe || !solid(*pe)) continue;
            const glm::mat3 r = glm::mat3_cast(glm::quat(glm::radians(pe->rotation)));
            const glm::vec3 ext = glm::abs(r[0]) * pe->half.x + glm::abs(r[1]) * pe->half.y + glm::abs(r[2]) * pe->half.z;
            lo = glm::min(lo, pe->center - ext);
            hi = glm::max(hi, pe->center + ext);
            const float v = std::max(pe->half.x * pe->half.y * pe->half.z, 1e-6f);
            mid += pe->center * v;
            weight += v;
        }
        mid = weight > 0.0f ? mid / weight : e.center;
        // Where it hangs from: the given point in its own frame -- or the top of
        // what hangs, above its middle: where a chain meets the ceiling.
        if (glm::dot(sc->pivot, sc->pivot) > 1e-10f)
            it.pivot = glm::vec3(it.rest * glm::vec4(sc->pivot, 1.0f));
        else if (weight > 0.0f)
            it.pivot = glm::vec3(0.5f * (lo.x + hi.x), hi.y, 0.5f * (lo.z + hi.z));
        else
            it.pivot = glm::vec3(it.rest * glm::vec4(0.0f, e.half.y, 0.0f, 1.0f));
        it.arm0     = mid - it.pivot;
        it.mass     = sc->mass;
        it.damping  = sc->damping;
        it.maxAngle = sc->maxAngle;
        if (glm::dot(it.arm0, it.arm0) < 1e-6f) it.arm0 = glm::vec3(0.0f, -0.01f, 0.0f);
        for (int id : scenegraph::subtree(entities, e.id)) {
            const Entity* pe = findEntity(entities, id);
            if (!pe) continue;
            Part part;
            part.id   = id;
            part.rest = scenegraph::worldOf(*pe);
            if (solid(*pe)) {
                const glm::quat rot = glm::quat_cast(glm::mat3(part.rest));
                part.body = physics.addKinematicBox(glm::max(pe->half, glm::vec3(0.01f)), pe->center, rot);
                if (part.body) bodies[id] = part.body;
            }
            it.parts.push_back(part);
        }
        m_items.emplace(e.id, std::move(it));
    }
}

bool System::kick(const std::vector<Entity>& entities, int id, const glm::vec3& j, const glm::vec3* at) {
    // The hit part, or anything above it, that swings.
    for (int cur = id; cur >= 0;) {
        auto it = m_items.find(cur);
        if (it != m_items.end()) {
            Item& item = it->second;
            glm::vec3 point = item.pivot + item.state.q * item.arm0;
            if (at) point = *at;
            else if (const Entity* hit = findEntity(entities, id)) point = hit->center;
            swing::kick(item.state, item.arm0, item.mass, point - item.pivot, j);
            return true;
        }
        const Entity* e = findEntity(entities, cur);
        cur = e ? e->parent : -1;
    }
    return false;
}

void System::update(std::vector<Entity>& entities, float dt, fitzel::PhysicsWorld* physics) {
    for (auto& [rootId, item] : m_items) {
        step(item.state, item.arm0, item.damping, item.maxAngle, dt);
        const glm::mat4 turn = turnAbout(item.pivot, item.state.q);
        // The root where the swing took it -- its children follow through the
        // hierarchy -- and every part's box led after its part.
        for (const Part& part : item.parts) {
            const glm::mat4 w = turn * part.rest;
            if (part.id == rootId) {
                Entity* e = nullptr;
                for (Entity& x : entities)
                    if (x.id == rootId) { e = &x; break; }
                if (e) {
                    glm::vec3 t, r, s;
                    scenegraph::decompose(w, t, r, s);
                    const glm::mat4 pw = scenegraph::parentWorld(entities, *e);
                    scenegraph::setWorld(*e, t, r, e->parent >= 0 ? &pw : nullptr);
                }
            }
            if (physics && part.body)
                physics->setKinematicTarget(part.body, glm::vec3(w[3]), glm::quat_cast(glm::mat3(w)), dt);
        }
    }
}

} // namespace swing
