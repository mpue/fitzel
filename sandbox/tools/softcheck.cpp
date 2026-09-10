// The soft-body check: does a wobbling object actually wobble -- and does it
// still exist afterwards?
//
// A soft body is the one physics object with no transform to look at. It is a few
// hundred particles, and every way it can go wrong looks the same from outside:
// the mesh is still drawn, still roughly where it was put, and either it never
// deformed at all (constraints too stiff, or the readback quietly handing back
// the rest pose) or it deformed once and never came back (a shell wound inwards
// so pressure implodes it, a lattice with no volume constraints, a solver that
// went to NaN and took the vertices with it). None of that is visible in a still
// frame of the editor, which is why it is measured here instead: drop each kind
// on a floor, and check the numbers a wobble is made of -- it falls, it squashes,
// it comes back, and what is pinned stays pinned.
//
// Console program, like meshpaintcheck and shadercheck, and for the same reason:
// the editor is /SUBSYSTEM:WINDOWS in Release and has nowhere to print to. No GL
// context needed -- physics and the mesh it writes are all this touches.
//   build/release/bin/softcheck.exe
// Exits non-zero if any check fails.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/physics/Physics.hpp>

#include "../src/Component.hpp"
#include "../src/SoftBodySystem.hpp"

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// One soft entity over a floor, stepped for `seconds`. Everything a check wants
// to know about the run comes back in Run: where it ended up, how flat it got at
// its flattest, and how tall it was at the end (did it spring back?).
struct Run {
    bool  built     = false;  // did the body come up at all?
    int   particles = 0;
    int   faces     = 0;
    float restHeight = 0.0f;  // the mesh's Y extent on the first frame
    float minHeight  = 0.0f;  // ...at its most squashed
    float endHeight  = 0.0f;  // ...when the run finished
    float startY     = 0.0f;  // centre height, first frame
    float endY       = 0.0f;  // ...and last
    float lowestVert = 0.0f;  // deepest particle at the end (floor is y = 0)
    float fitError   = 0.0f;  // worst |1 - render scale| over the run (see below)
    bool  finite     = true;  // no NaN/inf reached the mesh
};

Run drop(const SoftBodyComponent& soft, EntityType type, glm::vec3 half,
         glm::vec3 at, float seconds, MeshComponent* modelled = nullptr) {
    std::vector<Entity> entities;
    Entity e;
    e.type   = type;
    e.center = e.localCenter = at;
    e.half   = half;
    e.id     = 1;
    e.name   = "soft";
    if (modelled) e.components.items.push_back(modelled->clone());
    e.components.items.push_back(std::make_unique<SoftBodyComponent>(soft));
    entities.push_back(std::move(e));

    fitzel::PhysicsWorld world;
    world.addBox(glm::vec3(50.0f, 1.0f, 50.0f), glm::vec3(0.0f, -1.0f, 0.0f),
                 glm::quat(1, 0, 0, 0), 0.0f); // floor, top face at y = 0

    SoftBodySystem soft_;
    soft_.spawn(entities, world);
    Run r;
    r.built = soft_.has(1);
    if (!r.built) return r;

    const auto setWorld = [](Entity& en, const glm::vec3& p, const glm::vec3& rot) {
        en.center = en.localCenter = p;
        en.rotation = en.localRotation = rot;
    };
    const int steps = static_cast<int>(seconds * 60.0f);
    for (int i = 0; i < steps; ++i) {
        world.step(1.0f / 60.0f);
        soft_.sync(entities, world, setWorld);
        const Entity& en = entities[0];
        const auto* mc = en.components.get<MeshComponent>();
        if (!mc) { r.built = false; return r; }
        glm::vec3 mn, mx;
        mc->mesh.bounds(mn, mx);
        const float h = mx.y - mn.y;
        if (i == 0) {
            r.particles  = static_cast<int>(mc->mesh.verts.size());
            r.faces      = static_cast<int>(mc->mesh.faces.size());
            r.restHeight = h;
            r.minHeight  = h;
            r.startY     = en.center.y;
        }
        r.minHeight = std::min(r.minHeight, h);
        // What the renderer would do with this mesh. scenesubmit fits a modelled
        // mesh to its entity's half-extents, so unless the system keeps those in
        // step with the simulated bounds, every squash is scaled straight back
        // out again on the way to the screen and the object looks rigid. The
        // factor has to stay 1, and this is the arithmetic that says whether it
        // does -- without needing a window to look at.
        const glm::vec3 sz = glm::max(mx - mn, glm::vec3(1.0e-4f));
        const glm::vec3 fit = (en.half * 2.0f) / sz;
        for (int k = 0; k < 3; ++k)
            r.fitError = std::max(r.fitError, std::fabs(fit[k] - 1.0f));
        for (const glm::vec3& v : mc->mesh.verts)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                r.finite = false;
        if (i == steps - 1) {
            r.endHeight  = h;
            r.endY       = en.center.y;
            r.lowestVert = en.center.y + mn.y;
        }
    }
    return r;
}

