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
// The Rain brush (hydraulic erosion) is checked here too: repeatable, bounded,
// and actually cutting channels. `--dump <dir>` writes hillshaded before/after
// pictures of its test cone, for looking rather than measuring.
//
// No GL, no window, no assets: this is arithmetic about a height field.
//
//   build/release/bin/sculptcheck.exe [--dump <dir>]
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

// A hillshaded greyscale picture of the field around `c`, for looking at what
// the rain did (sculptcheck --dump <dir>). Base ground is flat in this tool.
void dumpShade(const fitzel::TerrainEditField& f, glm::vec2 c, float half,
               const std::string& path, const fitzel::TerrainSettings* ground = nullptr) {
    auto at = [&](float x, float z) {
        return (ground ? fitzel::terrainBaseHeight(*ground, x, z) : 0.0f) + f.sample(x, z);
    };
    const int n = static_cast<int>(half * 2.0f / f.cell);
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return;
    std::fprintf(out, "P5\n%d %d\n255\n", n, n);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const float x = c.x - half + i * f.cell, z = c.y - half + j * f.cell;
            const float sx = (at(x + f.cell, z) - at(x - f.cell, z)) / (2 * f.cell);
            const float sz = (at(x, z + f.cell) - at(x, z - f.cell)) / (2 * f.cell);
            const glm::vec3 nrm = glm::normalize(glm::vec3(-sx, 1.0f, -sz));
            const float lam = std::max(0.0f, glm::dot(nrm, glm::normalize(glm::vec3(-0.6f, 0.7f, -0.5f))));
            std::fputc(static_cast<int>(std::min(255.0f, 30.0f + 225.0f * lam)), out);
        }
    std::fclose(out);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string dumpDir = (argc > 2 && std::string(argv[1]) == "--dump") ? argv[2] : "";

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

    // --- 5) Rain: hydraulic erosion ------------------------------------------
    // A rough cone on flat ground, rained on in showers the way the brush does
    // it. The rain is not a height the gesture asks for, so the checks are about
    // behaviour: same seed same ground, nothing past the reach, flat ground left
    // alone, material taken off the slope and laid down at its foot, and the
    // slope cut into channels rather than lowered evenly.
    {
        fitzel::setTerrainPresent(false);                // flat base: only the field counts
        const fitzel::TerrainSettings ts;
        const float cone = 36.0f, rainR = 30.0f;
        fitzel::TerrainEditField hill = makeField();
        hill.stamp(C, cone, 22.0f, 1);
        hill.roughen(C, cone, 0.5f, 0.35f, 3.0f);
        fitzel::TerrainEditField a = hill, b = hill;
        float reach = 0.0f;
        const int showers = 20, drops = 150;   // about one drop per square metre in all
        for (int i = 0; i < showers; ++i) {
            reach = a.rain(ts, C, rainR, drops, static_cast<std::uint32_t>(i));
            b.rain(ts, C, rainR, drops, static_cast<std::uint32_t>(i));
        }
        if (!dumpDir.empty()) {
            dumpShade(hill, C, 50.0f, dumpDir + "/rain-before.pgm");
            dumpShade(a,    C, 50.0f, dumpDir + "/rain-after.pgm");
        }

        const float same = worstDiff(a, b);
        bool finite = true;
        float outside = 0.0f;
        double upper = 0.0, foot = 0.0; int nUpper = 0, nFoot = 0;
        double ringSq = 0.0, ringMean = 0.0; int nRing = 0;
        double moved = 0.0;
        for (const auto& [k, v] : a.deltas) {
            if (!std::isfinite(v)) finite = false;
            const auto it = hill.deltas.find(k);
            const float change = v - (it == hill.deltas.end() ? 0.0f : it->second);
            const int ix = static_cast<int>(k >> 32);
            const int iz = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(k)));
            const float r = glm::distance(glm::vec2(ix, iz) * a.cell, C);
            moved += std::fabs(change);
            if (r > reach + a.cell) outside = std::max(outside, std::fabs(change));
            if (r < 20.0f)                { upper += change; ++nUpper; }
            if (r > 32.0f && r < 44.0f)   { foot  += change; ++nFoot;  }
            if (r > 14.0f && r < 18.0f)   { ringSq += change * change; ringMean += change; ++nRing; }
        }
        upper /= std::max(1, nUpper); foot /= std::max(1, nFoot);
        ringMean /= std::max(1, nRing);
        const double ringDev = std::sqrt(std::max(0.0, ringSq / std::max(1, nRing) - ringMean * ringMean));

        fitzel::TerrainEditField flat = makeField();
        flat.rain(ts, C, rainR, drops, 7u);
        double flatMoved = 0.0;
        for (const auto& [k, v] : flat.deltas) flatMoved += std::fabs(v);

        char d[240];
        std::snprintf(d, sizeof d, "two runs with the same seeds differ by %.7f m", same);
        if (same > 1e-6f) fail("rain is repeatable", d); else pass("rain is repeatable", d);
        std::snprintf(d, sizeof d, "largest change past the %.1f m reach: %.6f m", reach, outside);
        if (!finite || outside > 1e-6f) fail("rain stays inside its reach", d);
        else                            pass("rain stays inside its reach", d);
        std::snprintf(d, sizeof d, "flat ground moved %.6f m in total", flatMoved);
        if (flatMoved > 1e-6) fail("rain leaves flat ground alone", d);
        else                  pass("rain leaves flat ground alone", d);
        std::snprintf(d, sizeof d, "upper slope %+.3f m on average, foot %+.3f m", upper, foot);
        if (!(upper < -0.05 && foot > 0.01)) fail("rain carries the slope to its foot", d);
        else                                 pass("rain carries the slope to its foot", d);
        std::snprintf(d, sizeof d, "mid-slope ring: mean %+.3f m, spread %.3f m", ringMean, ringDev);
        if (!(ringDev > 0.1)) fail("rain cuts channels, not a smooth lowering", d);
        else                  pass("rain cuts channels, not a smooth lowering", d);
        fitzel::setTerrainPresent(true);

        // For looking only: two seconds of holding the brush the way the editor
        // doses it (radius 20 m, strength 0.5, ten showers a second) on the
        // default generator's ground, which is what the dose was tuned against.
        if (!dumpDir.empty()) {
            const fitzel::TerrainSettings real;
            const glm::vec2 at(260.0f, 140.0f);
            fitzel::TerrainEditField g = makeField();
            dumpShade(g, at, 50.0f, dumpDir + "/rain-real-before.pgm", &real);
            const int dose = static_cast<int>(3.14159265f * 20.0f * 20.0f * 0.5f * 0.06f);
            for (int i = 0; i < 20; ++i) g.rain(real, at, 20.0f, dose, static_cast<std::uint32_t>(i));
            dumpShade(g, at, 50.0f, dumpDir + "/rain-real-after.pgm", &real);
        }
    }

    std::printf(g_fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
                g_fails);
    return g_fails ? 1 : 0;
}
