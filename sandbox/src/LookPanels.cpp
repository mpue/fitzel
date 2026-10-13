#include "LookPanels.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

#include <imgui.h>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/graphics/EnvironmentIBL.hpp>
#include <fitzel/render/Renderer.hpp>

#include "SkyLayers.hpp"
#include "UiStyle.hpp"

namespace lookui {

void drawSkyPanel(const SkyPanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Sky & atmosphere", &s.show)) {
        ImGui::SliderFloat("Time of day", &s.timeOfDay, 0.0f, 24.0f, "%.1f h");
        ImGui::SameLine();
        ImGui::Checkbox("Pause", &s.timePaused);
        ImGui::SliderFloat("Day length",  &s.dayLength, 0.0f, 600.0f, "%.0f s");
        ImGui::SliderFloat("Coverage",    &s.sky.coverage, 0.0f, 1.0f);
        ImGui::SliderFloat("Density",     &s.sky.density, 0.0f, 3.0f);
        ImGui::SliderFloat("Cloud scale", &s.sky.scale, 0.0003f, 0.005f, "%.4f");
        ImGui::SliderFloat("Wind",        &s.sky.wind, 0.0f, 20.0f);
        ImGui::SliderFloat("Cloud base",  &s.sky.base, 100.0f, 3000.0f, "%.0f m");
        ImGui::SliderFloat("Cloud top",   &s.sky.top, 300.0f, 7000.0f, "%.0f m");
        ui::hint("Base, top and scale decide whether the sky reads as\n"
                 "weather or as a ceiling. A cumulus is at least as\n"
                 "TALL as it is wide, so a thin slab under wide\n"
                 "features can only ever be a textured lid -- scale\n"
                 "sets that width, and LOWER means bigger clouds.\n"
                 "Coverage does two jobs: how much sky is taken, and\n"
                 "how far the tops build into it.");

        // --- The other layers ---------------------------------------
        // One section per cloud type, each with its own height, wind
        // and direction, drawn in SkyLayers.cpp. The sliders above are
        // the cumulus, which is the one layer that is raymarched and so
        // the one that needs a base, a top and a density rather than a
        // height; everything below is a sheet.
        ui::sectionText("Layers");
        ui::hint("Each type is its own deck at its own height, and they\n"
                 "stack in that order -- a stratus under the cumulus\n"
                 "hides it, one above it does not. The cumulus above is\n"
                 "the only layer with real depth; the rest are sheets,\n"
                 "which is what they are in the air as well.");
        skylayers::drawPanel(s.sky);
        // Logarithmic: a clear day lives at a few ten-thousandths and a storm
        // at a few thousandths, and on a linear 0..0.02 track the whole clear
        // range was the first twentieth of the slider.
        ImGui::SliderFloat("Fog density", &s.sky.fogDensity, 0.0f, 0.02f, "%.5f",
                           ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Fog falloff", &s.sky.fogFalloff, 0.005f, 0.1f, "%.3f");
        ui::hint("Everything in this panel down to the volumetric fog is\n"
                 "part of a weather preset. Weather & audio is where a\n"
                 "sky gets a name and is kept.");

        // --- Volumetric fog: the world-wide volume ----------------
        // Folded away by default, and deliberately sitting right under
        // the two sliders it is not: those are the height haze, which is
        // everywhere and has no shape.
        //
        // This one box is the WORLD's air. Mist that belongs somewhere in
        // particular is not authored here at all -- it is a Volumetric Fog
        // component on an entity, so it can be placed, scaled and rotated
        // like anything else in the scene, and there can be many. Both end
        // up in the same march; the hint says so, because a panel that
        // does not mention the other way is a panel that hides it.
        if (ui::header("Volumetric fog (world)")) {
            ImGui::Checkbox("Enabled##volfog", &s.volFog.enabled);
            ImGui::SameLine();
            ImGui::Checkbox("Show volume", &s.volFog.showVolume);
            ui::hint("The haze above does distance. This does shape:\n"
                     "banks that drift, holes that pass, sun shafts.\n"
                     "For mist in ONE place, add a Volumetric Fog\n"
                     "component to an Empty and scale it instead.");

            ui::sectionText("Volume");
            ImGui::DragFloat3("Centre", &s.volFog.center.x, 0.5f,
                              -20000.0f, 20000.0f, "%.0f m");
            ImGui::DragFloat3("Size", &s.volFog.size.x, 0.5f,
                              1.0f, 20000.0f, "%.0f m");
            // Placing a volume you cannot grab is the awkward part, so
            // the two placements anyone actually wants are buttons: put
            // it where I am standing, and sit it on the ground under it.
            if (ImGui::Button("Centre on camera"))
                s.volFog.center = s.cameraPos;
            ImGui::SameLine();
            if (ImGui::Button("Sit on ground"))
                s.volFog.center.y =
                    s.groundAt(s.volFog.center.x, s.volFog.center.z) +
                    s.volFog.size.y * 0.5f;
            ImGui::Checkbox("Follow camera (X/Z)", &s.volFog.followCamera);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Ground mist over a whole track without a\n"
                                  "box big enough to cover it: the same steps\n"
                                  "spread over kilometres lose all structure.");
            ImGui::SliderFloat("Edge fade", &s.volFog.medium.edge, 0.02f, 1.0f);
            ImGui::SliderFloat("Height falloff##volfog",
                               &s.volFog.medium.heightFalloff, 0.0f, 3.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How much harder it is for fog to exist near\n"
                                  "the top of the box. Carves the lid out of the\n"
                                  "noise, so the layer has a ragged top rather\n"
                                  "than a smooth fade.");

            ui::sectionText("Medium");
            ImGui::SliderFloat("Thickness", &s.volFog.medium.density, 0.0f, 0.5f,
                               "%.3f /m");
            ImGui::ColorEdit3("Tint##volfog", &s.volFog.medium.color.x);
            ImGui::SliderFloat("Coverage##volfog", &s.volFog.medium.coverage,
                               0.0f, 0.95f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How much of the volume has fog in it at all.\n"
                                  "Low = a solid body, high = separate banks\n"
                                  "with clear air between them.");

            ui::sectionText("Noise");
            ImGui::SliderFloat("Scale##volfog", &s.volFog.medium.noiseScale,
                               0.001f, 0.06f, "%.4f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Smaller = bigger banks.");
            ImGui::SliderFloat("Vertical detail",
                               &s.volFog.medium.verticalDetail, 0.25f, 8.0f,
                               "%.2fx");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How much finer the field is going up than\n"
                                  "sideways. At 1 a shallow layer sits inside a\n"
                                  "single feature and the fog looks like a flat\n"
                                  "pattern pulled upward.");
            ImGui::SliderFloat("Detail", &s.volFog.medium.detail, 0.0f, 0.95f);
            ImGui::SliderFloat("Swirl", &s.volFog.medium.warp, 0.0f, 1.5f);
            ImGui::DragFloat3("Wind##volfog", &s.volFog.medium.wind.x, 0.05f,
                              -30.0f, 30.0f, "%.2f m/s");

            ui::sectionText("Light");
            ImGui::SliderFloat("Forward scatter", &s.volFog.medium.anisotropy,
                               -0.9f, 0.9f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How much light keeps going the way it came.\n"
                                  "High values put the glow around the sun.");
            ImGui::SliderFloat("Sun##volfog", &s.volFog.medium.sunIntensity, 0.0f, 4.0f);
            ImGui::SliderFloat("Ambient##volfog", &s.volFog.medium.ambientIntensity,
                               0.0f, 4.0f);
            ImGui::SliderFloat("Lamps##volfog", &s.volFog.medium.lampIntensity, 0.0f, 4.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The point lights glow in the mist: halos round\n"
                                  "lamps, the cone of a muzzle flash. 0 = off.");
            ImGui::Checkbox("Sun shafts", &s.volFog.medium.shafts);
            ImGui::SameLine();
            ImGui::Checkbox("Self-shadow", &s.volFog.medium.selfShadow);

            ui::sectionText("Cost");
            ImGui::SliderInt("Steps", &s.volFog.medium.steps, 8, 128);
            ImGui::SliderInt("Resolution", &s.volFog.resScale, 1, 4,
                             "1/%d of the pane");
            ui::hint("Steps buy structure along the ray, resolution buys it\n"
                     "across the screen. Fog is soft, so 1/2 is free money.\n"
                     "Resolution is the whole PASS -- every placed volume\n"
                     "is marched into the same buffer.");
        }
    }
    ImGui::End();
}

