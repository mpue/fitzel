// rivercheck -- does the water flow downhill, and does the ground it cut come
// back?
//
// A watercourse is the only thing in this editor that both READS the terrain and
// WRITES it, and that pairing has exactly two ways to go wrong, both of them
// invisible in the viewport:
//
//   1. The bed creeps. Solve a profile against the ground, cut the ground down to
//      it, solve again -- and if the second solve sees its own cut, the channel
//      sinks a little further every time it is touched. Nobody notices at two
//      centimetres a drag. Everybody notices at the end of an afternoon, by which
//      time there is a canyon and no way back.
//   2. The ground does not come back. Move a course and the abandoned trench
//      stays as a scar, or a sculpt beside it is quietly eaten. There is no undo
//      for the height field -- the cut is derived, not stored -- so if the
//      difference published on a move is wrong, the terrain is wrong for good.
//
// Neither is visible by looking, which is why they are measured here: carve the
// same courses ten times over and demand the field be identical, move one and
// demand the vacated ground match the bare terrain to the millimetre, release
// everything and demand the field be empty.
//
// The rest is the promise the tool makes to the author: the author draws WHERE
// the water goes and the solver decides HOW HIGH it stands. So the profile is
// checked for the thing that promise means -- it descends, everywhere, including
// where the line was drawn straight over a ridge.
//
// And because the whole point of running water is that you can see it, it also
// renders the courses through the REAL river.vert/river.frag, over the ground
// they actually cut, and writes the frames out.
//
//   rivercheck [outDir] [shaderDir]
// Exits non-zero if any measurement fails.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <fitzel/world/Terrain.hpp>

// Nothing in the engine writes an image file, so this TU owns the implementation.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/RiverSystem.hpp"
#include "../src/SceneTypes.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int kW = 1100, kH = 620;

int g_fails = 0;

void fail(const char* what, const std::string& detail) {
    std::printf("[FAIL] %s: %s\n", what, detail.c_str());
    ++g_fails;
}
void pass(const char* what, const std::string& detail) {
    std::printf("  ok   %s -- %s\n", what, detail.c_str());
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

GLuint compile(GLenum stage, const std::string& src, const char* what) {
    const GLuint sh = glCreateShader(stage);
    const char* s = src.c_str();
    glShaderSource(sh, 1, &s, nullptr);
    glCompileShader(sh);
    GLint good = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &good);
    if (!good) {
        GLint n = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &n);
        std::vector<char> log(static_cast<std::size_t>(n > 1 ? n : 1));
        glGetShaderInfoLog(sh, n, nullptr, log.data());
        std::printf("[FAIL] %s:\n%s\n", what, log.data());
        ++g_fails;
    }
    return sh;
}

GLuint link(GLuint vs, GLuint fs_) {
    const GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs_);
    glLinkProgram(p);
    GLint good = GL_FALSE;
    glGetProgramiv(p, GL_LINK_STATUS, &good);
    if (!good) {
        char log[2048];
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::printf("[FAIL] link: %s\n", log);
        ++g_fails;
    }
    return p;
}

// --- The world ---------------------------------------------------------------
// Hilly enough that a straight line across it crosses ridges and hollows, which
// is the case the profile solver exists for. Fixed seed: a harness whose terrain
// moves between runs cannot say whether a number changed because the code did.
fitzel::TerrainSettings makeTerrain() {
    fitzel::TerrainSettings s;
    s.heightScale   = 60.0f;
    s.frequency     = 0.0060f;
    s.octaves       = 6;
    s.ridgeScale    = 55.0f;
    s.valleyDepth   = 30.0f;
    s.peakSharpness = 1.3f;
    // Small on purpose. The seed is a FLOAT offset into the noise field, so a
    // date-shaped one (20260830) lands where a float has no fractional bits left
    // and the whole landscape comes out constant to within a metre -- which looks
    // exactly like a working harness on a very boring planet.
    s.seed          = 7.0f;
    return s;
}

// The live edit field plus the publish the engine reads through. One place, so
// the harness cannot drift from what main does.
struct World {
    fitzel::TerrainSettings   settings = makeTerrain();
    fitzel::TerrainEditField  edit;
    fitzel::TerrainPaintField paint;

    World() {
        edit.cell  = 1.0f;
        paint.cell = 1.0f;
        fitzel::setTerrainPresent(true);
        publish();
    }
    void publish() {
        fitzel::setTerrainEditSnapshot(
            std::make_shared<const fitzel::TerrainEditField>(edit));
        fitzel::setTerrainPaintSnapshot(
            std::make_shared<const fitzel::TerrainPaintField>(paint));
    }
    float height(float x, float z) const {
        return fitzel::terrainHeight(settings, x, z);
    }
    float bare(float x, float z) const {
        return fitzel::terrainBaseHeight(settings, x, z);
    }
};

// The highest ground in a square, sampled coarsely. Where a stream starts.
glm::vec2 highPoint(const World& w, glm::vec2 centre, float span, float step) {
    glm::vec2 best = centre;
    float bestH = -1e30f;
    for (float z = -span; z <= span; z += step)
        for (float x = -span; x <= span; x += step) {
            const glm::vec2 q = centre + glm::vec2(x, z);
            const float h = w.height(q.x, q.y);
            if (h > bestH) { bestH = h; best = q; }
        }
    return best;
}

// The gentlest ground in a square. A meander is a LOWLAND habit -- the solver
// suppresses it wherever there is a gradient -- so testing that it happens at all
// means first finding somewhere it is allowed to.
glm::vec2 gentlePoint(const World& w, glm::vec2 centre, float span, float step) {
    glm::vec2 best = centre;
    float bestS = 1e30f;
    for (float z = -span; z <= span; z += step)
        for (float x = -span; x <= span; x += step) {
            const glm::vec2 q = centre + glm::vec2(x, z);
            const float e = 30.0f;
            const float dx = w.height(q.x + e, q.y) - w.height(q.x - e, q.y);
            const float dz = w.height(q.x, q.y + e) - w.height(q.x, q.y - e);
            const float sl = std::sqrt(dx * dx + dz * dz) / (2.0f * e);
            if (sl < bestS) { bestS = sl; best = q; }
        }
    return best;
}

// The line a river takes across gentle country: the FLATTEST descent, not the
// steepest. Water in a lowland follows the shallowest way down it can find and
// only plunges where it has no choice -- which is also, conveniently, the route
// that stays out of the gorges this test landscape is full of.
std::vector<glm::vec2> gentleDownhill(const World& w, glm::vec2 start, int pts,
                                      float step, glm::vec2 bias) {
    std::vector<glm::vec2> out{start};
    glm::vec2 p = start;
    glm::vec2 last = glm::normalize(bias);
    for (int i = 1; i < pts; ++i) {
        const float here = w.height(p.x, p.y);
        float bestCost = 1e30f;
        glm::vec2 bestP = p, bestD = last;
        for (int a = 0; a < 48; ++a) {
            const float th = a * 6.2831853f / 48.0f;
            const glm::vec2 dir(std::cos(th), std::sin(th));
            // Never back the way it came. Water in a valley has somewhere to be,
            // and a route that doubles back would hand the generator a fold that
            // the harness then measures as the generator's.
            if (glm::dot(dir, last) < -0.3f) continue;
            const glm::vec2 q = p + dir * step;
            const float drop = here - w.height(q.x, q.y);
            if (drop <= 0.0f) continue;                    // must go downhill
            // Cheapest is a small, steady drop in roughly the chosen direction.
            const float cost = drop - glm::dot(dir, bias) * step * 0.06f;
            if (cost < bestCost) { bestCost = cost; bestP = q; bestD = dir; }
        }
        if (bestP == p) break;
        p = bestP;
        last = bestD;
        out.push_back(p);
    }
    return out;
}

