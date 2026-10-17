#include "Sever.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/render/Renderer.hpp>
#include <fitzel/world/Model.hpp>

#include "BoneAttach.hpp"
#include "Component.hpp"   // ModelComponent
#include "Document.hpp"
#include "ModelLibrary.hpp"
#include "SceneGraph.hpp"  // scenegraph::compose

namespace sever {
namespace {

constexpr float kDensity  = 900.0f;   // kg/m^3 over the hull's box -- a limb is not a box
constexpr float kMinMass  = 0.8f;
constexpr float kMaxMass  = 45.0f;
constexpr int   kMinTris  = 8;        // fewer: the bone carries next to nothing
constexpr int   kHullMax  = 96;       // points handed to the hull builder
constexpr float kSeamWeight = 0.98f;  // a corner carried less than this by the cut
                                      // bones lies on the seam (the cap's rim)

const Entity* findEntity(const std::vector<Entity>& entities, int id) {
    for (const Entity& e : entities)
        if (e.id == id) return &e;
    return nullptr;
}

// Corners of different triangles that are the same point: a millimetre grid.
std::uint64_t weldKey(const glm::vec3& p) {
    const auto q = [](float v) {
        return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(v * 1000.0f)) & 0x1fffff);
    };
    return q(p.x) | (q(p.y) << 21) | (q(p.z) << 42);
}

} // namespace

System::System()  = default;
System::~System() = default;

