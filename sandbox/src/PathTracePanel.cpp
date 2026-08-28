#include "PathTracePanel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>

#include <imgui.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <tinyexr.h>

#include <fitzel/render/Renderer.hpp>
#include <fitzel/scene/Camera.hpp>

#include "UiStyle.hpp"

namespace pathpanel {
namespace {

// Output sizes. Fixed rather than free-form because the point of this panel is
// a picture for somebody else to look at, and the sizes that has ever meant are
// these -- plus the one case that is genuinely arbitrary, which is why "Custom"
// is on the end rather than being the only option.
struct Preset {
    const char* label;
    int         width, height;
};
constexpr Preset kPresets[] = {
    {"1280 x 720 (draft)",     1280,  720},
    {"1920 x 1080 (Full HD)",  1920, 1080},
    {"2560 x 1440",            2560, 1440},
    {"3840 x 2160 (4K)",       3840, 2160},
    {"2048 x 2048 (square)",   2048, 2048},
    {"Custom",                    0,    0},
};
constexpr int kPresetCount = static_cast<int>(sizeof(kPresets) / sizeof(kPresets[0]));

// A free filename in `dir`, as <stem>-0001.<ext>. Numbered rather than
// timestamped: a sequence of renders of the same shot is the normal case, and
// 0001..0004 sorts and reads as one set where four timestamps do not.
std::filesystem::path nextFreeName(const std::filesystem::path& dir,
                                   const std::string& stem, const char* ext) {
    for (int i = 1; i < 10000; ++i) {
        char name[256];
        std::snprintf(name, sizeof(name), "%s-%04d.%s", stem.c_str(), i, ext);
        std::filesystem::path p = dir / name;
        if (!std::filesystem::exists(p)) return p;
    }
    return dir / (stem + "-overflow." + ext);
}

// Refresh the preview texture from the job, at most a few times a second.
// Reallocating a GL texture every frame for a 4K render would cost more than
// the render.
void refreshPreview(State& st, double now) {
    if (!st.job.hasImage()) return;
    const bool running = st.job.running();
    const int  done    = st.job.samplesDone();
    if (done == st.previewSamples && !running) return;
    if (running && now - st.previewStamp < 0.35) return;

    std::vector<unsigned char> px;
    if (!st.job.snapshotLdr(px)) return;
    const int w = st.job.settings().width, h = st.job.settings().height;

    if (!st.preview.isValid() || st.previewW != w || st.previewH != h) {
        st.preview  = fitzel::Texture::blank(w, h);
        st.previewW = w;
        st.previewH = h;
    }
    st.preview.update(px.data(), w, h, 4);
    st.previewSamples = done;
    st.previewStamp   = now;
}

void drawNotes(const pathcapture::Report& rep) {
    if (rep.notes.empty()) return;
    // Shown, not hidden behind a log. Every entry here is a way the render
    // differs from the viewport, and an author who does not know about them
    // spends the difference looking for a bug in their scene.
    if (ui::header("What is not in the render")) {
        for (const std::string& n : rep.notes) {
            ImGui::Bullet();
            ImGui::TextWrapped("%s", n.c_str());
        }
    }
}

} // namespace

State::State() {
    settings.width      = kPresets[1].width;
    settings.height     = kPresets[1].height;
    settings.samples    = 256;
    settings.maxBounces = 6;
    settings.batch      = 4;
}

void draw(State& st, const std::filesystem::path& projectDir, double now) {
    if (!st.open) return;

    ImGui::SetNextWindowSize(ImVec2(420, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Render", &st.open)) {
        ImGui::End();
        return;
    }

    const bool running = st.job.running();

    ui::title("Offline render");
    ui::hint("Traces the scene the way it is framed in the viewport right now:\n"
             "real bounced light, real soft shadows, real reflections.\n"
             "Minutes, not milliseconds -- this is for a picture to show\n"
             "somebody, not for the game.");

    ImGui::Separator();

    // --- Size -------------------------------------------------------------
    ui::sectionText("Size");
    ImGui::BeginDisabled(running);
    if (ImGui::BeginCombo("Resolution", kPresets[st.resolutionPreset].label)) {
        for (int i = 0; i < kPresetCount; ++i)
            if (ImGui::Selectable(kPresets[i].label, i == st.resolutionPreset)) {
                st.resolutionPreset = i;
                if (kPresets[i].width > 0) {
                    st.settings.width  = kPresets[i].width;
                    st.settings.height = kPresets[i].height;
                }
            }
        ImGui::EndCombo();
    }
    if (kPresets[st.resolutionPreset].width == 0) {
        ImGui::DragInt("Width",  &st.settings.width,  4.0f, 64, 8192);
        ImGui::DragInt("Height", &st.settings.height, 4.0f, 64, 8192);
    }

    // --- Quality ----------------------------------------------------------
    ImGui::Spacing();
    ui::sectionText("Quality");
    ImGui::SliderInt("Samples", &st.settings.samples, 8, 4096, "%d per pixel",
                     ImGuiSliderFlags_Logarithmic);
    ui::hint("Noise falls as the square root of this: four times the samples is\n"
             "half the grain. It is a ceiling, not a commitment -- the image\n"
             "refines as it goes and Stop keeps whatever has arrived.");
    ImGui::SliderInt("Bounces", &st.settings.maxBounces, 1, 12);
    ui::hint("1 is direct light only -- shadows go black, as they do in the\n"
             "viewport. 3 or 4 is where a room stops looking lit from outside.\n"
             "Past 6 the difference is usually not worth the time.");

    // --- Light ------------------------------------------------------------
    ImGui::Spacing();
    ui::sectionText("Light");
    ImGui::SliderFloat("Sun size", &st.capture.sunAngleDeg, 0.0f, 10.0f, "%.2f deg");
    ui::hint("The sun's disc. 0 gives the viewport's hard-edged shadow; 0.5 is\n"
             "roughly the real sun; a few degrees reads as haze. The single\n"
             "most effective setting here for not looking like a screenshot.");
    ImGui::SliderFloat("Lamp size", &st.capture.lampRadius, 0.0f, 1.0f, "%.2f m");
    ui::hint("How big the scene's point and spot lamps are. Bigger bulbs mean\n"
             "softer shadow edges and wider highlights.");

    // --- Lens -------------------------------------------------------------
    ImGui::Spacing();
    ui::sectionText("Lens");
    ImGui::SliderFloat("Aperture", &st.aperture, 0.0f, 0.30f, "%.3f m");
    ui::hint("0 keeps everything sharp. Opening it throws the background out of\n"
             "focus, which is most of what separates a product shot from a\n"
             "screenshot -- 0.02 to 0.05 is plenty at car scale.");
    ImGui::BeginDisabled(st.aperture <= 0.0f);
    ImGui::Checkbox("Focus on what is in the middle", &st.autoFocus);
    ImGui::BeginDisabled(st.autoFocus);
    ImGui::SliderFloat("Focus distance", &st.focusDistance, 0.5f, 200.0f, "%.1f m",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // --- Diagnose ---------------------------------------------------------
    // Only worth its space when something is wrong, so it stays folded. But it
    // is the fastest route from "this render is the wrong colour" to which
    // stage made it wrong, and that is a question the finished picture cannot
    // answer -- texture, material, light, tonemap and grade all produce a
    // plausible image on their own.
    ImGui::Spacing();
    if (ui::header("Diagnose")) {
        static const char* kShowNames[] = {
            "Full render", "Base colour only", "Normals", "Depth",
        };
        int show = static_cast<int>(st.settings.show);
        if (ImGui::Combo("Show", &show, kShowNames, 4))
            st.settings.show = static_cast<pathtrace::Show>(show);
        ui::hint("Base colour is the one to reach for first: it draws each\n"
                 "surface's own colour with no light, no tonemap and no grade.\n"
                 "If the car is the right colour there and wrong in the full\n"
                 "render, the texture is fine and the lighting is not.\n"
                 "Normals catch a bad transform; depth catches geometry that\n"
                 "is not where it appears to be.");
        if (st.settings.show != pathtrace::Show::Full)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "Diagnostic view -- not a picture to keep.");
    }

    // --- Scope ------------------------------------------------------------
    ImGui::Spacing();
    ui::sectionText("Scope");
    ImGui::SliderFloat("Distance limit", &st.capture.maxDistance, 0.0f, 2000.0f,
                       st.capture.maxDistance <= 0.0f ? "everything" : "%.0f m",
                       ImGuiSliderFlags_Logarithmic);
    ui::hint("Only geometry within this far of the camera is traced. This is the\n"
             "difference between a ten-second render and a ten-minute one on a\n"
             "landscape. Too low and a reflection shows a hole where the world\n"
             "was cut away -- raise it if a mirror looks wrong.");
    ImGui::Checkbox("Include glass and transparency", &st.capture.includeTransparent);
    ImGui::EndDisabled();

    // --- Go ---------------------------------------------------------------
    ImGui::Spacing();
    ImGui::Separator();

    if (!running) {
        if (ImGui::Button("Render", ImVec2(-1.0f, 0.0f))) {
            st.captureRequested = true;
            st.status = "harvesting the scene...";
        }
    } else {
        if (ImGui::Button("Stop", ImVec2(-1.0f, 0.0f))) {
            st.job.cancel();
            st.status = "stopped -- the image so far is kept";
        }
    }

    if (st.job.hasImage()) {
        const float p = st.job.progress();
        char bar[96];
        const double elapsed = st.job.elapsedSeconds();
        if (running && p > 0.02f) {
            const double eta = elapsed / p - elapsed;
            std::snprintf(bar, sizeof(bar), "%d / %d samples  -  %.0fs left",
                          st.job.samplesDone(), st.job.samplesTotal(), eta);
        } else {
            std::snprintf(bar, sizeof(bar), "%d / %d samples  -  %.1fs",
                          st.job.samplesDone(), st.job.samplesTotal(), elapsed);
        }
        ImGui::ProgressBar(p, ImVec2(-1.0f, 0.0f), bar);
    }
    if (!st.status.empty()) ImGui::TextWrapped("%s", st.status.c_str());
    if (!st.reportLine.empty()) ImGui::TextDisabled("%s", st.reportLine.c_str());

    // --- Saving -----------------------------------------------------------
    ImGui::Spacing();
    // Where a render lands, spelled out. A save button whose destination is
    // implied is a file you go looking for afterwards -- and it landed in the
    // wrong place once already, inside a path that named the .fitzel FILE
    // rather than the folder holding it.
    const bool haveProject = !projectDir.empty();
    const std::filesystem::path dir = haveProject ? projectDir / "renders"
                                                  : std::filesystem::path();
    if (haveProject) ImGui::TextDisabled("into %s", dir.string().c_str());
    else             ImGui::TextDisabled("save a project first -- a render is "
                                         "kept beside the scene it is of");

    ImGui::BeginDisabled(!st.job.hasImage() || !haveProject);
    if (ImGui::Button("Save image")) {
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::vector<unsigned char> px;
        if (st.job.snapshotLdr(px)) {
            const int w = st.job.settings().width, h = st.job.settings().height;
            const std::filesystem::path png = nextFreeName(dir, "render", "png");
            if (stbi_write_png(png.string().c_str(), w, h, 4, px.data(), w * 4)) {
                st.lastSaved = png.string();
                if (st.saveExrToo) {
                    // The linear image as well: the PNG has already been through
                    // the tonemap and cannot be graded back out of it, and a
                    // press shot is exactly the picture somebody will want to
                    // regrade later.
                    std::vector<float> hdr;
                    if (st.job.snapshotHdr(hdr)) {
                        std::filesystem::path exr = png;
                        exr.replace_extension("exr");
                        const char* err = nullptr;
                        if (SaveEXR(hdr.data(), w, h, 3, 1, exr.string().c_str(),
                                    &err) == TINYEXR_SUCCESS)
                            st.lastSaved += "  (+ .exr)";
                        else if (err)
                            FreeEXRErrorMessage(err);
                    }
                }
            } else {
                st.lastSaved = "could not write into " + dir.string();
            }
        }
    }
    ImGui::SameLine();
    ImGui::Checkbox("with linear .exr", &st.saveExrToo);
    ImGui::EndDisabled();
    if (!st.lastSaved.empty()) ImGui::TextDisabled("%s", st.lastSaved.c_str());

    drawNotes(st.report);

    // --- The picture ------------------------------------------------------
    refreshPreview(st, now);
    if (st.preview.isValid()) {
        ImGui::Spacing();
        ImGui::Separator();
        const float avail = ImGui::GetContentRegionAvail().x;
        const float scale = avail / static_cast<float>(st.previewW);
        ImGui::Image(static_cast<ImTextureID>(
                         static_cast<std::uintptr_t>(st.preview.id())),
                     ImVec2(avail, static_cast<float>(st.previewH) * scale));
    }

    ImGui::End();
}

void service(State& st, const fitzel::Renderer& renderer,
             const fitzel::Camera& camera, const SceneLook& look) {
    if (!st.captureRequested) return;
    st.captureRequested = false;

    // A render in flight is abandoned rather than queued: pressing Render again
    // means "that framing, not the old one".
    st.job.cancel();

    pathcapture::Options opt = st.capture;
    opt.hdriPath      = look.hdriPath;
    opt.hdriIntensity = look.hdriIntensity;
    opt.grade         = look.grade;

    std::shared_ptr<pathtrace::Scene> scene =
        pathcapture::capture(renderer, camera, opt, &st.report);
    st.reportLine = st.report.summary();

    if (scene->triangles.empty()) {
        st.status = "nothing to render: the frame has no geometry the tracer "
                    "can read (grass, water and particles do not count).";
        return;
    }

    scene->camera.apertureRadius = std::max(0.0f, st.aperture);
    if (st.aperture > 0.0f) {
        if (st.autoFocus) {
            const float d = pathtrace::firstHitDistance(*scene, scene->camera.position,
                                                        scene->camera.forward);
            // Nothing under the crosshair (the camera is pointed at the sky):
            // keep the last distance rather than focusing at zero, which would
            // blur the entire picture and look like a bug.
            if (d > 0.0f) st.focusDistance = d;
        }
        scene->camera.focusDistance = std::max(0.1f, st.focusDistance);
    }

    st.previewSamples = -1;
    st.job.start(std::move(scene), st.settings);
    st.status = "rendering";
}

} // namespace pathpanel
