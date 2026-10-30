#include "SpringBones.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

#include <glm/gtc/quaternion.hpp>

#include "LimbIK.hpp"   // joints() / palette() / rotateSubtree()

namespace springbones {

namespace {

std::vector<std::string> splitNames(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        const auto a = part.find_first_not_of(" \t");
        const auto b = part.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(part.substr(a, b - a + 1));
    }
    return out;
}

int depthOf(const std::vector<fitzel::SkeletonJoint>& skel, int j) {
    int d = 0;
    for (int k = skel[static_cast<std::size_t>(j)].parent; k >= 0 && d < 512;
         k = skel[static_cast<std::size_t>(k)].parent)
        ++d;
    return d;
}

} // namespace

void System::build(Figure& f, const Settings& s, const fitzel::ModelData& model) const {
    f.bones = s.bones;
    f.model = &model;
    f.list.clear();
    const auto& skel = model.skeleton;
    for (const std::string& name : splitNames(s.bones)) {
        int j = -1;
        for (std::size_t k = 0; k < skel.size(); ++k)
            if (skel[k].name == name) { j = static_cast<int>(k); break; }
        if (j < 0) continue;
        const glm::mat4 bind = glm::inverse(skel[static_cast<std::size_t>(j)].inverseBind);
        const glm::vec3 head(bind[3]);

        // The point it swings by: its first child's origin, else the centre of
        // the vertices it moves (weighted by how much it moves them).
        glm::vec3 tail(0.0f);
        bool found = false;
        for (std::size_t k = 0; k < skel.size(); ++k)
            if (skel[k].parent == j) {
                tail  = glm::vec3(glm::inverse(skel[k].inverseBind)[3]);
                found = glm::length(tail - head) > 1e-6f;
                break;
            }
        if (!found) {
            glm::vec3 sum(0.0f);
            float     wsum = 0.0f;
            for (const fitzel::ModelPrimitive& p : model.primitives) {
                const std::size_t n = std::min(p.skin.size(), p.vertices.size() / 8);
                for (std::size_t v = 0; v < n; ++v)
                    for (int i = 0; i < 4; ++i)
                        if (p.skin[v].joints[i] == j && p.skin[v].weights[i] > 0.25f) {
                            const float w = p.skin[v].weights[i];
                            sum  += w * glm::vec3(p.vertices[v * 8], p.vertices[v * 8 + 1],
                                                  p.vertices[v * 8 + 2]);
                            wsum += w;
                        }
            }
            if (wsum > 0.0f) {
                tail  = sum / wsum;
                found = glm::length(tail - head) > 1e-6f;
            }
        }
        if (!found) continue;   // nothing hangs on it: nothing to swing

        Bone b;
        b.joint = j;
        b.depth = depthOf(skel, j);
        b.tail  = glm::vec3(skel[static_cast<std::size_t>(j)].inverseBind * glm::vec4(tail, 1.0f));
        f.list.push_back(b);
    }
    // Parents first: a bone swings from where its parent swung it.
    std::sort(f.list.begin(), f.list.end(), [](const Bone& a, const Bone& b) { return a.depth < b.depth; });
}

void System::apply(int id, const Settings& s, const fitzel::ModelData& model, const glm::mat4& toWorld,
                   std::vector<glm::mat4>& palette, float dt) {
    if (s.bones.empty() || s.amount <= 0.0f) { m_figures.erase(id); return; }
    Figure& f = m_figures[id];
    if (f.model != &model || f.bones != s.bones) build(f, s, model);
    if (f.list.empty() || palette.size() < model.skeleton.size()) return;

    const auto& skel = model.skeleton;
    std::vector<glm::mat4> G = ik::joints(palette, skel);
    const glm::mat4 toModel = glm::inverse(toWorld);
    const float omega   = 6.2831853f * std::max(s.frequency, 0.05f);
    const float zeta    = std::clamp(s.damping, 0.0f, 2.0f);
    const float maxRad  = glm::radians(std::clamp(s.maxAngle, 0.0f, 89.0f));
    // A frame that took long (a hitch, the editor waking up) is not simulated in
    // full: the spring would see one giant step and fling the mass.
    const float h     = std::min(dt, 0.1f);
    const int   steps = h > 0.0f ? std::max(1, static_cast<int>(std::ceil(h * 240.0f))) : 0;
    const float sub   = steps > 0 ? h / static_cast<float>(steps) : 0.0f;

    for (Bone& b : f.list) {
        const glm::mat4& J   = G[static_cast<std::size_t>(b.joint)];
        const glm::vec3  hm  = glm::vec3(J[3]);
        const glm::vec3  tm  = glm::vec3(J * glm::vec4(b.tail, 1.0f));
        const glm::vec3  Hw  = glm::vec3(toWorld * glm::vec4(hm, 1.0f));
        const glm::vec3  Tw  = glm::vec3(toWorld * glm::vec4(tm, 1.0f));
        const float      len = glm::length(Tw - Hw);
        if (len < 1e-6f) continue;

        // Started, or jumped further than any swing goes: start over where it is.
        if (!b.live || glm::length(b.x - Tw) > 4.0f * len) {
            b.x = Tw;
            b.v = glm::vec3(0.0f);
            b.live = true;
        }
        for (int i = 0; i < steps; ++i) {
            const glm::vec3 a = omega * omega * (Tw - b.x) - 2.0f * zeta * omega * b.v;
            b.v += a * sub;
            b.x += b.v * sub;
        }
        // Held within the cone it may swing in: past it the mass rests on the
        // edge, and what it carried outwards is spent.
        const float reach = 2.0f * len * std::sin(0.5f * maxRad);
        const glm::vec3 off = b.x - Tw;
        if (const float d = glm::length(off); d > reach && d > 1e-9f) {
            b.x = Tw + off * (reach / d);
            b.v *= 0.5f;
        }

        // Turn the bone (and all below it) from where the pose points it to
        // where the mass hangs, in the model's space.
        const glm::vec3 xm   = glm::vec3(toModel * glm::vec4(b.x, 1.0f));
        const glm::vec3 from = tm - hm, to = xm - hm;
        const float lf = glm::length(from), lt = glm::length(to);
        b.angle = 0.0f;
        if (lf < 1e-9f || lt < 1e-9f) continue;
        const glm::vec3 a = from / lf, c = to / lt;
        const glm::vec3 axis = glm::cross(a, c);
        const float     sinA = glm::length(axis);
        if (sinA < 1e-7f) continue;
        const float angle = std::min(std::atan2(sinA, glm::dot(a, c)) * s.amount, maxRad);
        ik::rotateSubtree(G, skel, b.joint, hm, glm::angleAxis(angle, axis / sinA));
        b.angle = glm::degrees(angle);
    }
    palette = ik::palette(G, skel);
}

float System::angleOf(int id, int joint) const {
    const auto it = m_figures.find(id);
    if (it == m_figures.end()) return -1.0f;
    for (const Bone& b : it->second.list)
        if (b.joint == joint) return b.angle;
    return -1.0f;
}

} // namespace springbones
