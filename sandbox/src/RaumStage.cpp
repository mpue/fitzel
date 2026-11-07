#include "RaumStage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/scene/Camera.hpp>

#if FITZEL_RAUM_STAGE
#define GLFW_EXPOSE_NATIVE_WAYLAND
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <wayland-client.h>

#include "raum-stage-v1-client-protocol.h"
#endif

namespace rhi = fitzel::rhi;

namespace {

// The finished picture's alpha. Drawn from raum's camera: 0 where the sky is
// (nothing drawn, depth at the far end) and where the world lies outside the
// square on the table, 1 on the square's world; drawn from our own camera: 1
// everywhere. The blend keeps the colour where alpha is 1 and clears it where
// it is 0 -- the picture goes out premultiplied, as Wayland wants it.
const char* kMaskVert = R"(#version 330 core
layout(location = 0) in vec3 aPos;
out vec2 vUV;
void main() {
    vUV = aPos.xy * 0.5 + 0.5;
    gl_Position = vec4(aPos.xy, 0.0, 1.0);
})";
const char* kMaskFrag = R"(#version 330 core
in vec2 vUV;
out vec4 frag;
uniform sampler2D uDepth;
uniform mat4 uInvViewProj;
uniform vec3 uSquare;   // centre x, centre z, half the edge
uniform int uMask;
void main() {
    float a = 1.0;
    if (uMask != 0) {
        float d = texture(uDepth, vUV).r;
        if (d >= 0.999999) {
            a = 0.0;
        } else {
            vec4 p = uInvViewProj * vec4(vUV * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
            vec2 q = abs(p.xz / p.w - uSquare.xy);
            if (max(q.x, q.y) > uSquare.z) a = 0.0;
        }
    }
    frag = vec4(0.0, 0.0, 0.0, a);
})";

} // namespace

struct RaumStage::Impl {
#if FITZEL_RAUM_STAGE
    wl_display*            display  = nullptr;
    wl_event_queue*        queue    = nullptr;
    wl_display*            wrapper  = nullptr;   // the display, its events onto `queue`
    wl_registry*           registry = nullptr;
    raum_stage_manager_v1* manager  = nullptr;
    raum_stage_v1*         stage    = nullptr;
#endif
    // raum's latest camera event: serial 0 = draw from our own camera.
    std::uint32_t serial = 0;
    float         view[14] = {};
    bool          haveTable = false;

    glm::vec3 worldCentre{0.0f};
    float     worldSize = 1024.0f;

    fitzel::Shader mask;
    bool           maskReady = false;

    // The table's middle and raum units per metre.
    glm::vec3 tableCentre() const { return {view[10], view[11], view[12]}; }
    float scale() const { return std::max(view[13], 1e-3f) / std::max(worldSize, 1e-3f); }
    glm::vec3 toWorld(const glm::vec3& r) const { return worldCentre + (r - tableCentre()) / scale(); }
    glm::vec3 toRaum(const glm::vec3& w) const { return tableCentre() + (w - worldCentre) * scale(); }
    void cleanup();
};

#if FITZEL_RAUM_STAGE
namespace {

void onGlobal(void* data, wl_registry* registry, std::uint32_t name, const char* interface,
              std::uint32_t version) {
    auto* m = static_cast<RaumStage::Impl*>(data);
    if (std::strcmp(interface, raum_stage_manager_v1_interface.name) == 0 && !m->manager)
        m->manager = static_cast<raum_stage_manager_v1*>(
            wl_registry_bind(registry, name, &raum_stage_manager_v1_interface, 1));
    (void)version;
}
void onGlobalRemove(void*, wl_registry*, std::uint32_t) {}
const wl_registry_listener kRegistry{onGlobal, onGlobalRemove};

void onCamera(void* data, raum_stage_v1*, std::uint32_t serial, wl_array* view) {
    auto* m = static_cast<RaumStage::Impl*>(data);
    const std::size_t n = std::min<std::size_t>(view->size / sizeof(float), 14);
    if (n < 14) return;   // not a camera this side understands
    std::memcpy(m->view, view->data, n * sizeof(float));
    m->serial = serial;
    m->haveTable = true;
}
const raum_stage_v1_listener kStage{onCamera};

void putCamera(float* out, const glm::vec3& eye, const fitzel::Camera& cam) {
    const glm::vec3 f = cam.front(), u = cam.up();
    const float v[10] = {eye.x, eye.y, eye.z, f.x, f.y, f.z, u.x, u.y, u.z, cam.fov()};
    std::memcpy(out, v, sizeof(v));
}

} // namespace
#endif

void RaumStage::Impl::cleanup() {
#if FITZEL_RAUM_STAGE
    if (stage) raum_stage_v1_destroy(stage);
    if (manager) raum_stage_manager_v1_destroy(manager);
    if (registry) wl_registry_destroy(registry);
    if (wrapper) wl_proxy_wrapper_destroy(wrapper);
    if (queue) wl_event_queue_destroy(queue);
    stage = nullptr;
    manager = nullptr;
    registry = nullptr;
    wrapper = nullptr;
    queue = nullptr;
    display = nullptr;
#endif
}

RaumStage::RaumStage() : m(std::make_unique<Impl>()) {}
RaumStage::~RaumStage() { m->cleanup(); }

bool RaumStage::wanted() {
    const char* e = std::getenv("RAUM_STAGE");
    return e && *e && std::strcmp(e, "0") != 0;
}

