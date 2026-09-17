#include "fitzel/world/Terrain.hpp"

#include <algorithm>
#include <cmath>

#include <stb_perlin.h>

namespace fitzel {

namespace {
// Whether the world has ground at all (see terrainPresent in the header). Read
// from worker threads while chunks build, written by the host when the scene's
// terrain object comes or goes -- hence atomic. Relaxed: nothing is published
// through it, it only answers a question.
std::atomic<bool> g_terrainPresent{true};
} // namespace

bool terrainPresent() { return g_terrainPresent.load(std::memory_order_relaxed); }
void setTerrainPresent(bool present) {
    g_terrainPresent.store(present, std::memory_order_relaxed);
}

float terrainBaseHeight(const TerrainSettings& s, float worldX, float worldZ) {
    if (!terrainPresent()) return 0.0f; // no terrain in the scene: flat void at y=0
    // Domain warp: displace the sample point by a low-frequency noise field so
    // features bend and meander instead of looking grid-aligned.
    const float wf = s.warpFrequency;
    const float wx = worldX + s.warpStrength *
        stb_perlin_fbm_noise3(worldX * wf, 0.0f, worldZ * wf, 2.0f, 0.5f, 4);
    const float wz = worldZ + s.warpStrength *
        stb_perlin_fbm_noise3((worldX + 101.7f) * wf, 0.0f,
                              (worldZ + 57.1f) * wf, 2.0f, 0.5f, 4);

    const float f  = s.frequency;
    const float bf = s.biomeFreq;

    // 1) Continents: big lowland basins (sea/lake floors) vs highlands.
    const float continent = stb_perlin_fbm_noise3(
        wx * bf + s.seed, 0.0f, wz * bf + s.seed, 2.0f, 0.5f, 4);
    const float baseElev = continent * s.heightScale * s.continentAmp;
    const float highland = glm::smoothstep(-0.15f, 0.45f, continent);

    // 2) Regional roughness: smooth plains in some regions, rugged in others.
    const float roughN = stb_perlin_fbm_noise3(
        (wx + 311.0f) * bf * 1.7f + s.seed, 0.0f,
        (wz + 117.0f) * bf * 1.7f + s.seed, 2.0f, 0.5f, 3);
    const float rough = glm::smoothstep(0.30f, 0.75f, roughN * 0.5f + 0.5f);

    // 3) Rolling hills (medium scale), stronger where it's rugged.
    const float hills = stb_perlin_fbm_noise3(
        wx * f + s.seed, 0.0f, wz * f + s.seed, s.lacunarity, s.gain, s.octaves)
        * s.heightScale * 0.5f * (0.3f + 0.7f * rough);

    // 4) Sharp ridged mountains, only on rugged highlands. peakSharpness bends the
    //    ridge profile: >1 pinches summits into sharp alpine crests, <1 rounds them
    //    into broad hills. Kept exact at 1.0 so old scenes are unchanged.
    const float ridge = stb_perlin_ridge_noise3(
        wx * f * 1.9f + s.seed, 0.0f, wz * f * 1.9f + s.seed,
        2.0f, 0.5f, 1.0f, s.octaves);
    float ridgeShaped = ridge;
    if (std::fabs(s.peakSharpness - 1.0f) > 0.001f)
        ridgeShaped = std::pow(std::max(ridge, 0.0f), s.peakSharpness);
    const float mountains = ridgeShaped * s.ridgeScale * rough * highland;

    float h = baseElev + hills + mountains;

    // 5) Plateaus / terraces in some regions (soft-stepped).
    if (s.terrace > 0.001f) {
        const float tMask = glm::smoothstep(0.55f, 0.85f,
            stb_perlin_fbm_noise3((wx - 733.0f) * bf * 1.3f, 0.0f,
                                  (wz + 401.0f) * bf * 1.3f, 2.0f, 0.5f, 3) * 0.5f + 0.5f);
        const float step = 5.0f;
        const float q    = std::floor(h / step) * step;
        const float frac = (h - q) / step;
        const float soft = q + step * glm::smoothstep(0.35f, 0.65f, frac);
        h = glm::mix(h, soft, tMask * s.terrace);
    }

    // 6) Valleys / canyons: carve meandering channels that follow a low-frequency
    //    ridged "river network" -- deepest along the channel centre-lines, tapering
    //    to nothing at the banks. Biased toward the lowlands (weaker on highlands)
    //    so mountain country keeps its ridges while basins gain river gorges.
    if (s.valleyDepth > 0.001f) {
        const float rv = stb_perlin_ridge_noise3(
            (wx + 512.0f) * f * 0.6f + s.seed, 0.0f,
            (wz - 377.0f) * f * 0.6f + s.seed, 2.0f, 0.5f, 1.0f, 4);
        const float channel = glm::smoothstep(0.55f, 0.95f, rv);
        h -= channel * s.valleyDepth * (1.2f - 0.6f * highland);
    }

    // 7) Master relief exaggeration: scale the whole silhouette for a more dramatic,
    //    "epic" vertical range without having to re-tune every other knob.
    h *= s.reliefGain;

    // 8) Island mask: turn the infinite field into a bounded landmass ringed by
    //    water. No-op at islandRadius 0 (the default), so ordinary worlds are
    //    untouched. `t` runs 0 at the island centre to 1 at the nominal coast.
    if (s.islandRadius > 0.001f) {
        const float dx = worldX - s.islandCenterX;
        const float dz = worldZ - s.islandCenterZ;
        const float t  = std::sqrt(dx * dx + dz * dz) / s.islandRadius;
        if (s.islandShape < 0.5f) {
            // Solid island: keep the full interior relief, then ramp the coast down
            // into a deep shelf so the shore falls away into open water.
            const float land  = glm::smoothstep(1.0f, 0.55f, t); // 1 inland .. 0 offshore
            const float shelf = -30.0f;                          // sea-floor depth
            h = h * land + 4.0f * land + shelf * (1.0f - land);  // +4 lifts land clear of the sea
        } else {
            // Atoll: a low reef ring (~0.72 of the radius) around a shallow lagoon,
            // open ocean beyond. Riding the ring on the interior noise breaks it
            // into separate motus/islets instead of a solid circular wall.
            const float ringPos = 0.72f, ringW = 0.15f;
            const float d        = (t - ringPos) / ringW;
            const float ring     = std::exp(-0.5f * d * d);       // gaussian bump 0..1
            const float floorMix = glm::smoothstep(ringPos, 1.2f, t);
            const float seafloor = glm::mix(-5.0f, -32.0f, floorMix); // lagoon -> ocean
            const float reef     = ring * (7.0f + 0.35f * h);
            h = seafloor + reef;
        }
    }

    // 9) The mountain backdrop, last and additive: it must not be scaled by the
    //    relief gain nor sunk by the island mask, because it is measured in
    //    metres the author typed, not in units of the valley's own relief.
    return h + terrainBackdrop(s, worldX, worldZ);
}

float terrainBackdrop(const TerrainSettings& s, float worldX, float worldZ) {
    if (s.backdropHeight <= 0.0f || !terrainPresent()) return 0.0f;

    // Into the valley's own frame: u along its axis, v across it. Dividing v by
    // the stretch makes the ellipse a circle, so everything below can think in
    // plain radii.
    const float a  = glm::radians(s.backdropAngle);
    const float ca = std::cos(a), sa = std::sin(a);
    const float dx = worldX - s.backdropCenterX;
    const float dz = worldZ - s.backdropCenterZ;
    const float u  = dx * ca + dz * sa;
    const float v  = (-dx * sa + dz * ca) / std::max(s.backdropStretch, 0.05f);
    const float r  = std::sqrt(u * u + v * v);

    // Where the valley ends. Sampled on a circle in noise space so the rim
    // closes on itself without a seam at +-180 degrees, and wandering by a third
    // of the radius: spurs reach into the valley, bays reach out of it.
    const float R   = std::max(s.backdropRadius, 1.0f);
    const float ang = std::atan2(v, u);
    const float rimN = stb_perlin_fbm_noise3(std::cos(ang) * 1.7f + 17.3f,
                                             std::sin(ang) * 1.7f - 4.1f, 0.37f,
                                             2.0f, 0.5f, 3);
    const float rim = R * (1.0f + 0.33f * rimN);
    if (r <= rim) return 0.0f;           // the valley: not a millimetre changed
    // How far out of the valley, in METRES: the frame above squeezed the
    // across-axis by the stretch, and a slope measured in its units would come
    // out that much steeper across the valley than along it -- a gentle end
    // wall and cliffs for sides.
    const float str  = std::max(s.backdropStretch, 0.05f);
    const float ssin = std::sin(ang) * str, scos = std::cos(ang);
    float dist = (r - rim) * std::sqrt(scos * scos + ssin * ssin);

    // The outlet: the valley running on along its axis, bending as a river
    // valley does, its floor narrowing downstream until the ranges close it.
    // Distance to it is measured across (in the stretched frame, like the rim),
    // so the mountains stand along both its sides.
    float lake = 0.0f;
    if (s.backdropOutlet > 0.0f && u > 0.0f) {
        const float Lo = s.backdropOutlet;
        const float k  = u / Lo;                                   // 0 .. 1 down the valley
        const float bend = R * 1.1f * stb_perlin_fbm_noise3(u / (R * 2.6f) + 5.7f, 1.9f, 3.3f,
                                                          2.0f, 0.5f, 2)
                         * glm::smoothstep(0.0f, 0.25f, k);
        const float hw = R * glm::mix(0.9f, 0.5f, glm::clamp(k, 0.0f, 1.0f))
                       * (1.0f - glm::smoothstep(0.85f, 1.0f, k))
                       * (1.0f + 0.25f * stb_perlin_noise3(u / (R * 0.9f), 8.8f, 0.5f, 0, 0, 0));
        const float across = std::abs(v - bend);
        dist = std::min(dist, (across - hw) * str);
        // The lake basin, a third of the way down: deepest in the middle of
        // the floor, shelving out to the shores.
        if (s.backdropLake > 0.0f && hw > 1.0f) {
            const float along = glm::smoothstep(0.26f, 0.34f, k) * (1.0f - glm::smoothstep(0.50f, 0.60f, k));
            const float bowl  = 1.0f - glm::smoothstep(0.15f, 0.95f, across / hw);
            lake = s.backdropLake * along * bowl;
        }
    }
    if (dist <= 0.0f) return -lake;      // on the valley floor (or under its lake)

    const float W = std::max(s.backdropWidth, 1.0f);
    const float t = dist / W;            // 0 at the valley's edge, 1 where the massif starts

    // The ranges themselves, in world space (not the valley frame, which would
    // print the ellipse into every ridge). Domain-warped so crests bend and
    // branch; a ridged multifractal because that is what gives knife-edge
    // aretes above smooth, broad valleys -- the one silhouette that reads as
    // "high mountains" from twenty kilometres away.
    const float S  = std::max(s.backdropScale, 10.0f);
    const float px = worldX / S, pz = worldZ / S;
    const float qx = px + 0.45f * stb_perlin_fbm_noise3(px * 0.8f + 3.1f, 5.5f,
                                                        pz * 0.8f - 1.7f, 2.0f, 0.5f, 3);
    const float qz = pz + 0.45f * stb_perlin_fbm_noise3(px * 0.8f - 7.9f, 9.1f,
                                                        pz * 0.8f + 5.3f, 2.0f, 0.5f, 3);
    const float ridged = stb_perlin_ridge_noise3(qx, 13.7f, qz, 2.0f, 0.5f, 1.0f, 8);

    // Some massifs tower over their neighbours; a range of equal peaks is the
    // tell-tale of noise.
    const float massif = glm::clamp(
        0.55f + 0.65f * stb_perlin_fbm_noise3(px * 0.33f + 41.0f, 2.2f,
                                              pz * 0.33f - 12.0f, 2.0f, 0.5f, 3),
        0.12f, 1.25f);

    // Two ranges, one behind the other. In front, the valley's own flanks:
    // they rise straight out of the floor to a third of the height, broad and
    // rounded enough to carry forest to their crests. Behind them, the high
    // range -- rock, ice, knife-edges -- stepped back so that from the valley
    // it stands OVER its foothills rather than being them. Between the two a
    // saddle, so a ridge reads against the one behind it instead of merging.
    const float flank  = glm::smoothstep(0.0f, 0.42f, t);
    // The flanks' crest is not a wall: summits and cols along it, spurs that
    // run down into the valley between side valleys (the ridged noise again,
    // at twice the frequency and gentler), so it carries forest and still has
    // a skyline of its own.
    const float spur   = stb_perlin_ridge_noise3(qx * 2.3f + 4.0f, 2.9f, qz * 2.3f - 6.0f,
                                                 2.0f, 0.5f, 1.0f, 5);
    const float flankR = 0.35f + 0.75f * glm::clamp(spur, 0.0f, 1.2f)
                       + 0.20f * stb_perlin_fbm_noise3(px * 2.1f + 9.0f, 4.4f,
                                                       pz * 2.1f - 2.0f, 2.0f, 0.5f, 4);
    const float saddle = 1.0f - 0.35f * glm::smoothstep(0.35f, 0.55f, t)
                                      * (1.0f - glm::smoothstep(0.55f, 0.85f, t));
    const float peaks  = glm::smoothstep(0.45f, 1.5f, t);
    const float relief = std::pow(std::max(ridged, 0.0f), 1.6f) * 1.3f;
    const float front  = 0.34f * flank * flankR * saddle;
    const float high   = peaks * (0.20f + 0.85f * massif) * relief;
    return s.backdropHeight * (0.06f * flank + front + high) - lake;
}

float terrainMoisture(const TerrainSettings& s, float worldX, float worldZ) {
    const float mf = s.biomeFreq * 0.8f;
    const float m = stb_perlin_fbm_noise3(
        worldX * mf + 137.0f + s.seed, 0.0f, worldZ * mf + 91.0f + s.seed,
        2.0f, 0.5f, 3);
    return glm::clamp(m * 0.5f + 0.5f, 0.0f, 1.0f);
}

// --- Editable deformation layer -------------------------------------------

std::int64_t TerrainEditField::cellKey(int ix, int iz) {
    return (static_cast<std::int64_t>(ix) << 32) ^
           (static_cast<std::uint32_t>(iz));
}

float TerrainEditField::sample(float worldX, float worldZ) const {
    if (deltas.empty()) return 0.0f;
    const float gx = worldX / cell, gz = worldZ / cell;
    const int   ix = static_cast<int>(std::floor(gx));
    const int   iz = static_cast<int>(std::floor(gz));
    const float fx = gx - ix, fz = gz - iz;
    auto at = [&](int x, int z) -> float {
        const auto it = deltas.find(cellKey(x, z));
        return it == deltas.end() ? 0.0f : it->second;
    };
    const float h00 = at(ix,     iz),     h10 = at(ix + 1, iz);
    const float h01 = at(ix,     iz + 1), h11 = at(ix + 1, iz + 1);
    return glm::mix(glm::mix(h00, h10, fx), glm::mix(h01, h11, fx), fz);
}

// Iterate the grid cells whose centre lies inside the brush disc, invoking
// `fn(ix, iz, worldX, worldZ, weight)` with a smooth 1-at-centre..0-at-rim
// weight. Shared by all three sculpt ops.
template <class Fn>
static void forBrushCells(float cell, glm::vec2 c, float radius, Fn&& fn) {
    if (radius <= 0.0f) return;
    const int x0 = static_cast<int>(std::floor((c.x - radius) / cell));
    const int x1 = static_cast<int>(std::ceil ((c.x + radius) / cell));
    const int z0 = static_cast<int>(std::floor((c.y - radius) / cell));
    const int z1 = static_cast<int>(std::ceil ((c.y + radius) / cell));
    const float r2 = radius * radius;
    for (int iz = z0; iz <= z1; ++iz) {
        for (int ix = x0; ix <= x1; ++ix) {
            const float wx = ix * cell, wz = iz * cell;
            const float d2 = (wx - c.x) * (wx - c.x) + (wz - c.y) * (wz - c.y);
            if (d2 > r2) continue;
            const float t = 1.0f - std::sqrt(d2) / radius; // 1 centre -> 0 rim
            fn(ix, iz, wx, wz, t * t * (3.0f - 2.0f * t));  // smoothstep weight
        }
    }
}

void TerrainEditField::raise(glm::vec2 c, float radius, float amount) {
    forBrushCells(cell, c, radius, [&](int ix, int iz, float, float, float w) {
        deltas[cellKey(ix, iz)] += amount * w;
    });
}

void TerrainEditField::pull(glm::vec2 c, float radius, float amount,
                            float falloff) {
    if (amount == 0.0f) return;
    const float f = glm::clamp(falloff, 0.5f, 6.0f);
    forBrushCells(cell, c, radius, [&](int ix, int iz, float, float, float w) {
        // The brush weight is already the smooth bell; the exponent bends it.
        // Raising the WEIGHT rather than remapping the distance keeps 1 at the
        // centre and 0 at the rim for every exponent, so changing the shape never
        // changes how far the edit reaches.
        const float g = (f == 1.0f) ? w : std::pow(w, f);
        if (g > 0.0f) deltas[cellKey(ix, iz)] += amount * g;
    });
}

void TerrainEditField::flatten(const TerrainSettings& s, glm::vec2 c,
                               float radius, float amount, float target) {
    forBrushCells(cell, c, radius, [&](int ix, int iz, float wx, float wz, float w) {
        const std::int64_t k = cellKey(ix, iz);
        const auto  it  = deltas.find(k);
        const float cur = (it == deltas.end()) ? 0.0f : it->second;
        const float combined = terrainBaseHeight(s, wx, wz) + cur;
        // Blend the surface toward the target height; store the resulting offset.
        deltas[k] = cur + (target - combined) * glm::clamp(amount * w, 0.0f, 1.0f);
    });
}

void TerrainEditField::smooth(const TerrainSettings& s, glm::vec2 c,
                              float radius, float amount) {
    // Buffer the new offsets so cells within one dab don't smear into each other
    // (every cell reads the pre-dab surface for its neighbourhood average).
    struct Upd { std::int64_t k; float v; };
    std::vector<Upd> ups;
    forBrushCells(cell, c, radius, [&](int ix, int iz, float wx, float wz, float w) {
        float sum = 0.0f; int n = 0;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const float nx = (ix + dx) * cell, nz = (iz + dz) * cell;
                sum += terrainBaseHeight(s, nx, nz) + sample(nx, nz);
                ++n;
            }
        const float base     = terrainBaseHeight(s, wx, wz);
        const float combined = base + sample(wx, wz);
        const float target   = glm::mix(combined, sum / n,
                                        glm::clamp(amount * w, 0.0f, 1.0f));
        ups.push_back({cellKey(ix, iz), target - base});
    });
    for (const Upd& u : ups) deltas[u.k] = u.v;
}

