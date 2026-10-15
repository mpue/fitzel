#pragma once

#include <optional>

#include <glm/glm.hpp>

#include <fitzel/graphics/RenderTarget.hpp>

#include "HalfResSky.hpp"
#include "OcclusionGate.hpp"
#include "PostChain.hpp"
#include "VolumetricFog.hpp"

// Everything one view of the world is drawn through that is sized to the view
// or carries history from one of its frames to the next: the HDR buffer, the
// post chain (TAA, bloom, auto exposure), the finished picture, the half-res
// sky and the fog's own buffer, and whether the lake drew a pixel lately.
//
// The first view has these as plain locals of main; the editor's further 3D
// views (EditorViews.hpp) each own a set. Shared by both builds only so the
// render loop reads the same either way -- the player never makes one.
struct ViewTargets {
    fitzel::RenderTarget hdr{1, 1, fitzel::RenderTarget::Format::RGBA16F,
                             /*depthTex=*/true, /*stencil=*/true};
    fitzel::RenderTarget viewport{1, 1, fitzel::RenderTarget::Format::RGBA8};
    // The target the view was resized AWAY from, alive until the frame it
    // still appears in has been drawn (see main's retiredRT for why).
    std::optional<fitzel::RenderTarget> retired;
    PostChain     post;
    HalfResSky    halfSky;
    VolumetricFog volFog;
    OcclusionGate waterGate;
    // Temporal AA's memory of this view's last frame.
    glm::mat4 taaPrevVP{1.0f};
    glm::vec3 taaPrevEye{0.0f};
    bool      taaHavePrev = false;

    bool init() {
        halfSky.init();
        volFog.init();            // survivable, as in main: no fog, not no view
        return post.init();
    }
};
