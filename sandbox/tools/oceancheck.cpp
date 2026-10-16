// oceancheck -- is the open water the sea the settings describe?
//
// A spectral ocean is wrong in ways no screenshot argues with: a transform
// scaled by the wrong factor of N makes every sea ten times too high or too
// low and still looks like water; a sign flipped sends the waves into the wind;
// a height lookup that forgets the crests lean puts a floating crate a hand
// above the surface it is drawn on. So here the sea is measured: its wave height
// against what oceanography says that wind raises, its waves against the wind's
// direction, its height lookup against the surface itself -- and the waves
// things make, and a box floated on it.
//
// No GL: Ocean, WaveSim and Buoyancy are plain arithmetic, which is why they
// can be checked without a window.
//
//   build/linux/bin/oceancheck
// Exits non-zero if any check fails.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../src/Buoyancy.hpp"
#include "../src/Ocean.hpp"
#include "../src/ShoreMap.hpp"
#include "../src/SeaSpray.hpp"
#include "../src/WaveSim.hpp"

namespace {

int g_fails = 0;

void check(bool ok, const char* what, const std::string& detail) {
    std::printf(ok ? "  ok   %s -- %s\n" : "[FAIL] %s: %s\n", what, detail.c_str());
    if (!ok) ++g_fails;
}
std::string num(const char* fmt, double a, double b = 0.0) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), fmt, a, b);
    return buf;
}

// The measured significant height: four standard deviations of the surface,
// sampled over the largest patch (which repeats, so that is all of it).
double measuredHs(const ocean::Ocean& o) {
    double s = 0.0, s2 = 0.0;
    int n = 0;
    const float L = ocean::kPatch[0];
    for (int z = 0; z < 160; ++z)
        for (int x = 0; x < 160; ++x) {
            const float h = o.displacementAt(L * x / 160.0f + 0.37f, L * z / 160.0f + 0.11f).y;
            s += h; s2 += static_cast<double>(h) * h; ++n;
        }
    const double mean = s / n;
    return 4.0 * std::sqrt(std::max(s2 / n - mean * mean, 0.0));
}

} // namespace

