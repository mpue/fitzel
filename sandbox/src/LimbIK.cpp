#include "LimbIK.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <sstream>

#include <glm/gtc/matrix_transform.hpp>

namespace ik {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Which side a joint's name says it is on: 0 left, 1 right, -1 neither.
// "LeftFoot", "CC_Base_L_Foot", "foot_l", "foot.L", "lFoot" (Daz).
int sideOf(const std::string& name) {
    const std::string n = lower(name);
    if (n.find("left") != std::string::npos) return 0;
    if (n.find("right") != std::string::npos) return 1;
    std::string tok;
    auto flush = [&]() -> int {
        const int s = tok == "l" ? 0 : tok == "r" ? 1 : -1;
        tok.clear();
        return s;
    };
    for (char c : n) {
        if (c == '_' || c == '.' || c == ':' || c == ' ' || c == '-') {
            if (const int s = flush(); s >= 0) return s;
        } else {
            tok += c;
        }
    }
    if (const int s = flush(); s >= 0) return s;
    if (name.size() > 1 && std::isupper(static_cast<unsigned char>(name[1]))) {
        if (name[0] == 'l') return 0;
        if (name[0] == 'r') return 1;
    }
    return -1;
}

// A bone that only spreads a twist along a limb: stepped over in a chain.
bool twistBone(const std::string& name) {
    const std::string n = lower(name);
    return n.find("twist") != std::string::npos || n.find("roll") != std::string::npos ||
           n.find("share") != std::string::npos;
}

bool hasAny(const std::string& n, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (n.find(w) != std::string::npos) return true;
    return false;
}

int depthOf(const std::vector<fitzel::SkeletonJoint>& skel, int j) {
    int d = 0;
    while (j >= 0 && d < 512) { j = skel[static_cast<std::size_t>(j)].parent; ++d; }
    return d;
}

// The first joint up from `j` that is not a twist bone.
int realParent(const std::vector<fitzel::SkeletonJoint>& skel, int j) {
    int p = j >= 0 ? skel[static_cast<std::size_t>(j)].parent : -1;
    while (p >= 0 && twistBone(skel[static_cast<std::size_t>(p)].name)) p = skel[static_cast<std::size_t>(p)].parent;
    return p;
}

// The limb ending in the shallowest joint of `side` whose name says `endWords`
// (and none of `notWords`), with its two real parents above it.
Chain limb(const std::vector<fitzel::SkeletonJoint>& skel, int side,
           std::initializer_list<const char*> endWords, std::initializer_list<const char*> notWords) {
    int best = -1, bestDepth = 1 << 30;
    for (int j = 0; j < static_cast<int>(skel.size()); ++j) {
        const std::string& name = skel[static_cast<std::size_t>(j)].name;
        const std::string n = lower(name);
        if (!hasAny(n, endWords) || hasAny(n, notWords) || twistBone(name) || sideOf(name) != side) continue;
        const int d = depthOf(skel, j);
        if (d < bestDepth) { best = j; bestDepth = d; }
    }
    Chain c;
    if (best < 0) return c;
    c.end  = best;
    c.mid  = realParent(skel, c.end);
    c.root = realParent(skel, c.mid);
    if (!c.ok()) c = Chain{};
    return c;
}

glm::vec3 posOf(const glm::mat4& m) { return glm::vec3(m[3]); }

// The turn of a transform, its scale taken out.
glm::quat turnOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int k = 0; k < 3; ++k) {
        const float len = glm::length(r[k]);
        if (len > 1e-9f) r[k] /= len;
    }
    return glm::normalize(glm::quat_cast(r));
}

// The shortest turn taking direction `a` onto `b` (both non-zero).
glm::quat fromTo(glm::vec3 a, glm::vec3 b) {
    a = glm::normalize(a);
    b = glm::normalize(b);
    const float d = glm::dot(a, b);
    if (d > 0.999999f) return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    if (d < -0.999999f) {
        glm::vec3 axis = glm::cross(glm::vec3(1.0f, 0.0f, 0.0f), a);
        if (glm::length(axis) < 1e-4f) axis = glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), a);
        return glm::angleAxis(3.14159265f, glm::normalize(axis));
    }
    const glm::vec3 c = glm::cross(a, b);
    return glm::normalize(glm::quat(1.0f + d, c.x, c.y, c.z));
}

bool isUnder(const std::vector<fitzel::SkeletonJoint>& skel, int j, int root) {
    for (int k = 0; j >= 0 && k < 512; ++k) {
        if (j == root) return true;
        j = skel[static_cast<std::size_t>(j)].parent;
    }
    return false;
}

