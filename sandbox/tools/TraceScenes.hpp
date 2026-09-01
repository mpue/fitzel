#pragma once

// The frames both tracers are judged on.
//
// Shared between pathcheck (which asks whether the CPU tracer's answers are
// right) and gpucheck (which asks whether the GPU one reproduces them). Shared
// rather than copied for the obvious reason and for a better one: the moment
// two harnesses build "the same" scene out of two pieces of code, a difference
// between the pictures has two possible explanations -- and the entire value of
// comparing them is that it should only ever have one.

#include <cmath>
#include <memory>

#include <glm/glm.hpp>

#include "../src/PathTrace.hpp"

namespace tracescenes {

inline void addQuad(pathtrace::Scene& sc, const glm::vec3& a, const glm::vec3& b,
             const glm::vec3& c, const glm::vec3& d, const glm::vec3& n, int mat) {
    pathtrace::Triangle t0, t1;
    t0.p0 = a; t0.p1 = b; t0.p2 = c;
    t0.n0 = t0.n1 = t0.n2 = n;
    t0.uv0 = {0, 0}; t0.uv1 = {1, 0}; t0.uv2 = {1, 1};
    t0.material = mat;
    t1.p0 = a; t1.p1 = c; t1.p2 = d;
    t1.n0 = t1.n1 = t1.n2 = n;
    t1.uv0 = {0, 0}; t1.uv1 = {1, 1}; t1.uv2 = {0, 1};
    t1.material = mat;
    sc.triangles.push_back(t0);
    sc.triangles.push_back(t1);
}

inline void addBox(pathtrace::Scene& sc, const glm::vec3& c, const glm::vec3& h, int mat) {
    const glm::vec3 lo = c - h, hi = c + h;
    addQuad(sc, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, { 0,  1,  0}, mat);
    addQuad(sc, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z}, { 0, -1,  0}, mat);
    addQuad(sc, {lo.x, lo.y, hi.z}, {lo.x, hi.y, hi.z}, {lo.x, hi.y, lo.z}, {lo.x, lo.y, lo.z}, {-1,  0,  0}, mat);
    addQuad(sc, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z}, { 1,  0,  0}, mat);
    addQuad(sc, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}, { 0,  0,  1}, mat);
    addQuad(sc, {hi.x, lo.y, lo.z}, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, { 0,  0, -1}, mat);
}

// A camera looking at `target` from `eye`, with world up. The same basis a
// scene camera hands over, built here by hand so the tests do not need one.
inline pathtrace::CameraDesc lookAt(const glm::vec3& eye, const glm::vec3& target, float fov) {
    pathtrace::CameraDesc c;
    c.position = eye;
    c.forward  = glm::normalize(target - eye);
    c.right    = glm::normalize(glm::cross(c.forward, glm::vec3(0, 1, 0)));
    c.up       = glm::normalize(glm::cross(c.right, c.forward));
    c.fovDegrees = fov;
    return c;
}

// --- The shadow -------------------------------------------------------------
// A box a metre above a plane, the sun straight down, seen from above. The
// shadow's outline is arithmetic, so the test can name the pixels that must be
// dark and the pixels that must not be, and a traversal bug has nowhere to hide.
inline std::shared_ptr<pathtrace::Scene> shadowScene() {
    auto sc = std::make_shared<pathtrace::Scene>();
    pathtrace::Material ground;
    ground.albedo    = glm::vec3(0.6f);
    ground.roughness = 0.9f;
    pathtrace::Material box;
    box.albedo    = glm::vec3(0.2f, 0.35f, 0.8f);
    box.roughness = 0.4f;
    sc->materials = {ground, box};

    addQuad(*sc, {-20, 0, -20}, {20, 0, -20}, {20, 0, 20}, {-20, 0, 20}, {0, 1, 0}, 0);
    addBox(*sc, {0.0f, 2.0f, 0.0f}, {1.0f, 0.5f, 1.0f}, 1);

    sc->sun.enabled          = true;
    sc->sun.direction        = glm::vec3(0.0f, 1.0f, 0.0f); // straight overhead
    sc->sun.color            = glm::vec3(3.0f);
    sc->sun.angularRadiusDeg = 0.0f;                        // hard edge, so the
                                                            // test can be exact
    sc->env.zenith = sc->env.horizon = sc->env.ground = glm::vec3(0.05f);
    // Looking straight down from high up, so a pixel maps to a ground position
    // by simple trigonometry.
    sc->camera = lookAt({0.0f, 12.0f, 0.001f}, {0.0f, 0.0f, 0.0f}, 60.0f);
    return sc;
}