void TerrainEditField::carve(const TerrainSettings& s, glm::vec2 c, float radius,
                             float rate, float depth) {
    if (radius <= 0.0f) return;
    // Each cell aims at `depth` below (or above, if negative) the *procedural base*
    // at that cell -- a fixed per-cell target, so holding the brush converges to a
    // clean valley floor at that depth instead of sinking without bound, while a
    // drag still follows the terrain because every cell tracks its own base height.
    const bool digging = depth > 0.0f;
    forBrushCells(cell, c, radius, [&](int ix, int iz, float wx, float wz, float w) {
        const std::int64_t k      = cellKey(ix, iz);
        const auto         it     = deltas.find(k);
        const float        cur    = (it == deltas.end()) ? 0.0f : it->second;
        const float        base   = terrainBaseHeight(s, wx, wz);
        const float        target = base - depth;      // -depth => valley, +|d| => crest
        const float        combined = base + cur;
        // One-sided: digging only lowers, raising only lifts -- so overlapping dabs
        // deepen a valley (or build a crest) instead of fighting each other.
        if (digging ? (combined <= target) : (combined >= target)) return;
        const float want = glm::mix(combined, target, glm::clamp(rate * w, 0.0f, 1.0f));
        deltas[k] = cur + (want - combined);
    });
}

