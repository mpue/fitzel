#include "CameraTexture.hpp"

#include <algorithm>

#include <glad/gl.h>
#include <glm/gtc/matrix_transform.hpp>

#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Texture.hpp>

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
    // The framebuffers and the viewport as they were, put back at the end --
    // taken only once there is a picture to draw.
    GLint drawFbo = 0, readFbo = 0, vp[4] = {0, 0, 0, 0};
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
                    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
                    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
                    glGetIntegerv(GL_VIEWPORT, vp);
                    saved = true;
                }
                const float aspect = static_cast<float>(size.x) / static_cast<float>(size.y);
                const glm::mat4 proj = glm::perspective(glm::radians(glm::clamp(p.fov, 5.0f, 150.0f)), aspect,
                                                        nearPlane, farPlane);
                const glm::mat4 view = glm::lookAt(p.position, p.position + p.front, p.up);
                f.target->bind();
                glClearColor(0.05f, 0.06f, 0.07f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                draw(view, proj, p);
                // ...and into the texture the surfaces sample (see the header).
                glBindFramebuffer(GL_READ_FRAMEBUFFER, f.target->framebuffer());
                glBindTexture(GL_TEXTURE_2D, f.picture->id());
                glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, size.x, size.y);
                glBindTexture(GL_TEXTURE_2D, 0);
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
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
        glViewport(vp[0], vp[1], vp[2], vp[3]);
    }
    // A picture no material shows any more is let go (a surface still holding
    // it keeps its last frame).
    for (auto it = m_feeds.begin(); it != m_feeds.end();)
        it = it->second.used ? std::next(it) : m_feeds.erase(it);
}

} // namespace camtex