// --- Jelly: the lattice with volume constraints ------------------------------
// The whole promise of it: squash on impact, then back to roughly a cube.
void checkJelly() {
    std::printf("\nJelly cube\n");
    SoftBodyComponent sc;
    sc.kind       = SoftBodyComponent::Jelly;
    sc.resolution = 4;
    sc.softness   = 0.5f;
    sc.mass       = 20.0f;
    const Run r = drop(sc, EntityType::Box, glm::vec3(0.5f),
                       glm::vec3(0.0f, 3.0f, 0.0f), 4.0f);
    check(r.built, "a jelly cube comes up as a soft body");
    if (!r.built) return;
    std::printf("       %d particles, %d faces; rest %.3f m, min %.3f, end %.3f;"
                " y %.2f -> %.2f\n",
                r.particles, r.faces, r.restHeight, r.minHeight, r.endHeight,
                r.startY, r.endY);
    check(r.finite, "no particle went to NaN");
    check(r.particles == 4 * 4 * 4, "the lattice is resolution^3 particles");
    check(r.faces == 6 * 2 * 3 * 3, "only the outer shell has faces to draw");
    check(r.endY < r.startY - 1.5f, "it falls");
    check(r.lowestVert > -0.2f && r.endY > 0.0f, "it lands ON the floor");
    check(r.minHeight < r.restHeight * 0.92f, "it squashes on impact");
    check(r.endHeight > r.restHeight * 0.7f, "and springs most of the way back");
    check(r.fitError < 1.0e-3f,
          "the deformation reaches the screen 1:1 (render fit stays at 1)");
}

// --- Softness actually does something ----------------------------------------
void checkSoftness() {
    std::printf("\nSoftness\n");
    SoftBodyComponent stiff;
    stiff.kind = SoftBodyComponent::Jelly;
    stiff.resolution = 4;
    stiff.softness = 0.0f;
    SoftBodyComponent slack = stiff;
    slack.softness = 1.0f;
    const Run a = drop(stiff, EntityType::Box, glm::vec3(0.5f),
                       glm::vec3(0.0f, 3.0f, 0.0f), 2.5f);
    const Run b = drop(slack, EntityType::Box, glm::vec3(0.5f),
                       glm::vec3(0.0f, 3.0f, 0.0f), 2.5f);
    std::printf("       stiff min %.3f | slack min %.3f (rest %.3f)\n",
                a.minHeight, b.minHeight, a.restHeight);
    check(a.built && b.built && a.finite && b.finite, "both bodies simulate");
    check(b.minHeight < a.minHeight, "a slack body squashes further than a stiff one");
}

// --- Balloon: a hollow shell, held out by pressure ---------------------------
void checkBalloon() {
    std::printf("\nBalloon\n");
    SoftBodyComponent sc;
    sc.kind       = SoftBodyComponent::Balloon;
    sc.resolution = 4;
    sc.softness   = 0.4f;
    sc.pressure   = 4000.0f;
    const Run full = drop(sc, EntityType::Sphere, glm::vec3(0.5f),
                          glm::vec3(0.0f, 3.0f, 0.0f), 4.0f);
    sc.pressure = 0.0f;
    const Run flat = drop(sc, EntityType::Sphere, glm::vec3(0.5f),
                          glm::vec3(0.0f, 3.0f, 0.0f), 4.0f);
    std::printf("       %d particles; inflated end %.3f | no pressure end %.3f"
                " (rest %.3f)\n",
                full.particles, full.endHeight, flat.endHeight, full.restHeight);
    check(full.built && full.finite, "an inflated shell simulates");
    check(full.endY > 0.0f && full.lowestVert > -0.2f, "it lands on the floor");
    check(full.endHeight > full.restHeight * 0.6f, "pressure keeps it inflated");
    check(flat.endHeight < full.endHeight,
          "the same shell with no pressure in it collapses further");
}