void TerrainEditField::erode(const TerrainSettings& s, glm::vec2 c, float radius,
                             float rate, int iterations) {
    if (radius <= 0.0f) return;
    // Work on a dense local grid covering the disc plus a one-cell apron (so the
    // interior cells can read all four neighbours). Sampling the procedural base
    // once here keeps the inner relaxation loop pure array arithmetic.
    const int x0 = static_cast<int>(std::floor((c.x - radius) / cell)) - 1;
    const int x1 = static_cast<int>(std::ceil ((c.x + radius) / cell)) + 1;
    const int z0 = static_cast<int>(std::floor((c.y - radius) / cell)) - 1;
    const int z1 = static_cast<int>(std::ceil ((c.y + radius) / cell)) + 1;
    const int W = x1 - x0 + 1, H = z1 - z0 + 1;
    if (W <= 2 || H <= 2) return;
    auto idx = [&](int x, int z) { return (z - z0) * W + (x - x0); };

    std::vector<float> h(static_cast<std::size_t>(W) * H);
    std::vector<float> base(static_cast<std::size_t>(W) * H);
    for (int iz = z0; iz <= z1; ++iz)
        for (int ix = x0; ix <= x1; ++ix) {
            const float b  = terrainBaseHeight(s, ix * cell, iz * cell);
            const auto  it = deltas.find(cellKey(ix, iz));
            base[idx(ix, iz)] = b;
            h[idx(ix, iz)]    = b + (it == deltas.end() ? 0.0f : it->second);
        }

    // Talus: the max stable height step between neighbouring cells. Excess above
    // it is shed downhill, split across the lower neighbours by how steep each is.
    const float talus = 0.7f * cell;
    const float carry = 0.5f * glm::clamp(rate, 0.0f, 1.0f);
    std::vector<float> move(static_cast<std::size_t>(W) * H);
    for (int it = 0; it < iterations; ++it) {
        std::fill(move.begin(), move.end(), 0.0f);
        for (int iz = z0 + 1; iz < z1; ++iz)
            for (int ix = x0 + 1; ix < x1; ++ix) {
                const int   i  = idx(ix, iz);
                const float hi = h[i];
                const int   nb[4] = {idx(ix - 1, iz), idx(ix + 1, iz),
                                     idx(ix, iz - 1), idx(ix, iz + 1)};
                float d[4], dsum = 0.0f, dmax = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    d[k] = hi - h[nb[k]] - talus;
                    if (d[k] < 0.0f) d[k] = 0.0f;
                    dsum += d[k];
                    if (d[k] > dmax) dmax = d[k];
                }
                if (dsum <= 0.0f) continue;
                const float shed = carry * dmax; // material leaving this cell
                move[i] -= shed;
                for (int k = 0; k < 4; ++k)
                    move[nb[k]] += shed * (d[k] / dsum);
            }
        for (std::size_t n = 0; n < h.size(); ++n) h[n] += move[n];
    }

    // Write the eroded surface back as deltas, faded by the brush falloff so the
    // disc rim blends into the untouched ground.
    const float r2 = radius * radius;
    for (int iz = z0 + 1; iz < z1; ++iz)
        for (int ix = x0 + 1; ix < x1; ++ix) {
            const float wx = ix * cell, wz = iz * cell;
            const float d2 = (wx - c.x) * (wx - c.x) + (wz - c.y) * (wz - c.y);
            if (d2 > r2) continue;
            const float t = 1.0f - std::sqrt(d2) / radius;
            const float w = t * t * (3.0f - 2.0f * t);
            const int          i   = idx(ix, iz);
            const std::int64_t k   = cellKey(ix, iz);
            const auto         itc = deltas.find(k);
            const float        cur = (itc == deltas.end()) ? 0.0f : itc->second;
            deltas[k] = glm::mix(cur, h[i] - base[i], w);
        }
}

