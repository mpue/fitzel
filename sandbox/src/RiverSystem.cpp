#include "RiverSystem.hpp"

#include <algorithm>
#include <cmath>

#include <nlohmann/json.hpp>

#include "SandboxMath.hpp"   // sampleSpline()

namespace {

// Below this a delta is not worth a cell. Shared by the publish and by the prune
// in carve(): the host field and this system's record of it MUST agree on what
// they keep, or `natural()` reads them apart (see carve).
constexpr float kNegligible      = 1e-5f;
constexpr float kNegligiblePaint = 1e-4f;

const std::vector<glm::vec3>   kNoLine;
const std::vector<int>         kNoSamples;
const rivergen::Course         kNoCourse;

// Ramp the per-control-point bias out to every sample, linearly between the
// points whose sample indices `ptSample` names. Same shape as the spline's lift
// ramp -- and deliberately not smoothed, so a handle raised to dam a pool holds
// the pool right up to the point rather than easing away from it.
std::vector<float> biasRamp(const std::vector<float>& bias,
                            const std::vector<int>& ptSample,
                            std::size_t samples) {
    std::vector<float> out(samples, 0.0f);
    if (bias.empty() || ptSample.size() < 2) return out;
    const std::size_t spans = ptSample.size() - 1;
    for (std::size_t k = 0; k < spans; ++k) {
        const int a = ptSample[k], b = ptSample[k + 1];
        if (a < 0 || b <= a) continue;
        const float ba = bias[std::min(k, bias.size() - 1)];
        const float bb = (k + 1 < bias.size()) ? bias[k + 1] : bias.back();
        for (int s = a; s <= b && static_cast<std::size_t>(s) < samples; ++s)
            out[s] = glm::mix(ba, bb, static_cast<float>(s - a) /
                                      static_cast<float>(b - a));
    }
    return out;
}

// Squared distance from p to segment [a,b], with the parameter along it.
float distToSeg(const glm::vec2& p, const glm::vec2& a, const glm::vec2& b,
                float& t) {
    const glm::vec2 ab = b - a;
    const float len2 = glm::dot(ab, ab);
    t = len2 > 1e-9f ? glm::clamp(glm::dot(p - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
    const glm::vec2 d = p - (a + ab * t);
    return glm::dot(d, d);
}

} // namespace

// --- Authoring ---------------------------------------------------------------

int RiverSystem::addPath(rivergen::Preset p, const std::string& name) {
    Path w;
    w.preset = p;
    w.kind   = rivergen::presetKind(p);
    w.style  = rivergen::preset(p);
    w.name   = name.empty() ? rivergen::presetName(p) : name;
    paths.push_back(std::move(w));
    m_built.push_back(Built{});
    m_runs.push_back(Run{});
    m_carveDirty = true;
    return static_cast<int>(paths.size()) - 1;
}

void RiverSystem::applyPreset(int path, rivergen::Preset p) {
    if (path < 0 || path >= static_cast<int>(paths.size())) return;
    paths[path].preset = p;
    paths[path].kind   = rivergen::presetKind(p);
    paths[path].style  = rivergen::preset(p);
    touch(path);
}

void RiverSystem::removePath(int i) {
    if (i < 0 || i >= static_cast<int>(paths.size())) return;
    paths.erase(paths.begin() + i);
    if (i < static_cast<int>(m_built.size())) m_built.erase(m_built.begin() + i);
    if (i < static_cast<int>(m_runs.size()))  m_runs.erase(m_runs.begin() + i);
    m_carveDirty = true;
}

void RiverSystem::insertPoint(int path, int at, glm::vec2 p, float bias) {
    if (path < 0 || path >= static_cast<int>(paths.size())) return;
    Path& w = paths[path];
    at = glm::clamp(at, 0, static_cast<int>(w.points.size()));
    w.bias.resize(w.points.size(), 0.0f);
    w.points.insert(w.points.begin() + at, p);
    w.bias.insert(w.bias.begin() + at, bias);
    touch(path);
}

void RiverSystem::erasePoint(int path, int at) {
    if (path < 0 || path >= static_cast<int>(paths.size())) return;
    Path& w = paths[path];
    if (at < 0 || at >= static_cast<int>(w.points.size())) return;
    w.bias.resize(w.points.size(), 0.0f);
    w.points.erase(w.points.begin() + at);
    w.bias.erase(w.bias.begin() + at);
    touch(path);
}

float RiverSystem::biasOf(int path, int i) const {
    if (path < 0 || path >= static_cast<int>(paths.size())) return 0.0f;
    const Path& w = paths[path];
    return (i >= 0 && i < static_cast<int>(w.bias.size())) ? w.bias[i] : 0.0f;
}

void RiverSystem::setBias(int path, int i, float bias) {
    if (path < 0 || path >= static_cast<int>(paths.size())) return;
    Path& w = paths[path];
    if (i < 0 || i >= static_cast<int>(w.points.size())) return;
    w.bias.resize(w.points.size(), 0.0f);
    w.bias[i] = bias;
    touch(path);
}

void RiverSystem::touch(int path) {
    m_built.resize(paths.size());
    m_runs.resize(paths.size());
    if (path < 0) {
        for (Built& b : m_built) b.dirty = true;
    } else if (path < static_cast<int>(m_built.size())) {
        m_built[path].dirty = true;
    }
    m_carveDirty = true;
}

// --- Solving -----------------------------------------------------------------

float RiverSystem::natural(float x, float z) const {
    // The ground as it would be if no watercourse had ever touched it: the bare
    // terrain, plus every manual edit that is not ours.
    //
    // The two field samples are differenced FIRST and the bare terrain added
    // afterwards. Both fields walk the same four cells with the same weights, so
    // where they hold the same numbers the difference is exactly zero and the
    // answer is exactly the bare ground -- every time, for as long as the cut
    // stands. Adding the cut in and taking it out again would instead round the
    // terrain's own low bits away and hand back something an ulp different on
    // every rebuild; see the header for what that costs.
    const float base  = baseAt ? baseAt(x, z) : 0.0f;
    const float other = (edits ? edits->sample(x, z) : 0.0f) - m_mine.sample(x, z);
    return base + other;
}

void RiverSystem::update() {
    m_built.resize(paths.size());
    m_runs.resize(paths.size());
    for (int i = 0; i < static_cast<int>(paths.size()); ++i)
        if (m_built[i].dirty) rebuild(i);
}

void RiverSystem::rebuild(int i) {
    Built& b = m_built[i];
    Run&   r = m_runs[i];
    b.dirty  = false;
    r.course = rivergen::Course{};
    r.mesh   = fitzel::Mesh{};
    r.spray.clear();
    r.dress.clear();
    r.dressMeshes.clear();
    r.verts = 0;

    const Path& p = paths[i];
    if (!p.enabled || p.points.size() < 2) return;

    std::vector<int> ptSample;
    const std::vector<glm::vec2> flat = sampleSpline(p.points, false, &ptSample);
    if (flat.size() < 2) return;

    std::vector<float> bias = p.bias;
    bias.resize(p.points.size(), 0.0f);
    const std::vector<float> ramp = biasRamp(bias, ptSample, flat.size());

    r.course = rivergen::solve(p.kind, p.style, flat, ramp, ptSample,
                               [this](float x, float z) { return natural(x, z); });
    if (r.course.empty()) return;

    const rivergen::Surface s = rivergen::surface(p.kind, p.style, r.course);
    if (s.data.vertices.empty()) return;
    r.mesh  = fitzel::Mesh::create(s.data);
    r.lo    = s.lo;
    r.hi    = s.hi;
    r.verts = s.verts;
    r.spray = rivergen::spray(p.style, r.course);

    // Stones and reeds. The materials are find-or-created (and re-coloured) here
    // rather than at the edit site, so a colour change lands on the next rebuild
    // without a separate apply step -- the same contract splinegen::ensurePalette
    // has.
    if (materials) {
        const rivergen::Dressing dm = rivergen::ensureDressing(*materials, p.style);
        r.dress = rivergen::dressing(p.kind, p.style, r.course, dm);
        r.dressMeshes.reserve(r.dress.size());
        for (rivergen::Batch& b : r.dress) {
            r.dressMeshes.push_back(fitzel::Mesh::create(b.data));
            // Release the CPU copy: a kilometre of gravel is tens of megabytes
            // and nothing here reads it again (the AABB and material stay).
            b.data = fitzel::MeshData{};
        }
    }
}

// --- The cut -----------------------------------------------------------------

void RiverSystem::cutInto(const Run& r, const Path& p,
                          fitzel::TerrainEditField& dst,
                          fitzel::TerrainPaintField& dstPaint,
                          glm::vec2& lo, glm::vec2& hi) const {
    const rivergen::Course& c = r.course;
    const int n = static_cast<int>(c.line.size());
    if (n < 2) return;

    float maxReach = 0.0f;
    for (float h : c.half) maxReach = std::max(maxReach, rivergen::reach(p.style, h));
    if (maxReach <= 0.0f) return;

    glm::vec2 clo(c.line[0].x, c.line[0].z), chi = clo;
    for (const glm::vec3& w : c.line) {
        clo = glm::min(clo, glm::vec2(w.x, w.z));
        chi = glm::max(chi, glm::vec2(w.x, w.z));
    }
    clo -= glm::vec2(maxReach);
    chi += glm::vec2(maxReach);
    lo = glm::min(lo, clo);
    hi = glm::max(hi, chi);

    // Bucket the segments so a cell tests a handful of them rather than all
    // fifteen hundred. The road can afford the full scan because it is cut once
    // on a button; this one is cut at the end of every drag.
    float maxSeg = 0.0f;
    for (int k = 0; k + 1 < n; ++k)
        maxSeg = std::max(maxSeg, glm::distance(glm::vec2(c.line[k].x, c.line[k].z),
                                                glm::vec2(c.line[k + 1].x, c.line[k + 1].z)));
    const float bs = std::max({maxReach, maxSeg, 2.0f});
    std::unordered_map<std::int64_t, std::vector<int>> buckets;
    auto bkey = [](int x, int z) {
        return (static_cast<std::int64_t>(x) << 32) ^ static_cast<std::uint32_t>(z);
    };
    for (int k = 0; k + 1 < n; ++k) {
        const glm::vec2 a(c.line[k].x, c.line[k].z);
        const glm::vec2 b(c.line[k + 1].x, c.line[k + 1].z);
        const int x0 = static_cast<int>(std::floor(std::min(a.x, b.x) / bs));
        const int x1 = static_cast<int>(std::floor(std::max(a.x, b.x) / bs));
        const int z0 = static_cast<int>(std::floor(std::min(a.y, b.y) / bs));
        const int z1 = static_cast<int>(std::floor(std::max(a.y, b.y) / bs));
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) buckets[bkey(x, z)].push_back(k);
    }
    // Every segment within maxReach of a cell has its own bucket within this
    // many rings of the cell's, because a bucket is at least maxReach across.
    const int rings = std::max(1, static_cast<int>(std::ceil(maxReach / bs)));

    // The paint reaches past the waterline but rarely as far as the cut, so the
    // two share the scan and each stops where it stops.
    const int   layer = p.style.bankLayer;
    const float paintPad = (layer >= 0 && layer <= 3)
                         ? std::max(p.style.bankPaint, 0.0f) : -1.0f;

    const float cell = dst.cell;
    const int ix0 = static_cast<int>(std::floor(clo.x / cell));
    const int ix1 = static_cast<int>(std::ceil (chi.x / cell));
    const int iz0 = static_cast<int>(std::floor(clo.y / cell));
    const int iz1 = static_cast<int>(std::ceil (chi.y / cell));
    for (int iz = iz0; iz <= iz1; ++iz) {
        for (int ix = ix0; ix <= ix1; ++ix) {
            const glm::vec2 w(ix * cell, iz * cell);
            const int bx = static_cast<int>(std::floor(w.x / bs));
            const int bz = static_cast<int>(std::floor(w.y / bs));
            float bestD2 = 1e30f, bestT = 0.0f;
            int   bestK  = -1;
            for (int dz = -rings; dz <= rings; ++dz) {
                for (int dx = -rings; dx <= rings; ++dx) {
                    const auto it = buckets.find(bkey(bx + dx, bz + dz));
                    if (it == buckets.end()) continue;
                    for (int k : it->second) {
                        float t;
                        const float d2 = distToSeg(
                            w, glm::vec2(c.line[k].x, c.line[k].z),
                            glm::vec2(c.line[k + 1].x, c.line[k + 1].z), t);
                        if (d2 < bestD2) { bestD2 = d2; bestT = t; bestK = k; }
                    }
                }
            }
            if (bestK < 0) continue;
            const float d    = std::sqrt(bestD2);
            const float half = glm::mix(c.half[bestK], c.half[bestK + 1], bestT);
            // WHICH side, not just how far: a bend's deep line is against its
            // outer bank, so the two halves of the section are different shapes
            // and an unsigned distance would cut the wrong one.
            const glm::vec2 a(c.line[bestK].x,     c.line[bestK].z);
            const glm::vec2 b(c.line[bestK + 1].x, c.line[bestK + 1].z);
            const glm::vec2 tg = b - a;
            const glm::vec2 nr(tg.y, -tg.x);       // the surface strip's +side
            const float nrl = glm::length(nr);
            const float dSigned = (nrl > 1e-6f)
                ? glm::dot(w - (a + tg * bestT), nr / nrl) : d;
            const float shift = glm::mix(c.shift[bestK], c.shift[bestK + 1], bestT);

            // The bank layer: full over the water, fading out across the margin.
            // Laid before the height test bails, because the paint's reach and the
            // cut's are two different numbers and either may be the larger.
            if (paintPad >= 0.0f && d <= half + paintPad) {
                const float t = (d <= half) ? 0.0f
                              : (d - half) / std::max(paintPad, 1e-3f);
                const float w = glm::clamp(p.style.bankBlend, 0.0f, 1.0f) *
                                (1.0f - t * t * (3.0f - 2.0f * t));
                if (w > 1e-3f) {
                    glm::vec4& px =
                        dstPaint.weights[fitzel::TerrainEditField::cellKey(ix, iz)];
                    // Strongest wins where two courses share a cell, the same rule
                    // the cut uses -- a tributary's gravel does not thin a river's.
                    px[layer] = std::max(px[layer], w);
                }
            }

            if (d > rivergen::reach(p.style, half)) continue;
            const float surf = glm::mix(c.line[bestK].y, c.line[bestK + 1].y, bestT);
            const float bed  = glm::mix(c.bed[bestK],    c.bed[bestK + 1],    bestT);
            const float g    = natural(w.x, w.y);
            const float target = rivergen::sectionHeight(
                p.kind, p.style, dSigned, half, shift, surf, bed, g);
            const float delta = target - g;
            const std::int64_t key = fitzel::TerrainEditField::cellKey(ix, iz);
            // Where two watercourses share a cell the DEEPER cut wins, so a
            // tributary meeting a river hands over a continuous channel instead
            // of a step at whichever one happened to be stamped second.
            const auto ex = dst.deltas.find(key);
            if (ex == dst.deltas.end()) dst.deltas[key] = delta;
            else                        ex->second = std::min(ex->second, delta);
        }
    }
}

bool RiverSystem::publish(fitzel::TerrainEditField& edit,
                          fitzel::TerrainPaintField& paint,
                          const fitzel::TerrainEditField& next,
                          const fitzel::TerrainPaintField& nextPaint,
                          glm::vec2 lo, glm::vec2 hi,
                          glm::vec2& outMin, glm::vec2& outMax) {
    bool changed = false;
    auto apply = [&](std::int64_t key, float diff) {
        if (std::fabs(diff) < kNegligible) return;
        const auto it = edit.deltas.find(key);
        const float v = (it == edit.deltas.end() ? 0.0f : it->second) + diff;
        if (std::fabs(v) < kNegligible) { if (it != edit.deltas.end()) edit.deltas.erase(it); }
        else if (it != edit.deltas.end()) it->second = v;
        else                              edit.deltas[key] = v;
        changed = true;
    };
    // Only the DIFFERENCE goes out, which is what lets an abandoned trench give
    // its ground back and a sculpt outside the new footprint survive untouched.
    for (const auto& [key, was] : m_mine.deltas) {
        const auto it = next.deltas.find(key);
        apply(key, (it == next.deltas.end() ? 0.0f : it->second) - was);
    }
    for (const auto& [key, now] : next.deltas)
        if (!m_mine.deltas.count(key)) apply(key, now);

    // The bank paint, on the same difference discipline. Component-wise and
    // clamped: the author's brush writes into the same field, so subtracting a
    // course's own contribution must not be able to push their work negative.
    auto paintApply = [&](std::int64_t key, const glm::vec4& diff) {
        if (glm::all(glm::lessThan(glm::abs(diff), glm::vec4(kNegligiblePaint)))) return;
        const auto it = paint.weights.find(key);
        glm::vec4 v = (it == paint.weights.end() ? glm::vec4(0.0f) : it->second) + diff;
        v = glm::clamp(v, glm::vec4(0.0f), glm::vec4(1.0f));
        if (glm::all(glm::lessThan(v, glm::vec4(kNegligiblePaint)))) {
            if (it != paint.weights.end()) paint.weights.erase(it);
        } else if (it != paint.weights.end()) {
            it->second = v;
        } else {
            paint.weights[key] = v;
        }
        changed = true;
    };
    for (const auto& [key, was] : m_paint.weights) {
        const auto it = nextPaint.weights.find(key);
        paintApply(key, (it == nextPaint.weights.end() ? glm::vec4(0.0f)
                                                       : it->second) - was);
    }
    for (const auto& [key, now] : nextPaint.weights)
        if (!m_paint.weights.count(key)) paintApply(key, now);

    // The rebuild has to cover the ground the cut LEFT as well as the ground it
    // took, or the chunks under an abandoned trench keep showing it.
    outMin = m_hasFootprint ? glm::min(lo, m_lo) : lo;
    outMax = m_hasFootprint ? glm::max(hi, m_hi) : hi;
    outMin -= glm::vec2(edit.cell);
    outMax += glm::vec2(edit.cell);

    m_mine  = next;
    m_paint = nextPaint;
    m_lo = lo; m_hi = hi;
    m_hasFootprint = !next.deltas.empty() || !nextPaint.weights.empty();
    m_carveDirty = false;
    return changed;
}

bool RiverSystem::carve(fitzel::TerrainEditField& edit,
                        fitzel::TerrainPaintField& paint, glm::vec2& outMin,
                        glm::vec2& outMax) {
    update();   // the surfaces must be current before their beds are stamped

    fitzel::TerrainEditField next;
    next.cell = edit.cell;
    m_mine.cell = edit.cell;   // the two grids must agree, see natural()
    fitzel::TerrainPaintField nextPaint;
    nextPaint.cell = paint.cell;
    m_paint.cell   = paint.cell;

    glm::vec2 lo(1e30f), hi(-1e30f);
    for (int i = 0; i < static_cast<int>(paths.size()) &&
                    i < static_cast<int>(m_runs.size()); ++i) {
        if (!paths[i].enabled) continue;
        cutInto(m_runs[i], paths[i], next, nextPaint, lo, hi);
    }
    if (lo.x > hi.x) { lo = glm::vec2(0.0f); hi = glm::vec2(0.0f); }

    // Drop what the host field would refuse to store anyway.
    //
    // This is not tidying. `natural()` is `groundAt() - mine.sample()`, and it is
    // exact only while the two fields hold the SAME numbers. The host erases a
    // delta it considers negligible; if our own record keeps it, the two drift
    // apart by that much, `natural` answers a hair differently every time, and
    // every threshold downstream of it -- which sample counts as a fall, how far
    // a pool reaches, where the meander is suppressed -- becomes a coin toss on
    // the last bit. The visible result is a bed that moves by metres at a
    // waterfall lip while every number involved looks like rounding.
    //
    // So the two records agree on what is worth keeping, and they agree here.
    for (auto it = next.deltas.begin(); it != next.deltas.end(); ) {
        if (std::fabs(it->second) < kNegligible) it = next.deltas.erase(it);
        else ++it;
    }
    for (auto it = nextPaint.weights.begin(); it != nextPaint.weights.end(); ) {
        if (glm::all(glm::lessThan(glm::abs(it->second), glm::vec4(kNegligiblePaint))))
            it = nextPaint.weights.erase(it);
        else ++it;
    }
    return publish(edit, paint, next, nextPaint, lo, hi, outMin, outMax);
}

bool RiverSystem::release(fitzel::TerrainEditField& edit,
                          fitzel::TerrainPaintField& paint, glm::vec2& outMin,
                          glm::vec2& outMax) {
    fitzel::TerrainEditField empty;
    empty.cell = edit.cell;
    fitzel::TerrainPaintField emptyPaint;
    emptyPaint.cell = paint.cell;
    return publish(edit, paint, empty, emptyPaint, glm::vec2(0.0f),
                   glm::vec2(0.0f), outMin, outMax);
}

std::vector<glm::vec3> RiverSystem::wetDiscs(float margin) const {
    std::vector<glm::vec3> out;
    for (int i = 0; i < static_cast<int>(m_runs.size()) &&
                    i < static_cast<int>(paths.size()); ++i) {
        if (!paths[i].enabled) continue;
        const rivergen::Course& c = m_runs[i].course;
        const int n = static_cast<int>(c.line.size());
        if (n < 2) continue;
        // One disc every half-radius of ground, so consecutive discs overlap and
        // the chain has no gaps for a tree to grow in. A kilometre of river at
        // seven metres wide is about three hundred of them, which is a copy worth
        // making once a frame and reading from four worker threads.
        float next = -1e9f;
        for (int k = 0; k < n; ++k) {
            const float r = c.half[k] + margin;
            if (c.s[k] < next) continue;
            next = c.s[k] + std::max(r * 0.7f, 1.0f);
            out.emplace_back(c.line[k].x, c.line[k].z, r);
        }
    }
    return out;
}

// --- Queries -----------------------------------------------------------------

const std::vector<glm::vec3>& RiverSystem::line(int i) const {
    if (i < 0 || i >= static_cast<int>(m_runs.size())) return kNoLine;
    return m_runs[i].course.line;
}

const std::vector<int>& RiverSystem::pointSamples(int i) const {
    if (i < 0 || i >= static_cast<int>(m_runs.size())) return kNoSamples;
    return m_runs[i].course.ptSample;
}

const rivergen::Course& RiverSystem::course(int i) const {
    if (i < 0 || i >= static_cast<int>(m_runs.size())) return kNoCourse;
    return m_runs[i].course;
}

bool RiverSystem::handleHeight(int path, int point, float& outY) const {
    if (path < 0 || path >= static_cast<int>(m_runs.size())) return false;
    const rivergen::Course& c = m_runs[path].course;
    const int K = static_cast<int>(c.ptSample.size());
    if (K < 2 || c.line.empty() || point < 0 || point >= K) return false;
    // The course runs downstream; the author's points may not. ptSample was
    // flipped with it, so the author's point `point` is counted from the other
    // end when it was.
    const int j = c.reversed ? (K - 1 - point) : point;
    const int sIdx = glm::clamp(c.ptSample[j], 0,
                                static_cast<int>(c.line.size()) - 1);
    outY = c.line[sIdx].y;
    return true;
}

bool RiverSystem::sample(const glm::vec2& xz, float& outSurface, float* outDepth,
                         glm::vec2* outFlow, float* outWhite) const {
    bool hit = false;
    float bestSurf = 0.0f;
    for (int i = 0; i < static_cast<int>(m_runs.size()) &&
                    i < static_cast<int>(paths.size()); ++i) {
        if (!paths[i].enabled) continue;
        const rivergen::Course& c = m_runs[i].course;
        const int n = static_cast<int>(c.line.size());
        if (n < 2) continue;
        // Cheap rejection on the run's own bounds before the segment walk: most
        // queries are nowhere near most watercourses.
        if (xz.x < m_runs[i].lo.x - 2.0f || xz.x > m_runs[i].hi.x + 2.0f ||
            xz.y < m_runs[i].lo.z - 2.0f || xz.y > m_runs[i].hi.z + 2.0f) continue;

        float bestD2 = 1e30f, bestT = 0.0f;
        int   bestK  = -1;
        for (int k = 0; k + 1 < n; ++k) {
            float t;
            const float d2 = distToSeg(xz, glm::vec2(c.line[k].x, c.line[k].z),
                                       glm::vec2(c.line[k + 1].x, c.line[k + 1].z), t);
            if (d2 < bestD2) { bestD2 = d2; bestT = t; bestK = k; }
        }
        if (bestK < 0) continue;
        const float d    = std::sqrt(bestD2);
        const float half = glm::mix(c.half[bestK], c.half[bestK + 1], bestT);
        if (d >= half) continue;
        const glm::vec2 sa(c.line[bestK].x,     c.line[bestK].z);
        const glm::vec2 sb(c.line[bestK + 1].x, c.line[bestK + 1].z);
        const glm::vec2 stg = sb - sa;
        const glm::vec2 snr(stg.y, -stg.x);
        const float snl = glm::length(snr);
        const float dSigned = (snl > 1e-6f)
            ? glm::dot(xz - (sa + stg * bestT), snr / snl) : d;
        const float shift = glm::mix(c.shift[bestK], c.shift[bestK + 1], bestT);
        const float surf = glm::mix(c.line[bestK].y, c.line[bestK + 1].y, bestT);
        // The HIGHEST water wins where two overlap: a tributary spilling into a
        // river is standing in the river, not beside it.
        if (hit && surf <= bestSurf) continue;
        hit = true;
        bestSurf = surf;
        outSurface = surf;
        const float bed = glm::mix(c.bed[bestK], c.bed[bestK + 1], bestT);
        const float white = glm::mix(c.white[bestK], c.white[bestK + 1], bestT);
        if (outDepth) {
            // The water column here, not the channel's deepest point: the same
            // section the bed was cut to (see rivergen::sectionHeight).
            const float floorY = rivergen::sectionHeight(
                paths[i].kind, paths[i].style, dSigned, half, shift, surf, bed, surf);
            *outDepth = std::max(surf - floorY, 0.0f);
        }
        if (outFlow) {
            const glm::vec2 dir = glm::mix(c.dir[bestK], c.dir[bestK + 1], bestT);
            const float u = d / std::max(half, 1e-3f);
            // Fastest mid-channel, dying at the banks -- which is why a boat set
            // adrift in the middle overtakes one drifting along the edge.
            const float prof = 1.0f - u * u;
            *outFlow = dir * (paths[i].style.current * prof * (0.7f + 0.9f * white));
        }
        if (outWhite) *outWhite = white;
    }
    return hit;
}

float RiverSystem::mineAt(std::int64_t key) const {
    const auto it = m_mine.deltas.find(key);
    return it == m_mine.deltas.end() ? 0.0f : it->second;
}

glm::vec4 RiverSystem::minePaintAt(std::int64_t key) const {
    const auto it = m_paint.weights.find(key);
    return it == m_paint.weights.end() ? glm::vec4(0.0f) : it->second;
}

void RiverSystem::forget() {
    m_mine.deltas.clear();
    m_paint.weights.clear();
    m_hasFootprint = false;
    m_carveDirty = true;
}

std::vector<RiverSystem::Audible> RiverSystem::audible(const glm::vec3& at,
                                                       int want,
                                                       float maxDist) const {
    std::vector<std::pair<float, Audible>> cand;
    for (int i = 0; i < static_cast<int>(m_runs.size()) &&
                    i < static_cast<int>(paths.size()); ++i) {
        if (!paths[i].enabled) continue;
        const rivergen::Course& c = m_runs[i].course;
        const int n = static_cast<int>(c.line.size());
        if (n < 2) continue;

        auto push = [&](const glm::vec3& p, float white, float half) {
            const float d = glm::distance(at, p);
            if (d > maxDist) return;
            // A wide river carries further than a brook, and broken water
            // further than smooth -- which is why a fall is on this list
            // separately from the stretch it belongs to.
            const float range = glm::clamp(30.0f + half * 9.0f + white * 90.0f,
                                           25.0f, 320.0f);
            const float gain = glm::clamp((0.18f + 0.90f * white) *
                                          glm::clamp(0.35f + half * 0.16f,
                                                     0.35f, 1.0f), 0.0f, 1.0f);
            if (gain < 0.02f) return;
            const float pitch = glm::clamp(1.40f - half * 0.055f, 0.72f, 1.45f);
            cand.push_back({gain * range / (range + d),
                            Audible{p, gain, pitch, range}});
        };

        // The nearest point on the course: even a calm stretch murmurs.
        float bestD2 = 1e30f, bestT = 0.0f;
        int   bestK  = -1;
        const glm::vec2 atXZ(at.x, at.z);
        for (int k = 0; k + 1 < n; ++k) {
            float t;
            const float d2 = distToSeg(atXZ, glm::vec2(c.line[k].x, c.line[k].z),
                                       glm::vec2(c.line[k + 1].x, c.line[k + 1].z), t);
            if (d2 < bestD2) { bestD2 = d2; bestT = t; bestK = k; }
        }
        if (bestK >= 0)
            push(glm::mix(c.line[bestK], c.line[bestK + 1], bestT),
                 glm::mix(c.white[bestK], c.white[bestK + 1], bestT),
                 glm::mix(c.half[bestK], c.half[bestK + 1], bestT));

        // ...and every rapid and fall, which are heard from much further off.
        for (const rivergen::SprayPoint& sp : m_runs[i].spray)
            push(sp.pos, sp.strength, sp.width / 1.6f);
    }

    std::sort(cand.begin(), cand.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });

