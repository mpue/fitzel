#include "BoneAttach.hpp"

#include <cctype>

#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/world/Model.hpp>

#include "Component.hpp"
#include "ModelLibrary.hpp"
#include "SceneGraph.hpp"

namespace boneattach {

namespace {

const Entity* findEntity(const std::vector<Entity>& entities, int id) {
    for (const Entity& e : entities)
        if (e.id == id) return &e;
    return nullptr;
}
Entity* findEntity(std::vector<Entity>& entities, int id) {
    for (Entity& e : entities)
        if (e.id == id) return &e;
    return nullptr;
}

// The figure's model with its skeleton, or null.
LoadedModel* skinnedModel(const Entity& e, ModelLibrary& models) {
    const auto* mc = e.components.get<ModelComponent>();
    LoadedModel* lm = mc ? models.byId(mc->modelId) : nullptr;
    if (!lm || !lm->animData || lm->animData->skeleton.empty()) return nullptr;
    return lm;
}

bool sameIgnoringCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

// A matrix as an object's place and turn, scale dropped: through the scene's
// own decomposition, so the Euler triple is the one an entity would store.
glm::mat4 rigid(const glm::mat4& m) {
    glm::vec3 t, rotDeg, s;
    scenegraph::decompose(m, t, rotDeg, s);
    return scenegraph::compose(t, rotDeg, glm::vec3(1.0f));
}

} // namespace

Attachments::Seat Attachments::seatOf(const glm::mat4& m) {
    Seat s;
    s.pos = glm::vec3(m[3]);
    s.rot = glm::normalize(glm::quat_cast(glm::mat3(m)));
    return s;
}

Attachments::Seat Attachments::current(const Link& l) {
    if (l.dur <= 0.0f || l.t >= l.dur) return l.to;
    const float u = glm::clamp(l.t / l.dur, 0.0f, 1.0f);
    const float e = u * u * (3.0f - 2.0f * u);   // eased in and out
    Seat s;
    s.pos = glm::mix(l.from.pos, l.to.pos, e);
    s.rot = glm::slerp(l.from.rot, l.to.rot, e);
    return s;
}

int jointIndex(const fitzel::ModelData& model, const std::string& name) {
    for (std::size_t j = 0; j < model.skeleton.size(); ++j)
        if (model.skeleton[j].name == name) return static_cast<int>(j);
    for (std::size_t j = 0; j < model.skeleton.size(); ++j)
        if (sameIgnoringCase(model.skeleton[j].name, name)) return static_cast<int>(j);
    return -1;
}

void Attachments::storePose(int figure, const std::vector<glm::mat4>& palette) {
    m_pose[figure] = palette;
}

bool Attachments::jointWorld(const Entity& figure, ModelLibrary& models, int joint,
                             glm::mat4& out) const {
    const auto pose = m_pose.find(figure.id);
    LoadedModel* lm = skinnedModel(figure, models);
    if (pose == m_pose.end() || !lm || joint < 0 ||
        joint >= static_cast<int>(pose->second.size()) ||
        joint >= static_cast<int>(lm->animData->skeleton.size()))
        return false;
    // The palette is joint * inverseBind; the joint itself is what is wanted.
    const glm::mat4 jointModel = pose->second[static_cast<std::size_t>(joint)] *
        glm::inverse(lm->animData->skeleton[static_cast<std::size_t>(joint)].inverseBind);
    // Model space to the world exactly as SceneSubmit draws a model: its box
    // fills the entity's centre +/- half.
    const glm::vec3 sz = glm::max(lm->size(), glm::vec3(1e-4f));
    const glm::mat4 modelToWorld =
        scenegraph::compose(figure.center, figure.rotation, (figure.half * 2.0f) / sz) *
        glm::translate(glm::mat4(1.0f), -lm->center());
    out = rigid(modelToWorld * jointModel);
    return true;
}

bool Attachments::boneWorld(const std::vector<Entity>& entities, ModelLibrary& models,
                            int figure, const std::string& bone, glm::mat4& out) const {
    const Entity* f = findEntity(entities, figure);
    LoadedModel* lm = f ? skinnedModel(*f, models) : nullptr;
    if (!lm) return false;
    return jointWorld(*f, models, jointIndex(*lm->animData, bone), out);
}

bool Attachments::rayFigure(const std::vector<Entity>& entities, ModelLibrary& models, int figure,
                            const glm::vec3& origin, const glm::vec3& dir, float maxT,
                            glm::vec3& hitPos, glm::vec3& hitNormal, std::string& bone) const {
    const Entity* f = findEntity(entities, figure);
    LoadedModel* lm = f ? skinnedModel(*f, models) : nullptr;
    const auto pose = m_pose.find(figure);
    if (!lm || pose == m_pose.end()) return false;
    const std::vector<glm::mat4>& pal = pose->second;
    const auto& skel = lm->animData->skeleton;

    // The ray into model space, where the skin is: the drawing transform scales
    // per axis, so the direction is carried as is and t stays a world fraction.
    const glm::vec3 sz = glm::max(lm->size(), glm::vec3(1e-4f));
    const glm::mat4 toWorld = scenegraph::compose(f->center, f->rotation, (f->half * 2.0f) / sz) *
                              glm::translate(glm::mat4(1.0f), -lm->center());
    const glm::mat4 toModel = glm::inverse(toWorld);
    const glm::vec3 len = glm::normalize(dir) * maxT;
    const glm::vec3 o = glm::vec3(toModel * glm::vec4(origin, 1.0f));
    const glm::vec3 d = glm::vec3(toModel * glm::vec4(len, 0.0f));

    float best = 2.0f;   // in units of the whole ray (0..1)
    glm::vec3 bestP{0.0f}, bestN{0.0f};
    int bestJoint = -1;
    for (const fitzel::ModelPrimitive& prim : lm->animData->primitives) {
        if (prim.skin.empty()) continue;
        const std::size_t n = prim.skin.size();
        if (prim.vertices.size() < n * 8) continue;
        // De-indexed: every three vertices are one triangle.
        for (std::size_t v = 0; v + 2 < n; v += 3) {
            glm::vec3 p[3];
            for (int k = 0; k < 3; ++k) {
                const float* src = &prim.vertices[(v + k) * 8];
                const glm::vec4 bind(src[0], src[1], src[2], 1.0f);
                const fitzel::VertexSkin& s = prim.skin[v + k];
                glm::vec4 acc(0.0f);
                for (int w = 0; w < 4; ++w) {
                    const int j = s.joints[w];
                    if (s.weights[w] <= 0.0f || j < 0 || j >= static_cast<int>(pal.size())) continue;
                    acc += s.weights[w] * (pal[static_cast<std::size_t>(j)] * bind);
                }
                p[k] = glm::vec3(acc);
            }
            // Moeller-Trumbore, both faces.
            const glm::vec3 e1 = p[1] - p[0], e2 = p[2] - p[0];
            const glm::vec3 pv = glm::cross(d, e2);
            const float det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-12f) continue;
            const float inv = 1.0f / det;
            const glm::vec3 tv = o - p[0];
            const float u = glm::dot(tv, pv) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const glm::vec3 qv = glm::cross(tv, e1);
            const float w = glm::dot(d, qv) * inv;
            if (w < 0.0f || u + w > 1.0f) continue;
            const float t = glm::dot(e2, qv) * inv;
            if (t < 0.0f || t >= best) continue;
            best  = t;
            bestP = o + d * t;
            bestN = glm::cross(e1, e2);
            // The bone with the most say over these three corners.
            float weight[12] = {};
            int   joint[12];
            int   used = 0;
            for (int k = 0; k < 3; ++k) {
                const fitzel::VertexSkin& s = prim.skin[v + k];
                for (int q = 0; q < 4; ++q) {
                    if (s.weights[q] <= 0.0f) continue;
                    int slot = -1;
                    for (int x = 0; x < used; ++x) if (joint[x] == s.joints[q]) slot = x;
                    if (slot < 0 && used < 12) { slot = used++; joint[slot] = s.joints[q]; }
                    if (slot >= 0) weight[slot] += s.weights[q];
                }
            }
            bestJoint = -1;
            float most = 0.0f;
            for (int x = 0; x < used; ++x) if (weight[x] > most) { most = weight[x]; bestJoint = joint[x]; }
        }
    }
    if (best > 1.0f || bestJoint < 0 || bestJoint >= static_cast<int>(skel.size())) return false;
    hitPos = glm::vec3(toWorld * glm::vec4(bestP, 1.0f));
    glm::vec3 nw = glm::normalize(glm::vec3(glm::transpose(glm::inverse(toWorld)) * glm::vec4(bestN, 0.0f)));
    if (glm::dot(nw, dir) > 0.0f) nw = -nw;   // facing the ray, whatever the winding
    hitNormal = nw;
    bone = skel[static_cast<std::size_t>(bestJoint)].name;
    return true;
}