float TerrainEditField::rain(const TerrainSettings& s, glm::vec2 c, float radius,
                             int droplets, std::uint32_t seed) {
    if (radius <= 0.0f || droplets <= 0) return 0.0f;
    // Drops carry their load past the rim, so the working grid gets room beyond
    // the disc and the edit fades out across that margin.
    const float reach = radius + std::max(8.0f * cell, radius);
    const int x0 = static_cast<int>(std::floor((c.x - reach) / cell));
    const int x1 = static_cast<int>(std::ceil ((c.x + reach) / cell));
    const int z0 = static_cast<int>(std::floor((c.y - reach) / cell));
    const int z1 = static_cast<int>(std::ceil ((c.y + reach) / cell));
    const int W = x1 - x0 + 1, H = z1 - z0 + 1;
    if (W < 8 || H < 8) return 0.0f;

    // The droplet constants come from the classic particle-erosion model, which
    // is tuned for a field whose height unit spans about 60 cells. The model does
    // not change if height and spacing scale together, so heights are measured
    // in that unit here and converted back on the way out.
    const float unit = 60.0f * cell, toUnit = 1.0f / unit;
    std::vector<float> h(static_cast<std::size_t>(W) * H);
    for (int iz = z0; iz <= z1; ++iz)
        for (int ix = x0; ix <= x1; ++ix) {
            const auto it = deltas.find(cellKey(ix, iz));
            h[(iz - z0) * W + (ix - x0)] =
                (terrainBaseHeight(s, ix * cell, iz * cell) +
                 (it == deltas.end() ? 0.0f : it->second)) * toUnit;
        }
    const std::vector<float> before = h;

    // Erosion takes material from a small disc around the drop, not one cell, so
    // it cuts channels instead of pits.
    constexpr int BR = 3;
    int   bOff[(2 * BR + 1) * (2 * BR + 1)];
    float bW  [(2 * BR + 1) * (2 * BR + 1)];
    int   bN = 0;
    {
        float sum = 0.0f;
        for (int dz = -BR; dz <= BR; ++dz)
            for (int dx = -BR; dx <= BR; ++dx) {
                const float d = std::sqrt(static_cast<float>(dx * dx + dz * dz));
                if (d >= BR) continue;
                bOff[bN] = dz * W + dx;
                bW[bN]   = 1.0f - d / BR;
                sum += bW[bN++];
            }
        for (int k = 0; k < bN; ++k) bW[k] /= sum;
    }

    // More inertia and a longer life than the textbook values: on metre-scale
    // ground a drop that follows every bump drowns in the first hollow and the
    // brush just smooths. Carried over the bumps, the drops gather into rills.
    constexpr float inertia = 0.3f, capacityK = 4.0f, minCapacity = 0.01f;
    constexpr float depositK = 0.3f, erodeK = 0.3f, gravity = 4.0f, evaporate = 0.02f;
    constexpr int   lifetime = 64;

    std::uint32_t rng = seed * 2654435761u + 0x9E3779B9u;
    auto rnd = [&rng] {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        return (rng >> 8) * (1.0f / 16777216.0f);
    };

    for (int n = 0; n < droplets; ++n) {
        const float a = rnd() * 6.2831853f, rr = radius * std::sqrt(rnd());
        float px = (c.x + std::cos(a) * rr) / cell - x0;
        float pz = (c.y + std::sin(a) * rr) / cell - z0;
        float dx = 0.0f, dz = 0.0f, speed = 1.0f, water = 1.0f, sediment = 0.0f;
        for (int life = 0; ; ++life) {
            if (px < 0.0f || pz < 0.0f || px >= W - 1 || pz >= H - 1) break;
            const int   nx = static_cast<int>(px), nz = static_cast<int>(pz);
            const int   ci = nz * W + nx;
            const float fx = px - nx, fz = pz - nz;
            // A drop that comes to rest leaves its load where it soaked in --
            // the silt fans at the foot of a slope are exactly that. (One that
            // runs off the working grid takes it along; the edit fades out
            // there anyway.)
            // Spread over the same small disc erosion takes from, or every
            // resting drop leaves a pimple.
            auto settle = [&] {
                if (nx >= BR && nz >= BR && nx < W - BR && nz < H - BR) {
                    for (int k = 0; k < bN; ++k) h[ci + bOff[k]] += sediment * bW[k];
                } else {
                    h[ci]         += sediment * (1 - fx) * (1 - fz);
                    h[ci + 1]     += sediment * fx * (1 - fz);
                    h[ci + W]     += sediment * (1 - fx) * fz;
                    h[ci + W + 1] += sediment * fx * fz;
                }
            };
            if (life == lifetime) { settle(); break; }
            const float hNW = h[ci], hNE = h[ci + 1], hSW = h[ci + W], hSE = h[ci + W + 1];
            const float gx = (hNE - hNW) * (1 - fz) + (hSE - hSW) * fz;
            const float gz = (hSW - hNW) * (1 - fx) + (hSE - hNE) * fx;
            const float here = hNW * (1 - fx) * (1 - fz) + hNE * fx * (1 - fz) +
                               hSW * (1 - fx) * fz + hSE * fx * fz;

            dx = dx * inertia - gx * (1 - inertia);
            dz = dz * inertia - gz * (1 - inertia);
            const float len = std::sqrt(dx * dx + dz * dz);
            if (len < 1e-10f) { settle(); break; } // flat ground: the drop soaks in
            dx /= len; dz /= len;
            px += dx; pz += dz;
            if (px < 0.0f || pz < 0.0f || px >= W - 1 || pz >= H - 1) break;

            const int   mx = static_cast<int>(px), mz = static_cast<int>(pz);
            const int   mi = mz * W + mx;
            const float qx = px - mx, qz = pz - mz;
            const float next = h[mi] * (1 - qx) * (1 - qz) + h[mi + 1] * qx * (1 - qz) +
                               h[mi + W] * (1 - qx) * qz + h[mi + W + 1] * qx * qz;
            const float dh = next - here;

            const float capacity = std::max(-dh * speed * water * capacityK, minCapacity);
            if (sediment > capacity || dh > 0.0f) {
                // Uphill it fills the hollow it came from; otherwise it sheds the
                // part of its load the slowing water can no longer carry.
                const float drop = dh > 0.0f ? std::min(dh, sediment)
                                             : (sediment - capacity) * depositK;
                sediment -= drop;
                h[ci]         += drop * (1 - fx) * (1 - fz);
                h[ci + 1]     += drop * fx * (1 - fz);
                h[ci + W]     += drop * (1 - fx) * fz;
                h[ci + W + 1] += drop * fx * fz;
            } else if (nx >= BR && nz >= BR && nx < W - BR && nz < H - BR) {
                // Never take more than the drop just fell, or it digs a pit
                // below where it is going.
                const float take = std::min((capacity - sediment) * erodeK, -dh);
                for (int k = 0; k < bN; ++k) {
                    const int   i = ci + bOff[k];
                    const float t = std::min(h[i], take * bW[k]);
                    h[i] -= t;
                    sediment += t;
                }
            }
            speed = std::sqrt(std::max(0.0f, speed * speed - dh * gravity));
            water *= 1.0f - evaporate;
        }
    }

    // Hand back only what changed, full strength out to halfway into the margin
    // (where the silt the drops carried off the disc lands) and fading to
    // nothing at the reach, so the gullies run out instead of stopping at a wall.
    for (int iz = z0; iz <= z1; ++iz)
        for (int ix = x0; ix <= x1; ++ix) {
            const int   i      = (iz - z0) * W + (ix - x0);
            const float change = h[i] - before[i];
            if (change == 0.0f) continue;
            const float wx = ix * cell, wz = iz * cell;
            const float d  = std::sqrt((wx - c.x) * (wx - c.x) + (wz - c.y) * (wz - c.y));
            const float fade = radius + 0.5f * (reach - radius);
            const float t  = glm::clamp((reach - d) / (reach - fade), 0.0f, 1.0f);
            const float w  = t * t * (3.0f - 2.0f * t);
            if (w <= 0.0f) continue;
            deltas[cellKey(ix, iz)] += change * unit * w;
        }
    return reach;
}