    std::vector<Audible> out;
    for (const auto& [score, a] : cand) {
        if (static_cast<int>(out.size()) >= std::max(want, 0)) break;
        // Two voices a few metres apart are one voice and a wasted one. Spread
        // them, so a valley with a stream and a fall in it gets both.
        bool tooClose = false;
        for (const Audible& o : out)
            if (glm::distance(o.pos, a.pos) < 25.0f) { tooClose = true; break; }
        if (!tooClose) out.push_back(a);
    }
    return out;
}

int RiverSystem::totalVerts() const {
    int v = 0;
    for (const Run& r : m_runs) v += r.verts;
    return v;
}

float RiverSystem::totalLength() const {
    float l = 0.0f;
    for (const Run& r : m_runs) l += r.course.length;
    return l;
}

void RiverSystem::clear() {
    paths.clear();
    m_built.clear();
    m_runs.clear();
    m_carveDirty = true;
    // `mine` is deliberately kept: the terrain still holds this system's cut, and
    // dropping the record of it here would strand every one of those deltas.
    // The owner calls release() to give the ground back before clearing.
}

// --- Persistence -------------------------------------------------------------

namespace {

void writeStyle(nlohmann::json& j, const rivergen::Style& s) {
    j["width"]      = s.width;      j["widen"]      = s.widen;
    j["depth"]      = s.depth;      j["bedFlat"]    = s.bedFlat;
    j["bankWidth"]  = s.bankWidth;  j["bankRise"]   = s.bankRise;
    j["minSlope"]   = s.minSlope;   j["maxCut"]     = s.maxCut;
    j["smooth"]     = s.smooth;     j["autoFlow"]   = s.autoFlow;
    j["flip"]       = s.flip;       j["rapidSlope"] = s.rapidSlope;
    j["fallSlope"]  = s.fallSlope;  j["fallMin"]    = s.fallMin;
    j["poolLength"] = s.poolLength; j["plunge"]     = s.plunge;
    j["flowSpeed"]  = s.flowSpeed;  j["clarity"]    = s.clarity;
    j["reflect"]    = s.reflect;    j["rippleScale"]= s.rippleScale;
    j["ripple"]     = s.ripple;     j["foamWidth"]  = s.foamWidth;
    j["sparkle"]    = s.sparkle;
    j["current"]    = s.current;    j["seed"]       = s.seed;
    j["meander"]    = s.meander;    j["meanderLength"] = s.meanderLength;
    j["widthVary"]  = s.widthVary;  j["depthVary"]  = s.depthVary;
    j["bendScour"]  = s.bendScour;  j["bendEase"]   = s.bendEase;
    j["bankLayer"]  = s.bankLayer;  j["bankPaint"]  = s.bankPaint;
    j["bankBlend"]  = s.bankBlend;
    j["stones"]     = s.stones;     j["stoneSize"]  = s.stoneSize;
    j["stoneSpread"]= s.stoneSpread;
    j["reeds"]      = s.reeds;      j["reedHeight"] = s.reedHeight;
    j["reedDepth"]  = s.reedDepth;
    j["stoneColor"] = {s.stoneColor.x, s.stoneColor.y, s.stoneColor.z};
    j["reedColor"]  = {s.reedColor.x,  s.reedColor.y,  s.reedColor.z};
    j["shallow"] = {s.shallow.x, s.shallow.y, s.shallow.z};
    j["deep"]    = {s.deep.x,    s.deep.y,    s.deep.z};
}

// Read with a default for every field, so a scene written by an older build (or
// by a newer one that has since gained a knob) loads instead of throwing.
void readStyle(const nlohmann::json& j, rivergen::Style& s) {
    auto col = [&](const char* key, glm::vec3& out) {
        if (j.contains(key) && j[key].is_array() && j[key].size() == 3)
            out = glm::vec3(j[key][0].get<float>(), j[key][1].get<float>(),
                            j[key][2].get<float>());
    };
    s.width      = j.value("width", s.width);
    s.widen      = j.value("widen", s.widen);
    s.depth      = j.value("depth", s.depth);
    s.bedFlat    = j.value("bedFlat", s.bedFlat);
    s.bankWidth  = j.value("bankWidth", s.bankWidth);
    s.bankRise   = j.value("bankRise", s.bankRise);
    s.minSlope   = j.value("minSlope", s.minSlope);
    s.maxCut     = j.value("maxCut", s.maxCut);
    s.smooth     = j.value("smooth", s.smooth);
    s.autoFlow   = j.value("autoFlow", s.autoFlow);
    s.flip       = j.value("flip", s.flip);
    s.rapidSlope = j.value("rapidSlope", s.rapidSlope);
    s.fallSlope  = j.value("fallSlope", s.fallSlope);
    s.fallMin    = j.value("fallMin", s.fallMin);
    s.poolLength = j.value("poolLength", s.poolLength);
    s.plunge     = j.value("plunge", s.plunge);
    s.flowSpeed  = j.value("flowSpeed", s.flowSpeed);
    s.clarity    = j.value("clarity", s.clarity);
    s.reflect    = j.value("reflect", s.reflect);
    s.rippleScale= j.value("rippleScale", s.rippleScale);
    s.ripple     = j.value("ripple", s.ripple);
    s.foamWidth  = j.value("foamWidth", s.foamWidth);
    s.sparkle    = j.value("sparkle", s.sparkle);
    s.current    = j.value("current", s.current);
    s.seed       = j.value("seed", s.seed);
    s.meander      = j.value("meander", s.meander);
    s.meanderLength= j.value("meanderLength", s.meanderLength);
    s.widthVary    = j.value("widthVary", s.widthVary);
    s.depthVary    = j.value("depthVary", s.depthVary);
    s.bendScour    = j.value("bendScour", s.bendScour);
    s.bendEase     = j.value("bendEase", s.bendEase);
    s.bankLayer  = j.value("bankLayer", s.bankLayer);
    s.bankPaint  = j.value("bankPaint", s.bankPaint);
    s.bankBlend  = j.value("bankBlend", s.bankBlend);
    s.stones     = j.value("stones", s.stones);
    s.stoneSize  = j.value("stoneSize", s.stoneSize);
    s.stoneSpread= j.value("stoneSpread", s.stoneSpread);
    s.reeds      = j.value("reeds", s.reeds);
    s.reedHeight = j.value("reedHeight", s.reedHeight);
    s.reedDepth  = j.value("reedDepth", s.reedDepth);
    col("stoneColor", s.stoneColor);
    col("reedColor",  s.reedColor);
    col("shallow", s.shallow);
    col("deep",    s.deep);
}

} // namespace

