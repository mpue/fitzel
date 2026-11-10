// worldcheck -- one seed, one region, one whole landscape plan.
//
// The world generator's whole claim is that its parts AGREE, and every way they
// can fail to is invisible from one screenshot of one seed:
//
//   1. A river that runs uphill, or stops in a field. The river solver will cut
//      a channel through anything, so the water still draws -- in a trench.
//   2. A town on ground it cannot stand on. Nothing levels a town site: houses
//      on a lot steeper than the rule allows are simply not built, so a town on
//      a slope arrives as a street grid with holes in it.
//   3. A town in the river, or half in the sea.
//   4. A town no road reaches, or a road that ends a few metres short of the
//      town's edge street -- neither a T nor an X, two ribbons overlapping.
//   5. A road through water that is not on a bridge: the corridor grader digs it
//      into the river bed.
//   6. A bridge span naming a control point that does not exist. RoadSystem
//      skips it in silence and the road dives into the water.
//   7. Farms and rocks on roads, in towns, in rivers.
//   8. A seed that does nothing, a plan that is not deterministic.
//
// No GL, no window: worldgen takes plain data and returns plain data.
//
//   worldcheck                        every region, a dozen seeds each
//   worldcheck --map <out.ppm> [region] [seed] [size]
//                                     the plan drawn as a map, for a look
// Exits non-zero if any measurement fails.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <fitzel/world/Terrain.hpp>

#include "../src/CityPlan.hpp"
#include "../src/RiverGen.hpp"
#include "../src/SandboxMath.hpp"
#include "../src/SunPosition.hpp"
#include "../src/WorldGen.hpp"

namespace {

int g_fails = 0;
sunpos::Place g_place;   // the place --map shades for (the region's)

void check(bool ok, const char* what, const std::string& detail) {
    if (ok) std::printf("  ok   %s -- %s\n", what, detail.c_str());
    else  { std::printf("[FAIL] %s: %s\n", what, detail.c_str()); ++g_fails; }
}

std::string fmt(const char* f, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, f);
    std::vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

float ground(const fitzel::TerrainSettings& s, float x, float z) {
    return fitzel::terrainBaseHeight(s, x, z);
}

float segDist(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    const glm::vec2 ab = b - a;
    const float t = glm::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-9f), 0.0f, 1.0f);
    return glm::length(p - (a + ab * t));
}
float lineDist(glm::vec2 p, const std::vector<glm::vec2>& l) {
    float d = 1e30f;
    for (std::size_t i = 1; i < l.size(); ++i) d = std::min(d, segDist(p, l[i - 1], l[i]));
    return d;
}

worldgen::Params paramsFor(worldgen::Region r, unsigned seed, float size) {
    worldgen::Params p;
    p.seed = seed;
    p.region = r;
    p.look = worldgen::character(r);
    p.size = size;
    // Experiments without a rebuild: WC_VALLEY, WC_OCT, WC_HS, WC_RIDGE, WC_F override the land.
    if (const char* v = std::getenv("WC_VALLEY")) p.look.valleyDepth = static_cast<float>(std::atof(v));
    if (const char* v = std::getenv("WC_OCT"))    p.look.octaves = std::atoi(v);
    if (const char* v = std::getenv("WC_HS"))     p.look.heightScale = static_cast<float>(std::atof(v));
    if (const char* v = std::getenv("WC_RIDGE"))  p.look.ridgeScale = static_cast<float>(std::atof(v));
    if (const char* v = std::getenv("WC_F"))      p.look.frequency = static_cast<float>(std::atof(v));
    return p;
}

worldgen::Plan make(const worldgen::Params& p) {
    return worldgen::plan(p, fitzel::TerrainSettings{}, -1000.0f, ground);
}

float riverReach(rivergen::Preset pr) {
    const rivergen::Style st = rivergen::preset(pr);
    return 0.5f * st.width * std::max(st.widen, 1.0f);
}

// --- The map -------------------------------------------------------------------------

