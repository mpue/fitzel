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
    checkFromMesh();
    std::printf("\n%d check(s), %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