namespace {

// A section bar of the Post processing panel, with the section's switch at its
// right end -- so a folded panel still reads as a list of what is on. `on` is
// null for the sections that cannot be off (the camera, anti-aliasing).
bool postSection(const char* label, bool* on, bool openByDefault = false) {
    const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_AllowOverlap;
    if (openByDefault) flags |= ImGuiTreeNodeFlags_DefaultOpen;
    const bool open = ui::header(label, flags);
    if (on) {
        ImGui::SameLine(right - ImGui::GetFrameHeight());
        ImGui::PushID(label);
        ImGui::Checkbox("##on", on);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Switch the section on or off.\n"
                              "Its settings are kept either way.");
        ImGui::PopID();
    }
    return open;
}

// The section's way back to where a new scene starts.
bool resetButton(const char* id) {
    ImGui::Spacing();
    ImGui::PushID(id);
    const bool hit = ImGui::SmallButton("Reset to defaults");
    ImGui::PopID();
    ImGui::Spacing();
    return hit;
}

} // namespace

void drawPostPanel(const PostPanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Post processing", &s.show)) {
        PostLook& look = s.look;
        const PostLook def;

        // The viewport's switch, mirrored from the toolbar: a panel whose
        // sliders do nothing has to say why, and the easiest answer to that is
        // the switch itself.
        ImGui::BeginDisabled(s.playMode);
        ImGui::Checkbox("Show in the viewport", &s.postFx);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("The Post FX button in the toolbar. It switches on\n"
                              "with Textured and off with the other view modes;\n"
                              "Play always shows the whole look.");
        ui::hint("The scene's look: saved with it, and what Play shows.");
        ImGui::Spacing();

        // --- The camera: always on, there is no picture without it -------
        if (postSection("Exposure & tonemap", nullptr, true)) {
            ImGui::SliderFloat("Exposure", &look.exposure, 0.2f, 3.0f);
            const char* curves[] = {"ACES (classic)", "AgX", "Neutral"};
            ImGui::Combo("Tonemap", &look.tonemapCurve, curves, IM_ARRAYSIZE(curves));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("ACES: punchy, but bright colours slide in hue\n"
                                  "(blue sky to cyan) and clip early.\n"
                                  "AgX: highlights fade to white along their own\n"
                                  "hue, three more stops before a cloud clips.\n"
                                  "Neutral: base colours exactly as authored.");
            ImGui::Checkbox("Auto exposure", &look.autoExposure);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Corrects the exposure above for how bright the\n"
                                  "frame is -- up in a tunnel or at dusk, down when\n"
                                  "it is all sky -- within the range below. A\n"
                                  "sunlit daytime frame stays as you set it.");
            if (look.autoExposure) {
                ImGui::SameLine();
                ImGui::TextDisabled("%+.1f EV", std::log2(s.autoExposureScale));
                if (ImGui::TreeNode("Advanced##exposure")) {
                    ImGui::SliderFloat("Darken at most", &look.autoMinEv, -4.0f, 0.0f, "%.1f EV");
                    ImGui::SliderFloat("Brighten at most", &look.autoMaxEv, 0.0f, 6.0f, "%.1f EV");
                    ImGui::SliderFloat("Adaptation", &look.adaptSpeed, 0.2f, 8.0f, "%.1f /s");
                    ImGui::TreePop();
                }
            }
            if (resetButton("exposure")) {
                look.exposure     = def.exposure;     look.tonemapCurve = def.tonemapCurve;
                look.autoExposure = def.autoExposure; look.autoMinEv    = def.autoMinEv;
                look.autoMaxEv    = def.autoMaxEv;    look.adaptSpeed   = def.adaptSpeed;
            }
        }

        if (postSection("Colour", &look.gradeOn)) {
            ImGui::BeginDisabled(!look.gradeOn);
            ImGui::SliderFloat("Hue",        &look.hueShift, -180.0f, 180.0f, "%.0f");
            ImGui::SliderFloat("Saturation", &look.saturation, 0.0f, 2.0f);
            ImGui::SliderFloat("Brightness", &look.valueGain, 0.3f, 2.0f);
            ImGui::SliderFloat("Warmth",     &look.warmth, -0.5f, 0.5f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("White balance: + golden, - blue.");
            ImGui::SliderFloat("Contrast",   &look.contrast, 0.0f, 0.6f);
            ImGui::SliderFloat("Vibrance",   &look.gradeVibrance, -0.5f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("More colour where there is little;\n"
                                  "already vivid colours stay as they are.");
            ImGui::SliderFloat("Split tone", &look.gradeSplit, 0.0f, 1.5f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Cool shadows, warm highlights.");
            ImGui::EndDisabled();
            if (resetButton("grade")) {
                look.hueShift   = def.hueShift;   look.saturation    = def.saturation;
                look.valueGain  = def.valueGain;  look.warmth        = def.warmth;
                look.contrast   = def.contrast;   look.gradeVibrance = def.gradeVibrance;
                look.gradeSplit = def.gradeSplit;
            }
        }

        if (postSection("Bloom & sun", &look.bloomOn)) {
            ImGui::BeginDisabled(!look.bloomOn);
            ImGui::SliderFloat("Bloom",    &look.bloomIntensity, 0.0f, 1.5f);
            ImGui::SliderFloat("Sun rays", &look.rayIntensity, 0.0f, 1.5f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Shafts from the sun through what stands in front\n"
                                  "of it. The lens flare goes with this section too:\n"
                                  "it fades with the rays, and only goes out\n"
                                  "entirely with the section.");
            if (ImGui::TreeNode("Advanced##bloom")) {
                ImGui::SliderFloat("Threshold", &look.bloomThreshold, 0.2f, 4.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Luminance where the glow starts. Lower it to make\n"
                                      "emissive materials bloom sooner; the knee below\n"
                                      "keeps the onset soft instead of popping.");
                ImGui::SliderFloat("Knee", &look.bloomKnee, 0.0f, 1.5f, "%.2f");
                ImGui::TreePop();
            }
            ImGui::EndDisabled();
            if (resetButton("bloom")) {
                look.bloomIntensity = def.bloomIntensity; look.rayIntensity = def.rayIntensity;
                look.bloomThreshold = def.bloomThreshold; look.bloomKnee    = def.bloomKnee;
            }
        }

        if (postSection("Ambient occlusion", &look.aoOn)) {
            ImGui::BeginDisabled(!look.aoOn);
            ImGui::SliderFloat("Strength", &look.ssaoStrength, 0.0f, 1.0f);
            ImGui::SliderFloat("Radius",   &look.ssaoRadius, 0.2f, 4.0f, "%.1f m");
            if (ImGui::TreeNode("Advanced##ao")) {
                ImGui::SliderFloat("Angle bias", &look.ssaoBias, 0.0f, 0.6f, "%.2f rad");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Horizons below this elevation don't occlude.\n"
                                      "Raise it if flat surfaces look dirty, lower it\n"
                                      "for more contact shading in creases.");
                ImGui::SliderFloat("Power", &look.ssaoPower, 0.5f, 3.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Higher keeps the darkening in the deep creases\n"
                                      "and lets the shallow ones go.");
                ImGui::TreePop();
            }
            ImGui::EndDisabled();
            if (resetButton("ao")) {
                look.ssaoStrength = def.ssaoStrength; look.ssaoRadius = def.ssaoRadius;
                look.ssaoBias     = def.ssaoBias;     look.ssaoPower  = def.ssaoPower;
            }
        }

        if (postSection("Depth of field", &look.dofOn)) {
            ImGui::BeginDisabled(!look.dofOn);
            ImGui::SliderFloat("Blur",       &look.dofMax, 0.0f, 12.0f, "%.1f px");
            ImGui::SliderFloat("Focus near", &look.dofNear, 2.0f, 120.0f, "%.0f m");
            ImGui::SliderFloat("Focus far",  &look.dofFar, 20.0f, 400.0f, "%.0f m");
            ui::hint("Sharp up to Focus near, fully blurred from Focus far.\n"
                     "A script can move the focus while the game runs.");
            ImGui::EndDisabled();
            if (resetButton("dof")) {
                look.dofMax = def.dofMax; look.dofNear = def.dofNear; look.dofFar = def.dofFar;
            }
        }

        if (postSection("Lens", &look.lensOn)) {
            ImGui::BeginDisabled(!look.lensOn);
            ImGui::SliderFloat("Vignette",   &look.vignette, 0.0f, 1.0f);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Light falling off towards the corners, as through\n"
                                  "a real lens. Frames the picture.");
            ImGui::SliderFloat("Film grain", &look.filmGrain, 0.0f, 0.1f, "%.3f");
            ImGui::EndDisabled();
            if (resetButton("lens")) {
                look.vignette = def.vignette; look.filmGrain = def.filmGrain;
            }
        }

        if (postSection("Motion blur", &look.motionBlurOn)) {
            ImGui::BeginDisabled(!look.motionBlurOn);
            ImGui::SliderFloat("Speed blur", &look.motionBlurStrength, 0.0f, 2.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Radial speed streak while driving/flying: the\n"
                                  "world smears outward past the craft, growing\n"
                                  "with speed. No effect on the free camera.");
            ImGui::EndDisabled();
            if (resetButton("motion"))
                look.motionBlurStrength = def.motionBlurStrength;
        }

        if (postSection("Comic", &look.comicOn)) {
            ImGui::BeginDisabled(!look.comicOn);
            ImGui::SliderFloat("Strength##comic", &look.comicStrength, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Bands",    &look.comicLevels, 2.0f, 8.0f, "%.0f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How many flat steps the shading is cut into.");
            ImGui::SliderFloat("Ink width", &look.comicLine, 0.5f, 4.0f, "%.1f px");
            ImGui::SliderFloat("Detail",   &look.comicDetail, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Ink along colour edges too, not only along shapes.\n"
                                  "High values hatch busy textures.");
            ImGui::SliderFloat("Dots",     &look.comicHalftone, 0.0f, 1.0f, "%.2f");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("A printed dot screen in the shadows.");
            ui::hint("A script can switch it with game.setComic(on).");
            ImGui::EndDisabled();
            if (resetButton("comic")) {
                look.comicStrength = def.comicStrength; look.comicLevels   = def.comicLevels;
                look.comicLine     = def.comicLine;     look.comicDetail   = def.comicDetail;
                look.comicHalftone = def.comicHalftone;
            }
        }

        if (postSection("Anti-aliasing", nullptr)) {
            ImGui::Checkbox("TAA", &look.taaEnabled);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Temporal anti-aliasing: every pixel gathered over\n"
                                  "several frames. Grass, fences and far edges stop\n"
                                  "crawling. Replaces FXAA while on; split screen\n"
                                  "and the plain view modes fall back to FXAA.");
            if (look.taaEnabled)
                ImGui::SliderFloat("Sharpen", &look.taaSharpen, 0.0f, 1.0f, "%.2f");
            ImGui::BeginDisabled(look.taaEnabled);
            ImGui::Checkbox("FXAA", &look.fxaaEnabled);
            ImGui::EndDisabled();
            ui::hint("The player's Graphics menu sets these as well.");
        }
    }
    ImGui::End();
}