// --- The lamp ---------------------------------------------------------------
// The same box over the same plane, lit by ONE point lamp and by nothing else:
// the sun is switched off and the sky is black, so every photon in the picture
// came from the lamp or there is no picture. That is what this frame is for --
// in every other scene here the sun would have covered for a lamp that was
// never sampled, and the failure would have read as "a bit dark".
//
// The lamp sits off to one side so the box's shadow falls clear of the box
// itself, where a camera looking straight down can see it: from (0, 6, 3)
// through the box, the ground is met around z = -1.5.
inline std::shared_ptr<pathtrace::Scene> lampScene(float range) {
    auto sc = std::make_shared<pathtrace::Scene>();
    pathtrace::Material ground;
    ground.albedo    = glm::vec3(0.6f);
    ground.roughness = 0.9f;
    pathtrace::Material box;
    box.albedo    = glm::vec3(0.2f, 0.35f, 0.8f);
    box.roughness = 0.4f;
    sc->materials = {ground, box};

    addQuad(*sc, {-20, 0, -20}, {20, 0, -20}, {20, 0, 20}, {-20, 0, 20}, {0, 1, 0}, 0);
    addBox(*sc, {0.0f, 2.0f, 0.0f}, {1.0f, 0.5f, 1.0f}, 1);

    sc->sun.enabled = false;
    sc->env.zenith = sc->env.horizon = sc->env.ground = glm::vec3(0.0f);

    pathtrace::Lamp lamp;
    lamp.position = {0.0f, 6.0f, 3.0f};
    lamp.color    = glm::vec3(8.0f);
    lamp.range    = range;
    lamp.radius   = 0.0f;   // a bare point, so the shadow edge is exact
    lamp.cosOuter = -2.0f;  // omnidirectional -- a point light, not a cone
    sc->lamps.push_back(lamp);

    sc->camera = lookAt({0.0f, 12.0f, 0.001f}, {0.0f, 0.0f, 0.0f}, 60.0f);
    return sc;
}

// --- A look -------------------------------------------------------------
// Not a test: three spheres-worth of material variety over a ground plane, so
// that "is the specular lobe sane, does glass look like glass, is the soft
// shadow actually soft" has a picture to be answered from.
inline std::shared_ptr<pathtrace::Scene> lookScene() {
    auto sc = std::make_shared<pathtrace::Scene>();
    pathtrace::Material ground;
    ground.albedo    = glm::vec3(0.30f, 0.30f, 0.32f);
    ground.roughness = 0.6f;

    pathtrace::Material paint;   // a car's body: glossy, coloured
    paint.albedo       = glm::vec3(0.65f, 0.08f, 0.06f);
    paint.roughness    = 0.15f;
    paint.reflectivity = 0.35f;

    pathtrace::Material chrome;
    chrome.albedo       = glm::vec3(0.95f);
    chrome.roughness    = 0.05f;
    chrome.reflectivity = 1.0f;

    pathtrace::Material glass;
    glass.albedo    = glm::vec3(0.85f, 0.95f, 0.9f);
    glass.roughness = 0.05f;
    glass.glass     = true;
    glass.opacity   = 0.15f;

    sc->materials = {ground, paint, chrome, glass};

    addQuad(*sc, {-40, 0, -40}, {40, 0, -40}, {40, 0, 40}, {-40, 0, 40}, {0, 1, 0}, 0);
    addBox(*sc, {-2.2f, 0.8f, 0.0f}, {0.8f, 0.8f, 0.8f}, 1);
    addBox(*sc, { 0.0f, 0.8f, 0.0f}, {0.8f, 0.8f, 0.8f}, 2);
    addBox(*sc, { 2.2f, 0.8f, 0.0f}, {0.8f, 0.8f, 0.8f}, 3);

    sc->sun.direction        = glm::normalize(glm::vec3(0.55f, 0.7f, 0.4f));
    sc->sun.color            = glm::vec3(2.2f, 2.05f, 1.85f);
    sc->sun.angularRadiusDeg = 1.5f;   // visibly soft, so the penumbra shows
    sc->env.zenith  = glm::vec3(0.16f, 0.24f, 0.42f);
    sc->env.horizon = glm::vec3(0.34f, 0.40f, 0.50f);
    sc->env.ground  = glm::vec3(0.09f, 0.085f, 0.08f);

    pathtrace::Lamp lamp;              // a warm fill from the left
    lamp.position = {-4.0f, 2.5f, 3.0f};
    lamp.color    = glm::vec3(6.0f, 3.2f, 1.4f);
    lamp.range    = 12.0f;
    lamp.radius   = 0.35f;
    sc->lamps.push_back(lamp);

    // Deliberately from the LEFT, so the chrome block's left face has the red
    // one in front of it. A mirror photographed against an empty sky is
    // indistinguishable from a flat white box, and that is exactly the mistake
    // this frame exists to catch.
    sc->camera = lookAt({-3.6f, 1.9f, 6.6f}, {0.2f, 0.9f, 0.0f}, 40.0f);
    sc->camera.apertureRadius = 0.03f;
    sc->camera.focusDistance  = 7.2f;
    // The editor's own starting grade, not a neutral one: this frame exists to
    // show what a render actually looks like, and a render never comes out
    // ungraded.
    sc->grade.saturation = 1.35f;
    sc->grade.warmth     = 0.18f;
    sc->grade.contrast   = 0.16f;
    sc->exposure = 1.0f;
    return sc;
}

