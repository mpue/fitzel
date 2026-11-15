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
// ...and that of things that knock: a cradle's ball at 2 m/s goes 2 mm in it,
// so a blow is found before the balls are into each other by more than that.
constexpr float kKnockStep = 1.0f / 1000.0f;
constexpr float kTouch = 0.03f;   // balls this far apart (of the smaller radius) still touch
constexpr float kQuiet = 1e-4f;   // m/s: closing slower than this is resting, not a blow

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

namespace {

// Only the turn about `axis`, when it has one: what two strings in a V allow.
glm::vec3 held(const glm::vec3& w, const glm::vec3& axis) {
    return glm::dot(axis, axis) > 0.5f ? axis * glm::dot(w, axis) : w;
}

// The two halves of a step of `h`. Gravity on the middle, about the pivot,
// over the moment of a point mass there: the pendulum's angular acceleration;
// and the damping.
void accelerate(State& s, const glm::vec3& arm0, float damping, float h, const glm::vec3& axis) {
    const float len2 = glm::dot(arm0, arm0);
    const glm::vec3 a = s.q * arm0;
    s.w += glm::cross(a, glm::vec3(0.0f, -kG, 0.0f)) / len2 * h;
    s.w = held(s.w * std::exp(-std::max(damping, 0.0f) * h), axis);
}

// Turn by w for h (world angular velocity: dq = 1/2 w q). No further out than
// the limit: back onto its edge, the outward part of the motion taken away
// (the chain is taut).
void advance(State& s, const glm::vec3& arm0, float maxAngleDeg, float h) {
    const glm::vec3 down0 = glm::normalize(arm0);
    const float cosMax = std::cos(glm::radians(std::clamp(maxAngleDeg, 1.0f, 179.0f)));
    const glm::quat dq(0.0f, s.w.x, s.w.y, s.w.z);
    s.q = glm::normalize(s.q + 0.5f * h * (dq * s.q));
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

// A body on its pivot as a point mass at its middle (what kick() takes it for):
// how much its turn changes for an angular impulse `t`.
glm::vec3 turnFor(const State& s, const glm::vec3& arm0, float mass, const glm::vec3& t,
                  const glm::vec3& axis) {
    const glm::vec3 a = s.q * arm0;
    const float inertia = std::max(mass, 0.01f) * std::max(glm::dot(a, a), 0.01f);
    return held(t / inertia, axis);
}

} // namespace

void step(State& s, const glm::vec3& arm0, float damping, float maxAngleDeg, float dt,
          const glm::vec3& axis) {
    if (glm::dot(arm0, arm0) < 1e-8f || dt <= 0.0f) return;
    const int n = std::max(1, static_cast<int>(std::ceil(dt / kStep)));
    const float h = dt / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        accelerate(s, arm0, damping, h, axis);
        advance(s, arm0, maxAngleDeg, h);
    }
}

void kick(State& s, const glm::vec3& arm0, float mass, const glm::vec3& r, const glm::vec3& j,
          const glm::vec3& axis) {
    s.w += turnFor(s, arm0, mass, glm::cross(r, j), axis);
    // A shot is a push, not a launch: no faster than a few turns a second.
    const float speed = glm::length(s.w);
    if (speed > 20.0f) s.w *= 20.0f / speed;
}

glm::vec3 ballAt(const State& s, const Pendulum& p) {
    return p.pivot + s.q * (p.ball - p.pivot);
}