float follow(float now, float want, float rate, float dt) {
    return now + (want - now) * (1.0f - std::exp(-std::max(rate, 0.0f) * std::max(dt, 0.0f)));
}

} // namespace

Rig findRig(const std::vector<fitzel::SkeletonJoint>& skel) {
    Rig r;
    for (int s = 0; s < 2; ++s) {
        r.leg[static_cast<std::size_t>(s)] = limb(skel, s, {"foot", "ankle"}, {"toe", "ball", "index"});
        r.arm[static_cast<std::size_t>(s)] =
            limb(skel, s, {"hand", "wrist"},
                 {"finger", "thumb", "index", "middle", "ring", "pinky", "carpal", "prop", "weapon"});
    }
    return r;
}

Chain chainNamed(const std::vector<fitzel::SkeletonJoint>& skel, const std::string& names) {
    std::vector<std::string> parts;
    std::stringstream ss(names);
    std::string part;
    while (std::getline(ss, part, ',')) {
        const auto a = part.find_first_not_of(" \t");
        const auto b = part.find_last_not_of(" \t");
        if (a != std::string::npos) parts.push_back(part.substr(a, b - a + 1));
    }
    Chain c;
    if (parts.size() != 3) return c;
    auto find = [&](const std::string& n) {
        for (int j = 0; j < static_cast<int>(skel.size()); ++j)
            if (skel[static_cast<std::size_t>(j)].name == n) return j;
        return -1;
    };
    c.root = find(parts[0]);
    c.mid  = find(parts[1]);
    c.end  = find(parts[2]);
    if (!c.ok()) c = Chain{};
    return c;
}

std::vector<glm::mat4> joints(const std::vector<glm::mat4>& pal, const std::vector<fitzel::SkeletonJoint>& skel) {
    std::vector<glm::mat4> g(pal.size());
    for (std::size_t j = 0; j < pal.size() && j < skel.size(); ++j) g[j] = pal[j] * glm::inverse(skel[j].inverseBind);
    return g;
}

std::vector<glm::mat4> palette(const std::vector<glm::mat4>& g, const std::vector<fitzel::SkeletonJoint>& skel) {
    std::vector<glm::mat4> p(g.size());
    for (std::size_t j = 0; j < g.size() && j < skel.size(); ++j) p[j] = g[j] * skel[j].inverseBind;
    return p;
}

void rotateSubtree(std::vector<glm::mat4>& G, const std::vector<fitzel::SkeletonJoint>& skel, int root,
                   const glm::vec3& pivot, const glm::quat& q) {
    const glm::mat4 R = glm::translate(glm::mat4(1.0f), pivot) * glm::mat4_cast(q) *
                        glm::translate(glm::mat4(1.0f), -pivot);
    for (int j = 0; j < static_cast<int>(G.size()) && j < static_cast<int>(skel.size()); ++j)
        if (isUnder(skel, j, root)) G[static_cast<std::size_t>(j)] = R * G[static_cast<std::size_t>(j)];
}