void writeMap(const std::string& path, const worldgen::Plan& pl, double shadeHour = -1.0,
              const sunpos::Place* place = nullptr) {
    const worldgen::Grid& g = pl.grid;
    const int px = 3;   // pixels per cell
    const int W = g.n * px;
    std::vector<unsigned char> img(static_cast<std::size_t>(W * W * 3), 0);
    auto put = [&](int x, int y, glm::vec3 c) {
        if (x < 0 || y < 0 || x >= W || y >= W) return;
        unsigned char* d = &img[static_cast<std::size_t>((y * W + x) * 3)];
        d[0] = static_cast<unsigned char>(glm::clamp(c.r, 0.0f, 1.0f) * 255.0f);
        d[1] = static_cast<unsigned char>(glm::clamp(c.g, 0.0f, 1.0f) * 255.0f);
        d[2] = static_cast<unsigned char>(glm::clamp(c.b, 0.0f, 1.0f) * 255.0f);
    };
    auto toPx = [&](glm::vec2 w, int& x, int& y) {
        x = static_cast<int>((w.x - g.origin.x) / g.cell * px);
        y = static_cast<int>((w.y - g.origin.y) / g.cell * px);
    };
    {
        int w = 0;
        std::vector<std::uint8_t> sh;
        if (shadeHour >= 0.0)
            sh = worldgen::shade(pl, sunpos::at(place ? *place : g_place, static_cast<float>(shadeHour)).dir);
        const std::vector<unsigned char> base = worldgen::image(pl, px, w, sh.empty() ? nullptr : &sh);
        for (int k = 0; k < W * W && w == W; ++k)
            for (int ch = 0; ch < 3; ++ch) img[static_cast<std::size_t>(k * 3 + ch)] = base[static_cast<std::size_t>(k * 4 + ch)];
    }
    auto line = [&](const std::vector<glm::vec2>& l, glm::vec3 c, int thick) {
        for (std::size_t q = 1; q < l.size(); ++q) {
            int x0, y0, x1, y1;
            toPx(l[q - 1], x0, y0);
            toPx(l[q], x1, y1);
            const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0)) + 1;
            for (int s = 0; s <= steps; ++s) {
                const int x = x0 + (x1 - x0) * s / steps, y = y0 + (y1 - y0) * s / steps;
                for (int dy = -thick; dy <= thick; ++dy)
                    for (int dx = -thick; dx <= thick; ++dx) put(x + dx, y + dy, c);
            }
        }
    };
    for (const worldgen::River& r : pl.rivers)
        line(r.points, glm::vec3(0.25f, 0.50f, 0.95f), r.brook ? 0 : 1);
    for (const worldgen::Road& r : pl.roads) {
        line(r.points, glm::vec3(0.95f, 0.92f, 0.80f), 1);
        for (const worldgen::Span& b : r.bridges)
            line({r.points[static_cast<std::size_t>(b.a)], r.points[static_cast<std::size_t>(b.b)]},
                 glm::vec3(0.9f, 0.2f, 0.9f), 2);
    }
    for (const worldgen::Town& t : pl.towns) {
        cityplan::Rule rr;
        cityplan::applyPreset(rr, t.preset);
        rr.grid.center = t.center; rr.grid.sizeX = t.sizeX; rr.grid.sizeZ = t.sizeZ;
        rr.grid.rotation = t.rotation; rr.grid.seed = t.seed;
        const cityplan::Layout L = cityplan::layout(rr, rr.grid);
        for (const cityplan::Street& s : L.streets) line(s.pts, glm::vec3(0.35f, 0.32f, 0.30f), 0);
        for (const glm::vec2& gt : t.gates) line({gt, gt + glm::vec2(1.0f)}, glm::vec3(1.0f, 0.1f, 0.1f), 2);
    }
    for (const worldgen::Farm& f : pl.farms) line({f.pos, f.pos + glm::vec2(0.5f)}, glm::vec3(0.9f, 0.6f, 0.1f), 2);
    for (const worldgen::Rock& r : pl.rocks) line({{r.pos.x, r.pos.z}, {r.pos.x + 0.5f, r.pos.z}}, glm::vec3(0.2f), 1);
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { std::printf("cannot write %s\n", path.c_str()); return; }
    std::fprintf(f, "P6\n%d %d\n255\n", W, W);
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
}

// --- One plan, measured -------------------------------------------------------------------