bool System::cut(const std::vector<Entity>& entities, ModelLibrary& models,
                 const boneattach::Attachments& poses, fitzel::PhysicsWorld& physics, int figure,
                 const std::string& bone, const Options& opts, Cut& out) {
    const Entity* f = findEntity(entities, figure);
    const auto* mc = f ? f->components.get<ModelComponent>() : nullptr;
    LoadedModel* lm = mc ? models.byId(mc->modelId) : nullptr;
    if (!lm || !lm->animData || lm->animData->skeleton.empty()) return false;
    const std::vector<glm::mat4>* pal = poses.pose(figure);
    if (!pal) return false;
    const auto& skel = lm->animData->skeleton;
    const int b = boneattach::jointIndex(*lm->animData, bone);
    if (b < 0) return false;

    // The bone and everything under it.
    std::vector<char> under(skel.size(), 0);
    for (std::size_t j = 0; j < skel.size(); ++j) {
        int k = static_cast<int>(j), guard = 0;
        while (k >= 0 && k != b && guard++ < 512) k = skel[static_cast<std::size_t>(k)].parent;
        under[j] = k == b;
    }

    // Model space to the world, as SceneSubmit draws the figure (and BoneAttach
    // places what it carries); normals through the inverse transpose, since the
    // drawing scale may differ per axis.
    const glm::vec3 sz = glm::max(lm->size(), glm::vec3(1e-4f));
    const glm::mat4 toWorld = scenegraph::compose(f->center, f->rotation, (f->half * 2.0f) / sz) *
                              glm::translate(glm::mat4(1.0f), -lm->center());
    const glm::mat3 nWorld = glm::transpose(glm::inverse(glm::mat3(toWorld)));

    struct Corner {
        glm::vec3 p, n;
        glm::vec2 uv;
        float     cut;   // how much of it the cut bones carry
    };
    std::vector<std::vector<Corner>> byPrim(lm->animData->primitives.size());
    int tris = 0;
    for (std::size_t pi = 0; pi < lm->animData->primitives.size(); ++pi) {
        const fitzel::ModelPrimitive& prim = lm->animData->primitives[pi];
        if (prim.skin.empty()) continue;
        const std::size_t n = prim.skin.size();
        if (prim.vertices.size() < n * 8) continue;
        // De-indexed: every three vertices are one triangle.
        for (std::size_t v = 0; v + 2 < n; v += 3) {
            float share = 0.0f;
            float c[3];
            for (int k = 0; k < 3; ++k) {
                const fitzel::VertexSkin& s = prim.skin[v + k];
                c[k] = 0.0f;
                for (int w = 0; w < 4; ++w) {
                    const int j = s.joints[w];
                    if (s.weights[w] > 0.0f && j >= 0 && j < static_cast<int>(under.size()) && under[j])
                        c[k] += s.weights[w];
                }
                share += c[k];
            }
            if (share < 1.5f) continue;   // the rest of the body keeps it
            for (int k = 0; k < 3; ++k) {
                const float* src = &prim.vertices[(v + k) * 8];
                const glm::vec4 bind(src[0], src[1], src[2], 1.0f);
                const glm::vec3 bn(src[3], src[4], src[5]);
                const fitzel::VertexSkin& s = prim.skin[v + k];
                glm::vec4 acc(0.0f);
                glm::vec3 nacc(0.0f);
                for (int w = 0; w < 4; ++w) {
                    const int j = s.joints[w];
                    if (s.weights[w] <= 0.0f || j < 0 || j >= static_cast<int>(pal->size())) continue;
                    const glm::mat4& m = (*pal)[static_cast<std::size_t>(j)];
                    acc += s.weights[w] * (m * bind);
                    nacc += s.weights[w] * (glm::mat3(m) * bn);
                }
                Corner cn;
                cn.p = glm::vec3(toWorld * glm::vec4(glm::vec3(acc), 1.0f));
                const glm::vec3 nw = nWorld * nacc;
                cn.n = glm::dot(nw, nw) > 1e-12f ? glm::normalize(nw) : glm::vec3(0.0f, 1.0f, 0.0f);
                cn.uv = glm::vec2(src[6], src[7]);
                cn.cut = c[k];
                byPrim[pi].push_back(cn);
            }
            ++tris;
        }
    }
    if (tris < kMinTris) return false;

    // Its middle (the body's origin) and its box.
    glm::vec3 mid(0.0f), lo(1e30f), hi(-1e30f);
    std::size_t count = 0;
    for (const auto& cs : byPrim)
        for (const Corner& c : cs) {
            mid += c.p;
            lo = glm::min(lo, c.p);
            hi = glm::max(hi, c.p);
            ++count;
        }
    mid /= static_cast<float>(count);

    // The open edge where it was joined: edges of only one of its triangles,
    // both ends on the seam (not wholly carried by the cut bones -- an eyelid or
    // a sleeve's hem is open too, but carried whole). The cap is a fan from the
    // middle of that rim over all of it.
    std::unordered_map<std::uint64_t, int> edgeUse;
    const auto edgeKey = [](std::uint64_t a, std::uint64_t b) {
        if (a > b) std::swap(a, b);
        return a * 0x9e3779b97f4a7c15ull ^ (b + 0x632be59bd9b4e019ull + (a << 6) + (a >> 2));
    };
    for (const auto& cs : byPrim)
        for (std::size_t t = 0; t + 2 < cs.size(); t += 3)
            for (int k = 0; k < 3; ++k) {
                const std::uint64_t a = weldKey(cs[t + k].p), b2 = weldKey(cs[t + (k + 1) % 3].p);
                if (a != b2) ++edgeUse[edgeKey(a, b2)];
            }
    std::vector<std::pair<glm::vec3, glm::vec3>> rim;
    glm::vec3 rimMid(0.0f);
    for (const auto& cs : byPrim)
        for (std::size_t t = 0; t + 2 < cs.size(); t += 3)
            for (int k = 0; k < 3; ++k) {
                const Corner& a = cs[t + k];
                const Corner& c2 = cs[t + (k + 1) % 3];
                if (a.cut >= kSeamWeight && c2.cut >= kSeamWeight) continue;
                const std::uint64_t ka = weldKey(a.p), kb = weldKey(c2.p);
                if (ka == kb || edgeUse[edgeKey(ka, kb)] != 1) continue;
                rim.push_back({a.p, c2.p});
                rimMid += a.p + c2.p;
            }
    // Without a rim (a skin that was never closed at the joint): the bone's own
    // origin as the cut.
    glm::vec3 jointAt = mid;
    if (b < static_cast<int>(pal->size()))
        jointAt = glm::vec3(toWorld * glm::vec4(glm::vec3(((*pal)[static_cast<std::size_t>(b)] *
                                                          glm::inverse(skel[static_cast<std::size_t>(b)].inverseBind))[3]), 1.0f));
    const glm::vec3 cutAt = rim.empty() ? jointAt : rimMid / static_cast<float>(rim.size() * 2);
    glm::vec3 cutN = cutAt - mid;
    cutN = glm::dot(cutN, cutN) > 1e-10f ? glm::normalize(cutN) : glm::vec3(0.0f, 1.0f, 0.0f);

    // The meshes, around the body's origin (`mid`).
    auto piece = std::make_unique<Piece>();
    for (std::size_t pi = 0; pi < byPrim.size(); ++pi) {
        if (byPrim[pi].empty()) continue;
        std::vector<fitzel::Vertex> vs;
        vs.reserve(byPrim[pi].size());
        for (const Corner& c : byPrim[pi]) {
            fitzel::Vertex v{};
            v.position = c.p - mid;
            v.normal   = c.n;
            v.uv       = c.uv;
            vs.push_back(v);
        }
        Part part;
        part.material = pi < lm->primMaterialId.size() ? lm->primMaterialId[pi] : fitzel::AssetId{};
        part.mesh = fitzel::Mesh::create(vs);
        piece->parts.push_back(std::move(part));
    }
    if (opts.cap.valid() && !rim.empty()) {
        std::vector<fitzel::Vertex> vs;
        vs.reserve(rim.size() * 3);
        for (const auto& [a, c2] : rim) {
            glm::vec3 p0 = cutAt, p1 = a, p2 = c2;
            if (glm::dot(glm::cross(p1 - p0, p2 - p0), cutN) < 0.0f) std::swap(p1, p2);
            for (const glm::vec3& p : {p0, p1, p2}) {
                fitzel::Vertex v{};
                v.position = p - mid;
                v.normal   = cutN;
                // flesh has no texture to follow: a little variation across the face
                v.uv = glm::vec2(glm::dot(p - cutAt, glm::vec3(7.0f, 3.0f, 5.0f)), glm::dot(p - cutAt, glm::vec3(2.0f, 9.0f, 4.0f)));
                vs.push_back(v);
            }
        }
        Part part;
        part.material = opts.cap;
        part.mesh = fitzel::Mesh::create(vs);
        piece->parts.push_back(std::move(part));
    }

    // The body: a hull round a spread of its corners.
    std::vector<glm::vec3> pts;
    const std::size_t stride = std::max<std::size_t>(1, count / kHullMax);
    std::size_t i = 0;
    for (const auto& cs : byPrim)
        for (const Corner& c : cs)
            if (i++ % stride == 0) pts.push_back(c.p - mid);
    for (const auto& [a, c2] : rim) {          // the rim as well: the cut end is flat
        if (pts.size() >= static_cast<std::size_t>(kHullMax) + 24) break;
        pts.push_back(a - mid);
    }
    const glm::vec3 box = glm::max(hi - lo, glm::vec3(0.02f));
    const float mass = std::clamp(box.x * box.y * box.z * kDensity * 0.45f, kMinMass, kMaxMass);
    piece->body = physics.addConvexHull(pts.data(), static_cast<int>(pts.size()), mid,
                                        glm::quat(1.0f, 0.0f, 0.0f, 0.0f), mass);
    if (piece->body == 0) return false;
    physics.setLinearVelocity(piece->body, opts.vel);
    physics.setAngularVelocity(piece->body, opts.spin);
    piece->model  = glm::translate(glm::mat4(1.0f), mid);
    piece->cutAt  = cutAt - mid;
    piece->cutN   = cutN;
    piece->handle = m_nextHandle++;
    out.piece = piece->handle;
    m_pieces.push_back(std::move(piece));

    // Too many lying about: the oldest are swept up.
    while (m_pieces.size() > kMaxPieces) {
        physics.removeBody(m_pieces.front()->body);
        m_pieces.erase(m_pieces.begin());
    }

    out.at = cutAt;
    out.normal = cutN;
    out.triangles = tris;
    return true;
}

