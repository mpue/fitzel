#include "TramCar.hpp"

#include <algorithm>
#include <cmath>

using splinegen::detail::appendBox;
using splinegen::detail::Slot;
using namespace tramsim;

namespace {

// [a, b] with the stretches c +- half cut out of it (c ascending).
std::vector<glm::vec2> spans(float a, float b, const float* c, int n, float half) {
    std::vector<glm::vec2> out;
    float from = a;
    for (int i = 0; i < n; ++i) {
        if (c[i] - half > from + 0.02f) out.push_back({from, std::min(c[i] - half, b)});
        from = std::max(from, c[i] + half);
    }
    if (b > from + 0.02f) out.push_back({from, b});
    return out;
}

} // namespace

namespace tramcar {


// One car, in its own frame (TramSim's "Inside"): x across, y up from the rail
// head, z along, +Z the way it faces. A cab car carries the driver's end at +Z;
// the middle car has a gangway at both ends and the pantograph. What a figure
// walks on and against goes to `hc`/`hh` as boxes besides.
void build(bool cab, Slot slot[MatCount], std::vector<glm::vec3>& hc,
              std::vector<glm::vec3>& hh) {
    const float L = kSectionLen * 0.5f;   // 4.8
    const float W = kHalfWidth;           // 1.2
    const float F = kFloor, C = kCeiling, Wi = kWallIn, D = kDoorHalf, T = kDoorTop;
    float dz[2];
    doorsOf(cab, dz);
    const float z0 = -L, z1 = cab ? kCabWall : L;      // the passengers' room
    auto vis = [&](int m, glm::vec3 c, glm::vec3 h) { appendBox(slot[m], c, h, 0.0f, 1.0f); };
    auto hit = [&](glm::vec3 c, glm::vec3 h) { hc.push_back(c); hh.push_back(h); };

    // --- Floor ------------------------------------------------------------------
    vis(Dark,  {0.0f, 0.17f, 0.0f},      {W - 0.15f, 0.12f, L - 0.3f});   // underframe, bogies
    vis(Dark,  {0.0f, F - 0.05f, 0.0f},  {W, 0.05f, L});                  // the floor's edge
    vis(Floor, {0.0f, F + 0.004f, 0.5f * (z0 + z1)}, {Wi, 0.004f, 0.5f * (z1 - z0)});
    hit({0.0f, F - 0.1f, 0.0f}, {W, 0.1f, L});

    // --- Sides: skirt, windows, the band above; doorways left open -------------
    const float zs1 = cab ? L - 0.08f : L;
    for (float sx : {-1.0f, 1.0f}) {
        for (const glm::vec2& s : spans(z0, zs1, dz, 2, D)) {
            const float zc = 0.5f * (s.x + s.y), hz = 0.5f * (s.y - s.x);
            vis(Accent, {sx * (W - 0.03f), 0.60f, zc},   {0.03f, 0.35f, hz});
            vis(Glass,  {sx * (W - 0.045f), 0.5f * (0.95f + T), zc}, {0.012f, 0.5f * (T - 0.95f), hz});
            hit({sx * (W - 0.04f), 0.5f * (F + C), zc}, {0.04f, 0.5f * (C - F), hz});
            // Inside, below the windows, a lining in the room's colour.
            const float la = s.x, lb = std::min(s.y, z1);
            if (lb > la + 0.02f)
                vis(Lining, {sx * (Wi + 0.008f), 0.5f * (F + 0.95f), 0.5f * (la + lb)},
                    {0.008f, 0.5f * (0.95f - F), 0.5f * (lb - la)});
            // Window pillars, every ~1.6 m, and one each side of a doorway.
            for (float z = s.x + 0.8f; z < s.y - 0.4f; z += 1.6f)
                vis(Body, {sx * (W - 0.03f), 0.5f * (0.95f + T), z}, {0.035f, 0.5f * (T - 0.95f), 0.06f});
            for (float z : {s.x + 0.05f, s.y - 0.05f})
                if (z > z0 + 0.1f && z < zs1 - 0.1f)
                    vis(Body, {sx * (W - 0.03f), 0.5f * (F + T), z}, {0.035f, 0.5f * (T - F), 0.05f});
        }
        vis(Body, {sx * (W - 0.03f), 0.5f * (T + C), 0.0f}, {0.03f, 0.5f * (C - T), L});
        for (float z : dz)
            hit({sx * (W - 0.04f), 0.5f * (T + C), z}, {0.04f, 0.5f * (C - T), D});
    }

    // --- Roof and ceiling, light strips -----------------------------------------
    vis(Body, {0.0f, 0.5f * (C + 2.95f), 0.0f}, {W, 0.5f * (2.95f - C), L});
    vis(Roof, {0.0f, 3.02f, 0.0f}, {W - 0.1f, 0.07f, L - 0.05f});
    hit({0.0f, C + 0.08f, 0.0f}, {W, 0.08f, L});
    for (float sx : {-1.0f, 1.0f})
        vis(Light, {sx * 0.55f, C - 0.012f, 0.5f * (z0 + z1)}, {0.07f, 0.012f, 0.5f * (z1 - z0) - 0.3f});

    // --- Seats in bays between the doorways, poles and rails --------------------
    for (const glm::vec2& zone : spans(z0 + 0.15f, z1 - 0.05f, dz, 2, D + 0.25f)) {
        const float len = zone.y - zone.x;
        if (len < 0.6f) continue;
        const int n = std::max(1, static_cast<int>(len / 0.8f));
        for (int i = 0; i < n; ++i) {
            const float zr = zone.x + (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
            // Facing each other across a bay; a single row faces the car's middle.
            const float f = n == 1 ? (zr < 0.0f ? 1.0f : -1.0f) : (i < n / 2 ? 1.0f : -1.0f);
            for (float sx : {-1.0f, 1.0f}) {
                // A double seat from the wall to the aisle, which is left wide
                // enough for someone to walk down it.
                const float x = sx * 0.73f;
                vis(Dark, {x, 0.5f * (F + 0.62f), zr}, {0.35f, 0.5f * (0.62f - F), 0.18f});
                vis(Seat, {x, 0.66f, zr + f * 0.02f}, {0.38f, 0.04f, 0.24f});
                vis(Seat, {x, 0.98f, zr - f * 0.21f}, {0.38f, 0.28f, 0.04f});
                hit({x, 0.5f * (F + 0.70f), zr}, {0.39f, 0.5f * (0.70f - F), 0.24f});
                hit({x, 0.98f, zr - f * 0.21f}, {0.39f, 0.28f, 0.04f});
            }
        }
        for (float sx : {-1.0f, 1.0f}) {
            for (float z : {zone.x, zone.y})
                vis(Pole, {sx * 0.33f, 0.5f * (F + C), z}, {0.018f, 0.5f * (C - F), 0.018f});
            vis(Pole, {sx * 0.36f, 1.95f, 0.5f * (zone.x + zone.y)}, {0.015f, 0.015f, 0.5f * len});
        }
    }
    // A pole in each doorway's middle, where whoever stands holds on.
    for (float z : dz)
        vis(Pole, {0.0f, 0.5f * (F + C), z}, {0.02f, 0.5f * (C - F), 0.02f});

    // --- The gangway to the next car ---------------------------------------------
    std::vector<float> ends{-1.0f};
    if (!cab) ends.push_back(1.0f);
    for (float e : ends) {
        for (float sx : {-1.0f, 1.0f}) {
            vis(Body, {sx * 0.91f, 0.5f * (F + C), e * (L - 0.04f)}, {0.29f, 0.5f * (C - F), 0.04f});
            hit({sx * 0.91f, 0.5f * (F + C), e * (L - 0.04f)}, {0.29f, 0.5f * (C - F), 0.04f});
            // The bellows, out into the gap; the next car's meets it there.
            vis(Dark, {sx * (W - 0.12f), 0.5f * (F + C), e * (L + 0.15f)}, {0.04f, 0.5f * (C - F) + 0.05f, 0.15f});
            hit({sx * (W - 0.12f), 0.5f * (F + C), e * (L + 0.15f)}, {0.04f, 0.5f * (C - F), 0.15f});
        }
        vis(Body, {0.0f, 0.5f * (2.2f + C), e * (L - 0.04f)}, {0.62f, 0.5f * (C - 2.2f), 0.04f});
        hit({0.0f, 0.5f * (2.2f + C), e * (L - 0.04f)}, {0.62f, 0.5f * (C - 2.2f), 0.04f});
        vis(Dark, {0.0f, C + 0.1f, e * (L + 0.15f)}, {W - 0.12f, 0.1f, 0.15f});
        vis(Dark, {0.0f, F - 0.03f, e * (L + 0.15f)}, {W - 0.16f, 0.03f, 0.15f});
        hit({0.0f, F - 0.1f, e * (L + 0.15f)}, {W - 0.16f, 0.1f, 0.15f});
    }

    if (cab) {
        // The cab's back wall, glazed above the waist; nobody walks past it.
        vis(Body,  {0.0f, 0.5f * (F + 1.2f), kCabWall}, {Wi, 0.5f * (1.2f - F), 0.03f});
        vis(Glass, {0.0f, 0.5f * (1.2f + T), kCabWall}, {Wi, 0.5f * (T - 1.2f), 0.012f});
        vis(Body,  {0.0f, 0.5f * (T + C), kCabWall},    {Wi, 0.5f * (C - T), 0.03f});
        hit({0.0f, 0.5f * (F + C), kCabWall}, {W, 0.5f * (C - F), 0.04f});
        // The nose: skirt, windscreen, the band over it, the destination board
        // and two lamps.
        vis(Accent, {0.0f, 0.60f, L - 0.04f}, {W, 0.35f, 0.04f});
        vis(Glass,  {0.0f, 0.5f * (0.95f + T), L - 0.05f}, {W - 0.03f, 0.5f * (T - 0.95f), 0.012f});
        vis(Body,   {0.0f, 0.5f * (T + C), L - 0.04f}, {W, 0.5f * (C - T), 0.04f});
        vis(Dark,   {0.0f, 2.46f, L + 0.005f}, {0.75f, 0.10f, 0.02f});
        for (float x : {-0.78f, 0.78f})
            vis(Body, {x, 0.78f, L + 0.005f}, {0.16f, 0.06f, 0.02f});
        // The driver's desk and seat.
        vis(Dark, {0.0f, 0.95f, L - 0.45f}, {W - 0.15f, 0.10f, 0.28f});
        vis(Dark, {0.0f, 0.5f * (F + 0.63f), L - 1.05f}, {0.10f, 0.5f * (0.63f - F), 0.10f});
        vis(Seat, {0.0f, 0.68f, L - 1.05f}, {0.25f, 0.05f, 0.25f});
        vis(Seat, {0.0f, 1.00f, L - 1.30f}, {0.25f, 0.30f, 0.04f});
        vis(Roof, {0.0f, 3.20f, -1.5f}, {0.6f, 0.12f, 1.0f});           // roof equipment
    } else {
        vis(Roof, {0.0f, 3.25f, -2.0f}, {0.7f, 0.16f, 1.4f});           // the air conditioning
        // The pantograph: its frame on the roof, the arm, the bow at the wire.
        vis(Dark, {0.0f, 3.15f, 1.0f}, {0.40f, 0.06f, 0.60f});
        vis(Dark, {0.0f, 4.36f, 1.0f}, {0.04f, 1.20f, 0.04f});
        vis(Dark, {0.0f, 5.58f, 1.0f}, {0.80f, 0.025f, 0.06f});
    }
}

// A door leaf: the middle of its pane, in the car's frame. Doorway `j` (0, 1),
// side `sx` (+-1), `k` = -1 for the leaf towards -z, +1 for the other; `open`
// 0..1 -- out of the wall a hand's breadth, then aside over the outer skin.
glm::vec3 leafAt(bool cab, int j, float sx, float k, float open) {
    float dz[2];
    doorsOf(cab, dz);
    const float out = 0.07f * std::clamp(open * 4.0f, 0.0f, 1.0f);
    const float u = std::clamp((open - 0.2f) / 0.8f, 0.0f, 1.0f);
    const float slide = kDoorHalf * u * u * (3.0f - 2.0f * u);
    return {sx * (kHalfWidth - 0.045f + out), 0.5f * (kFloor + kDoorTop),
            dz[j] + k * (0.5f * kDoorHalf + slide)};
}
const glm::vec3 kLeafHalf{0.03f, 0.5f * (kDoorTop - kFloor), 0.5f * kDoorHalf};


// Leaf `n` of a car (0..7): doorway n/4, side by bit 1, end by bit 0.
void leafOf(int n, int& j, float& sx, float& k) {
    j  = n / 4;
    sx = (n & 2) ? 1.0f : -1.0f;
    k  = (n & 1) ? 1.0f : -1.0f;
}

// Every leaf of a car, those on `side` opened by `open`: pane and frame.
void buildLeaves(bool cab, int side, float open, Slot& glass, Slot& frame) {
    const float hy = kLeafHalf.y, hz = kLeafHalf.z;
    for (int n = 0; n < 8; ++n) {
        int j;
        float sx, k;
        leafOf(n, j, sx, k);
        const glm::vec3 c = leafAt(cab, j, sx, k, sx == static_cast<float>(side) ? open : 0.0f);
        appendBox(glass, c, {0.012f, hy - 0.09f, hz - 0.06f}, 0.0f, 1.0f);
        appendBox(frame, c + glm::vec3(0.0f, hy - 0.045f, 0.0f), {0.025f, 0.045f, hz}, 0.0f, 1.0f);
        appendBox(frame, c - glm::vec3(0.0f, hy - 0.045f, 0.0f), {0.025f, 0.045f, hz}, 0.0f, 1.0f);
        for (float e : {-1.0f, 1.0f})
            appendBox(frame, c + glm::vec3(0.0f, 0.0f, e * (hz - 0.03f)), {0.025f, hy, 0.03f}, 0.0f, 1.0f);
    }
}


} // namespace tramcar