struct Totals {
    int plans = 0, rivers = 0, brooks = 0, towns = 0, villages = 0, roads = 0, bridges = 0;
    int farms = 0, rocks = 0, uphill = 0, rocksStray = 0;
    float roadKm = 0.0f, steepest = 0.0f, ms = 0.0f, relief = 0.0f, worstSite = 0.0f;
};

void measure(const worldgen::Params& p, const worldgen::Plan& pl, Totals& t, bool verbose) {
    const worldgen::Grid& g = pl.grid;
    const std::string tag = fmt("%s seed %u", worldgen::regionName(p.region), p.seed);
    const float half = 0.5f * p.size;
    auto onEdge = [&](glm::vec2 q) {
        const glm::vec2 d = glm::abs(q - p.center);
        return std::max(d.x, d.y) > half - 2.5f * g.cell;
    };
    auto inSea = [&](glm::vec2 q) { return pl.sea && g.heightAt(q) < pl.seaLevel + 0.5f; };

    // 1. Rivers: downhill to an outlet, and a course the solver can keep falling.
    for (const worldgen::River& r : pl.rivers) {
        const glm::vec2 mouth = r.points.back();
        bool outlet = onEdge(mouth) || inSea(mouth);
        if (r.brook && r.joins >= 0)
            outlet = lineDist(mouth, pl.rivers[static_cast<std::size_t>(r.joins)].points) < 40.0f;
        if (!outlet) check(false, "River reaches an outlet", tag + fmt(" -- ends at %.0f,%.0f", mouth.x, mouth.y));
        if (r.drop < -0.5f) check(false, "River runs downhill", tag + fmt(" -- climbs %.1f m", -r.drop));
        std::vector<int> ptSample;
        const std::vector<glm::vec2> flat = sampleSpline(r.points, false, &ptSample);
        rivergen::Style st = rivergen::preset(r.preset);
        st.maxCut = r.maxCut;
        const rivergen::Course c = rivergen::solve(rivergen::presetKind(r.preset), st, flat,
                                                   std::vector<float>(flat.size(), 0.0f), ptSample,
                                                   [&](float x, float z) { return ground(pl.terrain, x, z); });
        if (c.uphill) ++t.uphill;
        if (verbose)
            std::printf("       %s %s: %.2f km, drop %.0f m, deepest cut %.1f m%s\n",
                        r.brook ? "brook" : "river", rivergen::presetName(r.preset),
                        r.length / 1000.0f, r.drop, c.maxCut, c.uphill ? "  UPHILL" : "");
    }

    // 2./3. Towns: on dry, gentle ground; gates on their own edge streets.
    for (const worldgen::Town& tw : pl.towns) {
        const float R = tw.radius();
        const float grade = tw.relief / (2.0f * R);
        t.worstSite = std::max(t.worstSite, grade);
        if (grade > 2.5f * p.look.siteSlope)
            check(false, "Town site is gentle", tag + fmt(" -- %s rises %.0f m over %.0f m", tw.name.c_str(), tw.relief, 2.0f * R));
        for (const worldgen::River& r : pl.rivers)
            if (lineDist(tw.center, r.points) < 0.5f * std::min(tw.sizeX, tw.sizeZ) + riverReach(r.preset) * 0.5f)
                check(false, "Town is not in the river", tag + " -- " + tw.name);
        if (inSea(tw.center)) check(false, "Town is not in the sea", tag + " -- " + tw.name);
        cityplan::Rule rr;
        cityplan::applyPreset(rr, tw.preset);
        rr.grid.center = tw.center; rr.grid.sizeX = tw.sizeX; rr.grid.sizeZ = tw.sizeZ;
        rr.grid.rotation = tw.rotation; rr.grid.seed = tw.seed;
        const cityplan::Layout L = cityplan::layout(rr, rr.grid);
        for (const glm::vec2& gt : tw.gates) {
            float d = 1e30f;
            for (const cityplan::Street& s : L.streets) d = std::min(d, lineDist(gt, s.pts));
            if (d > 0.5f) check(false, "Gate is on an edge street", tag + fmt(" -- %s, %.1f m off", tw.name.c_str(), d));
        }
    }

    // 4./5./6. Roads.
    for (const worldgen::Road& r : pl.roads) {
        const int np = static_cast<int>(r.points.size());
        for (const worldgen::Span& b : r.bridges)
            if (b.a < 0 || b.b >= np || b.a >= b.b)
                check(false, "Bridge span names real points", tag + fmt(" -- %s: %d..%d of %d", r.name.c_str(), b.a, b.b, np));
        // Water only under a span.
        for (int q = 1; q < np; ++q) {
            bool spanned = false;
            for (const worldgen::Span& b : r.bridges) spanned = spanned || (q > b.a && q <= b.b);
            if (spanned) continue;
            const glm::vec2 a = r.points[static_cast<std::size_t>(q - 1)], c = r.points[static_cast<std::size_t>(q)];
            for (float s = 0.0f; s <= 1.0f; s += 0.1f) {
                const glm::vec2 x = glm::mix(a, c, s);
                bool wet = inSea(x);
                for (const worldgen::River& rv : pl.rivers)
                    wet = wet || lineDist(x, rv.points) < riverReach(rv.preset) + 1.0f;
                if (wet) {
                    check(false, "Road crosses water only on a bridge",
                          tag + fmt(" -- %s at %.0f,%.0f", r.name.c_str(), x.x, x.y));
                    q = np;
                    break;
                }
            }
        }
        // It ends ON a gate of the town it is for, arriving square to the street.
        auto endsAtGate = [&](int town, glm::vec2 end, glm::vec2 prev) {
            if (town < 0) return;
            const worldgen::Town& tw = pl.towns[static_cast<std::size_t>(town)];
            float best = 1e30f; int gi = -1;
            for (std::size_t q = 0; q < tw.gates.size(); ++q) {
                const float d = glm::length(tw.gates[q] - end);
                if (d < best) { best = d; gi = static_cast<int>(q); }
            }
            if (best > 0.5f) { check(false, "Road ends at a town gate", tag + fmt(" -- %s, %.1f m off", r.name.c_str(), best)); return; }
            const glm::vec2 in = glm::normalize(prev - end);
            const float c = glm::dot(in, tw.gateDir[static_cast<std::size_t>(gi)]);
            if (c < 0.8f) check(false, "Road meets the edge street square", tag + fmt(" -- %s, cos %.2f", r.name.c_str(), c));
        };
        if (np >= 2) {
            endsAtGate(r.from, r.points.front(), r.points[1]);
            endsAtGate(r.to, r.points.back(), r.points[static_cast<std::size_t>(np - 2)]);
        }
        t.steepest = std::max(t.steepest, r.maxGrade);
        if (verbose) {
            glm::vec2 w(0.0f);
            const float st = worldgen::steepest(g, r, 30.0f, &w);
            const float d0 = glm::length(w - r.points.front()), d1 = glm::length(w - r.points.back());
            std::printf("       road %s: %.2f km, %d points, %d bridge(s), steepest %.1f%% at %.0f,%.0f "
                        "(%.0f m from start, %.0f from end)\n",
                        r.name.c_str(), r.length / 1000.0f, np, static_cast<int>(r.bridges.size()),
                        st * 100.0f, w.x, w.y, d0, d1);
            if (std::getenv("WC_PROFILE") && st > 1.5f * p.look.maxGrade) {
                // The ground along the road around its steepest stretch, and the
                // control points there: what made it steep.
                std::printf("         profile:");
                for (int q = 0; q < np; ++q) {
                    const glm::vec2 x = r.points[static_cast<std::size_t>(q)];
                    if (glm::length(x - w) < 90.0f)
                        std::printf(" [%d %.0f,%.0f h%.1f]", q, x.x, x.y, g.heightAt(x));
                }
                std::printf("\n");
            }
        }
    }
    if (pl.report.unreached > 0) check(false, "Every town is reached", tag + fmt(" -- %d not", pl.report.unreached));

    // 7. Farms and rocks keep off everything else.
    for (const worldgen::Farm& f : pl.farms) {
        for (const worldgen::Road& r : pl.roads)
            if (lineDist(f.pos, r.points) < 0.5f * r.width + 12.0f)
                check(false, "Farm is off the road", tag + fmt(" -- at %.0f,%.0f", f.pos.x, f.pos.y));
        for (const worldgen::River& rv : pl.rivers)
            if (lineDist(f.pos, rv.points) < riverReach(rv.preset) + 15.0f)
                check(false, "Farm is out of the river", tag + fmt(" -- at %.0f,%.0f", f.pos.x, f.pos.y));
        if (inSea(f.pos)) check(false, "Farm is out of the sea", tag);
    }
    // Rock belongs where the land breaks: on and below crags, and up where the
    // forest gives out. A boulder in the middle of a meadow is the exception, and
    // a map full of them reads as debris, not as a landscape.
    int stray = 0;
    for (const worldgen::Rock& rk : pl.rocks) {
        const glm::vec2 at(rk.pos.x, rk.pos.z);
        bool rocky = pl.treeLine > 0.0f && g.heightAt(at) > pl.treeLine - 40.0f;
        for (int dj = -2; dj <= 2 && !rocky; ++dj)
            for (int di = -2; di <= 2 && !rocky; ++di) {
                const glm::vec2 q = at + glm::vec2(static_cast<float>(di), static_cast<float>(dj)) * g.cell;
                const int ix = std::clamp(static_cast<int>(std::lround((q.x - g.origin.x) / g.cell)), 0, g.n - 1);
                const int iz = std::clamp(static_cast<int>(std::lround((q.y - g.origin.y) / g.cell)), 0, g.n - 1);
                rocky = g.slope[static_cast<std::size_t>(g.index(ix, iz))] > 0.8f * p.look.rockSlope;
            }
        if (!rocky) ++stray;
        for (const worldgen::Town& tw : pl.towns)
            if (glm::length(at - tw.center) < tw.radius() + 40.0f)
                check(false, "Rock is clear of town", tag + " -- " + tw.name);
    }
    t.rocksStray += stray;
    for (const worldgen::Rock& rk : pl.rocks) {
        const glm::vec2 at(rk.pos.x, rk.pos.z);
        for (const worldgen::Road& r : pl.roads)
            if (lineDist(at, r.points) < 0.5f * r.width + 2.0f)
                check(false, "Rock is off the road", tag + fmt(" -- at %.0f,%.0f", at.x, at.y));
        for (const worldgen::Town& tw : pl.towns)
            if (glm::length(at - tw.center) < 0.5f * std::min(tw.sizeX, tw.sizeZ))
                check(false, "Rock is out of town", tag + " -- " + tw.name);
    }

    t.plans++;
    t.rivers += pl.report.rivers; t.brooks += pl.report.brooks;
    t.towns += pl.report.towns; t.villages += pl.report.villages;
    t.roads += pl.report.roads; t.bridges += pl.report.bridges;
    t.farms += pl.report.farms; t.rocks += pl.report.rocks;
    t.roadKm += pl.report.roadKm;
    t.relief += pl.report.highest - pl.report.lowest;
    if (verbose)
        std::printf("       %d river(s), %d brook(s), %d town(s), %d village(s), %d road(s) %.1f km, "
                    "%d bridge(s), %d farm(s), %d rock(s); land %.0f..%.0f m, sea %s; steepest road %.1f%%%s%s\n",
                    pl.report.rivers, pl.report.brooks, pl.report.towns, pl.report.villages, pl.report.roads,
                    pl.report.roadKm, pl.report.bridges, pl.report.farms, pl.report.rocks, pl.report.lowest,
                    pl.report.highest, pl.sea ? fmt("%.0f m", pl.seaLevel).c_str() : "none",
                    pl.report.steepestRoad * 100.0f, pl.report.why.empty() ? "" : " -- ", pl.report.why.c_str());
}

} // namespace