// --- Cloth: pinned corners hold, the middle sags -----------------------------
void checkCloth() {
    std::printf("\nCloth\n");
    SoftBodyComponent sc;
    sc.kind       = SoftBodyComponent::Cloth;
    sc.resolution = 6;
    sc.softness   = 0.3f;
    sc.mass       = 5.0f;
    sc.pinning    = 1; // the four corners
    const Run r = drop(sc, EntityType::Box, glm::vec3(1.0f, 0.05f, 1.0f),
                       glm::vec3(0.0f, 4.0f, 0.0f), 3.0f);
    check(r.built, "a cloth comes up as a soft body");
    if (!r.built) return;
    std::printf("       %d particles, %d faces (both windings); y %.2f -> %.2f,"
                " sag %.3f m\n",
                r.particles, r.faces, r.startY, r.endY, r.endHeight);
    check(r.finite, "no particle went to NaN");
    check(r.faces > 0 && r.faces % 2 == 0,
          "every triangle has its mirror, so the sheet is visible from both sides");
    check(std::fabs(r.endY - r.startY) < 0.6f,
          "pinned corners hold the sheet up where it was hung");
    check(r.endHeight > 0.05f, "and the middle of it sags between them");

    // How MUCH it sags is what the Softness slider is for. Relational, not a
    // number: a flat 13 cm is meaningless on its own, and would have to be
    // re-guessed every time the solver changes underneath it.
    SoftBodyComponent slack = sc;
    slack.softness = 1.0f;
    const Run drapey = drop(slack, EntityType::Box, glm::vec3(1.0f, 0.05f, 1.0f),
                            glm::vec3(0.0f, 4.0f, 0.0f), 3.0f);
    std::printf("       slack sag %.3f m vs %.3f\n", drapey.endHeight, r.endHeight);
    check(drapey.endHeight > r.endHeight, "a slacker cloth drapes further");

    sc.pinning = 0;
    const Run loose = drop(sc, EntityType::Box, glm::vec3(1.0f, 0.05f, 1.0f),
                           glm::vec3(0.0f, 4.0f, 0.0f), 3.0f);
    std::printf("       unpinned y %.2f -> %.2f\n", loose.startY, loose.endY);
    check(loose.endY < loose.startY - 2.0f, "with nothing pinned it simply falls");
}

// --- Hanging cloth: curtains, banners, flags ---------------------------------
// A hanging sheet is judged by where its particles are, in the world: does the
// held edge stay held, does it hang rather than fall, does a curtain keep its
// pleats, does a flag fly. One run records exactly that.
struct Hang {
    bool      built  = false;
    bool      finite = true;
    glm::vec3 lo0{0.0f}, hi0{0.0f};   // world bounds, first frame
    glm::vec3 lo{0.0f},  hi{0.0f};    // ...last frame
    float     heldDrift = 0.0f;       // worst move of a held particle (see below)
    float     topSag    = 0.0f;       // how far the top row's lowest particle fell
    glm::vec3 tipLo{1e9f}, tipHi{-1e9f}; // the free corner's range, last 2 s
};

