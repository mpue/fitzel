#pragma once

#include <fitzel/render/Renderer.hpp>   // kDefaultEnvProbeRes

// How the scene's picture is finished: exposure and tonemap, bloom and sun rays,
// ambient occlusion, reflections, depth of field, motion blur, anti-aliasing and
// the colour grade -- the post chain's knobs, as the scene's author set them.
//
// They stay out of the chain itself: they are edited by the Sky & atmosphere and
// Colour grade panels (LookPanels.hpp), saved with the scene (main's settings
// registry), gated by the player's graphics choices where they are consumed
// (gfxmenu::gatePost), and handed to the chain every frame. One value rather
// than thirty-four loose ones in main().
//
// Each section of the Post processing panel has a switch of its own. Off keeps
// the section's values -- switching it back on returns the look that was set,
// which a slider pulled to zero could not. resolved() is where a switch takes
// effect: it is the look as the chain should see it.
struct PostLook {
    bool gradeOn      = true;   // colour grade
    bool bloomOn      = true;   // bloom, sun rays and the lens flare
    bool aoOn         = true;   // screen-space ambient occlusion
    bool dofOn        = true;   // depth of field
    bool lensOn       = true;   // vignette and film grain
    bool motionBlurOn = true;   // the speed blur
    // The comic look (composite.frag comic()): ink lines, cel bands, dot
    // screen. An experiment, so off unless a scene or a script asks for it.
    bool comicOn      = false;

    float bloomIntensity = 0.35f;
    float rayIntensity   = 0.5f;
    float bloomThreshold = 1.0f;  // luminance where the glow starts
    float bloomKnee      = 0.5f;  // soft-knee width below it

    bool fxaaEnabled = true;
    // Temporal AA (see PostChain / taa.frag). Wins over FXAA when on; not
    // used in split screen, where one history would serve two cameras.
    bool  taaEnabled = true;
    float taaSharpen = 0.35f;
    // Screen-space reflections (lit.frag ssrTrace), traced through the last
    // frame the post chain kept. Needs main's taaPrevVP, which is kept either way.
    bool ssrEnabled = true;
    // The environment probe: the cube captured around the camera that
    // reflective materials mirror. Off, nothing is captured and they take
    // their sheen from the light grid / the ambient like every other surface.
    bool envProbeEnabled = true;
    // Contact shadows (lit.frag contactShadow): short rays to the sun
    // through the same history, for what the cascades are too coarse for.
    bool contactShadows = true;

    // The horizon-based AO samples along screen-space directions it derives
    // itself, so there is no sample kernel to upload any more.
    float ssaoStrength = 0.7f;
    float ssaoRadius   = 1.5f;
    float ssaoBias     = 0.15f; // radians: horizons below this don't occlude
    float ssaoPower    = 1.6f;
    // Cube-face size of the reflection probe, mirrored here so it can be a
    // scene setting; the renderer owns the actual cubes (see
    // setEnvProbeResolution, which reallocates them).
    int envProbeRes = fitzel::Renderer::kDefaultEnvProbeRes;
    // Cap on the probe's cube faces per frame. The default buys back most of
    // the reflection lag at speed; 1 is the old amortized behaviour.
    int envProbeFaces = 3;

    // Depth of field (distance blur). dofMax = 0 disables it.
    float dofMax  = 5.0f;      // max blur radius (pixels)
    float dofNear = 25.0f;     // sharp up to here (metres)
    float dofFar  = 140.0f;    // fully blurred beyond here

    // Camera motion blur: streaks the scene along per-pixel screen velocity
    // (this frame's camera transform vs last frame's, by depth reprojection).
    // Purely camera motion -- fast turns/flight smear, a static view stays
    // sharp. 0 disables it (like dofMax).
    float motionBlurStrength = 0.6f; // 0 off .. ~2 heavy (exposure fraction)

    // Tonemapping exposure + HSV colour grade.
    float exposure   = 1.0f;
    float hueShift   = 0.0f;
    float saturation = 1.35f; // richer, less milky greens
    float valueGain  = 1.0f;
    float warmth     = 0.18f; // golden-hour white balance
    float contrast   = 0.16f; // lift the flat look
    // Split toning and vibrance (composite.frag): cool shadows, warm
    // highlights, and more colour where there is little -- the graded look
    // of a landscape photograph. 0 = off, the default.
    float gradeSplit    = 0.0f;
    float gradeVibrance = 0.0f;
    // Tonemap curve (0 ACES fit, 1 AgX, 2 PBR Neutral -- see composite.frag)
    // and auto exposure relative to `exposure` (see PostChain::Params).
    int   tonemapCurve = 1;
    float vignette     = 0.2f;   // lens fall-off to the corners (composite.frag)
    float filmGrain    = 0.0f;
    bool  autoExposure = true;
    float comicStrength = 1.0f;   // 0 .. 1: mix of the comic picture
    float comicLevels   = 4.0f;   // cel bands
    float comicLine     = 2.0f;   // ink width, pixels at 1080 lines
    float comicDetail   = 0.35f;  // lines from colour edges as well as shape
    float comicHalftone = 0.5f;   // dot screen in the shadows
    float autoMinEv    = -1.5f;
    float autoMaxEv    = 2.5f;
    float adaptSpeed   = 1.5f;

    // The look the chain is handed: these values with every section that is
    // switched off put back to neutral -- all of them while `effects` is false
    // (the viewport's Post FX switch), which leaves the camera alone: exposure
    // and the tonemap curve, without which there would be no picture. The
    // switches in the copy say what is actually on.
    PostLook resolved(bool effects) const {
        PostLook r = *this;
        r.gradeOn      = effects && gradeOn;
        r.bloomOn      = effects && bloomOn;
        r.aoOn         = effects && aoOn;
        r.dofOn        = effects && dofOn;
        r.lensOn       = effects && lensOn;
        r.motionBlurOn = effects && motionBlurOn;
        r.comicOn      = effects && comicOn;
        if (!r.gradeOn) {
            r.hueShift = 0.0f;  r.saturation = 1.0f; r.valueGain = 1.0f;
            r.warmth   = 0.0f;  r.contrast   = 0.0f;
            r.gradeSplit = 0.0f; r.gradeVibrance = 0.0f;
        }
        if (!r.bloomOn)      { r.bloomIntensity = 0.0f; r.rayIntensity = 0.0f; }
        if (!r.aoOn)         r.ssaoStrength = 0.0f;
        if (!r.dofOn)        r.dofMax = 0.0f;
        if (!r.lensOn)       { r.vignette = 0.0f; r.filmGrain = 0.0f; }
        if (!r.motionBlurOn) r.motionBlurStrength = 0.0f;
        if (!effects)        r.autoExposure = false;
        return r;
    }
};
