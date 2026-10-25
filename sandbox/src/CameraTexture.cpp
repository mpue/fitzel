#include "CameraTexture.hpp"

#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/rhi/Device.hpp>

#include "Component.hpp"   // CameraComponent

namespace camtex {

CameraTextures::CameraTextures()  = default;
CameraTextures::~CameraTextures() = default;

void CameraTextures::clear() { m_feeds.clear(); }

const Entity* findCamera(const std::vector<Entity>& entities, const std::string& name) {
    if (name.empty()) return nullptr;
    for (const Entity& e : entities)
        if (e.name == name && e.components.get<CameraComponent>()) return &e;
    return nullptr;
}

void CameraTextures::update(std::vector<MaterialDef>& materials, const std::vector<Entity>& entities,
                            const PoseOf& pose, float nearPlane, float farPlane, double now,
                            const DrawView& draw) {
    m_drawn = 0;
    for (auto& [key, f] : m_feeds) f.used = false;
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    // The framebuffer and the viewport as they were, put back at the end --
    // taken only once there is a picture to draw.
    fitzel::rhi::FramebufferHandle fbo;
    fitzel::rhi::Rect              vp;
    bool  saved = false;

    for (MaterialDef& md : materials) {
        if (md.cameraName.empty()) continue;
        const Entity* cam = findCamera(entities, md.cameraName);
        // No such camera in this scene: the surface keeps what it last showed.
        if (!cam) continue;
        const glm::ivec2 size = glm::clamp(md.cameraSize, glm::ivec2(16), glm::ivec2(4096));
        const std::string key = md.cameraName + "#" + std::to_string(size.x) + "x" + std::to_string(size.y);
        Feed& f = m_feeds[key];
        if (!f.picture) {
            f.target  = std::make_unique<fitzel::RenderTarget>(size.x, size.y, fitzel::RenderTarget::Format::RGBA8);
            f.picture = std::make_shared<fitzel::Texture>(fitzel::Texture::blank(size.x, size.y));
        }
        // Drawn once, for the first material that shows it.
        if (!f.used) {
            f.used = true;
            camerasys::Pose p;
            if (now >= f.next && pose(cam->id, p)) {
                f.next = now + kInterval;
                if (!saved) {
                    fbo = gpu.boundFramebuffer();
                    vp  = gpu.viewport();
                    saved = true;
                }
                const float aspect = static_cast<float>(size.x) / static_cast<float>(size.y);
                const glm::mat4 proj = glm::perspective(glm::radians(glm::clamp(p.fov, 5.0f, 150.0f)), aspect,
                                                        nearPlane, farPlane);
                const glm::mat4 view = glm::lookAt(p.position, p.position + p.front, p.up);
                f.target->bind();
                gpu.setClearColor(glm::vec4(0.05f, 0.06f, 0.07f, 1.0f));
                gpu.clear(fitzel::rhi::ClearColor | fitzel::rhi::ClearDepth);
                draw(view, proj, p);
                // ...and into the texture the surfaces sample (see the header).
                gpu.copyToTexture(f.target->framebuffer(), 0,
                                  fitzel::rhi::Rect{0, 0, size.x, size.y}, f.picture->id(),
                                  0, -1, 0, 0);
                ++m_drawn;
            }
        }
        if (md.tex != f.picture) md.tex = f.picture;
        // Glowing: the picture is the emission map too -- unless the material
        // has a map of its own there. Not glowing any more: the map it had.
        if (md.cameraGlow) {
            if (!md.emissionTexId.valid() && md.emissionTex != f.picture) md.emissionTex = f.picture;
        } else if (md.emissionTex == f.picture) {
            md.emissionTex = md.modelEmissionTex;
        }
    }
    if (saved) {
        gpu.bindFramebuffer(fbo);
        gpu.setViewport(vp);
    }
    // A picture no material shows any more is let go (a surface still holding
    // it keeps its last frame).
    for (auto it = m_feeds.begin(); it != m_feeds.end();)
        it = it->second.used ? std::next(it) : m_feeds.erase(it);
}

} // namespace camtex