// `poleHeld` says which particles should not move, picked by their first-frame
// world position: the pole edge (-X) if set, the top row otherwise.
Hang hang(const SoftBodyComponent& soft, glm::vec3 half, glm::vec3 at, float seconds,
          bool poleHeld) {
    std::vector<Entity> entities;
    Entity e;
    e.type   = EntityType::Box;
    e.center = e.localCenter = at;
    e.half   = half;
    e.id     = 1;
    entities.push_back(std::move(e));
    entities[0].components.items.push_back(std::make_unique<SoftBodyComponent>(soft));

    fitzel::PhysicsWorld world;
    world.addBox(glm::vec3(50.0f, 1.0f, 50.0f), glm::vec3(0.0f, -1.0f, 0.0f),
                 glm::quat(1, 0, 0, 0), 0.0f);
    SoftBodySystem sys;
    sys.spawn(entities, world);
    Hang h;
    h.built = sys.has(1);
    if (!h.built) return h;

    const auto setWorld = [](Entity& en, const glm::vec3& p, const glm::vec3& r) {
        en.center = en.localCenter = p;
        en.rotation = en.localRotation = r;
    };
    std::vector<glm::vec3> first;
    std::vector<int> held, top;
    int tip = 0;
    const int steps = static_cast<int>(seconds * 60.0f);
    // Frame -1 is the rest pose, read before anything has been stepped: after
    // even one step the free particles have fallen a few millimetres, and "the
    // top row" would shrink to the pinned ones.
    for (int i = -1; i < steps; ++i) {
        const float t = std::max(i, 0) / 60.0f;
        if (i >= 0) {
            sys.blow(world, t, 1.0f / 60.0f);
            world.step(1.0f / 60.0f);
        }
        sys.sync(entities, world, setWorld);
        const Entity& en = entities[0];
        const auto* mc = en.components.get<MeshComponent>();
        if (!mc) { h.built = false; return h; }
        glm::vec3 lo(1e9f), hi(-1e9f);
        std::vector<glm::vec3> wp(mc->mesh.verts.size());
        for (std::size_t k = 0; k < wp.size(); ++k) {
            wp[k] = en.center + mc->mesh.verts[k];
            lo = glm::min(lo, wp[k]);
            hi = glm::max(hi, wp[k]);
            if (!std::isfinite(wp[k].x) || !std::isfinite(wp[k].y) || !std::isfinite(wp[k].z))
                h.finite = false;
        }
        if (i < 0) {
            first = wp;
            h.lo0 = lo; h.hi0 = hi;
            for (int k = 0; k < static_cast<int>(wp.size()); ++k) {
                const bool isTop  = wp[k].y > hi.y - 1.0e-3f;
                const bool isPole = wp[k].x < lo.x + 1.0e-3f;
                if (isTop) top.push_back(k);
                if (poleHeld ? isPole : isTop) held.push_back(k);
                // The free corner: furthest from the pole, lowest of those.
                if (wp[k].x > wp[tip].x + 1.0e-4f ||
                    (std::fabs(wp[k].x - wp[tip].x) <= 1.0e-4f && wp[k].y < wp[tip].y))
                    tip = k;
            }
            continue;
        }
        h.lo = lo; h.hi = hi;
        for (int k : held)
            h.heldDrift = std::max(h.heldDrift, glm::distance(wp[k], first[k]));
        if (i == steps - 1) {
            float topLow = 1e9f;
            for (int k : top) topLow = std::min(topLow, wp[k].y);
            h.topSag = h.hi0.y - topLow;
        }
        if (t >= seconds - 2.0f) {
            h.tipLo = glm::min(h.tipLo, wp[tip]);
            h.tipHi = glm::max(h.tipHi, wp[tip]);
        }
    }
    return h;
}