// Procedural stamp profiles, evaluated in normalised, rotated disc coordinates
// (u, v) in [-1, 1]. Returns the height weight (0 outside the shape's support;
// the crater is signed: a raised rim around a sunken floor).
static float stampProfile(int shape, float u, float v) {
    const float r = std::sqrt(u * u + v * v);
    switch (shape) {
        case 1: // cone
            return (r >= 1.0f) ? 0.0f : (1.0f - r);
        case 2: { // plateau / mesa: flat top, soft skirt
            if (r >= 1.0f) return 0.0f;
            return 1.0f - glm::smoothstep(0.55f, 1.0f, r);
        }
        case 3: { // crater: gaussian rim minus a bowl
            if (r >= 1.1f) return 0.0f;
            const float rim  = std::exp(-((r - 0.75f) * (r - 0.75f)) /
                                        (2.0f * 0.09f * 0.09f));
            const float bowl = (r < 0.75f) ? (1.0f - r / 0.75f) : 0.0f;
            return rim * 0.8f - bowl;
        }
        case 4: { // ridge: elongated along u, falls off across v
            if (std::fabs(u) >= 1.0f || std::fabs(v) >= 1.0f) return 0.0f;
            const float along  = 1.0f - glm::smoothstep(0.6f, 1.0f, std::fabs(u));
            float       across = glm::clamp(1.0f - std::fabs(v), 0.0f, 1.0f);
            across = across * across * (3.0f - 2.0f * across);
            return along * across;
        }
        case 5: { // mountain range: a rugged crest of several summits along u
            if (std::fabs(u) >= 1.0f || std::fabs(v) >= 1.0f) return 0.0f;
            // Two incommensurate waves make distinct peaks and saddles, never a
            // clean sine; a floor keeps the ridge continuous between summits.
            float crest = 0.5f + 0.5f * std::sin(u * 6.3f + 0.7f) * std::cos(u * 2.7f);
            crest = 0.35f + 0.65f * glm::clamp(crest, 0.0f, 1.0f);
            const float along  = 1.0f - glm::smoothstep(0.8f, 1.0f, std::fabs(u));
            float       across = glm::clamp(1.0f - std::fabs(v), 0.0f, 1.0f);
            across = across * across * (3.0f - 2.0f * across); // smooth foothill skirts
            return crest * along * across;
        }
        default: { // dome: smooth bell
            if (r >= 1.0f) return 0.0f;
            const float t = 1.0f - r;
            return t * t * (3.0f - 2.0f * t);
        }
    }
}