// --- The texture ------------------------------------------------------------
// Everything a base-colour map has to survive on the way to a picture, in one
// frame: a tiled checker on a floor whose UVs run well past 1 (so wrapping is
// exercised, and a wrap done with a signed remainder tears where the
// coordinates cross zero), a coloured gradient on a box with a TINT over it,
// and a cutout quad standing in front of the box, whose holes have to be holes
// to the camera AND to the sun.
//
// Sharper as a comparison than as a picture: a tracer that samples with the rows
// swapped, or clamps where it should wrap, or reads the alpha of an opaque
// material as transparency, all still render -- and each of them renders
// differently from the other tracer, which is what gpucheck measures.
inline pathtrace::Image checkerImage(int n, const glm::vec3& a, const glm::vec3& b) {
    pathtrace::Image img;
    img.width = img.height = n;
    img.pixels.resize(static_cast<std::size_t>(n) * n * 4);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            // Squares of a quarter of the image, so a picture that is off by a
            // texel is not the same picture at all.
            const bool odd = ((x * 4 / n) + (y * 4 / n)) % 2 == 1;
            const glm::vec3 c = odd ? b : a;
            const std::size_t o = (static_cast<std::size_t>(y) * n + x) * 4;
            for (int k = 0; k < 3; ++k)
                img.pixels[o + k] = static_cast<unsigned char>(
                    glm::clamp(c[k], 0.0f, 1.0f) * 255.0f + 0.5f);
            img.pixels[o + 3] = 255;
        }
    return img;
}

// A gradient across x and y, so a swapped axis is a visibly different picture
// rather than a subtly different one. The alpha carries a disc: opaque in the
// middle, empty at the corners -- the shape a cutout cuts.
inline pathtrace::Image gradientImage(int n) {
    pathtrace::Image img;
    img.width = img.height = n;
    img.pixels.resize(static_cast<std::size_t>(n) * n * 4);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(n);
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(n);
            const float r = glm::length(glm::vec2(u, v) - glm::vec2(0.5f));
            const std::size_t o = (static_cast<std::size_t>(y) * n + x) * 4;
            img.pixels[o + 0] = static_cast<unsigned char>(u * 255.0f);
            img.pixels[o + 1] = static_cast<unsigned char>(v * 255.0f);
            img.pixels[o + 2] = static_cast<unsigned char>((1.0f - u) * 200.0f);
            img.pixels[o + 3] = r < 0.36f ? 255 : 0;
        }
    return img;
}