bool knock(State& sa, const Pendulum& a, State& sb, const Pendulum& b, float* speed,
           glm::vec3* where) {
    const glm::vec3 ca = ballAt(sa, a), cb = ballAt(sb, b);
    const glm::vec3 d = cb - ca;
    const float dist = glm::length(d);
    // Touching: within a hair of it, so a row hung ball to ball counts as one
    // whatever the last digit of where each was put.
    const float reach = a.radius + b.radius;
    if (dist < 1e-6f || dist > reach + kTouch * std::min(a.radius, b.radius)) return false;
    const glm::vec3 n = d / dist;                          // from a to b
    const glm::vec3 at = ca + n * (a.radius * dist / reach); // where they meet
    const glm::vec3 ra = at - a.pivot, rb = at - b.pivot;
    const float closing = glm::dot(glm::cross(sb.w, rb) - glm::cross(sa.w, ra), n);
    if (closing >= -kQuiet) return false;                 // parting, or as good as still
    // How far a unit blow along n moves each one's contact point along n.
    const glm::vec3 ta = turnFor(sa, a.arm0, a.mass, glm::cross(ra, n), a.axis);
    const glm::vec3 tb = turnFor(sb, b.arm0, b.mass, glm::cross(rb, n), b.axis);
    const float give = glm::dot(glm::cross(ta, ra), n) + glm::dot(glm::cross(tb, rb), n);
    if (give < 1e-9f) return false;                       // neither can move that way
    const float e = 0.5f * (std::clamp(a.bounce, 0.0f, 1.0f) + std::clamp(b.bounce, 0.0f, 1.0f));
    const float j = -(1.0f + e) * closing / give;
    sa.w -= ta * j;
    sb.w += tb * j;
    if (speed) *speed = -closing;
    if (where) *where = at;
    return true;
}

void stepTogether(const std::vector<State*>& s, const std::vector<const Pendulum*>& p, float dt,
                  std::vector<Knock>* knocks) {
    const std::size_t count = std::min(s.size(), p.size());
    if (count == 0 || dt <= 0.0f) return;
    const int n = std::max(1, static_cast<int>(std::ceil(dt / kKnockStep)));
    const float h = dt / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        for (std::size_t k = 0; k < count; ++k)
            if (glm::dot(p[k]->arm0, p[k]->arm0) >= 1e-8f)
                accelerate(*s[k], p[k]->arm0, p[k]->damping, h, p[k]->axis);
        // Pair after pair until no two close in: a blow passed on is a new
        // blow for the next pair. Each pass sorts the row a step further (with
        // a bounce of 1 it is a bubble sort of the speeds), so a row of n needs
        // n passes; the cap only stops a pile that cannot settle from hanging.
        const int passes = 2 * static_cast<int>(count) + 4;
        for (int pass = 0; pass < passes; ++pass) {
            bool any = false;
            for (std::size_t x = 0; x < count; ++x)
                for (std::size_t y = x + 1; y < count; ++y) {
                    Knock k;
                    if (!knock(*s[x], *p[x], *s[y], *p[y], &k.speed, &k.at)) continue;
                    any = true;
                    if (knocks) {
                        k.a = static_cast<int>(x);
                        k.b = static_cast<int>(y);
                        knocks->push_back(k);
                    }
                }
            if (!any) break;
        }
        for (std::size_t k = 0; k < count; ++k)
            if (glm::dot(p[k]->arm0, p[k]->arm0) >= 1e-8f)
                advance(*s[k], p[k]->arm0, p[k]->maxAngle, h);
    }
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
            // A floor only for what has no volume at all (a plane): a thread a
            // millimetre thick weighs next to nothing, and a floor the size of a
            // centimetre cube made a cradle's strings a tenth of its weight --
            // its middle lifted, its swing six per cent too fast.
            const float v = std::max(pe->half.x * pe->half.y * pe->half.z, 1e-12f);
            mid += pe->center * v;
            weight += v;
        }
        mid = weight > 0.0f ? mid / weight : e.center;
        // Where it hangs from: the given point in its own frame -- or the top of
        // what hangs, above its middle: where a chain meets the ceiling.
        Pendulum& pd = it.p;
        if (glm::dot(sc->pivot, sc->pivot) > 1e-10f)
            pd.pivot = glm::vec3(it.rest * glm::vec4(sc->pivot, 1.0f));
        else if (weight > 0.0f)
            pd.pivot = glm::vec3(0.5f * (lo.x + hi.x), hi.y, 0.5f * (lo.z + hi.z));
        else
            pd.pivot = glm::vec3(it.rest * glm::vec4(0.0f, e.half.y, 0.0f, 1.0f));
        pd.arm0     = mid - pd.pivot;
        pd.mass     = sc->mass;
        pd.damping  = sc->damping;
        pd.maxAngle = sc->maxAngle;
        if (glm::dot(pd.arm0, pd.arm0) < 1e-6f) pd.arm0 = glm::vec3(0.0f, -0.01f, 0.0f);
        // The one way it may turn, in its own frame -- and the way it is
        // pulled aside to start (about its X when it may swing any way).
        const glm::mat3 own(it.rest);
        const glm::vec3 x = glm::normalize(own[0]), z = glm::normalize(own[2]);
        pd.axis = sc->swingAxis == 1 ? x : sc->swingAxis == 2 ? z : glm::vec3(0.0f);
        if (sc->startAngle != 0.0f) {
            const float a = std::clamp(sc->startAngle, -pd.maxAngle, pd.maxAngle);
            it.state.q = glm::angleAxis(glm::radians(a), sc->swingAxis == 2 ? z : x);
        }
        // What it knocks with: a ball about its heaviest part.
        it.knocks = sc->knocks;
        it.sound  = sc->knockSound;
        pd.bounce = sc->bounce;
        pd.ball   = mid;
        pd.radius = 0.1f;
        float heaviest = -1.0f;
        for (int id : scenegraph::subtree(entities, e.id)) {
            const Entity* pe = findEntity(entities, id);
            if (!pe || !solid(*pe)) continue;
            const float v = pe->half.x * pe->half.y * pe->half.z;
            if (v <= heaviest) continue;
            heaviest  = v;
            pd.ball   = pe->center;
            pd.radius = pe->type == EntityType::Sphere
                            ? (pe->half.x + pe->half.y + pe->half.z) / 3.0f   // as its collider
                            : std::min({pe->half.x, pe->half.y, pe->half.z});
        }
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
            const Pendulum& p = item.p;
            glm::vec3 point = p.pivot + item.state.q * p.arm0;
            if (at) point = *at;
            else if (const Entity* hit = findEntity(entities, id)) point = hit->center;
            swing::kick(item.state, p.arm0, p.mass, point - p.pivot, j, p.axis);
            return true;
        }
        const Entity* e = findEntity(entities, cur);
        cur = e ? e->parent : -1;
    }
    return false;
}