int main() {
    using namespace ocean;

    // --- The sea the wind raises ---------------------------------------------
    {
        Ocean o;
        Settings s;
        s.windSpeed = 10.0f;
        s.fetch     = 1.0e6f;   // open ocean: fully developed
        s.choppiness = 0.0f;
        o.configure(s);
        o.computeNow(5.0);
        // Pierson-Moskowitz: a fully developed sea under a 10 m/s wind has a
        // significant height of 0.21 U^2 / g -- about 2.1 m.
        const double pm = 0.21 * 100.0 / 9.81;
        check(std::fabs(o.significantHeight() - pm) / pm < 0.35,
              "an ocean under a fresh breeze is as high as Pierson-Moskowitz says",
              num("Hs %.2f m, PM %.2f m", o.significantHeight(), pm));
        const double meas = measuredHs(o);
        check(std::fabs(meas - o.significantHeight()) / o.significantHeight() < 0.25,
              "and the surface the transform builds has that height",
              num("measured %.2f m, spectrum %.2f m", meas, o.significantHeight()));
        check(o.peakWavelength() > 60.0f && o.peakWavelength() < 120.0f,
              "its biggest waves are the length a 10 m/s sea has",
              num("peak wavelength %.0f m", o.peakWavelength()));

        Settings pond = s;
        pond.fetch = 150.0f;
        o.configure(pond);
        o.computeNow(5.0);
        check(o.significantHeight() < 0.12f, "the same wind over a pond raises only ripples",
              num("Hs %.3f m", o.significantHeight()));
        check(o.peakWavelength() < 3.0f, "short ones",
              num("peak wavelength %.2f m", o.peakWavelength()));

        Settings lake = s;
        lake.fetch = 5000.0f;
        o.configure(lake);
        const float lakeHs = o.significantHeight();
        check(lakeHs > 0.2f && lakeHs < 1.2f, "and over a five-kilometre lake, a choppy sea",
              num("Hs %.2f m", lakeHs));

        Settings calm = s;
        calm.windSpeed = 0.0f;
        o.configure(calm);
        o.computeNow(5.0);
        check(o.significantHeight() < 1e-4f && std::fabs(o.heightAt(3.0f, 4.0f)) < 1e-4f,
              "no wind, no waves", num("Hs %.5f m", o.significantHeight()));
    }

    // --- Direction, and identity --------------------------------------------
    {
        Ocean o;
        Settings s;
        s.windSpeed = 9.0f;
        s.fetch     = 20000.0f;
        s.windDir   = 0.0f;   // toward +x
        o.configure(s);
        o.computeNow(3.0);
        // Slope along the wind is steeper than across it: crests stand across
        // the wind.
        double along = 0.0, across = 0.0;
        for (int i = 0; i < 4000; ++i) {
            const float x = static_cast<float>(i % 63) * 3.9f, z = static_cast<float>(i / 63) * 4.1f;
            const glm::vec3 n = o.surfaceAt(x, z).normal;
            along += n.x * n.x;
            across += n.z * n.z;
        }
        check(along > across * 1.3, "crests stand across the wind",
              num("slope variance along %.4f, across %.4f", along / 4000, across / 4000));

        // A crest moves downwind: the surface a little later looks like the
        // surface now, shifted toward +x.
        std::vector<float> a, b;
        o.computeNow(10.0);
        for (int i = 0; i < 256; ++i) a.push_back(o.displacementAt(i * 0.5f, 7.0f).y);
        o.computeNow(10.4);
        for (int i = 0; i < 256; ++i) b.push_back(o.displacementAt(i * 0.5f, 7.0f).y);
        auto match = [&](int lag) {   // b[i] vs a[i - lag]
            double c = 0.0;
            for (int i = 40; i < 216; ++i) c += a[static_cast<std::size_t>(i - lag)] * b[static_cast<std::size_t>(i)];
            return c;
        };
        double best = -1e30;
        int bestLag = 0;
        for (int lag = -20; lag <= 20; ++lag)
            if (match(lag) > best) { best = match(lag); bestLag = lag; }
        check(bestLag > 0, "and the waves travel with the wind, not into it",
              num("best match %.1f m downwind after 0.4 s", bestLag * 0.5));

        // Measured over many points: the two surfaces should be largely the
        // same sea (strongly correlated), not two independent draws (~0).
        std::vector<float> s1, s2v;
        for (int i = 0; i < 400; ++i) s1.push_back(o.heightAt(i * 2.3f, 30.0f + i * 1.7f));
        Settings s2 = s;
        s2.windSpeed = 10.0f;
        o.configure(s2);
        o.computeNow(10.4);
        for (int i = 0; i < 400; ++i) s2v.push_back(o.heightAt(i * 2.3f, 30.0f + i * 1.7f));
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (std::size_t i = 0; i < s1.size(); ++i) {
            ab += s1[i] * s2v[i]; aa += s1[i] * s1[i]; bb += s2v[i] * s2v[i];
        }
        const double r = ab / std::sqrt(std::max(aa * bb, 1e-12));
        check(r > 0.6, "turning the wind up grows the same sea rather than dealing a new one",
              num("correlation %.2f between 9 and 10 m/s", r));
    }

    // --- Asking the surface ---------------------------------------------------
    {
        Ocean o;
        Settings s;
        s.windSpeed  = 12.0f;
        s.fetch      = 30000.0f;
        s.choppiness = 1.0f;
        o.configure(s);
        o.computeNow(7.0);
        double worst = 0.0;
        for (int i = 0; i < 200; ++i) {
            const float x = i * 1.37f, z = i * 0.91f;
            const glm::vec3 d = o.displacementAt(x, z);
            // The water that started at (x, z) is at (x + dx, z + dz): the
            // height there must be its height.
            worst = std::max(worst, static_cast<double>(std::fabs(o.heightAt(x + d.x, z + d.z) - d.y)));
        }
        check(worst < 0.06, "the height above a point is the surface that leaned over it",
              num("worst disagreement %.3f m on choppy water", worst));

        // Smooth in time: a frame later the surface has hardly moved.
        const float a = o.heightAt(5.0f, 5.0f);
        o.computeNow(7.016);
        const float b = o.heightAt(5.0f, 5.0f);
        check(std::fabs(a - b) < 0.1f, "one frame later the water is where it was, near enough",
              num("moved %.3f m in 16 ms", std::fabs(a - b)));
        const glm::vec3 v = o.surfaceAt(5.0f, 5.0f).velocity;
        check(glm::length(v) < 6.0f, "and moves at the speed water moves",
              num("orbital speed %.2f m/s", glm::length(v)));

        // The worker: frames arrive without anybody waiting for them.
        const auto g0 = o.generation();
        for (int i = 0; i < 200 && o.generation() < g0 + 3; ++i) {
            o.update(8.0 + i * 0.016);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        check(o.generation() >= g0 + 3, "the worker thread delivers new surfaces",
              num("%.0f new in the wait", static_cast<double>(o.generation() - g0)));

        for (int detail : {128, 256}) {
            Settings d = s;
            d.detail = detail;
            o.configure(d);
            o.computeNow(8.9);   // waits out the worker, so only this is timed
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 10; ++i) o.computeNow(9.0 + i * 0.016);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count() / 10.0;
            std::printf("  --   one surface at detail %d: %.2f ms (on the worker)\n", detail, ms);
        }
    }

    // --- Names ----------------------------------------------------------------
    check(beaufort(0.1f) == 0 && beaufort(7.0f) == 4 && beaufort(25.0f) == 10,
          "wind speeds name their Beaufort force", num("7 m/s is force %.0f", beaufort(7.0f)));
    check(std::fabs(windForHeight(0.21f * 100.0f / 9.81f) - 10.0f) < 0.01f,
          "and a wave height names the wind that raises it", num("%.2f m/s", windForHeight(2.14f)));

    // --- The waves things make ------------------------------------------------
    {
        WaveSim w(128, 0.5f);
        w.follow(glm::vec2(0.0f));
        auto energy = [&] {
            double e = 0.0;
            for (const glm::vec4& t : w.texels()) e += t.x * t.x;
            return e;
        };
        // A crate lands and then floats still.
        WaveSim::Body crate{1, glm::vec2(2.0f, 1.0f), 0.6f, 0.3f};
        w.step(1.0f / 60.0f, {crate});
        double peak = 0.0;
        for (int i = 0; i < 60; ++i) { w.step(1.0f / 60.0f, {crate}); peak = std::max(peak, energy()); }
        check(peak > 1e-3, "a body landing in the water throws rings", num("energy %.4f", peak));
        for (int i = 0; i < 60 * 20; ++i) w.step(1.0f / 60.0f, {crate});
        check(energy() < peak * 0.02, "and one floating still lets them die away",
              num("energy after 20 s %.6f", energy()));

        // A boat at speed drags a wake.
        w.clear();
        double wake = 0.0;
        for (int i = 0; i < 240; ++i) {
            WaveSim::Body boat{2, glm::vec2(-20.0f + i * 0.15f, 0.0f), 1.2f, 0.5f};   // 9 m/s
            w.step(1.0f / 60.0f, {boat});
        }
        for (int i = -8; i <= 8; ++i) wake += std::fabs(w.heightAt(5.0f, i * 1.0f));
        check(wake > 0.01, "a boat at speed leaves a wake behind it", num("sum %.3f m", wake));
        check(std::fabs(w.heightAt(30.0f, 0.0f)) < 0.01f, "and calm water ahead of it",
              num("%.4f m ahead", w.heightAt(30.0f, 0.0f)));

        // Stable at the fastest the dials allow, with a lot thrown at it.
        w.clear();
        w.waveSpeed = 10.0f;
        bool finite = true;
        float most = 0.0f;
        for (int i = 0; i < 60 * 30; ++i) {
            std::vector<WaveSim::Body> bs;
            for (int k = 0; k < 6; ++k)
                bs.push_back({static_cast<std::uint64_t>(10 + k),
                              glm::vec2(std::sin(i * 0.05f + k) * 20.0f, std::cos(i * 0.07f + k) * 20.0f),
                              1.0f, 0.5f + 0.5f * std::sin(i * 0.3f + k)});
            w.step(1.0f / 60.0f, bs);
        }
        for (const glm::vec4& t : w.texels()) {
            if (!std::isfinite(t.x)) finite = false;
            most = std::max(most, std::fabs(t.x));
        }
        check(finite && most < 2.5f, "stays stable at full wave speed under a crowd",
              num("highest %.2f m", most));

        // Following the camera keeps the waves where they are in the world.
        w.waveSpeed = 4.0f;
        w.clear();
        w.step(1.0f / 60.0f, {{3, glm::vec2(4.0f, 4.0f), 1.0f, 0.5f}});
        const float before = w.heightAt(4.0f, 4.0f);
        w.follow(glm::vec2(9.0f, -7.0f));
        const float after = w.heightAt(4.0f, 4.0f);
        check(std::fabs(before) > 0.01f && std::fabs(before - after) < 1e-5f,
              "the patch follows the camera without moving the water",
              num("%.4f m before, %.4f m after", before, after));
    }

    // --- Where the coast is ---------------------------------------------------
    {
        // A straight beach: the ground rises 1 in 20 toward +x and meets the
        // water (level 0) at x = 100.
        auto beach = [](float x, float) { return (x - 100.0f) * 0.05f; };
        std::vector<glm::vec4> m;
        const int n = 128;
        const float cell = 2.0f;
        ShoreMap::build(n, cell, glm::vec2(0.0f, -128.0f), 0.0f, beach, m);
        auto at = [&](float x, float z) {
            const int ix = static_cast<int>(x / cell), iz = static_cast<int>((z + 128.0f) / cell);
            return m[static_cast<std::size_t>(iz) * n + ix];
        };
        const glm::vec4 sea = at(60.0f, 0.0f), dune = at(130.0f, 0.0f);
        check(std::fabs(sea.x - 2.0f) < 0.1f, "forty metres out the water is two metres deep",
              num("depth %.2f m", sea.x));
        check(std::fabs(sea.y - 40.0f) < 2.5f, "and forty metres from the shore",
              num("distance %.1f m", sea.y));
        check(sea.z > 0.95f && std::fabs(sea.w) < 0.1f, "with the shore straight ahead (+x)",
              num("direction %.2f, %.2f", sea.z, sea.w));
        check(dune.y < -25.0f && dune.y > -35.0f && dune.x < 0.0f,
              "thirty metres up the beach is land, thirty metres from the water",
              num("distance %.1f m, depth %.2f m", dune.y, dune.x));

        // No coast anywhere: open sea, no breakers.
        ShoreMap::build(32, cell, glm::vec2(0.0f), 0.0f, [](float, float) { return -50.0f; }, m);
        check(m[100].y > 1000.0f && glm::length(glm::vec2(m[100].z, m[100].w)) < 1e-3f,
              "open water with no shore in reach has none to break on",
              num("distance %.0f m", m[100].y));
    }

    // --- Spray ----------------------------------------------------------------
    {
        // The same straight beach: surf breaking along it should throw spray
        // over the breaking water and nowhere else.
        auto beach = [](float x, float) { return (x - 100.0f) * 0.05f; };
        ShoreMap shore(128, 2.0f);
        for (int i = 0; i < 500 && !shore.valid(); ++i) {
            shore.update(glm::vec2(60.0f, 0.0f), 0.0f, 0.0, beach, true);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        SprayPool pool;
        std::mt19937 rng(7);
        SeaSprayInput in;
        in.shore = &shore;
        in.breakers.height = 1.5f;
        in.breakers.length = 25.0f;
        in.breakers.speed  = 5.0f;
        in.eye = glm::vec3(60.0f, 5.0f, 0.0f);
        in.dt  = 1.0f / 60.0f;
        float minDepth = 1e9f, maxDepth = -1e9f;
        int born = 0;
        for (int f = 0; f < 240; ++f) {
            in.time = f / 60.0f;
            const int before = pool.count();
            emitSeaSpray(in, pool, rng);
            for (int i = before; i < pool.count(); ++i) {
                const float d = shore.sample(pool.particles()[i].pos.x, pool.particles()[i].pos.z).x;
                minDepth = std::min(minDepth, d);
                maxDepth = std::max(maxDepth, d);
            }
            born += pool.count() - before;
            pool.update(in.dt, 0.0f);
        }
        check(born > 100, "breaking surf throws spray", num("%.0f particles in 4 s", born));
        check(maxDepth < 6.0f && minDepth > -3.0f, "only over the surf, not out at sea or up the beach",
              num("born over water %.1f .. %.1f m deep", minDepth, maxDepth));

        SprayPool calm;
        SeaSprayInput none = in;
        none.breakers.height = 0.0f;
        emitSeaSpray(none, calm, rng);
        check(calm.count() == 0, "a calm sea with no surf and no gale throws none", num("%.0f", calm.count()));

        SprayPool splash;
        emitSplash(splash, rng, glm::vec3(0.0f), glm::vec3(0.0f, -6.0f, 0.0f), 0.8f);
        const int big = splash.count();
        SprayPool plop;
        emitSplash(plop, rng, glm::vec3(0.0f), glm::vec3(0.0f, -1.2f, 0.0f), 0.2f);
        check(big > plop.count() * 3 && plop.count() > 0, "a crate dropped hard splashes more than a pebble",
              num("%.0f against %.0f particles", big, plop.count()));
    }

    // --- A box, floated ---------------------------------------------------------
    {
        // A 1 m box of 200 kg on four bottom corners, dropped from half a
        // metre into still water, told to sit 0.3 m deep.
        const float mass = 200.0f, g = 9.81f;
        FloatParams fp;
        fp.sitDepth = 0.3f;
        fp.maxDepth = 1.0f;
        fp.drag     = 2.0f;
        float y = 0.5f, vy = 0.0f;   // bottom of the box
        for (int i = 0; i < 60 * 15; ++i) {
            const float dt = 1.0f / 60.0f;
            float fy = -mass * g;
            for (int c = 0; c < 4; ++c) {
                float sub = 0.0f;
                FloatPoint p{glm::vec3(0.0f, y, 0.0f), glm::vec3(0.0f, vy, 0.0f)};
                fy += floatForce(p, mass * g / 4.0f, 0.0f, glm::vec3(0.0f), fp, sub).y;
            }
            vy += fy / mass * dt;
            y  += vy * dt;
        }
        check(std::fabs(y + 0.3f) < 0.01f && std::fabs(vy) < 0.01f,
              "a box told to sit 0.3 m deep settles 0.3 m deep",
              num("bottom at %.3f m, moving %.3f m/s", y, vy));
    }

    std::printf(g_fails ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
