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
    return e.components.get<MoverComponent>() || e.components.get<SpinnerComponent>();
}

// Does this part stand in the way: something with a static collider?
bool solid(const Entity& e) {
    const auto* pc = e.components.get<PhysicsComponent>();
    if (!pc || pc->dynamic || !e.activeInHierarchy) return false;
    return e.type != EntityType::Empty && e.type != EntityType::Light && e.type != EntityType::Sun;
}

using Index = std::unordered_map<int, const Entity*>;

Index indexOf(const std::vector<Entity>& entities) {
    Index idx;
    idx.reserve(entities.size());
    for (const Entity& e : entities) idx[e.id] = &e;
    return idx;
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

PhysicsBodyId addCollider(fitzel::PhysicsWorld& physics, const Entity& e, const glm::mat4& w) {
    if (e.type == EntityType::Cylinder)
        return physics.addKinematicCylinder(e.half.x, e.half.y, glm::vec3(w[3]), rotationOf(w));
    return physics.addKinematicBox(glm::max(e.half, glm::vec3(0.01f)), glm::vec3(w[3]), rotationOf(w));
}

} // namespace

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
                   std::map<int, PhysicsBodyId>& bodies) {
    clear();
    const Index idx = indexOf(entities);
    std::unordered_map<int, size_t> slot;
    for (const Entity& e : entities) {
        if (!moves(e)) continue;
        Item it;
        it.id      = e.id;
        it.homePos = e.localCenter;
        it.homeRot = e.localRotation;
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
    // Every solid part goes with the moving object nearest above it.
    for (const Entity& e : entities) {
        if (!solid(e)) continue;
        const int owner = moverOf(idx, e);
        auto s = slot.find(owner);
        if (s == slot.end()) continue;
        const glm::mat4 w = scenegraph::worldOf(e);
        Part part;
        part.id   = e.id;
        part.rel  = glm::inverse(scenegraph::worldOf(*idx.at(owner))) * w;
        part.body = addCollider(physics, e, w);
        if (part.body) bodies[e.id] = part.body;
        m_items[s->second].parts.push_back(part);
    }
    std::stable_sort(m_items.begin(), m_items.end(),
                     [](const Item& a, const Item& b) { return a.depth < b.depth; });
}

void System::update(std::vector<Entity>& entities, float dt, fitzel::PhysicsWorld* physics) {
    for (Entity& e : entities)
        if (moves(e) && m_known.insert(e.id).second) {
            Item it;
            it.id      = e.id;
            it.homePos = e.localCenter;
            it.homeRot = e.localRotation;
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
        const glm::mat4 local = localAt(e, it.homePos, it.homeRot, it.time);
        glm::vec3 scale;
        scenegraph::decompose(local, e.localCenter, e.localRotation, scale);
        // Its world now, under a parent that may itself have just moved (outer
        // objects come first) -- so the inner ones and the colliders are placed
        // from where it is this frame, not where it was.
        glm::mat4 world = local;
        if (e.parent >= 0)
            if (auto p = idx.find(e.parent); p != idx.end())
                world = scenegraph::worldOf(*p->second) * local;
        scenegraph::decompose(world, e.center, e.rotation, scale);
        for (const Part& part : it.parts) {
            const glm::mat4 w = world * part.rel;
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

} // namespace platforms