void TerrainEditField::stamp(glm::vec2 c, float radius, float height, int shape,
                             float rotation) {
    if (radius <= 0.0f) return;
    const float cs = std::cos(rotation), sn = std::sin(rotation);
    // Cover the bounding square (the ridge/plateau reach into the corners).
    const int x0 = static_cast<int>(std::floor((c.x - radius) / cell));
    const int x1 = static_cast<int>(std::ceil ((c.x + radius) / cell));
    const int z0 = static_cast<int>(std::floor((c.y - radius) / cell));
    const int z1 = static_cast<int>(std::ceil ((c.y + radius) / cell));
    for (int iz = z0; iz <= z1; ++iz)
        for (int ix = x0; ix <= x1; ++ix) {
            const float dx = (ix * cell - c.x) / radius;
            const float dz = (iz * cell - c.y) / radius;
            const float u  = dx * cs - dz * sn; // rotate into stamp space
            const float v  = dx * sn + dz * cs;
            const float val = stampProfile(shape, u, v);
            if (val == 0.0f) continue;
            deltas[cellKey(ix, iz)] += height * val;
        }
}

void TerrainEditField::roughen(glm::vec2 c, float radius, float amount,
                               float frequency, float seed) {
    forBrushCells(cell, c, radius, [&](int ix, int iz, float wx, float wz, float w) {
        const float n = stb_perlin_fbm_noise3((wx + seed) * frequency, seed * 0.7f,
                                              (wz + seed) * frequency, 2.0f, 0.5f, 4);
        deltas[cellKey(ix, iz)] += amount * w * n;
    });
}

// --- Global deformation snapshot ------------------------------------------

namespace {
std::mutex                                g_editMutex;
std::shared_ptr<const TerrainEditField>   g_edits;
} // namespace

std::shared_ptr<const TerrainEditField> terrainEditSnapshot() {
    std::lock_guard<std::mutex> lock(g_editMutex);
    return g_edits;
}

void setTerrainEditSnapshot(std::shared_ptr<const TerrainEditField> field) {
    std::lock_guard<std::mutex> lock(g_editMutex);
    g_edits = std::move(field);
}

float terrainHeight(const TerrainSettings& s, float worldX, float worldZ) {
    // Without ground the sculpted deltas have nothing to sit on either -- a road's
    // graded corridor must not survive as a floating ribbon of edits.
    if (!terrainPresent()) return 0.0f;
    float h = terrainBaseHeight(s, worldX, worldZ);
    if (const auto e = terrainEditSnapshot()) h += e->sample(worldX, worldZ);
    return h;
}

// --- Editable texture-paint layer -----------------------------------------

glm::vec4 TerrainPaintField::sample(float worldX, float worldZ) const {
    if (weights.empty()) return glm::vec4(0.0f);
    const float gx = worldX / cell, gz = worldZ / cell;
    const int   ix = static_cast<int>(std::floor(gx));
    const int   iz = static_cast<int>(std::floor(gz));
    const float fx = gx - ix, fz = gz - iz;
    auto at = [&](int x, int z) -> glm::vec4 {
        const auto it = weights.find(TerrainEditField::cellKey(x, z));
        return it == weights.end() ? glm::vec4(0.0f) : it->second;
    };
    const glm::vec4 w00 = at(ix,     iz),     w10 = at(ix + 1, iz);
    const glm::vec4 w01 = at(ix,     iz + 1), w11 = at(ix + 1, iz + 1);
    return glm::mix(glm::mix(w00, w10, fx), glm::mix(w01, w11, fx), fz);
}

void TerrainPaintField::paint(glm::vec2 c, float radius, int layer, float amount) {
    if (layer < 0 || layer > 3) return;
    forBrushCells(cell, c, radius, [&](int ix, int iz, float, float, float wgt) {
        glm::vec4&  p = weights[TerrainEditField::cellKey(ix, iz)];
        const float a = glm::clamp(amount * wgt, 0.0f, 1.0f);
        // Raise the painted layer toward 1, fade the others toward 0, so a stroke
        // converges to "this layer only" and overlapping dabs don't overshoot.
        for (int k = 0; k < 4; ++k)
            p[k] = (k == layer) ? p[k] + (1.0f - p[k]) * a : p[k] * (1.0f - a);
    });
}