void System::update(std::vector<Entity>& entities, float dt, fitzel::PhysicsWorld* physics) {
    // Those that knock swing together, so that the blows between them are
    // taken in the same instant; the rest on their own.
    std::vector<State*> together;
    std::vector<const Pendulum*> knockers;
    std::vector<const Item*> owner;
    for (auto& [rootId, item] : m_items) {
        if (item.knocks) {
            together.push_back(&item.state);
            knockers.push_back(&item.p);
            owner.push_back(&item);
        } else {
            step(item.state, item.p.arm0, item.p.damping, item.p.maxAngle, dt, item.p.axis);
        }
    }
    std::vector<Knock> knocks;
    stepTogether(together, knockers, dt, &knocks);
    // One click per instant and place: a blow handed down a cradle is four
    // knocks in the same millisecond, and the ear hears one. A knock joins a
    // click when it is next to one of the knocks already in it -- a row links
    // up pair by pair, however long it is -- else it is a click of its own.
    m_clicks.clear();
    std::vector<std::vector<glm::vec3>> where;   // each click's knocks
    for (const Knock& k : knocks) {
        const Item& a = *owner[static_cast<std::size_t>(k.a)];
        const Item& b = *owner[static_cast<std::size_t>(k.b)];
        const std::string& sound = !a.sound.empty() ? a.sound : b.sound;
        if (sound.empty()) continue;
        const float next = 3.0f * std::max(a.p.radius, b.p.radius);
        std::size_t join = m_clicks.size();
        for (std::size_t c = 0; c < m_clicks.size() && join == m_clicks.size(); ++c)
            if (m_clicks[c].sound == sound)
                for (const glm::vec3& w : where[c])
                    if (glm::distance(w, k.at) < next) { join = c; break; }
        if (join == m_clicks.size()) {
            m_clicks.push_back({k.at, k.speed, sound});
            where.push_back({k.at});
            continue;
        }
        where[join].push_back(k.at);
        if (k.speed > m_clicks[join].speed) {
            m_clicks[join].speed = k.speed;
            m_clicks[join].at    = k.at;
        }
    }
    for (auto& [rootId, item] : m_items) {
        const glm::mat4 turn = turnAbout(item.p.pivot, item.state.q);
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