inline std::shared_ptr<pathtrace::Scene> textureScene() {
    auto sc = std::make_shared<pathtrace::Scene>();
    sc->textures.push_back(checkerImage(64, glm::vec3(0.85f, 0.82f, 0.75f),
                                            glm::vec3(0.12f, 0.14f, 0.20f)));
    sc->textures.push_back(gradientImage(64));

    pathtrace::Material floor;      // tiled, and OPAQUE: its alpha means nothing
    floor.texture   = 0;
    floor.roughness = 0.7f;
    floor.alphaMode = 0;

    pathtrace::Material painted;    // the same map through a tint
    painted.texture   = 1;
    painted.tint      = glm::vec3(1.0f, 0.6f, 0.35f);
    painted.roughness = 0.35f;
    painted.alphaMode = 0;

    pathtrace::Material cut;        // a cutout: alpha below the line is a hole
    cut.texture     = 1;
    cut.roughness   = 0.5f;
    cut.alphaMode   = 1;
    cut.alphaCutoff = 0.5f;

    sc->materials = {floor, painted, cut};

    // The floor's UVs run 0..6, so the checker tiles and the wrap is what puts
    // it there. addQuad lays down 0..1, so this one is built by hand.
    {
        pathtrace::Triangle t0, t1;
        const glm::vec3 a{-6, 0, -6}, b{6, 0, -6}, c{6, 0, 6}, d{-6, 0, 6};
        const glm::vec3 n{0, 1, 0};
        t0.p0 = a; t0.p1 = b; t0.p2 = c; t0.n0 = t0.n1 = t0.n2 = n;
        t0.uv0 = {-3, -3}; t0.uv1 = {3, -3}; t0.uv2 = {3, 3};
        t0.material = 0;
        t1.p0 = a; t1.p1 = c; t1.p2 = d; t1.n0 = t1.n1 = t1.n2 = n;
        t1.uv0 = {-3, -3}; t1.uv1 = {3, 3}; t1.uv2 = {-3, 3};
        t1.material = 0;
        sc->triangles.push_back(t0);
        sc->triangles.push_back(t1);
    }
    addBox(*sc, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, 1);
    // The cutout stands between the sun and the box, upright and facing the
    // camera, so its holes show as light on the box behind it.
    addQuad(*sc, {-2.6f, 0.05f, 2.2f}, {-0.2f, 0.05f, 2.2f},
                 {-0.2f, 2.45f, 2.2f}, {-2.6f, 2.45f, 2.2f}, {0, 0, 1}, 2);

    sc->sun.direction        = glm::normalize(glm::vec3(0.25f, 0.65f, 0.72f));
    sc->sun.color            = glm::vec3(2.6f, 2.5f, 2.3f);
    sc->sun.angularRadiusDeg = 0.6f;
    sc->env.zenith  = glm::vec3(0.20f, 0.28f, 0.45f);
    sc->env.horizon = glm::vec3(0.36f, 0.42f, 0.52f);
    sc->env.ground  = glm::vec3(0.10f, 0.10f, 0.09f);
    sc->camera = lookAt({-2.4f, 2.6f, 6.4f}, {0.0f, 0.9f, 0.0f}, 45.0f);
    sc->exposure = 1.0f;
    return sc;
}