bool solveTwoBone(std::vector<glm::mat4>& G, const std::vector<fitzel::SkeletonJoint>& skel, const Chain& c,
                  const glm::vec3& target, const glm::vec3& poleHint, bool keepEnd) {
    const int n = static_cast<int>(G.size());
    if (!c.ok() || c.root >= n || c.mid >= n || c.end >= n) return false;
    const glm::vec3 H = posOf(G[static_cast<std::size_t>(c.root)]);
    const glm::vec3 K = posOf(G[static_cast<std::size_t>(c.mid)]);
    const glm::vec3 A = posOf(G[static_cast<std::size_t>(c.end)]);
    const glm::quat endTurn = turnOf(G[static_cast<std::size_t>(c.end)]);
    const float a = glm::length(K - H), b = glm::length(A - K);
    const glm::vec3 toT = target - H;
    const float dist = glm::length(toT);
    if (a < 1e-6f || b < 1e-6f || dist < 1e-6f) return false;
    const float reach = std::clamp(dist, std::abs(a - b) + 1e-5f, a + b - 1e-5f);
    const glm::vec3 d = toT / dist;
    // The way the limb bends: where the middle joint stands off the limb's
    // OWN line, root to end, as the animation has it -- not off the line to
    // the target, which a knee can sit behind when the foot is sent forward
    // (the leg would fold backwards). The hint for a straight limb.
    const glm::vec3 own = glm::normalize(A - H);
    glm::vec3 pole = (K - H) - own * glm::dot(K - H, own);
    if (glm::length(pole) < 1e-3f * a) pole = poleHint;
    pole -= d * glm::dot(pole, d);
    if (glm::length(pole) < 1e-6f) pole = glm::cross(d, glm::vec3(1.0f, 0.0f, 0.0f));
    if (glm::length(pole) < 1e-6f) pole = glm::cross(d, glm::vec3(0.0f, 0.0f, 1.0f));
    pole = glm::normalize(pole);
    const float cosA = std::clamp((a * a + reach * reach - b * b) / (2.0f * a * reach), -1.0f, 1.0f);
    const float sinA = std::sqrt(std::max(0.0f, 1.0f - cosA * cosA));
    const glm::vec3 kneeTo = H + a * (cosA * d + sinA * pole);
    rotateSubtree(G, skel, c.root, H, fromTo(K - H, kneeTo - H));
    const glm::vec3 K1 = posOf(G[static_cast<std::size_t>(c.mid)]);
    const glm::vec3 A1 = posOf(G[static_cast<std::size_t>(c.end)]);
    const glm::vec3 endTo = H + d * reach;
    if (glm::length(A1 - K1) > 1e-6f && glm::length(endTo - K1) > 1e-6f)
        rotateSubtree(G, skel, c.mid, K1, fromTo(A1 - K1, endTo - K1));
    if (keepEnd) {
        const glm::quat now = turnOf(G[static_cast<std::size_t>(c.end)]);
        rotateSubtree(G, skel, c.end, posOf(G[static_cast<std::size_t>(c.end)]),
                      glm::normalize(endTurn * glm::inverse(now)));
    }
    return true;
}

void System::reach(int id, int side, const glm::vec3& target, float weight) {
    if (side < 0 || side > 1) return;
    Reach& r = m_reach[id][static_cast<std::size_t>(side)];
    r.target = target;
    r.weight = std::clamp(weight, 0.0f, 1.0f);
    r.on     = true;
}

const Rig& System::rigOf(const fitzel::ModelData& model, const FeetOptions* o) {
    std::string key = std::to_string(reinterpret_cast<std::uintptr_t>(&model));
    if (o) key += "|" + o->leftLeg + "|" + o->rightLeg + "|" + o->leftArm + "|" + o->rightArm;
    auto it = m_rigs.find(key);
    if (it != m_rigs.end()) return it->second;
    Rig r = findRig(model.skeleton);
    if (o) {
        const std::string* names[4] = {&o->leftLeg, &o->rightLeg, &o->leftArm, &o->rightArm};
        Chain* chains[4] = {&r.leg[0], &r.leg[1], &r.arm[0], &r.arm[1]};
        for (int k = 0; k < 4; ++k)
            if (!names[k]->empty()) *chains[k] = chainNamed(model.skeleton, *names[k]);
    }
    return m_rigs.emplace(key, r).first->second;
}

