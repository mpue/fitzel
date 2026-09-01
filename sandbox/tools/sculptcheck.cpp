// sculptcheck -- does the proportional pull leave the ground where the gesture
// stopped, and nowhere else?
//
// The Pull tool is the one sculpt brush here that is not a dab. Every other one
// applies a step per frame and the ground ends up wherever holding the button
// for that long put it: the result is a function of TIME, so aiming for a
// particular height means creeping up on it, and a hand that shakes writes every
// shake into the terrain. Pull is a function of WHERE THE MOUSE IS -- the caller
// keeps the total it has already written and hands over only the difference, so
// the same gesture ending in the same place leaves the same hill however it got
// there.
//
// That contract is split across two files (TerrainEditField::pull in the engine,
// the bookkeeping in the editor's viewport loop), which is exactly the kind of
// thing that is right on the day it is written and quietly stops being right
// later -- and it cannot be seen by looking, because a hill that is 30% too tall
// looks like a hill. So the wobble is played out here against the real field and
// the answer is compared with the one straight line that should have produced
// it, cell for cell.
//
// No GL, no window, no assets: this is arithmetic about a height field.
//
//   build/release/bin/sculptcheck.exe
// Exits non-zero if any measurement fails.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <fitzel/world/Terrain.hpp>

namespace {

int g_fails = 0;

void fail(const char* what, const std::string& detail) {
    std::printf("[FAIL] %s: %s\n", what, detail.c_str());
    ++g_fails;
}
void pass(const char* what, const std::string& detail) {
    std::printf("  ok   %s -- %s\n", what, detail.c_str());
}

fitzel::TerrainEditField makeField() {
    fitzel::TerrainEditField f;
    f.cell = 1.0f;
    return f;
}

// The worst disagreement between two fields, over every cell either of them has.
float worstDiff(const fitzel::TerrainEditField& a,
                const fitzel::TerrainEditField& b) {
    auto look = [](const fitzel::TerrainEditField& f, std::int64_t k) {
        const auto it = f.deltas.find(k);
        return it == f.deltas.end() ? 0.0f : it->second;
    };
    float worst = 0.0f;
    for (const auto& [k, v] : a.deltas) worst = std::max(worst, std::fabs(look(b, k) - v));
    for (const auto& [k, v] : b.deltas) worst = std::max(worst, std::fabs(look(a, k) - v));
    return worst;
}

// How far from the centre the field still holds anything.
float reachOf(const fitzel::TerrainEditField& f, glm::vec2 c) {
    float far_ = 0.0f;
    for (const auto& [k, v] : f.deltas) {
        if (std::fabs(v) < 1e-6f) continue;
        const int ix = static_cast<int>(k >> 32);
        const int iz = static_cast<int>(
            static_cast<std::int32_t>(static_cast<std::uint32_t>(k)));
        far_ = std::max(far_, glm::distance(glm::vec2(ix * f.cell, iz * f.cell), c));
    }
    return far_;
}

// The total volume moved, as a stand-in for "how much of the disc came along".
double volumeOf(const fitzel::TerrainEditField& f) {
    double v = 0.0;
    for (const auto& [k, d] : f.deltas) v += d * f.cell * f.cell;
    return v;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const glm::vec2 C(120.0f, -37.0f);   // off the grid origin on purpose
    const float R = 12.0f;

    // --- 1) It reaches the height it was asked for, and stops at the rim -----
    {
        fitzel::TerrainEditField f = makeField();
        f.pull(C, R, 8.0f, 1.0f);
        const float peak = f.sample(C.x, C.y);
        const float rim  = f.sample(C.x + R, C.y);
        const float out  = f.sample(C.x + R * 1.5f, C.y);
        const float far_ = reachOf(f, C);
        char d[200];
        std::snprintf(d, sizeof d,
                      "peak %.3f m of 8, rim %.4f m, %.1f m out %.4f m, "
                      "nothing written past %.1f m of %.1f",
                      peak, rim, R * 1.5f, out, far_, R);
        // The centre cell is not exactly under the centre (the brush is on a
        // grid), so the peak is a bilinear sample of cells that are all slightly
        // off it -- a few centimetres short is the grid, not the tool.
        if (peak < 7.85f || peak > 8.0001f) fail("pull reaches its height", d);
        else if (std::fabs(rim) > 0.02f)    fail("pull stops at the rim", d);
        else if (std::fabs(out) > 1e-6f)    fail("pull stays inside", d);
        else if (far_ > R + 1e-3f)          fail("pull stays inside", d);
        else                                pass("pull reaches its height", d);
    }

    // --- 2) The gesture, not the journey ------------------------------------
    // A drag that overshoots, comes back, shakes, and settles at six metres --
    // the shape of a real hand on a real mouse, and the shape of a hand with a
    // tremor in particular. Played through the same bookkeeping the viewport
    // does: keep the total, write the difference.
    {
        const float wobble[] = {2.0f, 9.0f, 4.5f, 11.0f, 3.0f, 7.5f, 5.0f,
                                6.4f, 5.6f, 6.1f, 5.9f, 6.0f};
        fitzel::TerrainEditField dragged = makeField();
        float applied = 0.0f;
        for (float want : wobble) {
            dragged.pull(C, R, want - applied, 1.0f);
            applied = want;
        }
        fitzel::TerrainEditField straight = makeField();
        straight.pull(C, R, 6.0f, 1.0f);

        const float diff = worstDiff(dragged, straight);
        char d[220];
        std::snprintf(d, sizeof d,
                      "12 moves up to 11 m and back to 6 -- worst cell differs "
                      "from a single 6 m pull by %.6f m (peak %.3f m)",
                      diff, dragged.sample(C.x, C.y));
        // Float addition is not associative, so twelve steps and one step cannot
        // be bit-identical; a millimetre is the whole budget.
        if (diff > 1e-3f) fail("the gesture, not the journey", d);
        else              pass("the gesture, not the journey", d);

        // ...and the contrast that says why this tool exists next to Raise. The
        // same wobble through the dab brush writes every leg of it into the
        // ground, because a dab has no memory of where it already was.
        fitzel::TerrainEditField dabbed = makeField();
        float prev = 0.0f;
        for (float want : wobble) { dabbed.raise(C, R, std::fabs(want - prev)); prev = want; }
        char e[200];
        std::snprintf(e, sizeof e,
                      "the same wobble through Raise ends at %.1f m instead of 6",
                      dabbed.sample(C.x, C.y));
        if (dabbed.sample(C.x, C.y) < 6.5f)
            fail("Raise is the one that accumulates", e);   // the contrast is gone
        else
            pass("Raise is the one that accumulates", e);
    }

    // --- 3) The shape moves, the reach does not ------------------------------
    // What the Proportion knob is allowed to change. A falloff that also changed
    // how far the edit went would make the two sliders fight each other, and the
    // ring drawn in the viewport would stop being the truth.
    {
        struct Row { float f; float peak, reach; double vol; std::size_t cells; };
        Row rows[3] = {{0.5f}, {1.0f}, {3.0f}};
        for (Row& r : rows) {
            fitzel::TerrainEditField f = makeField();
            f.pull(C, R, 8.0f, r.f);
            r.peak  = f.sample(C.x, C.y);
            r.reach = reachOf(f, C);
            r.vol   = volumeOf(f);
            r.cells = f.deltas.size();
        }
        char d[300];
        std::snprintf(d, sizeof d,
                      "0.5: %.2f m peak, %.0f m3, %d cells | 1.0: %.2f/%.0f/%d | "
                      "3.0: %.2f/%.0f/%d (visible edge %.0f/%.0f/%.0f m)",
                      rows[0].peak, rows[0].vol, static_cast<int>(rows[0].cells),
                      rows[1].peak, rows[1].vol, static_cast<int>(rows[1].cells),
                      rows[2].peak, rows[2].vol, static_cast<int>(rows[2].cells),
                      rows[0].reach, rows[1].reach, rows[2].reach);
        // The CELLS TOUCHED, not the last one still worth a micron. A sharp
        // falloff writes a millionth of a millimetre near the rim and the eye
        // sees a smaller hill, but the edit covers the same ground -- and it is
        // the ground that matters here, because the ring drawn in the viewport
        // promises exactly that and the two sliders must not fight.
        const bool sameReach = rows[0].cells == rows[2].cells &&
                               rows[1].cells == rows[2].cells;
        const bool samePeak  = std::fabs(rows[0].peak - rows[2].peak) < 0.05f;
        const bool ordered   = rows[0].vol > rows[1].vol && rows[1].vol > rows[2].vol;
        if (!sameReach)     fail("the shape moves, the reach does not", d);
        else if (!samePeak) fail("every shape reaches the same height", d);
        else if (!ordered)  fail("a lower proportion brings more along", d);
        else                pass("the shape moves, the reach does not", d);
    }

    // --- 4) Down is the same tool as up --------------------------------------
    {
        fitzel::TerrainEditField up = makeField(), down = makeField();
        up.pull(C, R, 5.0f, 1.4f);
        down.pull(C, R, -5.0f, 1.4f);
        float worst = 0.0f;
        for (const auto& [k, v] : up.deltas) {
            const auto it = down.deltas.find(k);
            const float o = (it == down.deltas.end()) ? 0.0f : it->second;
            worst = std::max(worst, std::fabs(v + o));
        }
        char d[160];
        std::snprintf(d, sizeof d, "a -5 m pull mirrors a +5 m one to %.6f m", worst);
        if (worst > 1e-5f) fail("down is the same tool as up", d);
        else               pass("down is the same tool as up", d);
    }

    std::printf(g_fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
                g_fails);
    return g_fails ? 1 : 0;
}