// --- The ground -------------------------------------------------------------
// A terrain, which is coloured by nothing a normal material does: no UVs, but
// layers that claim the ground by its HEIGHT and SLOPE, projected down all three
// axes, jittered at the band edges by a noise field, and overridden where
// somebody has painted. Every one of those is a place two renderers can part
// company while both producing a picture of a hillside.
//
// So: a hill and a valley, a flat layer and a steep one whose bands overlap
// where the slope turns, and a painted stripe across the middle that has to show
// the steep layer on ground that is not steep.
// `n` is the resolution of the field: 24 is the frame you can read, and a few
// hundred is the same picture over a tree deep enough to be a different test --
// the traversal stack is sized from the tree's depth, so a scene of a handful of
// triangles never exercises the arithmetic that a real one does.
inline std::shared_ptr<pathtrace::Scene> terrainScene(int n = 24) {
    auto sc = std::make_shared<pathtrace::Scene>();
    sc->textures.push_back(checkerImage(32, glm::vec3(0.24f, 0.42f, 0.16f),
                                            glm::vec3(0.32f, 0.50f, 0.20f)));
    sc->textures.push_back(checkerImage(32, glm::vec3(0.52f, 0.48f, 0.44f),
                                            glm::vec3(0.34f, 0.31f, 0.28f)));

    pathtrace::Material ground;
    ground.roughness = 0.85f;
    // Grass on the low and flat, rock on the high and steep, with the bands
    // overlapping so the cross-fade is exercised rather than a hard edge.
    pathtrace::TerrainLayer grass;
    grass.texture = 0;
    grass.band    = glm::vec4(-4.0f, 2.5f, 0.0f, 34.0f);   // height lo/hi, slope lo/hi
    grass.scale   = 0.35f;
    pathtrace::TerrainLayer rock;
    rock.texture = 1;
    rock.band    = glm::vec4(0.5f, 12.0f, 18.0f, 90.0f);
    rock.scale   = 0.22f;
    ground.layers = {grass, rock};
    ground.detailScale = 0.12f;    // the height-edge jitter, as the shader has it
    sc->materials.push_back(ground);

    // A heightfield, because that is what a terrain is: bands read the height
    // and the slope, and a flat plane would test neither.
    const float half = 7.0f;
    auto heightAt = [](float x, float z) {
        return 1.6f * std::sin(x * 0.42f) * std::cos(z * 0.36f) + 0.4f * std::sin(z * 0.9f);
    };
    auto at = [&](int i, int j) {
        const float x = -half + 2.0f * half * (static_cast<float>(i) / n);
        const float z = -half + 2.0f * half * (static_cast<float>(j) / n);
        return glm::vec3(x, heightAt(x, z), z);
    };
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const glm::vec3 a = at(i, j),     b = at(i + 1, j);
            const glm::vec3 c = at(i + 1, j + 1), d = at(i, j + 1);
            // Flat-shaded, so the slope a band tests is the triangle's own --
            // an interpolated normal would make the two renderers agree about
            // the picture while disagreeing about which layer is where.
            // Wound so the normal points UP. Not a detail: every band here is
            // read off the slope, and a heightfield whose normals point at the
            // ground is 180 degrees steep everywhere -- which no layer covers,
            // so the whole terrain falls back to its flat colour and the frame
            // tests nothing while looking like a hillside.
            for (int t = 0; t < 2; ++t) {
                pathtrace::Triangle tri;
                tri.p0 = a;
                tri.p1 = t == 0 ? c : d;
                tri.p2 = t == 0 ? b : c;
                const glm::vec3 nrm =
                    glm::normalize(glm::cross(tri.p1 - tri.p0, tri.p2 - tri.p0));
                tri.n0 = tri.n1 = tri.n2 = nrm;
                tri.material = 0;
                sc->triangles.push_back(tri);
            }
        }

    // The painted stripe: full weight on the second layer across the middle of
    // the field, which is flat ground the automatic bands would give to grass.
    // Three weights per triangle, in the triangles' own order -- the layout
    // paintAt() reads on both sides.
    sc->vertexPaint.resize(sc->triangles.size() * 3, glm::vec4(0.0f));
    for (std::size_t t = 0; t < sc->triangles.size(); ++t) {
        const pathtrace::Triangle& tri = sc->triangles[t];
        const glm::vec3 corner[3] = {tri.p0, tri.p1, tri.p2};
        for (int k = 0; k < 3; ++k) {
            const float w = corner[k].z > -1.2f && corner[k].z < 1.2f ? 1.0f : 0.0f;
            sc->vertexPaint[t * 3 + k] = glm::vec4(0.0f, w, 0.0f, 0.0f);
        }
    }

    sc->sun.direction        = glm::normalize(glm::vec3(0.4f, 0.75f, 0.5f));
    sc->sun.color            = glm::vec3(2.4f, 2.3f, 2.1f);
    sc->sun.angularRadiusDeg = 0.6f;
    sc->env.zenith  = glm::vec3(0.22f, 0.30f, 0.48f);
    sc->env.horizon = glm::vec3(0.38f, 0.44f, 0.54f);
    sc->env.ground  = glm::vec3(0.10f, 0.10f, 0.09f);
    sc->camera = lookAt({0.0f, 5.5f, 11.0f}, {0.0f, 0.0f, 0.0f}, 45.0f);
    sc->exposure = 1.0f;
    return sc;
}

// --- The furnace ------------------------------------------------------------
// One horizontal plane, albedo 1, roughness 1, no reflectivity, no sun, and an
// environment that is uniformly 1. Every pixel that lands on the plane should
// come back at 1, and every pixel that misses it should come back at 1 too --
// the surface is indistinguishable from the sky it stands in. That is what
// makes this test sharp: an integrator that loses a bounce shows up as a plane
// darker than the background, and one that double-counts shows up as brighter.
inline std::shared_ptr<pathtrace::Scene> furnaceScene() {
    auto sc = std::make_shared<pathtrace::Scene>();
    pathtrace::Material m;
    m.albedo       = glm::vec3(1.0f);
    m.roughness    = 1.0f;
    m.reflectivity = 0.0f;
    sc->materials.push_back(m);

    addQuad(*sc, {-50, 0, -50}, {50, 0, -50}, {50, 0, 50}, {-50, 0, 50}, {0, 1, 0}, 0);

    sc->sun.enabled = false;
    sc->env.zenith = sc->env.horizon = sc->env.ground = glm::vec3(1.0f);
    sc->env.intensity = 1.0f;
    sc->camera = lookAt({0.0f, 2.0f, 6.0f}, {0.0f, 0.0f, 0.0f}, 50.0f);
    sc->exposure = 1.0f;
    return sc;
}

} // namespace tracescenes
