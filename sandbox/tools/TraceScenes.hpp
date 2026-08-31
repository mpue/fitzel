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