void RiverSystem::save(nlohmann::json& j) const {
    nlohmann::json arr = nlohmann::json::array();
    for (const Path& p : paths) {
        nlohmann::json e;
        e["name"]    = p.name;
        e["kind"]    = static_cast<int>(p.kind);
        e["preset"]  = static_cast<int>(p.preset);
        e["enabled"] = p.enabled;
        nlohmann::json pts = nlohmann::json::array();
        for (const glm::vec2& q : p.points) pts.push_back({q.x, q.y});
        e["points"] = std::move(pts);
        nlohmann::json bias = nlohmann::json::array();
        for (std::size_t k = 0; k < p.points.size(); ++k)
            bias.push_back(k < p.bias.size() ? p.bias[k] : 0.0f);
        e["bias"] = std::move(bias);
        nlohmann::json st;
        writeStyle(st, p.style);
        e["style"] = std::move(st);
        arr.push_back(std::move(e));
    }
    j["paths"] = std::move(arr);
}

void RiverSystem::load(const nlohmann::json& j) {
    paths.clear();
    m_built.clear();
    m_runs.clear();
    if (j.contains("paths") && j["paths"].is_array()) {
        for (const auto& e : j["paths"]) {
            Path p;
            p.name    = e.value("name", std::string("Brook"));
            const int k = e.value("kind", 0);
            p.kind = static_cast<rivergen::Kind>(
                glm::clamp(k, 0, static_cast<int>(rivergen::Kind::Count) - 1));
            p.enabled = e.value("enabled", true);
            const int pr = e.value("preset", -1);
            p.preset = (pr >= 0 && pr < static_cast<int>(rivergen::Preset::Count))
                     ? static_cast<rivergen::Preset>(pr)
                     : rivergen::Preset::Brook;
            // Defaults for anything the file doesn't carry: from the named preset
            // when it names one, from the kind when it doesn't -- so a scene that
            // says "mountain stream" and then gains a field gets the mountain
            // stream's value for it rather than the plain brook's.
            p.style = (pr >= 0) ? rivergen::preset(p.preset)
                                : rivergen::preset(p.kind);
            if (e.contains("style")) readStyle(e["style"], p.style);
            if (e.contains("points"))
                for (const auto& q : e["points"])
                    if (q.is_array() && q.size() == 2)
                        p.points.emplace_back(q[0].get<float>(), q[1].get<float>());
            if (e.contains("bias"))
                for (const auto& v : e["bias"]) p.bias.push_back(v.get<float>());
            p.bias.resize(p.points.size(), 0.0f);
            paths.push_back(std::move(p));
        }
    }
    m_built.assign(paths.size(), Built{});
    m_runs.resize(paths.size());
    touch();
}