// A line an author would actually draw: start high and click downhill. Steepest
// descent sampled on a RING rather than from a finite-difference gradient, so
// the walk steps over the one-cell dimples the noise is full of instead of
// stopping in the first one.
std::vector<glm::vec2> downhill(const World& w, glm::vec2 start, int pts,
                                float step, glm::vec2 bias = glm::vec2(0.0f)) {
    std::vector<glm::vec2> out{start};
    glm::vec2 p = start;
    glm::vec2 last = (glm::length(bias) > 1e-4f) ? glm::normalize(bias)
                                                 : glm::vec2(1.0f, 0.0f);
    int stalls = 0;
    for (int i = 1; i < pts; ++i) {
        float lowest = 1e30f;
        glm::vec2 lowestP = p, lowestD = last;
        const float here = w.height(p.x, p.y);
        for (int a = 0; a < 32; ++a) {
            const float th = a * 6.2831853f / 32.0f;
            glm::vec2 dir(std::cos(th), std::sin(th));
            // Never back the way it came: a walk that oscillates between two
            // cells draws a hairpin, and the fold that comes out of one is the
            // route's, not the generator's.
            if (glm::dot(dir, last) < -0.3f) continue;
            // A nudge in one direction keeps the walk from doubling back on
            // itself, which a pure steepest descent will do the moment two
            // neighbouring samples tie.
            const float pull = glm::dot(dir, bias) * 0.35f;
            const glm::vec2 q = p + dir * step;
            const float h = w.height(q.x, q.y) - pull * step;
            if (h < lowest) { lowest = h; lowestP = q; lowestD = dir; }
        }
        if (lowestP == p) break;
        last = lowestD;
        // Uphill is allowed, but not for long: an author's line does cross a
        // rise now and then, and the profile solver is the thing that has to
        // cope with it -- but a walk that climbs forever is not a river.
        if (lowest >= here) { if (++stalls > 2) break; } else { stalls = 0; }
        p = lowestP;
        out.push_back(p);
    }
    return out;
}

// A snapshot of the height field, so "did the cut move" is a measurement in
// METRES rather than a bit comparison. That distinction is the whole point: the
// field is a float accumulation, so re-publishing the same cut a hundred times
// walks the last bit around a little. What must not happen is a TREND -- a bed
// that sinks a fraction every time it is touched is a canyon by the evening, and
// it shows up as a mean that drifts one way, not as noise.
struct Drift { float worst = 0.0f; double mean = 0.0; };

Drift driftBetween(const fitzel::TerrainEditField& a,
                   const fitzel::TerrainEditField& b) {
    Drift d;
    double sum = 0.0;
    long long n = 0;
    auto look = [](const fitzel::TerrainEditField& f, std::int64_t k) {
        const auto it = f.deltas.find(k);
        return it == f.deltas.end() ? 0.0f : it->second;
    };
    for (const auto& [k, v] : a.deltas) {
        const float diff = look(b, k) - v;
        d.worst = std::max(d.worst, std::fabs(diff));
        sum += diff; ++n;
    }
    for (const auto& [k, v] : b.deltas)
        if (!a.deltas.count(k)) {
            d.worst = std::max(d.worst, std::fabs(v));
            sum += v; ++n;
        }
    d.mean = n ? sum / static_cast<double>(n) : 0.0;
    return d;
}