void drawShadowsPanel(const ShadowsPanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Shadows & reflections", &s.show)) {
        ui::sectionText("Shadows");
        ImGui::SliderFloat("Cascade split", &s.renderer.shadows().splitLambda, 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How the sun's shadow map is shared out by distance.\n"
                              "Higher keeps more of it near the camera: crisper\n"
                              "close up, coarser in the distance.");
        ImGui::Checkbox("Contact shadows", &s.look.contactShadows);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Short rays towards the sun for the small shadows\n"
                              "the cascades are too coarse to cast -- a wheel\n"
                              "on the road, a stone on the ground. Near the\n"
                              "camera only.");
        // Reflection probe: the cubemap a wet road (and any reflective
        // material) mirrors. Applied on pick rather than per frame --
        // changing it reallocates both cubes.
        ui::sectionText("Reflections");
        ImGui::Checkbox("Screen-space reflections", &s.look.ssrEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Smooth surfaces -- wet roads, puddles, paint,\n"
                              "glass -- reflect what is actually beside them,\n"
                              "traced through the last frame. The probe fills\n"
                              "in whatever is off screen. Only with Reflections\n"
                              "above Off in the Graphics menu (F10).");
        ImGui::Checkbox("Environment probe", &s.look.envProbeEnabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("The cube captured around the camera that\n"
                              "reflective materials mirror. Off, they take\n"
                              "their sheen from the light grid / the ambient.");
        const int sizes[] = {128, 256, 512, 1024};
        char cur[16];
        std::snprintf(cur, sizeof(cur), "%d", s.look.envProbeRes);
        if (ImGui::BeginCombo("Probe resolution", cur)) {
            for (int res : sizes) {
                char lbl[16];
                std::snprintf(lbl, sizeof(lbl), "%d", res);
                if (ImGui::Selectable(lbl, res == s.look.envProbeRes)) {
                    s.look.envProbeRes = res;
                    s.renderer.setEnvProbeResolution(res);
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cube-face size of the environment probe: how\n"
                              "sharp reflections are on a wet road or a\n"
                              "reflective material. Six scene passes either\n"
                              "way -- raising it costs fill, not draw calls --\n"
                              "but 1024 is 64x the pixels of 128.");
        // How fresh that cube is kept. This is a LATENCY control,
        // not a quality one: the probe is filled one face at a
        // time, so a cube filled at one face per frame is six to
        // twelve frames old when it is sampled -- twenty metres of
        // it at racing speed, which reads as the reflection
        // dragging behind the car. The rate is only spent when the
        // viewpoint actually moves, so raising this costs nothing
        // in a parked editor.
        const char* faceLbl[] = {"1 face (cheapest)", "2 faces",
                                 "3 faces", "4 faces", "5 faces",
                                 "6 faces (no lag)"};
        const int fi = glm::clamp(s.look.envProbeFaces, 1, 6) - 1;
        if (ImGui::BeginCombo("Probe refresh", faceLbl[fi])) {
            for (int k = 0; k < 6; ++k)
                if (ImGui::Selectable(faceLbl[k], k == fi)) {
                    s.look.envProbeFaces = k + 1;
                    s.renderer.setEnvProbeMaxFaces(s.look.envProbeFaces);
                }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Cube faces the probe may refresh per frame,\n"
                              "at most. The actual rate follows how fast the\n"
                              "camera moves, so a still scene pays one face\n"
                              "whatever this says. Raise it if reflections\n"
                              "lag behind at speed; lower it if the probe\n"
                              "costs too much (it is six scene passes).");
    }
    ImGui::End();
}

namespace {

// A PBR material's map rather than a panorama (normal, roughness, albedo ...).
bool isMaterialMap(const std::string& n) {
    std::string s = n;
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const char* t : {"_nor", "_normal", "_rough", "_disp", "_diff", "_albedo", "_ao",
                          "_spec", "_metal", "_height", "_bump", "_opacity", "_mask", "_gloss",
                          "_translucent", "_color"})
        if (s.find(t) != std::string::npos) return true;
    return false;
}

} // namespace