void System::apply(int id, const FeetOptions* feet, const fitzel::ModelData& model, const glm::mat4& toWorld,
                   float baseY, std::vector<glm::mat4>& pal, const GroundFn& ground, float dt) {
    const auto& skel = model.skeleton;
    if (pal.empty() || pal.size() != skel.size()) return;
    const auto reachIt = m_reach.find(id);
    const bool doFeet  = feet && feet->feet && ground;
    if (!doFeet && reachIt == m_reach.end()) return;
    const Rig& rig = rigOf(model, feet);
    std::vector<glm::mat4> G = joints(pal, skel);
    const glm::mat4 toModel = glm::inverse(toWorld);
    const glm::mat3 dirToModel(toModel);
    auto world = [&](const glm::vec3& p) { return glm::vec3(toWorld * glm::vec4(p, 1.0f)); };
    auto model3 = [&](const glm::vec3& p) { return glm::vec3(toModel * glm::vec4(p, 1.0f)); };
    // The way a knee points when the leg is straight: the figure's forward.
    const glm::vec3 kneeHint = glm::vec3(0.0f, 0.0f, 1.0f);

    if (doFeet) {
        State& st = m_state[id];
        const float maxStep = std::max(feet->maxStep, 0.0f);
        float want[2] = {0.0f, 0.0f};
        glm::vec3 normal[2] = {glm::vec3(0, 1, 0), glm::vec3(0, 1, 0)};
        bool found[2] = {false, false};
        glm::vec3 ankle[2];
        for (int s = 0; s < 2; ++s) {
            const Chain& c = rig.leg[static_cast<std::size_t>(s)];
            if (!c.ok()) continue;
            ankle[s] = world(posOf(G[static_cast<std::size_t>(c.end)]));
            float y = 0.0f;
            glm::vec3 nrm(0.0f, 1.0f, 0.0f);
            const glm::vec3 from(ankle[s].x, baseY + maxStep + 0.5f, ankle[s].z);
            // Higher than a step is not ground to stand on (a wall, a ledge):
            // that foot stays as the animation has it.
            if (ground(from, 2.0f * maxStep + 1.0f, y, nrm) && y - baseY <= maxStep) {
                want[s]   = std::max(y - baseY, -maxStep);
                normal[s] = glm::length(nrm) > 1e-6f ? glm::normalize(nrm) : glm::vec3(0, 1, 0);
                found[s]  = true;
            }
        }
        // The body comes down by what the lower foot needs; it never goes up --
        // a raised foot bends its own knee instead.
        float drop = 0.0f;
        for (int s = 0; s < 2; ++s)
            if (found[s]) drop = std::min(drop, want[s]);
        const float rate = st.fresh ? 1e6f : feet->response;
        st.pelvis = follow(st.pelvis, drop, rate, dt);
        for (int s = 0; s < 2; ++s) {
            st.lift[s] = follow(st.lift[s], want[s], rate, dt);
            st.normal[s] = glm::normalize(glm::mix(st.normal[s], normal[s],
                                                   1.0f - std::exp(-rate * std::max(dt, 0.0f))));
        }
        st.fresh = false;
        // Lower the whole figure...
        if (std::abs(st.pelvis) > 1e-5f) {
            const glm::mat4 T = glm::translate(glm::mat4(1.0f), dirToModel * glm::vec3(0.0f, st.pelvis, 0.0f));
            for (glm::mat4& m : G) m = T * m;
        }
        // ...and each leg onto its own ground, at the height the clip lifts it.
        const glm::vec3 upModel = glm::normalize(dirToModel * glm::vec3(0.0f, 1.0f, 0.0f));
        for (int s = 0; s < 2; ++s) {
            const Chain& c = rig.leg[static_cast<std::size_t>(s)];
            if (!c.ok()) continue;
            const glm::vec3 target = ankle[s] + glm::vec3(0.0f, st.lift[s], 0.0f);
            solveTwoBone(G, skel, c, model3(target), kneeHint, true);
            if (!feet->align) continue;
            // Tilted with the slope -- fully while the foot is down, not at all
            // once the clip has lifted it a hand's breadth off the floor.
            const glm::vec3 restAnkle = world(posOf(glm::inverse(skel[static_cast<std::size_t>(c.end)].inverseBind)));
            const float lifted = (ankle[s].y - baseY) - (restAnkle.y - baseY);
            const float down   = std::clamp(1.0f - lifted / 0.15f, 0.0f, 1.0f);
            glm::vec3 nModel = glm::normalize(dirToModel * st.normal[s]);
            // At most 35 degrees: past that it is a wall, not a slope.
            const float cosMax = std::cos(glm::radians(35.0f));
            if (glm::dot(nModel, upModel) < cosMax) {
                const glm::vec3 side = glm::normalize(nModel - upModel * glm::dot(nModel, upModel));
                nModel = upModel * cosMax + side * std::sqrt(1.0f - cosMax * cosMax);
            }
            const glm::quat tilt = glm::slerp(glm::quat(1.0f, 0.0f, 0.0f, 0.0f), fromTo(upModel, nModel), down);
            rotateSubtree(G, skel, c.end, posOf(G[static_cast<std::size_t>(c.end)]), tilt);
        }
    }

    if (reachIt != m_reach.end()) {
        for (int s = 0; s < 2; ++s) {
            const Reach& r = reachIt->second[static_cast<std::size_t>(s)];
            const Chain& c = rig.arm[static_cast<std::size_t>(s)];
            if (!r.on || r.weight <= 0.0f || !c.ok()) continue;
            const glm::vec3 hand = posOf(G[static_cast<std::size_t>(c.end)]);
            const glm::vec3 target = glm::mix(hand, model3(r.target), r.weight);
            // A straight arm bends its elbow down and back.
            solveTwoBone(G, skel, c, target, glm::vec3(0.0f, -1.0f, -1.0f), true);
        }
    }
    pal = palette(G, skel);
}

} // namespace ik
