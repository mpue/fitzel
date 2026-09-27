#pragma once

#include <functional>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/RenderTarget.hpp>
#include <fitzel/graphics/Shader.hpp>

// The main view's sky at a fraction of the resolution, stretched back up.
//
// The sky is a full-screen raymarch (sky.frag: the layer stack and one marched
// cumulus) and it was the single most expensive thing in a frame: at 3440 x
// 1440 about 20 ms of GPU time, more than the whole scene, and paid for every
// pixel whether a house stood in front of it or not. Clouds are soft and the
// scene is drawn over the sky at full resolution afterwards, so rendering it
// smaller and upsampling it bilinearly costs nothing you can see and gives most
// of that time back. How much smaller follows the screen: the sky keeps at
// least `minRows` rows (480 -- a third of 1440p, half of 1080p, a quarter of
// 4K), which is where a side-by-side comparison stopped showing a difference.
//
// Only the main view goes through here: the environment probe and the water
// mirror draw the sky into small targets already.
class HalfResSky {
public:
    bool init();   // the upsampling shader

    // Draw the sky with `draw` into this object's reduced target, then stretch
    // it over the whole of `dst` (which is bound again afterwards). A target
    // too small to reduce is drawn straight into, as before.
    void render(const fitzel::RenderTarget& dst, const fitzel::Mesh& quad,
                const std::function<void()>& draw);

    // The same, drawn LAST, behind a finished scene: only where `dst` still
    // holds the cleared depth (1.0) and a zero stencil -- what the caller's
    // scene left of the sky. At the reduced size the sky is marched only for
    // texels with sky within one texel of them (the stretch filters across
    // one), so a street view pays for its strip of sky, not for the houses
    // in front of it. `dst` needs a stencil (RenderTarget's `stencil`). Leaves
    // `dst` bound, the stencil test on, stencil writes off.
    void renderBehind(const fitzel::RenderTarget& dst, const fitzel::Mesh& quad,
                      const std::function<void()>& draw);

    int minRows = 480;

private:
    int divisorFor(const fitzel::RenderTarget& dst) const;

    fitzel::RenderTarget m_rt{1, 1, fitzel::RenderTarget::Format::RGBA16F};
    fitzel::RenderTarget m_masked{1, 1, fitzel::RenderTarget::Format::RGBA16F, true, true};
    fitzel::RenderTarget m_cover{1, 1, fitzel::RenderTarget::Format::RGBA8};
    fitzel::Shader       m_blit, m_coverShader, m_maskShader;
};