void checkHanging() {
    std::printf("\nHanging cloth\n");
    const glm::vec3 drape(1.0f, 1.25f, 0.02f);   // a 2 m x 2.5 m curtain
    const glm::vec3 above(0.0f, 3.0f, 0.0f);     // bottom 1.75 m clear of the floor

    SoftBodyComponent curtain;
    curtain.kind       = SoftBodyComponent::Cloth;
    curtain.pinning    = SoftBodyComponent::PinTop;
    curtain.resolution = 5;
    curtain.mass       = 3.0f;
    curtain.softness   = 0.35f;
    const Hang c = hang(curtain, drape, above, 3.0f, false);
    check(c.built && c.finite, "a curtain comes up and stays finite");
    std::printf("       built %.2f m tall; after 3 s top drift %.3f m, bottom %.2f -> %.2f,"
                " depth %.3f m\n",
                c.hi0.y - c.lo0.y, c.heldDrift, c.lo0.y, c.lo.y, c.hi.z - c.lo.z);
    check(std::fabs((c.hi0.y - c.lo0.y) - 2.5f) < 0.05f,
          "it is built hanging (2.5 m tall), not lying down");
    check(c.heldDrift < 0.02f, "its top edge stays exactly where it was hung");
    check(std::fabs(c.lo.y - c.lo0.y) < 0.3f, "it hangs rather than falling or bunching up");
    check(c.hi.z - c.lo.z < 0.3f, "without wind it hangs in its own plane");

    SoftBodyComponent pleated = curtain;
    pleated.folds = 0.8f;
    const Hang p = hang(pleated, drape, above, 3.0f, false);
    std::printf("       pleated depth %.3f m vs flat %.3f m\n",
                p.hi.z - p.lo.z, c.hi.z - c.lo.z);
    check(p.built && p.finite, "a pleated curtain simulates");
    check(p.hi.z - p.lo.z > (c.hi.z - c.lo.z) + 0.05f, "and keeps its pleats");
    check(p.heldDrift < 0.02f, "its pleated top stays on the rail");

    // Rings only let the top sag where there is fabric to sag with -- a flat
    // sheet is taut between any two points of its own top edge, as a real one
    // is. So the comparison is between pleated curtains: held all along, and on
    // four rings.
    SoftBodyComponent ringed = pleated;
    ringed.pinning = SoftBodyComponent::PinRings;
    ringed.rings   = 4;
    const Hang r = hang(ringed, drape, above, 3.0f, false);
    std::printf("       pleated, on 4 rings the top sags %.3f m (held all along %.3f)\n",
                r.topSag, p.topSag);
    check(r.built && r.finite, "a curtain on rings simulates");
    check(r.topSag > p.topSag + 0.02f, "between its rings the top edge sags");

    SoftBodyComponent banner = curtain;
    banner.pinning = SoftBodyComponent::PinTopCorners;
    banner.mass    = 1.0f;
    const Hang b = hang(banner, glm::vec3(0.5f, 1.0f, 0.01f), above, 3.0f, false);
    std::printf("       banner: bottom %.2f -> %.2f\n", b.lo0.y, b.lo.y);
    check(b.built && b.finite, "a banner simulates");
    check(std::fabs(b.lo.y - b.lo0.y) < 0.3f, "held at its two top corners, it hangs");

    // A flag: held along the pole, 1.5 m x 1 m, weighing what a flag weighs.
    SoftBodyComponent flag;
    flag.kind       = SoftBodyComponent::Cloth;
    flag.pinning    = SoftBodyComponent::PinPole;
    flag.resolution = 5;
    flag.mass       = 0.5f;
    flag.softness   = 0.3f;
    flag.damping    = 0.05f;
    const glm::vec3 flagHalf(0.75f, 0.5f, 0.01f);
    const Hang calm = hang(flag, flagHalf, above, 4.0f, true);
    flag.wind = glm::vec3(6.0f, 0.0f, 0.0f);
    const Hang windy = hang(flag, flagHalf, above, 4.0f, true);
    const float calmReach  = calm.hi.x - calm.lo0.x;
    const float windyReach = windy.hi.x - windy.lo0.x;
    const glm::vec3 wander = windy.tipHi - windy.tipLo;
    std::printf("       reach from the pole: calm %.2f m, 6 m/s %.2f m (1.5 m of flag);"
                " pole drift %.3f m; free corner wanders %.2f/%.2f/%.2f m\n",
                calmReach, windyReach, windy.heldDrift, wander.x, wander.y, wander.z);
    std::printf("       calm: bottom %.2f -> %.2f, free corner y %.2f..%.2f x %.2f..%.2f\n",
                calm.lo0.y, calm.lo.y, calm.tipLo.y, calm.tipHi.y, calm.tipLo.x, calm.tipHi.x);
    check(calm.built && windy.built && calm.finite && windy.finite, "a flag simulates");
    check(calm.heldDrift < 0.02f && windy.heldDrift < 0.02f,
          "its pole edge stays on the pole, wind or not");
    check(calmReach < 1.5f * 0.9f, "with no wind it droops off the pole");
    check(windyReach > calmReach + 0.1f && windyReach > 1.5f * 0.75f,
          "in a wind it flies out from the pole");
    check(std::max(wander.y, wander.z) > 0.05f,
          "and keeps moving -- it flutters instead of standing out like a board");
}

// --- A modelled mesh, made soft ----------------------------------------------
void checkFromMesh() {
    std::printf("\nFrom a modelled mesh\n");
    MeshComponent mesh; // the default: a unit box, 8 corners and 6 quads
    SoftBodyComponent sc;
    sc.kind     = SoftBodyComponent::FromMesh;
    sc.softness = 0.3f;
    sc.pressure = 3000.0f;
    const Run r = drop(sc, EntityType::Box, glm::vec3(0.5f),
                       glm::vec3(0.0f, 3.0f, 0.0f), 3.0f, &mesh);
    check(r.built, "a modelled mesh becomes a soft shell");
    if (!r.built) return;
    std::printf("       %d particles (the mesh's own corners), %d faces;"
                " end %.3f (rest %.3f)\n",
                r.particles, r.faces, r.endHeight, r.restHeight);
    check(r.finite, "no particle went to NaN");
    check(r.particles == 8, "it simulates the corners the mesh already had");
    check(r.faces == 12, "its quads are triangulated for the solver");
    check(r.endY > 0.0f && r.lowestVert > -0.2f, "it lands on the floor");

    // Without a mesh to work from there is nothing to simulate, and the entity
    // must come through untouched rather than half-converted.
    const Run none = drop(sc, EntityType::Box, glm::vec3(0.5f),
                          glm::vec3(0.0f, 3.0f, 0.0f), 0.5f);
    check(!none.built, "and an entity with no mesh is left alone");
}

} // namespace

int main() {
    std::printf("Soft body check\n");
    checkJelly();
    checkSoftness();
    checkBalloon();
    checkCloth();
    checkHanging();
    checkFromMesh();
    std::printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