std::vector<std::string> Attachments::boneNames(const std::vector<Entity>& entities,
                                                ModelLibrary& models, int figure) const {
    std::vector<std::string> out;
    const Entity* f = findEntity(entities, figure);
    LoadedModel* lm = f ? skinnedModel(*f, models) : nullptr;
    if (!lm) return out;
    for (const fitzel::SkeletonJoint& j : lm->animData->skeleton) out.push_back(j.name);
    return out;
}

bool Attachments::attach(std::vector<Entity>& entities, ModelLibrary& models, int child,
                         int figure, const std::string& bone, const glm::mat4* offset,
                         float blend) {
    if (child == figure) return false;
    Entity*       c = findEntity(entities, child);
    const Entity* f = findEntity(entities, figure);
    LoadedModel*  lm = f ? skinnedModel(*f, models) : nullptr;
    if (!c || !lm) return false;
    const int joint = jointIndex(*lm->animData, bone);
    if (joint < 0) return false;
    Link link;
    link.figure = figure;
    link.joint  = joint;
    // Where it is now, seen from the bone: the seat to keep, or to blend from.
    glm::mat4 here(1.0f);
    const bool needHere = !offset || blend > 0.0f;
    if (needHere) {
        glm::mat4 b;
        if (!jointWorld(*f, models, joint, b)) return false;
        here = glm::inverse(b) * scenegraph::compose(c->center, c->rotation, glm::vec3(1.0f));
    }
    link.to = seatOf(offset ? rigid(*offset) : here);
    if (offset && blend > 0.0f) {
        link.from = seatOf(here);
        // The short way round: the two turns may be the same one, signed apart.
        if (glm::dot(link.from.rot, link.to.rot) < 0.0f) link.from.rot = -link.from.rot;
        link.dur = blend;
    }
    // Carried things are roots: the bone is the only parent they follow.
    if (c->parent >= 0) {
        c->parent        = -1;
        c->localCenter   = c->center;
        c->localRotation = c->rotation;
    }
    m_links[child] = link;
    return true;
}

void Attachments::detach(int child) { m_links.erase(child); }

void Attachments::apply(std::vector<Entity>& entities, ModelLibrary& models, float dt) {
    for (auto& [child, link] : m_links) {
        if (link.dur > 0.0f) link.t += dt;
        Entity*       c = findEntity(entities, child);
        const Entity* f = findEntity(entities, link.figure);
        glm::mat4 b;
        if (!c || !f || !jointWorld(*f, models, link.joint, b)) continue;
        const Seat s0 = current(link);
        const glm::mat4 seat = glm::translate(glm::mat4(1.0f), s0.pos) * glm::mat4_cast(s0.rot);
        glm::vec3 t, rotDeg, s;
        scenegraph::decompose(b * seat, t, rotDeg, s);
        c->center   = c->localCenter   = t;
        c->rotation = c->localRotation = rotDeg;
    }
}

void Attachments::clear() {
    m_pose.clear();
    m_links.clear();
}

} // namespace boneattach