bool RaumStage::connect(GLFWwindow* window) {
#if FITZEL_RAUM_STAGE
    if (!window || glfwGetPlatform() != GLFW_PLATFORM_WAYLAND) return false;
    m->display = glfwGetWaylandDisplay();
    wl_surface* surface = glfwGetWaylandWindow(window);
    if (!m->display || !surface) return false;
    // Our own queue: GLFW reads the connection and dispatches its default
    // queue; what raum says to the stage waits on this one until poll().
    m->queue = wl_display_create_queue(m->display);
    m->wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(m->display));
    wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(m->wrapper), m->queue);
    m->registry = wl_display_get_registry(m->wrapper);
    wl_registry_add_listener(m->registry, &kRegistry, m.get());
    wl_display_roundtrip_queue(m->display, m->queue);
    if (!m->manager) {
        m->cleanup();
        return false;
    }
    m->stage = raum_stage_manager_v1_get_stage(m->manager, surface);
    raum_stage_v1_add_listener(m->stage, &kStage, m.get());
    // raum answers with where the table is.
    wl_display_roundtrip_queue(m->display, m->queue);
    return true;
#else
    (void)window;
    return false;
#endif
}

bool RaumStage::connected() const {
#if FITZEL_RAUM_STAGE
    return m->stage != nullptr;
#else
    return false;
#endif
}

void RaumStage::poll() {
#if FITZEL_RAUM_STAGE
    if (m->stage) wl_display_dispatch_queue_pending(m->display, m->queue);
#endif
}

bool RaumStage::diorama() const { return connected() && m->serial != 0 && m->haveTable; }

void RaumStage::setWorld(const glm::vec3& centre, float size) {
    m->worldCentre = centre;
    m->worldSize = std::max(size, 1.0f);
}

glm::vec3 RaumStage::worldCentre() const { return m->worldCentre; }
float RaumStage::worldSize() const { return m->worldSize; }

void RaumStage::applyCamera(fitzel::Camera& cam) const {
    if (!diorama()) return;
    const float* v = m->view;
    const glm::vec3 eye = m->toWorld({v[0], v[1], v[2]});
    cam.setPosition(eye);
    cam.setBasis({v[3], v[4], v[5]}, {v[6], v[7], v[8]});
    cam.setFov(v[9]);
    // From a kilometre or two off: the far plane past the whole square, the near
    // one pushed out with the distance so the depth keeps its precision there.
    const float dist = glm::length(eye - m->worldCentre);
    cam.setFarPlane(std::max(cam.farPlane(), dist + m->worldSize * 1.5f));
    cam.setNearPlane(std::clamp(dist * 0.04f, 0.1f, 50.0f));
}

void RaumStage::restoreCamera(fitzel::Camera& cam, const fitzel::Camera& own) {
    cam.setPosition(own.position());
    cam.setYaw(own.yaw());
    cam.setPitch(own.pitch());
    cam.setHeadTilt(own.headPitch(), own.headRoll());
    cam.setBasis(own.front(), own.up());   // exactly, if it was set outright
    cam.setFov(own.fov());
    cam.setNearPlane(own.nearPlane());
    cam.setFarPlane(own.farPlane());
}

void RaumStage::finishPicture(fitzel::Mesh& fsQuad, rhi::TextureHandle depth,
                              const glm::mat4& viewProj, int width, int height) {
    if (!connected()) return;
    if (!m->maskReady) {
        m->mask = fitzel::Shader::fromSource(kMaskVert, kMaskFrag);
        m->maskReady = true;
    }
    rhi::Device& dev = rhi::device();
    rhi::RenderState s;
    s.blend.enabled  = true;
    s.blend.srcColor = rhi::BlendFactor::Zero;
    s.blend.dstColor = rhi::BlendFactor::SrcAlpha;
    s.blend.srcAlpha = rhi::BlendFactor::One;
    s.blend.dstAlpha = rhi::BlendFactor::Zero;
    s.depth.test  = false;
    s.depth.write = false;
    dev.setRenderState(s);
    dev.setViewport(0, 0, width, height);
    m->mask.bind();
    const bool diorama = this->diorama();
    m->mask.setInt("uMask", diorama && depth.valid() ? 1 : 0);
    m->mask.setInt("uDepth", 0);
    m->mask.setMat4("uInvViewProj", glm::inverse(viewProj));
    m->mask.setVec3("uSquare", glm::vec3(m->worldCentre.x, m->worldCentre.z, m->worldSize * 0.5f));
    if (depth.valid()) dev.bindTexture(0, depth);
    fsQuad.draw();
    dev.setRenderState(rhi::RenderState{});
}

void RaumStage::frameDrawn(const fitzel::Camera& drawn, const fitzel::Camera& own) {
#if FITZEL_RAUM_STAGE
    if (!m->stage || !m->haveTable) return;
    float cams[20];
    putCamera(cams, m->toRaum(drawn.position()), drawn);
    putCamera(cams + 10, m->toRaum(own.position()), own);
    wl_array a;
    wl_array_init(&a);
    if (void* p = wl_array_add(&a, sizeof(cams))) std::memcpy(p, cams, sizeof(cams));
    raum_stage_v1_frame(m->stage, diorama() ? m->serial : 0u, &a);
    wl_array_release(&a);
#else
    (void)drawn;
    (void)own;
#endif
}