void drawEnvironmentPanel(const EnvironmentPanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Environment", &s.show)) {
        ImGui::TextDisabled("Equirectangular .hdr / .exr panorama.");
        // HDRI panoramas from the asset library: .hdr/.exr textures, excluding
        // PBR material maps (normal/rough/etc).
        std::vector<std::pair<std::string, std::string>> hdris; // (label, path)
        for (const fitzel::AssetId id : s.assetDb.allAssets()) {
            const fitzel::AssetDatabase::Entry* e = s.assetDb.entry(id);
            if (!e || e->type != fitzel::AssetType::Texture) continue;
            std::string ext = e->absPath.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if ((ext != ".exr" && ext != ".hdr") || isMaterialMap(e->relPath)) continue;
            hdris.push_back({e->relPath, e->absPath.string()});
        }
        std::sort(hdris.begin(), hdris.end());

        ImGui::SetNextItemWidth(260.0f);
        const char* curLabel = s.hdriLoaded.empty() ? "(select HDRI)" : s.hdriLoaded.c_str();
        if (ImGui::BeginCombo("HDRI", curLabel)) {
            if (hdris.empty()) ImGui::TextDisabled("(no .hdr/.exr panoramas found)");
            for (const auto& [label, path] : hdris)
                if (ImGui::Selectable(label.c_str(), label == s.hdriLoaded)) {
                    if (s.environment.load(path)) {
                        s.hdriLoaded  = label;
                        s.hdriAbsPath = path;
                        s.iblEnabled  = true;
                    }
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::TextDisabled(s.environment.valid() ? "loaded" : "not loaded");

        ImGui::BeginDisabled(!s.environment.valid());
        ImGui::Checkbox("Enable IBL lighting", &s.iblEnabled);
        ImGui::Checkbox("Show HDRI as background", &s.iblSkybox);
        ImGui::SliderFloat("Intensity", &s.iblIntensity, 0.0f, 4.0f);
        if (s.environment.valid())
            ImGui::TextDisabled("auto-normalised x%.3g (panoramas differ\n"
                                "in absolute brightness by decades)",
                                s.environment.exposureScale());
        ImGui::EndDisabled();
        ImGui::TextDisabled("Lights surfaces from the panorama\n"
                            "(diffuse irradiance + specular).");
    }
    ImGui::End();
}

void drawWaterPanel(const WaterPanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Water", &s.show)) {
        ImGui::SliderFloat("Level",        &s.level, -15.0f, 15.0f);
        ImGui::SliderFloat("Swell height", &s.waveHeight, 0.0f, 2.5f);
        ImGui::SliderFloat("Choppiness",   &s.waveChoppy, 0.0f, 1.0f);
        ImGui::SliderFloat("Ripples",      &s.rippleStrength, 0.0f, 0.05f, "%.3f");
        ImGui::SliderFloat("Ripple size",  &s.rippleScale, 0.01f, 0.2f, "%.3f");
        ImGui::SliderFloat("Shore foam",   &s.foamWidth, 0.0f, 8.0f);
        ImGui::SliderFloat("Reflectivity", &s.reflectivity, 0.0f, 1.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Max mirror strength. Lower = less glassy,\n"
                              "more of the water body shows through.");
        ImGui::SliderFloat("Clarity",      &s.clarity, 0.2f, 3.0f, "%.2f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("How clear the water is. Higher = see the bed\n"
                              "deeper; lower = murkier, tints sooner.");
        ImGui::SliderFloat("IOR",          &s.ior, 1.0f, 2.0f, "%.3f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Index of refraction. Water = 1.33 (~2%% edge-on\n"
                              "reflection); higher = more reflective + more bend.");
        ImGui::ColorEdit3("Tint",          &s.tint.x);
    }
    ImGui::End();
}

} // namespace lookui