int main(int argc, char** argv) {
    fitzel::setTerrainPresent(true);   // or every height is 0 and every test passes for nothing

    if (argc >= 3 && std::strcmp(argv[1], "--map") == 0) {
        const int region = argc >= 4 ? std::atoi(argv[3]) : 0;
        const unsigned seed = argc >= 5 ? static_cast<unsigned>(std::atoi(argv[4])) : 1u;
        const float size = argc >= 6 ? static_cast<float>(std::atof(argv[5])) : 3000.0f;
        const worldgen::Params p = paramsFor(static_cast<worldgen::Region>(region), seed, size);
        g_place = p.look.place;
        if (const char* d = std::getenv("WC_DAY")) g_place.day = std::atoi(d);
        const auto t0 = std::chrono::steady_clock::now();
        const worldgen::Plan pl = make(p);
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
        Totals t;
        measure(p, pl, t, true);
        for (const worldgen::Town& tw : pl.towns)
            std::printf("       %s %s at %.0f,%.0f, %.0fx%.0f m, rot %.0f, relief %.1f m\n",
                        tw.village ? "village" : "town", tw.name.c_str(), tw.center.x, tw.center.y,
                        tw.sizeX, tw.sizeZ, tw.rotation, tw.relief);
        writeMap(argv[2], pl, std::getenv("WC_SHADE") ? std::atof(std::getenv("WC_SHADE")) : -1.0);
        std::printf("planned in %.0f ms, map in %s\n", ms, argv[2]);
        return g_fails ? 1 : 0;
    }

    std::printf("Determinism and seeds\n");
    {
        const worldgen::Params p = paramsFor(worldgen::Region::Uplands, 7, 3000.0f);
        const worldgen::Plan a = make(p), b = make(p);
        bool same = a.towns.size() == b.towns.size() && a.roads.size() == b.roads.size() &&
                    a.rivers.size() == b.rivers.size() && a.rocks.size() == b.rocks.size();
        for (std::size_t i = 0; same && i < a.roads.size(); ++i) same = a.roads[i].points == b.roads[i].points;
        for (std::size_t i = 0; same && i < a.rivers.size(); ++i) same = a.rivers[i].points == b.rivers[i].points;
        check(same, "Same params, same plan", "seed 7, twice");
        const worldgen::Plan c = make(paramsFor(worldgen::Region::Uplands, 8, 3000.0f));
        const bool differ = c.terrain.seed != a.terrain.seed &&
                            (c.towns.empty() || a.towns.empty() ||
                             glm::length(c.towns[0].center - a.towns[0].center) > 1.0f);
        check(differ, "Another seed, another place", "seed 7 vs 8");
    }

    std::printf("\nThe sun at a place\n");
    {
        auto city = [](const char* n) {
            sunpos::Place p;
            for (const sunpos::City& c : sunpos::cities())
                if (std::string(c.name) == n) {
                    p.latitude = c.latitude; p.longitude = c.longitude;
                    p.utcOffset = c.utcOffset; p.summerTime = c.summerTime;
                }
            p.day = 172;   // 21 June
            return p;
        };
        // Frankfurt, 21 June, Central European Summer Time: the almanac has the
        // sun highest at 13:27 at 63.3 degrees, up at 05:14 and down at 21:39.
        const sunpos::Place fra = city("Frankfurt");
        const sunpos::Sun   noon = sunpos::at(fra, 12.0f);
        const sunpos::Sun   high = sunpos::at(fra, noon.solarNoon);
        check(std::fabs(noon.solarNoon - (13.0f + 27.0f / 60.0f)) < 4.0f / 60.0f, "Frankfurt: highest at 13:27 CEST",
              fmt("%.3f h", noon.solarNoon));
        check(std::fabs(high.elevation - 63.3f) < 0.5f && std::fabs(high.azimuth - 180.0f) < 2.0f,
              "...63.3 degrees high, due south", fmt("%.2f deg at %.1f", high.elevation, high.azimuth));
        check(std::fabs(noon.sunrise - (5.0f + 14.0f / 60.0f)) < 6.0f / 60.0f &&
              std::fabs(noon.sunset - (21.0f + 39.0f / 60.0f)) < 6.0f / 60.0f,
              "...up at 05:14, down at 21:39", fmt("%.3f / %.3f h", noon.sunrise, noon.sunset));
        const sunpos::Sun morning = sunpos::at(fra, 8.0f);
        check(morning.dir.x > 0.5f && morning.elevation > 15.0f && morning.elevation < 35.0f,
              "...in the east in the morning", fmt("%.0f deg high at azimuth %.0f", morning.elevation, morning.azimuth));
        sunpos::Place winter = fra;
        winter.day = sunpos::dayOfYear(12, 21);
        const sunpos::Sun w = sunpos::at(winter, sunpos::at(winter, 12.0f).solarNoon);
        check(std::fabs(w.elevation - (90.0f - 50.11f - 23.44f)) < 0.6f && !sunpos::inSummerTime(winter),
              "...16.5 degrees at midwinter, on standard time", fmt("%.2f deg", w.elevation));
        // South of the equator the noon sun is in the NORTH.
        const sunpos::Place cpt = city("Kapstadt");
        const sunpos::Sun c = sunpos::at(cpt, sunpos::at(cpt, 12.0f).solarNoon);
        check(std::fabs(c.elevation - (90.0f - 33.92f - 23.44f)) < 0.6f && (c.azimuth < 3.0f || c.azimuth > 357.0f) &&
              c.dir.z < 0.0f, "Cape Town: low in the north in June", fmt("%.2f deg at %.1f", c.elevation, c.azimuth));
        const sunpos::Sun tro = sunpos::at(city("Tromsø"), 0.5f);
        check(tro.sunrise < 0.0f && tro.elevation > 0.0f, "Tromsø: the midnight sun", fmt("%.1f deg at 00:30", tro.elevation));
        // A scene from before places is lit exactly as it was.
        sunpos::Place old;
        old.legacy = true; old.latitude = 0.0f; old.legacyDeclination = -10.4f;
        float worst = 0.0f;
        for (float hr = 0.0f; hr < 24.0f; hr += 0.25f) {
            const float H = (hr - 12.0f) / 24.0f * 6.2831853f, lat = 0.0f, dec = glm::radians(-10.4f);
            const glm::vec3 was = glm::normalize(glm::vec3(-std::cos(dec) * std::sin(H),
                std::sin(lat) * std::sin(dec) + std::cos(lat) * std::cos(dec) * std::cos(H),
                -(std::cos(lat) * std::sin(dec) - std::sin(lat) * std::cos(dec) * std::cos(H))));
            worst = std::max(worst, glm::length(was - sunpos::at(old, hr).dir));
        }
        check(worst < 1e-6f, "An old scene's sun is the old sun", fmt("off by %.1e", worst));
    }

    std::printf("\nThe valley in the sun\n");
    {
        // The Alpental at Innsbruck: a June morning lights the valley floor where
        // the villages stand; a December morning leaves it in the mountains' shade.
        int lit = 0, shaded = 0, places = 0;
        float summerShade = 0.0f, winterShade = 0.0f;
        for (int sd = 1; sd <= 6; ++sd) {
            const worldgen::Params p = paramsFor(worldgen::Region::Alpine, static_cast<unsigned>(sd * 37 + 1), 3000.0f);
            const worldgen::Plan pl = make(p);
            sunpos::Place summer = p.look.place, december = p.look.place;
            december.day = sunpos::dayOfYear(12, 21);
            const std::vector<std::uint8_t> a = worldgen::shade(pl, sunpos::at(summer, 11.0f).dir);
            const std::vector<std::uint8_t> b = worldgen::shade(pl, sunpos::at(december, 9.5f).dir);
            auto share = [&](const std::vector<std::uint8_t>& m) {
                int n = 0, d = 0;
                for (std::size_t k = 0; k < m.size(); ++k)
                    if (pl.grid.use[k] != worldgen::use::Sea) { ++n; d += m[k]; }
                return n ? static_cast<float>(d) / static_cast<float>(n) : 0.0f;
            };
            summerShade += share(a) / 6.0f;
            winterShade += share(b) / 6.0f;
            for (const worldgen::Town& t : pl.towns) {
                int i = 0, j = 0;
                i = std::clamp(static_cast<int>(std::lround((t.center.x - pl.grid.origin.x) / pl.grid.cell)), 0, pl.grid.n - 1);
                j = std::clamp(static_cast<int>(std::lround((t.center.y - pl.grid.origin.y) / pl.grid.cell)), 0, pl.grid.n - 1);
                ++places;
                (a[static_cast<std::size_t>(pl.grid.index(i, j))] ? shaded : lit)++;
            }
        }
        check(summerShade < 0.25f, "A June morning lights the valley", fmt("%.0f%% of the land in shade at 11:00", summerShade * 100.0f));
        check(lit * 4 >= places * 3, "...and the villages in it", fmt("%d of %d in the sun", lit, places));
        check(winterShade > summerShade + 0.2f, "A December morning leaves it in the shade",
              fmt("%.0f%% at 9:30", winterShade * 100.0f));
        (void)shaded;
    }

    std::printf("\nThe recipe in the scene file\n");
    {
        // A forest of the user's own trees must come back as their trees, not
        // as the region's presets -- that is the whole of the link to the Tree
        // generator, and a key lost on the way would plant beeches over them.
        worldgen::Params p = paramsFor(worldgen::Region::Fjord, 4242, 4500.0f);
        p.center = {120.0f, -340.0f};
        p.stages.rocks = false;
        p.look.trees = {{"Spruce", 0.7f, "My_spruce.glb"}, {"Birch", 0.3f, ""}};
        p.look.roof = {0.1f, 0.2f, 0.3f};
        nlohmann::json j;
        worldgen::save(j, p);
        worldgen::Params q;
        worldgen::load(nlohmann::json::parse(j.dump()), q);
        check(q.seed == p.seed && q.region == p.region && q.size == p.size && q.center == p.center,
              "Seed, region, size and centre come back", j["region"].dump());
        check(!q.stages.rocks && q.stages.terrain, "Stages come back", j["stages"].dump());
        check(q.look.trees == p.look.trees, "The forest's trees come back, own files included",
              j["look"]["trees"].dump());
        check(q.look.roof == p.look.roof, "The look comes back", j["look"]["roof"].dump());
    }

    for (int r = 0; r < static_cast<int>(worldgen::Region::Count); ++r) {
        const auto region = static_cast<worldgen::Region>(r);
        std::printf("\n%s\n", worldgen::regionName(region));
        Totals t;
        const int seeds = 12;
        for (int s = 1; s <= seeds; ++s) {
            const worldgen::Params p = paramsFor(region, static_cast<unsigned>(s * 37 + r), 3000.0f);
            const auto t0 = std::chrono::steady_clock::now();
            const worldgen::Plan pl = make(p);
            t.ms += std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
            measure(p, pl, t, false);
        }
        const float k = 1.0f / static_cast<float>(std::max(t.plans, 1));
        std::printf("  per plan: %.1f rivers, %.1f brooks, %.1f towns, %.1f villages, %.1f roads (%.1f km), "
                    "%.1f bridges, %.1f farms, %.0f rocks; relief %.0f m; %.0f ms\n",
                    t.rivers * k, t.brooks * k, t.towns * k, t.villages * k, t.roads * k, t.roadKm * k,
                    t.bridges * k, t.farms * k, t.rocks * k, t.relief * k, t.ms * k);
        const worldgen::Character c = worldgen::character(region);
        check(c.rivers == 0 || t.rivers * k >= 0.75f, "Rivers are found", fmt("%.2f per plan", t.rivers * k));
        check(t.towns + t.villages >= static_cast<int>(0.75f * static_cast<float>((c.towns + c.villages) * t.plans)),
              "Towns find sites", fmt("%.1f of %d per plan", (t.towns + t.villages) * k, c.towns + c.villages));
        check(t.steepest <= 2.0f * c.maxGrade, "Roads keep to their grade",
              fmt("steepest %.1f%% (limit %.0f%%)", t.steepest * 100.0f, c.maxGrade * 100.0f));
        check(t.uphill == 0, "River courses fall all the way", fmt("%d forced uphill", t.uphill));
        check(t.ms * k < 1500.0f, "Planning is quick", fmt("%.0f ms", t.ms * k));
        check(t.rocksStray * 10 <= t.rocks, "Rocks lie at crags or above the trees",
              fmt("%d of %d out on gentle ground", t.rocksStray, t.rocks));
    }

    std::printf("\n%s\n", g_fails ? "FAILED" : "all ok");
    return g_fails ? 1 : 0;
}