// Is (x,z) inside some OTHER course's corridor? Where two are, the deeper cut
// wins and the shallower one's own section is not what is on the ground -- which
// is the confluence working, not a defect. Anything measuring one course's cut
// has to step around those cells.
bool contested(const RiverSystem& rv, int self, glm::vec2 xz) {
    for (int i = 0; i < static_cast<int>(rv.paths.size()); ++i) {
        if (i == self || !rv.paths[i].enabled) continue;
        const rivergen::Course& c = rv.course(i);
        for (std::size_t k = 0; k + 1 < c.line.size(); ++k) {
            const glm::vec2 a(c.line[k].x, c.line[k].z);
            const glm::vec2 b(c.line[k + 1].x, c.line[k + 1].z);
            const glm::vec2 ab = b - a;
            const float len2 = glm::dot(ab, ab);
            const float t = len2 > 1e-9f
                ? glm::clamp(glm::dot(xz - a, ab) / len2, 0.0f, 1.0f) : 0.0f;
            const float reach = rivergen::reach(rv.paths[i].style,
                                                glm::mix(c.half[k], c.half[k + 1], t));
            if (glm::distance(xz, a + ab * t) < reach + 2.0f) return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    // Unbuffered, always. Redirect this harness to a file and stdio goes
    // block-buffered; crash it and the whole log is still in that buffer, so the
    // one run that most needed to say where it got to says nothing at all.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path outDir = (argc > 1) ? fs::path(argv[1]) : fs::path(".");
    const fs::path shDir  = (argc > 2) ? fs::path(argv[2])
                                       : fs::path("assets/shaders");
    // How hard it is raining in the pictures. Off by default -- every existing
    // shot is a fair-weather shot and has to stay one -- but the rings on running
    // water have nowhere else to be looked at: they are a normal perturbation,
    // so they show up in the reflection or they show up nowhere.
    float rainRings = 0.0f;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--rain") rainRings = std::strtof(argv[i + 1], nullptr);
    std::error_code ec;
    fs::create_directories(outDir, ec);

    if (!glfwInit()) { std::printf("[FAIL] glfwInit\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "rivercheck", nullptr, nullptr);
    if (!win) { std::printf("[FAIL] no GL 3.3 core context\n"); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(glfwGetProcAddress)) {
        std::printf("[FAIL] glad\n"); return 2;
    }
    std::printf("GL %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)));

    World world;
    // The stones and reeds find-or-create their two shared materials in here, so
    // the harness has to own a library even though it has no project.
    std::vector<MaterialDef> mats;
    RiverSystem rivers;
    rivers.baseAt    = [&world](float x, float z) { return world.bare(x, z); };
    rivers.edits     = &world.edit;
    rivers.materials = &mats;

    // Three courses, each drawn the way an author would: a line across the
    // landscape, with no heights given anywhere.
    //
    //  - A brook drawn straight over whatever is in the way, which is the case
    //    the descent guarantee exists for.
    //  - A river drawn the other way round (mouth first), to make the flow guess
    //    work for its living.
    //  - A canal on the flat, where minSlope is 0 and the water must still not
    //    climb.
    // A bed material on every course: the paint rides the same difference
    // discipline as the cut, so it has the same two ways to go wrong and has to
    // be measured the same way.
    const int brook = rivers.addPath(rivergen::Preset::MountainStream, "Brook");
    {
        const std::vector<glm::vec2> pts =
            downhill(world, highPoint(world, glm::vec2(0.0f), 240.0f, 20.0f), 14,
                     32.0f, glm::vec2(1.0f, 0.3f));
        for (int i = 0; i < static_cast<int>(pts.size()); ++i)
            rivers.insertPoint(brook, i, pts[i]);
    }

    // Drawn from the MOUTH upward, so the flow guess has to earn its living.
    const int river = rivers.addPath(rivergen::Preset::River, "River");
    {
        std::vector<glm::vec2> pts =
            downhill(world, highPoint(world, glm::vec2(600.0f, 500.0f), 240.0f, 20.0f),
                     10, 60.0f, glm::vec2(-0.4f, 1.0f));
        std::reverse(pts.begin(), pts.end());
        for (int i = 0; i < static_cast<int>(pts.size()); ++i)
            rivers.insertPoint(river, i, pts[i]);
    }

    // The case the meander exists for: a lowland river across gentle ground,
    // drawn dead straight. Everything interesting about it -- the bends, the
    // pools, the riffles, the deep line against the outer bank -- has to be
    // derived, because none of it was drawn.
    const int lowland = rivers.addPath(rivergen::Preset::River, "Lowland river");
    {
        // Started on the gentlest ground in a wide search and routed by the
        // flattest descent from there. This landscape is rugged on purpose (it
        // has to be, for the falls), and a lowland course dropped into a gorge
        // would only be testing the meander SUPPRESSION again -- which the
        // mountain stream already covers.
        const glm::vec2 a = gentlePoint(world, glm::vec2(-1200.0f, 900.0f),
                                        1500.0f, 60.0f);
        const std::vector<glm::vec2> pts =
            gentleDownhill(world, a, 14, 60.0f, glm::vec2(0.7f, 0.7f));
        for (int i = 0; i < static_cast<int>(pts.size()); ++i)
            rivers.insertPoint(lowland, i, pts[i]);
    }

    const int canal = rivers.addPath(rivergen::Preset::Canal, "Canal");
    for (int i = 0; i <= 4; ++i)
        rivers.insertPoint(canal, i, glm::vec2(-200.0f + i * 90.0f, -900.0f));

    for (RiverSystem::Path& pp : rivers.paths) {
        pp.style.bankLayer = 2;
        if (pp.style.reeds <= 0.0f) pp.style.reeds = 8.0f;   // exercise both halves
    }
    rivers.touch();

    // --- 1) The profile ------------------------------------------------------
    rivers.update();
    for (int i = 0; i < static_cast<int>(rivers.paths.size()); ++i) {
        const rivergen::Course& c = rivers.course(i);
        const std::string name = rivers.paths[i].name;
        if (c.empty()) { fail("course solved", name + " came out empty"); continue; }

        float worstRise = 0.0f;
        for (std::size_t k = 1; k < c.line.size(); ++k)
            worstRise = std::max(worstRise, c.line[k].y - c.line[k - 1].y);
        char d[200];
        std::snprintf(d, sizeof d,
                      "%s: %.0f m, falls %.1f m, worst rise %.4f m, %d fall(s), "
                      "cut %.1f m, %s", name.c_str(), c.length, c.drop, worstRise,
                      c.falls, c.maxCut, c.reversed ? "flows back-to-front" : "flows as drawn");
        // The promise: the water descends. `uphill` is the one licensed exception
        // (the maxCut rail bit), and it is REPORTED rather than silent -- so the
        // test is that a rise implies the flag, not that no rise ever happens.
        if (worstRise > 1e-3f && !c.uphill)
            fail("descends", std::string(d) + " -- rises without saying so");
        else
            pass("descends", d);

        // The bed is under the water, everywhere. A section that comes out above
        // its own surface is a channel drawn over solid ground.
        float worstBed = -1e9f;
        for (std::size_t k = 0; k < c.bed.size(); ++k)
            worstBed = std::max(worstBed, c.bed[k] - c.line[k].y);
        if (worstBed > -0.01f)
            fail("bed under water", name + ": bed reaches the surface");
    }

    // --- 1a) It bends rather than kinks -------------------------------------
    // The measurement the falls exist for. A profile that drops its whole height
    // between two stations two metres apart draws a surface that turns a right
    // angle at the lip, runs down the rock as a flat ribbon and turns another one
    // at the bottom -- and those two creases are the first thing anybody sees in
    // a waterfall. They are also invisible to every other check here: the water
    // still descends, the bed is still under it, nothing is out of tolerance.
    //
    // So the worst CORNER in the centreline is measured directly, in degrees,
    // over segments long enough not to be sampling noise. Water bends; it does
    // not fold.
    {
        float worst = 0.0f, worstAt = 0.0f;
        glm::vec3 worstIn(0.0f), worstOut(0.0f);
        const char* where = "";
        for (int i = 0; i < static_cast<int>(rivers.paths.size()); ++i) {
            const rivergen::Course& c = rivers.course(i);
            for (std::size_t k = 1; k + 1 < c.line.size(); ++k) {
                const glm::vec3 a = c.line[k]     - c.line[k - 1];
                const glm::vec3 b = c.line[k + 1] - c.line[k];
                if (glm::length(a) < 0.15f || glm::length(b) < 0.15f) continue;
                const float d = glm::clamp(
                    glm::dot(glm::normalize(a), glm::normalize(b)), -1.0f, 1.0f);
                const float deg = glm::degrees(std::acos(d));
                if (deg > worst) {
                    worst = deg; where = rivers.paths[i].name.c_str();
                    worstAt = c.s[k];
                    worstIn = a; worstOut = b;
                }
            }
        }
        char d[180];
        std::snprintf(d, sizeof d,
                      "worst corner %.1f deg (%s, at %.0f m; %.2f m over %.2f m "
                      "into %.2f m over %.2f m)",
                      worst, where, worstAt, -worstIn.y,
                      glm::length(glm::vec2(worstIn.x, worstIn.z)), -worstOut.y,
                      glm::length(glm::vec2(worstOut.x, worstOut.z)));
        // A lip rounded over a ballistic arc turns maybe fifteen degrees from one
        // station to the next; an unrounded one turns ninety. Anything over forty
        // is a crease, whatever else it is.
        if (worst > 40.0f) fail("water bends, not kinks", d);
        else               pass("water bends, not kinks", d);
    }

    // --- 1b) It wanders, and it is not one width or one depth ---------------
    // A straight line was drawn; a river has to come out. Measured on the lowland
    // course alone, because the others are on gradients steep enough that the
    // solver is SUPPOSED to straighten them -- a mountain stream that meanders
    // would be the defect.
    {
        const rivergen::Course& c = rivers.course(lowland);
        const float span = glm::distance(
            glm::vec2(c.line.front().x, c.line.front().z),
            glm::vec2(c.line.back().x,  c.line.back().z));
        const float sinuosity = span > 1.0f ? c.length / span : 1.0f;

        float wLo = 1e30f, wHi = 0.0f, dLo = 1e30f, dHi = 0.0f, shHi = 0.0f;
        for (float h : c.half) { wLo = std::min(wLo, h); wHi = std::max(wHi, h); }
        for (float d : c.deep) { dLo = std::min(dLo, d); dHi = std::max(dHi, d); }
        for (float sh : c.shift) shHi = std::max(shHi, std::fabs(sh));

        char d[220];
        std::snprintf(d, sizeof d,
                      "sinuosity %.3f, %.1f-%.1f m wide, %.2f-%.2f m deep, "
                      "deep line up to %.2f m off centre",
                      sinuosity, wLo * 2.0f, wHi * 2.0f, dLo, dHi, shHi);
        if (sinuosity < 1.02f)          fail("meanders", d);
        else if (wHi < wLo * 1.15f)     fail("width varies", d);
        else if (dHi < dLo * 1.15f)     fail("depth varies", d);
        else if (shHi < 0.2f)           fail("bend scours", d);
        else                            pass("wanders and varies", d);

        // A pool is where the bend is, and a riffle is where the crossings are --
        // that phase relationship is the whole reason the three come off one
        // wave, and getting it backwards would look plausible and be wrong.
        double bendDepth = 0.0, crossDepth = 0.0;
        int bends = 0, crosses = 0;
        for (std::size_t k = 0; k < c.wander.size(); ++k) {
            if (std::fabs(c.wander[k]) > 0.8f)      { bendDepth  += c.deep[k]; ++bends; }
            else if (std::fabs(c.wander[k]) < 0.2f) { crossDepth += c.deep[k]; ++crosses; }
        }
        if (bends > 0 && crosses > 0) {
            const double bd = bendDepth / bends, cd = crossDepth / crosses;
            char e[160];
            std::snprintf(e, sizeof e,
                          "%.2f m deep at the bends, %.2f m at the crossings",
                          bd, cd);
            if (bd <= cd) fail("pools sit at the bends", e);
            else          pass("pools sit at the bends", e);
        }
    }

    // --- 1c) The pattern can only travel one way ----------------------------
    // The surface texture is scrolled by shifting `Course::flow` against one
    // clock. For the pattern to move downstream everywhere and never tear, that
    // coordinate has to advance monotonically along the course, and its rate has
    // to stay inside the band the local speed can produce (1x calm to 3x white).
    // A coordinate that stalled or ran backwards anywhere is a stretch where the
    // water visibly flows the wrong way -- which is not a thing a still picture
    // can show, so it is measured.
    {
        float worstRate = 1e30f, fastest = 0.0f;
        bool  backwards = false;
        for (int i = 0; i < static_cast<int>(rivers.paths.size()); ++i) {
            const rivergen::Course& c = rivers.course(i);
            for (std::size_t k = 1; k < c.flow.size(); ++k) {
                const float ds = c.sSurf[k] - c.sSurf[k - 1];
                const float df = c.flow[k] - c.flow[k - 1];
                if (df < -1e-6f) backwards = true;
                if (ds < 1e-4f) continue;
                worstRate = std::min(worstRate, df / ds);
                fastest   = std::max(fastest, ds / std::max(df, 1e-6f));
            }
        }
        char d[160];
        std::snprintf(d, sizeof d,
                      "flow coordinate advances at %.3f--1.000 of arclength "
                      "(pattern up to %.2fx speed)", worstRate, fastest);
        if (backwards || worstRate < 0.30f) fail("flow runs one way", d);
        else                                pass("flow runs one way", d);
    }

    // --- 1d) No folds -------------------------------------------------------
    // A bend tighter than the channel is wide is not a bend: neighbouring
    // sections overlap and cut through each other's inside, and the water
    // surface self-intersects. Two things prevent it -- the course is eased, and
    // where that is not enough the CHANNEL pinches (see the width clamp in
    // solve). The second is arithmetic: half <= 0.8 R puts the radius at 0.625
    // of the full width or better, whatever line was drawn. So this is not an
    // aspiration to be tuned, it is a bound that either holds or has been
    // broken by an edit.
    //
    // Measured on every course, dug channels included: the clamp does not care
    // whether the easing was switched off.
    {
        float worst = 1e30f, worstAt = 0.0f, worstLen = 0.0f;
        const char* where = "";
        for (int i = 0; i < static_cast<int>(rivers.paths.size()); ++i) {
            const rivergen::Course& c = rivers.course(i);
            // Over a window in METRES, and the same one the clamp uses. Not in
            // stations: a fall is resampled to centimetre spacing, and a turn
            // measured across two of those is measuring the sampling.
            for (std::size_t k = 2; k + 2 < c.dir.size(); ++k) {
                std::size_t a = k, b = k;
                while (a > 0 && c.s[k] - c.s[a] < 4.0f) --a;
                while (b + 1 < c.dir.size() && c.s[b] - c.s[k] < 4.0f) ++b;
                const float dot = glm::clamp(glm::dot(c.dir[a], c.dir[b]),
                                             -1.0f, 1.0f);
                const float run = std::max(c.s[b] - c.s[a], 1e-3f);
                const float turn = std::acos(dot) / run;          // 1 / radius
                if (turn < 1e-5f) continue;
                const float widths = (1.0f / turn) / (c.half[k] * 2.0f);
                if (widths < worst) {
                    worst = widths; where = rivers.paths[i].name.c_str();
                    worstAt = c.s[k]; worstLen = c.length;
                }
            }
        }
        char d[160];
        std::snprintf(d, sizeof d,
                      "tightest bend is %.2f channel widths across "
                      "(%s, at %.0f m of %.0f)", worst, where, worstAt, worstLen);
        // 0.60, just under the 0.625 the clamp guarantees: anything below it
        // means the clamp is not doing what its arithmetic says.
        if (worst < 0.60f) fail("no folds", d);
        else               pass("no folds", d);
    }

    // --- 2) The cut ----------------------------------------------------------
    glm::vec2 mn, mx;
    const bool cut = rivers.carve(world.edit, world.paint, mn, mx);
    world.publish();
    if (!cut) fail("carve", "nothing was cut at all");
    const fitzel::TerrainEditField afterFirst = world.edit;
    std::printf("  cut: %d cells\n", static_cast<int>(world.edit.deltas.size()));

    // The water is standing in the channel it cut, not on the hillside it was
    // drawn over. Measured at the centreline, where the section is deepest.
    {
        float worst = 0.0f;
        for (int i = 0; i < static_cast<int>(rivers.paths.size()); ++i) {
            const rivergen::Course& c = rivers.course(i);
            const rivergen::Style& st = rivers.paths[i].style;
            for (std::size_t k = 4; k + 4 < c.line.size(); k += 7) {
                if (contested(rivers, i, glm::vec2(c.line[k].x, c.line[k].z))) continue;
                // Not at a step, and not within reach of one. The height field
                // is a one-metre grid sampled bilinearly, and around a fall the
                // surface it is asked to hold moves metres between neighbouring
                // nodes -- so the interpolated answer is half a metre out by
                // construction and measuring it would only be measuring the grid.
                //
                // "Within reach" and not merely "on it", because the flat POOL
                // above a lip is not itself steep: its own cells are shared with
                // stations already over the edge, and it is those that carry the
                // discontinuity. Judged on the bed the cut is actually being
                // asked for rather than on the gradient, so it catches a plunge
                // scoop and a collapsing pool as well as a fall.
                bool nearStep = false;
                for (int q = -3; q <= 3 && !nearStep; ++q) {
                    const std::size_t j = k + q;
                    if (j >= c.bed.size()) continue;
                    if (std::fabs(c.bed[j] - c.bed[k]) > 0.5f) nearStep = true;
                }
                // Nor on a bend tighter than the channel is wide. There the
                // sections of neighbouring stations overlap on the inside of the
                // curve and several of them claim the same cells; the deepest
                // wins (see RiverSystem::cutInto), which is both what the ground
                // should do -- a bend that tight is where an oxbow gets cut
                // through -- and not what this station's own section asks for.
                float turn = 0.0f;
                if (k >= 2 && k + 2 < c.dir.size()) {
                    const float dot = glm::clamp(glm::dot(c.dir[k - 2], c.dir[k + 2]),
                                                 -1.0f, 1.0f);
                    const float run = std::max(c.s[k + 2] - c.s[k - 2], 1e-3f);
                    turn = std::acos(dot) / run;          // 1 / radius
                }
                const bool tightBend = turn * (c.half[k] * 2.5f) > 1.0f;
                if (c.slope[k] > st.rapidSlope || nearStep || tightBend) continue;
                // At the DEEP LINE, not at the centreline: on a bend the
                // deepest part of the section is against the outer bank, and the
                // middle of the channel is a slope on the way there.
                const glm::vec2 dir = c.dir[k];
                const glm::vec3 side(dir.y, 0.0f, -dir.x);
                const glm::vec3 at = c.line[k] + side * c.shift[k];
                const float g = world.height(at.x, at.z);
                const float want = rivergen::sectionHeight(
                    rivers.paths[i].kind, st, c.shift[k], c.half[k], c.shift[k],
                    c.line[k].y, c.bed[k], c.line[k].y);
                worst = std::max(worst, std::fabs(g - want));
            }
        }
        char d[120];
        std::snprintf(d, sizeof d, "worst centreline error %.3f m", worst);
        // A cell grid samples the section at 1 m, so a metre of lateral offset
        // between a station and its nearest node costs a few centimetres.
        if (worst > 0.35f) fail("bed was cut", d); else pass("bed was cut", d);
    }

    // The bed material actually reached the ground.
    {
        int painted = 0;
        const rivergen::Course& c = rivers.course(brook);
        for (std::size_t k = 4; k + 4 < c.line.size(); k += 7) {
            const glm::vec4 w = world.paint.sample(c.line[k].x, c.line[k].z);
            if (w[2] > 0.3f) ++painted;
        }
        char d[120];
        std::snprintf(d, sizeof d, "%d of %d midstream samples on layer 3",
                      painted, static_cast<int>((c.line.size() - 8) / 7 + 1));
        if (painted == 0) fail("bed material laid", d);
        else              pass("bed material laid", d);
    }

    // --- 3) The creep --------------------------------------------------------
    // The one that matters. Re-solve and re-cut twenty-five times without
    // touching a single control point; if the solver can see its own cut, the
    // bed sinks a fraction each time and the mean walks one way. Float noise in
    // the field's own accumulation is not that and must not be mistaken for it,
    // hence the tolerance -- a real creep is centimetres, this allows microns.
    // The COURSE itself first: if the solved line moves, the field was always
    // going to. Separating the two is the difference between "the cut drifts"
    // and "the cut drifts because the solver is reading its own bed".
    std::vector<std::vector<glm::vec3>> lines0;
    for (const RiverSystem::Run& r : rivers.runs()) lines0.push_back(r.course.line);
    for (int pass_ = 0; pass_ < 25; ++pass_) {
        rivers.touch();
        rivers.carve(world.edit, world.paint, mn, mx);
        world.publish();
    }
    {
        float moved = 0.0f;
        for (std::size_t i = 0; i < lines0.size() && i < rivers.runs().size(); ++i) {
            const auto& a = lines0[i];
            const auto& b = rivers.runs()[i].course.line;
            if (a.size() != b.size()) { moved = 1e9f; break; }
            for (std::size_t k = 0; k < a.size(); ++k)
                moved = std::max(moved, glm::distance(a[k], b[k]));
        }
        char det[120];
        std::snprintf(det, sizeof det, "25 re-solves moved the line %.6f m", moved);
        if (moved > 1.0e-3f) fail("course is stable", det);
        else                 pass("course is stable", det);
    }
    {
        const Drift d = driftBetween(afterFirst, world.edit);
        char det[160];
        std::snprintf(det, sizeof det,
                      "25 re-cuts: worst cell moved %.6f m, mean %+.3e m",
                      d.worst, d.mean);
        if (d.worst > 5.0e-3f || std::fabs(d.mean) > 1.0e-5)
            fail("no creep", det);
        else
            pass("no creep", det);
    }

    // --- 4) The ground comes back -------------------------------------------
    // Move a course a long way and demand the ground it left match the BARE
    // terrain. This is the scar case, and there is no undo for the height field.
    {
        std::vector<glm::vec2> before = rivers.paths[canal].points;
        for (glm::vec2& q : rivers.paths[canal].points) q.y -= 420.0f;
        rivers.touch(canal);
        rivers.carve(world.edit, world.paint, mn, mx);
        world.publish();
        float worst = 0.0f;
        for (const glm::vec2& q : before)
            for (float dx = -6.0f; dx <= 6.0f; dx += 2.0f) {
                const glm::vec2 at(q.x + dx, q.y);
                if (contested(rivers, canal, at)) continue;
                worst = std::max(worst, std::fabs(world.height(at.x, at.y) -
                                                  world.bare(at.x, at.y)));
            }
        char d[120];
        std::snprintf(d, sizeof d, "worst leftover %.4f m", worst);
        if (worst > 0.01f) fail("vacated ground restored", d);
        else               pass("vacated ground restored", d);
        rivers.paths[canal].points = before;
        rivers.touch(canal);
        rivers.carve(world.edit, world.paint, mn, mx);
        world.publish();
    }

    // --- 5) A sculpt beside the water survives -------------------------------
    // The cut publishes a DIFFERENCE, so anything else in the field has to come
    // through it untouched. A river that ate the hill somebody sculpted next to
    // it would be discovered a long way from here.
    {
        const glm::vec2 spot(-260.0f, 300.0f);   // well clear of every course
        world.edit.raise(spot, 12.0f, 5.0f);
        world.publish();
        const float raised = world.height(spot.x, spot.y);
        rivers.touch();
        rivers.carve(world.edit, world.paint, mn, mx);
        world.publish();
        const float after = world.height(spot.x, spot.y);
        char d[140];
        std::snprintf(d, sizeof d, "hill kept %.4f of %.4f m",
                      after - world.bare(spot.x, spot.y),
                      raised - world.bare(spot.x, spot.y));
        if (std::fabs(after - raised) > 1e-3f) fail("sculpt survives the cut", d);
        else                                   pass("sculpt survives the cut", d);
    }

    // --- 6) Play queries -----------------------------------------------------
    {
        const rivergen::Course& c = rivers.course(river);
        int inside = 0, flowing = 0;
        for (std::size_t k = 5; k + 5 < c.line.size(); k += 5) {
            float surf = 0.0f, depth = 0.0f;
            glm::vec2 flow(0.0f);
            if (rivers.sample(glm::vec2(c.line[k].x, c.line[k].z), surf, &depth, &flow)) {
                ++inside;
                if (glm::length(flow) > 0.05f) ++flowing;
                if (std::fabs(surf - c.line[k].y) > 0.2f)
                    fail("sample height", "the surface query disagrees with the course");
                if (depth < 0.05f)
                    fail("sample depth", "no water at the middle of the channel");
            }
        }
        char d[120];
        std::snprintf(d, sizeof d, "%d midstream points, %d carrying a current",
                      inside, flowing);
        if (inside == 0 || flowing == 0) fail("sample", d); else pass("sample", d);
    }

    // --- 7) Release ----------------------------------------------------------
    // Everything given back. Run last, because it takes the world apart.
    {
        const fitzel::TerrainEditField keep = world.edit;
        rivers.release(world.edit, world.paint, mn, mx);
        world.publish();
        float worst = 0.0f;
        for (int i = 0; i < static_cast<int>(rivers.paths.size()); ++i) {
            const rivergen::Course& c = rivers.course(i);
            for (std::size_t k = 0; k < c.line.size(); k += 9)
                worst = std::max(worst, std::fabs(world.height(c.line[k].x, c.line[k].z) -
                                                  world.bare(c.line[k].x, c.line[k].z)));
        }
        float paintLeft = 0.0f;
        for (const auto& [k, w] : world.paint.weights)
            paintLeft = std::max({paintLeft, w.x, w.y, w.z, w.w});
        char d[160];
        std::snprintf(d, sizeof d,
                      "worst residue %.4f m, %d painted cell(s) left (worst %.3f)",
                      worst, static_cast<int>(world.paint.weights.size()), paintLeft);
        if (worst > 0.01f || paintLeft > 0.01f)
            fail("release restores the ground", d);
        else
            pass("release restores the ground", d);
        // Put it back for the picture -- by CUTTING it again, not by restoring
        // the old field. release() has already handed the ground back and
        // emptied its record of what it took; dropping the old field on top of
        // that would leave the system believing it had cut nothing, and the next
        // solve would read its own trench as the natural ground. (Which is the
        // bug this harness exists to catch, so it had better not be in here.)
        (void)keep;
        rivers.touch();
        rivers.carve(world.edit, world.paint, mn, mx);
        world.publish();
    }

    // --- 8) The picture ------------------------------------------------------
    // The measurements above cannot say whether the water LOOKS like water, and
    // that question has no other answer than looking at it.
    const GLuint rv = compile(GL_VERTEX_SHADER, readFile(shDir / "river.vert"),
                              "river.vert");
    const GLuint rf = compile(GL_FRAGMENT_SHADER, readFile(shDir / "river.frag"),
                              "river.frag");
    const GLuint riverProg = link(rv, rf);

    // A plain ground shader for the terrain the water is standing in -- shaded by
    // slope and height so the cut channel reads as a cut channel. Deliberately
    // NOT lit.frag: this is scaffolding for the picture, not a claim about how
    // the ground is lit in the editor.
    const char* groundVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in float aBank;
uniform mat4 uViewProj;
out vec3 vN; out vec3 vP; out float vBank;
void main(){ vN = aNormal; vP = aPos; vBank = aBank;
             gl_Position = uViewProj * vec4(aPos,1.0); })";
    // The bank layer is in here for a reason that took a while to notice: without
    // it the bed of a brook is painted MEADOW, and a foot of clear water over
    // grass comes out as a green stain. Every judgement about how shallow water
    // looks was being made against the wrong thing underneath it.
    const char* groundFS = R"(#version 330 core
in vec3 vN; in vec3 vP; in float vBank; out vec4 F;
uniform vec3 uLightDir;
void main(){
    vec3 n = normalize(vN);
    float d = max(dot(n, normalize(uLightDir)), 0.0);
    float rock = 1.0 - smoothstep(0.55, 0.9, n.y);
    vec3 grass = vec3(0.28, 0.38, 0.20);
    vec3 stone = vec3(0.42, 0.40, 0.37);
    vec3 c = mix(grass, stone, rock);
    c = mix(c, vec3(0.44, 0.41, 0.35), clamp(vBank, 0.0, 1.0));   // wet gravel
    c *= 0.35 + 0.75 * d;
    F = vec4(pow(c, vec3(1.0/2.2)), 1.0);
})";
    const GLuint gv = compile(GL_VERTEX_SHADER, groundVS, "ground.vert");
    const GLuint gf = compile(GL_FRAGMENT_SHADER, groundFS, "ground.frag");
    const GLuint groundProg = link(gv, gf);

    // Stones and reeds: the same scaffolding shader with the material's albedo
    // instead of the slope palette. Enough to answer "is there a bank there".
    const char* dressVS = R"(#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
uniform mat4 uViewProj;
out vec3 vN;
void main(){ vN = aNormal; gl_Position = uViewProj * vec4(aPos,1.0); })";
    const char* dressFS = R"(#version 330 core
in vec3 vN; out vec4 F;
uniform vec3 uLightDir; uniform vec3 uAlbedo;
void main(){
    float d = max(dot(normalize(vN), normalize(uLightDir)), 0.0);
    F = vec4(pow(uAlbedo * (0.35 + 0.8 * d), vec3(1.0/2.2)), 1.0);
})";
    const GLuint dv = compile(GL_VERTEX_SHADER, dressVS, "dress.vert");
    const GLuint df = compile(GL_FRAGMENT_SHADER, dressFS, "dress.frag");
    const GLuint dressProg = link(dv, df);

    if (g_fails == 0) {
        // A ground patch around each shot, built from the CUT terrain.
        struct Shot {
            const char* name;
            glm::vec3   eye, at;
            glm::vec2   centre;
            float       span, step;
        };
        const rivergen::Course& cb = rivers.course(brook);
        const rivergen::Course& cr = rivers.course(river);
        const rivergen::Course& cl = rivers.course(lowland);
        // Halfway along the WATER, not halfway through the array. The falls are
        // resampled to centimetre spacing, so the middle station of a course with
        // a waterfall in it is somewhere on that waterfall -- and a shot framed
        // on it is a shot of the inside of a hill.
        auto midway = [](const rivergen::Course& c) {
            const float want = c.length * 0.5f;
            std::size_t best = c.line.size() / 2;
            for (std::size_t k = 0; k < c.s.size(); ++k)
                if (c.s[k] >= want) { best = k; break; }
            return c.line[best];
        };
        const glm::vec3 bmid = midway(cb);
        const glm::vec3 rmid = midway(cr);
        const glm::vec3 lmid = midway(cl);
        for (std::size_t i = 0; i < rivers.runs().size(); ++i) {
            int dressVerts = 0;
            for (const fitzel::Mesh& m : rivers.runs()[i].dressMeshes)
                dressVerts += static_cast<int>(m.vertexCount());
            std::printf("  run %d: %d water verts, %d dressing verts in %d batch(es)\n",
                        int(i), rivers.runs()[i].verts, dressVerts,
                        static_cast<int>(rivers.runs()[i].dress.size()));
        }
        for (std::size_t i = 0; i < rivers.runs().size(); ++i)
            std::printf("  run %d bounds: lo(%.1f %.1f %.1f) hi(%.1f %.1f %.1f)\n",
                        int(i),
                        rivers.runs()[i].lo.x, rivers.runs()[i].lo.y, rivers.runs()[i].lo.z,
                        rivers.runs()[i].hi.x, rivers.runs()[i].hi.y, rivers.runs()[i].hi.z);
        float relLo = 1e30f, relHi = -1e30f;
        for (float z = -400.0f; z <= 800.0f; z += 10.0f)
            for (float x = -400.0f; x <= 800.0f; x += 10.0f) {
                const float h = world.bare(x, z);
                relLo = std::min(relLo, h); relHi = std::max(relHi, h);
            }
        std::printf("  landscape: bare ground %.0f..%.0f m\n", relLo, relHi);
        std::printf("  brook mid: water %.2f, bed %.2f, bare ground %.2f\n",
                    bmid.y, world.height(bmid.x, bmid.z), world.bare(bmid.x, bmid.z));
        // Standing beside the water, which is the only distance at which a
        // three-metre brook is a brook rather than a line on a hillside.
        auto standing = [&](const glm::vec3& at, glm::vec3 off) {
            glm::vec3 eye = at + off;
            // Eye height on the bank -- but never below the water it is looking
            // at. A wide river cuts its banks down to its own level, so an eye
            // planted on the ground beside one ends up UNDER the surface, and the
            // shot comes out as the underside of a sheet of water.
            eye.y = std::max(world.height(eye.x, eye.z), at.y) + 1.7f;
            return eye;
        };
        std::vector<Shot> shots = {
            // Looking down the brook from a few metres up the bank.
            {"brook", standing(bmid, glm::vec3(6.0f, 0.0f, 7.0f)),
             bmid + glm::vec3(-14.0f, -0.6f, -16.0f),
             glm::vec2(bmid.x, bmid.z), 90.0f, 0.5f},
            // Across it, close, where the bank cut and the waterline are readable.
            {"brook_bank", standing(bmid, glm::vec3(9.0f, 0.0f, 0.0f)),
             bmid + glm::vec3(0.0f, -0.3f, 0.0f),
             glm::vec2(bmid.x, bmid.z), 60.0f, 0.4f},
            // The whole course from above: does it read as one channel going
            // downhill, or as a line that wanders across the contours.
            {"brook_above", bmid + glm::vec3(30.0f, 55.0f, 60.0f), bmid,
             glm::vec2(bmid.x, bmid.z), 220.0f, 1.5f},
            // A river is a different animal at a different distance.
            {"river", standing(rmid, glm::vec3(22.0f, 0.0f, 20.0f)),
             rmid + glm::vec3(-40.0f, -1.0f, -35.0f),
             glm::vec2(rmid.x, rmid.z), 260.0f, 1.5f},
            // The lowland course from above: the shot where a meander either
            // reads as a river or does not.
            {"lowland_above", lmid + glm::vec3(40.0f, 130.0f, 150.0f), lmid,
             glm::vec2(lmid.x, lmid.z), 520.0f, 3.0f},
            // ...and from the bank, where the pools, the riffles and the deep
            // line against the outer bank are what there is to look at.
            {"lowland_bank", standing(lmid, glm::vec3(44.0f, 0.0f, 28.0f)),
             lmid + glm::vec3(-60.0f, -1.5f, -40.0f),
             glm::vec2(lmid.x, lmid.z), 300.0f, 1.5f},
        };

        // --- The fall ---------------------------------------------------------
        // The one shot the others cannot stand in for. A waterfall is where the
        // surface stops being a draped ribbon and becomes a thing with a lip, a
        // curtain and a foot -- and every one of those three is a place the strip
        // can come out as a crease instead. So the biggest step in any course is
        // found and looked at from below, from the side, and over the lip.
        struct Step { int run = -1; glm::vec3 lip{0.0f}, foot{0.0f}; float drop = 0.0f; };
        Step big;
        for (std::size_t i = 0; i < rivers.runs().size(); ++i) {
            const rivergen::Course& c = rivers.course(static_cast<int>(i));
            const float fs = rivers.paths[i].style.fallSlope;
            for (std::size_t k = 1; k < c.line.size(); ) {
                const float ds = std::max(c.s[k] - c.s[k - 1], 1e-4f);
                if ((c.line[k - 1].y - c.line[k].y) / ds <= fs) { ++k; continue; }
                const std::size_t a = k;
                while (k < c.line.size() &&
                       (c.line[k - 1].y - c.line[k].y) /
                           std::max(c.s[k] - c.s[k - 1], 1e-4f) > fs) ++k;
                const float drop = c.line[a - 1].y - c.line[k - 1].y;
                if (drop > big.drop) {
                    big.run = static_cast<int>(i);
                    big.lip = c.line[a - 1];
                    big.foot = c.line[k - 1];
                    big.drop = drop;
                }
            }
        }
        if (big.run >= 0) {
            const rivergen::Course& c = rivers.course(big.run);
            // Downstream, and across it: the frame every fall shot is set up in.
            glm::vec2 d2(0.0f, 1.0f);
            {
                float best = 1e30f;
                for (std::size_t k = 0; k < c.line.size(); ++k) {
                    const float e = glm::distance(c.line[k], big.foot);
                    if (e < best) { best = e; d2 = c.dir[k]; }
                }
            }
            const glm::vec3 fwd(d2.x, 0.0f, d2.y);
            const glm::vec3 side(d2.y, 0.0f, -d2.x);
            const glm::vec3 mid = (big.lip + big.foot) * 0.5f;
            const float reach = std::max(big.drop, 6.0f);
            std::printf("  biggest fall: %.1f m, lip (%.0f %.0f %.0f) foot (%.0f %.0f %.0f)\n",
                        big.drop, big.lip.x, big.lip.y, big.lip.z,
                        big.foot.x, big.foot.y, big.foot.z);
            // From the plunge pool, looking back up the curtain.
            shots.push_back({"fall_foot",
                             big.foot + fwd * (reach * 0.9f) + glm::vec3(0.0f, 2.0f, 0.0f),
                             mid, glm::vec2(mid.x, mid.z),
                             std::max(reach * 4.0f, 60.0f), 0.5f});
            // From the side at half height: the shot the crease shows up in.
            shots.push_back({"fall_side",
                             mid + side * (reach * 1.1f) + fwd * (reach * 0.25f) +
                                 glm::vec3(0.0f, reach * 0.25f, 0.0f),
                             mid, glm::vec2(mid.x, mid.z),
                             std::max(reach * 4.0f, 60.0f), 0.5f});
            // Beside the lip looking along it, which is where the crest either
            // rounds over or snaps off. Framed off the channel WIDTH, not off a
            // fixed number of metres: at seven metres from a thirty-metre river
            // the shot is inside the water and shows nothing but one bank.
            const float wide = std::max(2.0f * c.half[0], 3.0f);
            float lipW = wide;
            {
                float best = 1e30f;
                for (std::size_t k = 0; k < c.line.size(); ++k) {
                    const float e = glm::distance(c.line[k], big.lip);
                    if (e < best) { best = e; lipW = std::max(2.0f * c.half[k], 3.0f); }
                }
            }
            shots.push_back({"fall_lip",
                             big.lip - fwd * (lipW * 1.6f) + side * (lipW * 1.5f) +
                                 glm::vec3(0.0f, std::max(lipW * 0.5f, 4.0f), 0.0f),
                             big.lip + fwd * (lipW * 0.8f) -
                                 glm::vec3(0.0f, lipW * 0.6f, 0.0f),
                             glm::vec2(big.lip.x, big.lip.z),
                             std::max(reach * 2.5f, 50.0f), 0.4f});
        }

        GLuint fbo = 0, tex = 0, rbo = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kW, kH, 0, GL_RGBA,
                     GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kW, kH);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, rbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::printf("[FAIL] framebuffer\n");
            return 2;
        }
        glViewport(0, 0, kW, kH);
        glEnable(GL_DEPTH_TEST);

        const glm::vec3 lightDir = glm::normalize(glm::vec3(0.45f, 0.72f, 0.28f));

        // river.frag samples an environment cubemap. There is no scene to capture
        // one from here, so one is built: a sky gradient, a sun, and ground below
        // the horizon.
        //
        // It used to be a single flat colour per face, and that was quietly the
        // worst thing in this harness. Half of what makes water look like water
        // is the reflection MOVING as the ripples tilt it, and a reflection that
        // is the same colour in every direction cannot move -- so every shading
        // change came out looking like no change at all, and the pictures said a
        // rippled surface and a flat one were the same picture. Thirty-two pixels
        // a face is enough to tell them apart.
        GLuint envCube = 0;
        glGenTextures(1, &envCube);
        glBindTexture(GL_TEXTURE_CUBE_MAP, envCube);
        {
            const int E = 32;
            std::vector<float> face(static_cast<std::size_t>(E) * E * 3);
            for (int f = 0; f < 6; ++f) {
                for (int y = 0; y < E; ++y) {
                    for (int x = 0; x < E; ++x) {
                        // The direction this texel looks in, per the cube map
                        // face convention.
                        const float u = 2.0f * (x + 0.5f) / E - 1.0f;
                        const float v = 1.0f - 2.0f * (y + 0.5f) / E;
                        glm::vec3 d;
                        switch (f) {
                            case 0: d = { 1.0f,    v,   -u}; break;
                            case 1: d = {-1.0f,    v,    u}; break;
                            case 2: d = {    u, 1.0f,   -v}; break;
                            case 3: d = {    u,-1.0f,    v}; break;
                            case 4: d = {    u,    v, 1.0f}; break;
                            default:d = {   -u,    v,-1.0f}; break;
                        }
                        d = glm::normalize(d);
                        glm::vec3 c;
                        if (d.y >= 0.0f) {
                            c = glm::mix(glm::vec3(0.78f, 0.86f, 0.98f),
                                         glm::vec3(0.30f, 0.50f, 0.92f),
                                         std::pow(d.y, 0.6f));
                            const float sun = glm::dot(d, lightDir);
                            if (sun > 0.995f) c += glm::vec3(24.0f);
                            else c += glm::vec3(2.4f) * std::pow(
                                          std::max(sun, 0.0f), 120.0f);
                        } else {
                            c = glm::mix(glm::vec3(0.30f, 0.34f, 0.26f),
                                         glm::vec3(0.16f, 0.18f, 0.14f),
                                         std::min(-d.y * 2.0f, 1.0f));
                        }
                        const std::size_t i =
                            (static_cast<std::size_t>(y) * E + x) * 3;
                        face[i] = c.x; face[i + 1] = c.y; face[i + 2] = c.z;
                    }
                }
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGB16F,
                             E, E, 0, GL_RGB, GL_FLOAT, face.data());
            }
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        std::vector<unsigned char> px(static_cast<std::size_t>(kW) * kH * 4);
        stbi_flip_vertically_on_write(1);

        for (const Shot& sh : shots) {
            // The ground, sampled off the terrain the courses actually cut.
            const int n = static_cast<int>(sh.span / sh.step) + 1;
            std::vector<float> verts;
            verts.reserve(static_cast<std::size_t>(n) * n * 7);
            auto H = [&](int ix, int iz) {
                return world.height(sh.centre.x - sh.span * 0.5f + ix * sh.step,
                                    sh.centre.y - sh.span * 0.5f + iz * sh.step);
            };
            std::vector<std::uint32_t> idx;
            for (int iz = 0; iz < n; ++iz) {
                for (int ix = 0; ix < n; ++ix) {
                    const float x = sh.centre.x - sh.span * 0.5f + ix * sh.step;
                    const float z = sh.centre.y - sh.span * 0.5f + iz * sh.step;
                    const float hL = H(std::max(ix - 1, 0), iz);
                    const float hR = H(std::min(ix + 1, n - 1), iz);
                    const float hD = H(ix, std::max(iz - 1, 0));
                    const float hU = H(ix, std::min(iz + 1, n - 1));
                    const glm::vec3 nrm = glm::normalize(
                        glm::vec3(hL - hR, 2.0f * sh.step, hD - hU));
                    // Layer 3 is the one every course in this harness paints its
                    // bed with (see the loop that sets bankLayer above).
                    const float bank = world.paint.sample(x, z)[2];
                    verts.insert(verts.end(),
                                 {x, H(ix, iz), z, nrm.x, nrm.y, nrm.z, bank});
                }
            }
            for (int iz = 0; iz + 1 < n; ++iz)
                for (int ix = 0; ix + 1 < n; ++ix) {
                    const std::uint32_t a = iz * n + ix, b = a + 1;
                    const std::uint32_t c = a + n, d = c + 1;
                    idx.insert(idx.end(), {a, c, b, b, c, d});
                }
            GLuint gvao = 0, gvbo = 0, gebo = 0;
            glGenVertexArrays(1, &gvao);
            glBindVertexArray(gvao);
            glGenBuffers(1, &gvbo);
            glBindBuffer(GL_ARRAY_BUFFER, gvbo);
            glBufferData(GL_ARRAY_BUFFER, verts.size() * sizeof(float),
                         verts.data(), GL_STATIC_DRAW);
            glGenBuffers(1, &gebo);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * sizeof(std::uint32_t),
                         idx.data(), GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float),
                                  reinterpret_cast<void*>(0));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float),
                                  reinterpret_cast<void*>(3 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 7 * sizeof(float),
                                  reinterpret_cast<void*>(6 * sizeof(float)));

            const glm::mat4 view = glm::lookAt(sh.eye, sh.at, glm::vec3(0, 1, 0));
            const glm::mat4 proj = glm::perspective(glm::radians(55.0f),
                                                    float(kW) / float(kH),
                                                    0.15f, 4000.0f);
            const glm::mat4 vp = proj * view;

            glClearColor(0.55f, 0.70f, 0.88f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            glUseProgram(groundProg);
            glUniformMatrix4fv(glGetUniformLocation(groundProg, "uViewProj"), 1,
                               GL_FALSE, glm::value_ptr(vp));
            glUniform3fv(glGetUniformLocation(groundProg, "uLightDir"), 1,
                         glm::value_ptr(lightDir));
            glBindVertexArray(gvao);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(idx.size()),
                           GL_UNSIGNED_INT, nullptr);

            // Stones and reeds, before the water: they stand in it.
            glUseProgram(dressProg);
            glUniformMatrix4fv(glGetUniformLocation(dressProg, "uViewProj"), 1,
                               GL_FALSE, glm::value_ptr(vp));
            glUniform3fv(glGetUniformLocation(dressProg, "uLightDir"), 1,
                         glm::value_ptr(lightDir));
            for (const RiverSystem::Run& run : rivers.runs())
                for (std::size_t bi = 0; bi < run.dress.size() &&
                                         bi < run.dressMeshes.size(); ++bi) {
                    glm::vec3 albedo(0.5f);
                    for (const MaterialDef& m : mats)
                        if (m.assetId == run.dress[bi].material) albedo = m.albedo;
                    glUniform3fv(glGetUniformLocation(dressProg, "uAlbedo"), 1,
                                 glm::value_ptr(albedo));
                    run.dressMeshes[bi].draw();
                }

            // The water, through the real shader with the real uniforms.
            glUseProgram(riverProg);
            auto f1 = [&](const char* nm, float v) {
                glUniform1f(glGetUniformLocation(riverProg, nm), v); };
            auto v3 = [&](const char* nm, const glm::vec3& v) {
                glUniform3fv(glGetUniformLocation(riverProg, nm), 1, glm::value_ptr(v)); };
            glUniformMatrix4fv(glGetUniformLocation(riverProg, "uViewProj"), 1,
                               GL_FALSE, glm::value_ptr(vp));
            glUniform4f(glGetUniformLocation(riverProg, "uClipPlane"),
                        0.0f, 1.0f, 0.0f, 1.0e6f);
            v3("uCameraPos", sh.eye);
            v3("uLightDir", lightDir);
            v3("uLightColor", glm::vec3(1.0f, 0.96f, 0.90f));
            v3("uAmbient", glm::vec3(0.30f, 0.34f, 0.40f));
            f1("uTime", 8.0f);
            v3("uFogColor", glm::vec3(0.62f, 0.72f, 0.86f));
            v3("uFogSunColor", glm::vec3(0.95f, 0.88f, 0.75f));
            f1("uFogDensity", 0.0009f);
            f1("uFogHeightFalloff", 0.006f);
            f1("uFogHeight", 0.0f);
            f1("uExposure", 1.0f);
            glUniform1i(glGetUniformLocation(riverProg, "uTonemap"), 1);
            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_CUBE_MAP, envCube);
            glUniform1i(glGetUniformLocation(riverProg, "uEnvProbe"), 2);
            // No mips on the stand-in cube, so no LOD to blur into: the
            // roughness the shader asks for is exercised in the editor, where the
            // probe has them. What this harness is here to show is that the
            // reflection MOVES with the ripples at all.
            f1("uEnvMaxLod", 0.0f);

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            for (std::size_t i = 0; i < rivers.runs().size(); ++i) {
                const RiverSystem::Run& run = rivers.runs()[i];
                if (run.mesh.vertexCount() == 0) continue;
                const rivergen::Style& st = rivers.paths[i].style;
                v3("uShallow", st.shallow);
                v3("uDeep", st.deep);
                f1("uClarity", st.clarity);
                f1("uReflect", st.reflect);
                f1("uRippleScale", st.rippleScale);
                f1("uRipple", st.ripple);
                f1("uFlowSpeed", st.flowSpeed);
                f1("uFoamWidth", st.foamWidth);
                f1("uSparkle", st.sparkle);
                // Strength and count, the two halves the shader now takes: a
                // drop hits water just as hard in a drizzle, what changes is how
                // many land. Setting only the first would draw no rings at all,
                // which is the kind of silence a harness exists to prevent.
                f1("uRainRings", rainRings > 0.0f ? 1.0f : 0.0f);
                f1("uRainDensity", rainRings);
                run.mesh.draw();
            }
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);

            glFinish();
            glReadPixels(0, 0, kW, kH, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            const fs::path out = outDir / (std::string("river_") + sh.name + ".png");
            if (stbi_write_png(out.string().c_str(), kW, kH, 4, px.data(), kW * 4))
                std::printf("wrote %s\n", out.string().c_str());
            else
                std::printf("[FAIL] could not write %s\n", out.string().c_str());

            glDeleteBuffers(1, &gvbo);
            glDeleteBuffers(1, &gebo);
            glDeleteVertexArrays(1, &gvao);
        }
    }

    // The meshes hold GPU handles; drop them before the context goes.
    rivers.clear();
    glfwDestroyWindow(win);
    glfwTerminate();

    std::printf(g_fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n",
                g_fails);
    return g_fails ? 1 : 0;
}