void TerrainPaintField::erase(glm::vec2 c, float radius, float amount) {
    forBrushCells(cell, c, radius, [&](int ix, int iz, float, float, float wgt) {
        const auto it = weights.find(TerrainEditField::cellKey(ix, iz));
        if (it == weights.end()) return;
        it->second *= (1.0f - glm::clamp(amount * wgt, 0.0f, 1.0f));
    });
}

namespace {
std::mutex                                g_paintMutex;
std::shared_ptr<const TerrainPaintField>  g_paints;
} // namespace

std::shared_ptr<const TerrainPaintField> terrainPaintSnapshot() {
    std::lock_guard<std::mutex> lock(g_paintMutex);
    return g_paints;
}

void setTerrainPaintSnapshot(std::shared_ptr<const TerrainPaintField> field) {
    std::lock_guard<std::mutex> lock(g_paintMutex);
    g_paints = std::move(field);
}

MeshData TerrainChunk::buildMeshData(const TerrainSettings& s, glm::ivec2 coord) {
    MeshData data;

    const int   verts   = std::max(2, s.resolution + 1);
    const float step    = s.chunkSize / static_cast<float>(s.resolution);
    const float originX = coord.x * s.chunkSize;
    const float originZ = coord.y * s.chunkSize;

    // Grab the edit snapshot once for the whole chunk (avoids a mutex-guarded
    // shared_ptr load per vertex); sample base noise + this chunk's edits.
    const std::shared_ptr<const TerrainEditField>  edits  = terrainEditSnapshot();
    const std::shared_ptr<const TerrainPaintField> paints = terrainPaintSnapshot();
    auto height = [&](float wx, float wz) {
        float h = terrainBaseHeight(s, wx, wz);
        if (edits) h += edits->sample(wx, wz);
        return h;
    };

    // Every height once, on the grid plus a one-sample apron, and the normals
    // read their neighbours out of it. The same central differences as before
    // (same points, same step -- still continuous across chunks), at a fifth of
    // the height evaluations: asking for four neighbours per vertex evaluated
    // each interior sample five times, which is most of what a chunk costs, and
    // far more of it once the mountain backdrop makes a sample expensive.
    const int apron = verts + 2;
    std::vector<float> hgrid(static_cast<std::size_t>(apron) * apron);
    for (int z = 0; z < apron; ++z)
        for (int x = 0; x < apron; ++x)
            hgrid[static_cast<std::size_t>(z) * apron + x] =
                height(originX + (x - 1) * step, originZ + (z - 1) * step);
    const auto H = [&](int x, int z) {   // grid coordinates, -1..verts
        return hgrid[static_cast<std::size_t>(z + 1) * apron + (x + 1)];
    };

    data.vertices.reserve(static_cast<std::size_t>(verts) * verts);
    for (int z = 0; z < verts; ++z) {
        for (int x = 0; x < verts; ++x) {
            const float wx = originX + x * step;
            const float wz = originZ + z * step;

            // Central differences in world space (continuous across chunks).
            const float hl = H(x - 1, z);
            const float hr = H(x + 1, z);
            const float hd = H(x, z - 1);
            const float hu = H(x, z + 1);
            const glm::vec3 normal =
                glm::normalize(glm::vec3(hl - hr, 2.0f * step, hd - hu));

            Vertex v;
            v.position = {wx, H(x, z), wz};
            v.normal   = normal;
            v.uv       = {static_cast<float>(x) / s.resolution,
                          static_cast<float>(z) / s.resolution};
            v.paint    = paints ? paints->sample(wx, wz) : glm::vec4(0.0f);
            data.vertices.push_back(v);
        }
    }

    data.indices.reserve(static_cast<std::size_t>(s.resolution) * s.resolution * 6);
    for (int z = 0; z < verts - 1; ++z) {
        for (int x = 0; x < verts - 1; ++x) {
            const std::uint32_t i0 = static_cast<std::uint32_t>(z * verts + x);
            const std::uint32_t i1 = i0 + 1;
            const std::uint32_t i2 = i0 + verts;
            const std::uint32_t i3 = i2 + 1;
            data.indices.insert(data.indices.end(),
                                {i0, i2, i1, i1, i2, i3});
        }
    }

    return data;
}

TerrainChunk TerrainChunk::fromData(glm::ivec2 coord, const MeshData& data) {
    TerrainChunk chunk;
    chunk.m_coord = coord;
    chunk.m_mesh  = Mesh::create(data);
    return chunk;
}

TerrainChunk TerrainChunk::generate(const TerrainSettings& s, glm::ivec2 coord) {
    return fromData(coord, buildMeshData(s, coord));
}

// --- TerrainStreamer -------------------------------------------------------

TerrainStreamer::TerrainStreamer(const TerrainSettings& settings, int radius)
    : m_settings(settings), m_radius(std::max(0, radius)) {
    unsigned hw = std::thread::hardware_concurrency();
    unsigned count = (hw > 2) ? hw - 1 : 1;
    count = std::min(count, 4u);
    for (unsigned i = 0; i < count; ++i) {
        m_workers.emplace_back([this] { workerLoop(); });
    }
}

TerrainStreamer::~TerrainStreamer() {
    m_stop.store(true);
    m_jobCv.notify_all();
    for (std::thread& t : m_workers) {
        if (t.joinable()) t.join();
    }
}

std::int64_t TerrainStreamer::key(glm::ivec2 c) {
    return (static_cast<std::int64_t>(c.x) << 32) ^
           (static_cast<std::uint32_t>(c.y));
}

glm::ivec2 TerrainStreamer::coordOf(std::int64_t k) {
    return {static_cast<int>(k >> 32),
            static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(k)))};
}

glm::ivec2 TerrainStreamer::chunkCoordOf(const glm::vec3& pos) const {
    return {static_cast<int>(std::floor(pos.x / m_settings.chunkSize)),
            static_cast<int>(std::floor(pos.z / m_settings.chunkSize))};
}

bool TerrainStreamer::inRange(glm::ivec2 c, glm::ivec2 center) const {
    return std::abs(c.x - center.x) <= m_radius &&
           std::abs(c.y - center.y) <= m_radius;
}

bool TerrainStreamer::inRangeOfAny(glm::ivec2 c) const {
    for (const glm::ivec2& centre : m_centers)
        if (inRange(c, centre)) return true;
    return false;
}

void TerrainStreamer::workerLoop() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_jobMutex);
            m_jobCv.wait(lock, [this] { return m_stop.load() || !m_jobs.empty(); });
            if (m_stop.load()) return;
            job = std::move(m_jobs.front());
            m_jobs.pop();
        }
        MeshData data = TerrainChunk::buildMeshData(job.settings, job.coord);
        {
            std::lock_guard<std::mutex> lock(m_resultMutex);
            m_results.push({job.coord, job.generation, job.editVersion, std::move(data)});
        }
    }
}