bool System::piece(int handle, const fitzel::PhysicsWorld* physics, PieceState& out) const {
    for (const auto& p : m_pieces) {
        if (p->handle != handle) continue;
        out.at     = glm::vec3(p->model * glm::vec4(p->cutAt, 1.0f));
        out.normal = glm::normalize(glm::mat3(p->model) * p->cutN);
        out.resting = false;
        glm::vec3 v, w;
        if (physics && physics->getLinearVelocity(p->body, v) && physics->getAngularVelocity(p->body, w))
            out.resting = glm::dot(v, v) < 0.05f * 0.05f && glm::dot(w, w) < 0.3f * 0.3f;
        return true;
    }
    return false;
}

bool System::owns(fitzel::PhysicsBodyId body) const {
    for (const auto& p : m_pieces)
        if (p->body == body) return true;
    return false;
}

void System::update(const fitzel::PhysicsWorld* physics) {
    if (!physics) return;
    for (auto it = m_pieces.begin(); it != m_pieces.end();) {
        glm::vec3 pos;
        glm::quat rot;
        // Out of the world (fell through a gap in it), or its body is gone: dropped.
        if (!physics->getTransform((*it)->body, pos, rot) || pos.y < -500.0f) {
            it = m_pieces.erase(it);
            continue;
        }
        (*it)->model = glm::translate(glm::mat4(1.0f), pos) * glm::mat4_cast(rot);
        ++it;
    }
}

void System::submit(fitzel::Renderer& renderer, const std::vector<fitzel::Material>& gpuMats,
                    const std::vector<MaterialDef>& materials, const Document& document) const {
    for (const auto& p : m_pieces)
        for (const Part& part : p->parts) {
            if (part.mesh.vertexCount() == 0) continue;
            const int mi = document.materialIndex(part.material);
            if (mi < 0 || mi >= static_cast<int>(gpuMats.size()) || mi >= static_cast<int>(materials.size())) continue;
            const MaterialDef& md = materials[static_cast<std::size_t>(mi)];
            renderer.submit(part.mesh, gpuMats[static_cast<std::size_t>(mi)], p->model,
                            /*castsPointShadow=*/true, /*reflective=*/false, md.opacity,
                            /*forceTransparent=*/md.alphaMode == AlphaMode::Blend);
        }
}

void System::clear(fitzel::PhysicsWorld* physics) {
    if (physics)
        for (const auto& p : m_pieces) physics->removeBody(p->body);
    m_pieces.clear();
}

} // namespace sever
