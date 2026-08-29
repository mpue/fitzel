#include "MeshPaint.hpp"

#include <algorithm>
#include <cmath>

#include "SandboxMath.hpp" // rayTriangle

namespace meshpaint {

namespace {

// A face's corners in world space. The mesh is drawn through the entity's model
// matrix, so every distance the brush measures has to be taken there too --
// otherwise a stretched object paints with a stretched brush.
std::vector<glm::vec3> faceWorld(const EditMesh& m, const glm::mat4& model, int f) {
    std::vector<glm::vec3> out;
    if (!m.validFace(f)) return out;
    out.reserve(m.faces[f].size());
    for (int i : m.faces[f]) out.push_back(glm::vec3(model * glm::vec4(m.verts[i], 1.0f)));
    return out;
}

// Brush falloff: 1 at the centre, 0 at the rim, flat at both ends. The soft edge
// is what lets a stroke be built up out of several passes instead of demanding
// one accurate one.
float falloff(float dist, float radius) {
    if (radius <= 1e-5f) return 0.0f;
    const float t = std::clamp(1.0f - dist / radius, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

bool pick(const EditMesh& m, const glm::mat4& model,
          const glm::vec3& ro, const glm::vec3& rd, Hit& out) {
    Hit best;
    best.t = 1e30f;
    for (int f = 0; f < static_cast<int>(m.faces.size()); ++f) {
        const std::vector<glm::vec3> w = faceWorld(m, model, f);
        // The same fan build() uses, so the brush lands where the pixels are.
        for (std::size_t i = 1; i + 1 < w.size(); ++i) {
            const float t = rayTriangle(ro, rd, w[0], w[i], w[i + 1]);
            if (t >= 0.0f && t < best.t) { best.t = t; best.face = f; }
        }
    }
    if (best.face < 0) return false;
    best.world = ro + rd * best.t;
    out = best;
    return true;
}

int refine(EditMesh& m, const glm::mat4& model, const glm::vec3& center,
           float radius, float target, int maxFaces) {
    if (target <= 1e-3f) return 0;
    int split = 0;
    // A quad splits into four, and a child may still be too coarse -- hence
    // passes rather than one sweep. Four of them take a face down to a sixteenth
    // of its edge length, which is as fine as any brush here asks for.
    for (int pass = 0; pass < 4; ++pass) {
        const int n = static_cast<int>(m.faces.size());
        if (n >= maxFaces) break;
        bool any = false;
        for (int f = 0; f < n; ++f) {
            if (static_cast<int>(m.faces.size()) >= maxFaces) break;
            if (m.faces[f].size() != 4) continue; // only quads have an obvious split
            const std::vector<glm::vec3> w = faceWorld(m, model, f);
            if (w.size() != 4) continue;

            glm::vec3 fc(0.0f);
            for (const glm::vec3& p : w) fc += p;
            fc *= 0.25f;

            float longest = 0.0f, reach = 0.0f;
            for (int i = 0; i < 4; ++i) {
                longest = std::max(longest, glm::length(w[(i + 1) % 4] - w[i]));
                reach   = std::max(reach, glm::length(w[i] - fc));
            }
            if (longest <= target) continue;                  // fine enough already
            if (glm::length(fc - center) > radius + reach) continue; // brush misses it
            if (editmesh::subdivide(m, f) >= 0) { ++split; any = true; }
        }
        if (!any) break;
    }
    return split;
}

bool dab(EditMesh& m, const glm::mat4& model, const glm::vec3& center,
         float radius, int layer, float amount, bool erase) {
    if (!erase && (layer < 0 || layer >= kPaintSlots)) return false;
    m.syncPaint();
    bool changed = false;
    for (std::size_t i = 0; i < m.verts.size(); ++i) {
        const glm::vec3 wp = glm::vec3(model * glm::vec4(m.verts[i], 1.0f));
        const float     a  = std::clamp(amount * falloff(glm::length(wp - center), radius),
                                        0.0f, 1.0f);
        if (a <= 1e-4f) continue;
        glm::vec4& p = m.paint[i];
        if (erase) {
            if (p.x <= 0.0f && p.y <= 0.0f && p.z <= 0.0f && p.w <= 0.0f) continue;
            // Subtract rather than fade. The terrain's erase multiplies its
            // weights down, which only ever APPROACHES zero -- and near the rim
            // of the brush, where the step is small, it approaches so slowly that
            // rubbing a stroke out leaves a halo of a few thousandths behind
            // forever. Invisible, but not nothing: the object keeps its painted
            // material and the scene file keeps writing the weights. Taking a
            // fixed step off gets there, in about as many passes as painting took.
            for (int k = 0; k < kPaintSlots; ++k)
                p[k] = (p[k] - a < 1e-3f) ? 0.0f : p[k] - a;
        } else {
            // Raise the painted layer toward 1 and fade the others -- the terrain
            // brush's rule, so a stroke converges on "this layer only" instead of
            // piling weight up past what the shader normalises.
            for (int k = 0; k < kPaintSlots; ++k)
                p[k] = (k == layer) ? p[k] + (1.0f - p[k]) * a : p[k] * (1.0f - a);
        }
        changed = true;
    }
    return changed;
}

bool clear(EditMesh& m) {
    if (!m.painted()) return false;
    std::fill(m.paint.begin(), m.paint.end(), glm::vec4(0.0f));
    return true;
}

} // namespace meshpaint