int TerrainStreamer::update(const glm::vec3& cameraPos, int maxUploads) {
    return update(std::vector<glm::vec3>{cameraPos}, maxUploads);
}

int TerrainStreamer::update(const std::vector<glm::vec3>& viewers, int maxUploads) {
    // No terrain object in the scene: nothing to stream. setEnabled(false) already
    // dropped the chunks, and results from jobs still in flight are discarded as
    // stale the moment streaming resumes (rebuild bumped the generation).
    if (!m_enabled) return 0;
    // The viewers' chunk coordinates, without duplicates: two players on the
    // same stretch of track share a ring, and queueing it twice would just make
    // the job set bigger for nothing.
    std::vector<glm::ivec2> centers;
    centers.reserve(viewers.size());
    for (const glm::vec3& v : viewers) {
        const glm::ivec2 c = chunkCoordOf(v);
        bool seen = false;
        for (const glm::ivec2& o : centers) if (o == c) { seen = true; break; }
        if (!seen) centers.push_back(c);
    }
    if (centers.empty()) return 0;
    // The resident set follows the viewers, so it has to be current before
    // anything below decides what is in range.
    const bool centersMoved = (centers != m_centers);
    m_centers = std::move(centers);

    // 1) Upload finished chunks (render thread). Bounded per frame.
    int uploaded = 0;
    std::vector<Result> ready;
    {
        std::lock_guard<std::mutex> lock(m_resultMutex);
        while (!m_results.empty() && static_cast<int>(ready.size()) < maxUploads) {
            ready.push_back(std::move(m_results.front()));
            m_results.pop();
        }
    }
    for (Result& r : ready) {
        const std::int64_t k = key(r.coord);
        m_pending.erase(k);
        const bool stale = (r.generation != m_generation) || !inRangeOfAny(r.coord);
        if (!stale) {
            // insert_or_assign so a sculpt rebuild swaps the mesh in place (the
            // old one kept rendering until now -> no hole). The replaced Mesh's
            // GL resources free here on the render thread, which is correct.
            m_chunks.insert_or_assign(k, TerrainChunk::fromData(r.coord, r.data));
            m_chunkEdit[k] = r.editVersion;
            ++uploaded;
        }
        // stale / out-of-range results are simply dropped.
    }

    bool changed = uploaded > 0;

    // 2) When a viewer changes chunk (or after a rebuild), refresh the desired
    //    set: drop chunks no viewer can still see, and queue any missing ones.
    if (centersMoved || m_dirty) {
        m_dirty  = false;
        changed  = true;

        for (auto it = m_chunks.begin(); it != m_chunks.end();) {
            if (!inRangeOfAny(it->second.coord())) {
                m_chunkEdit.erase(key(it->second.coord()));
                it = m_chunks.erase(it);
            } else ++it;
        }

        std::vector<Job> newJobs;
        for (const glm::ivec2& centre : m_centers)
        for (int dz = -m_radius; dz <= m_radius; ++dz) {
            for (int dx = -m_radius; dx <= m_radius; ++dx) {
                const glm::ivec2 c{centre.x + dx, centre.y + dz};
                const std::int64_t k = key(c);
                if (m_chunks.find(k) == m_chunks.end() &&
                    m_pending.find(k) == m_pending.end()) {
                    m_pending.insert(k);
                    newJobs.push_back({c, m_settings, m_generation, m_editVersion});
                }
            }
        }
        if (!newJobs.empty()) {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            for (Job& j : newJobs) m_jobs.push(std::move(j));
            m_jobCv.notify_all();
        }
    }

    // 3) Sculpt rebuilds: re-queue loaded chunks whose mesh predates the latest
    //    edit. A chunk stays dirty until a build that ran against the current
    //    edit version lands, so a stroke always converges to the final shape.
    if (!m_editDirty.empty()) {
        std::vector<Job> editJobs;
        for (auto it = m_editDirty.begin(); it != m_editDirty.end();) {
            const std::int64_t k = *it;
            const glm::ivec2   c = coordOf(k);
            const auto ce = m_chunkEdit.find(k);
            const bool loaded  = m_chunks.find(k) != m_chunks.end();
            const bool current = ce != m_chunkEdit.end() && ce->second >= m_editVersion;
            if (!inRangeOfAny(c) || (current && loaded)) {
                it = m_editDirty.erase(it);          // done or gone out of range
                continue;
            }
            if (m_pending.find(k) == m_pending.end()) {
                m_pending.insert(k);
                editJobs.push_back({c, m_settings, m_generation, m_editVersion});
            }
            ++it; // keep until a current-version result replaces it
        }
        if (!editJobs.empty()) {
            std::lock_guard<std::mutex> lock(m_jobMutex);
            for (Job& j : editJobs) m_jobs.push(std::move(j));
            m_jobCv.notify_all();
        }
    }

    if (changed) {
        refreshVisible();
    }
    return uploaded;
}

void TerrainStreamer::rebuild() {
    // Bump generation so in-flight results are discarded; clear loaded + pending.
    ++m_generation;
    m_chunks.clear();
    m_pending.clear();
    m_visible.clear();
    m_editDirty.clear();
    m_chunkEdit.clear();
    m_dirty = true;
}

void TerrainStreamer::editsChanged(const glm::vec2& worldMin,
                                   const glm::vec2& worldMax) {
    ++m_editVersion; // any chunk built before this is now out of date
    const float cs = m_settings.chunkSize;
    const int x0 = static_cast<int>(std::floor(worldMin.x / cs));
    const int x1 = static_cast<int>(std::floor(worldMax.x / cs));
    const int z0 = static_cast<int>(std::floor(worldMin.y / cs));
    const int z1 = static_cast<int>(std::floor(worldMax.y / cs));
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const glm::ivec2 c{x, z};
            const std::int64_t k = key(c);
            // Only chunks we actually hold (or are already building) matter; ones
            // still off-screen will pick up the edits when they first stream in.
            if (m_chunks.find(k) != m_chunks.end() ||
                m_pending.find(k) != m_pending.end())
                m_editDirty.insert(k);
        }
}

void TerrainStreamer::refreshVisible() {
    m_visible.clear();
    m_visible.reserve(m_chunks.size());
    for (const auto& [k, chunk] : m_chunks) {
        m_visible.push_back(&chunk);
    }
}

} // namespace fitzel
