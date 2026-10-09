#include "PathTracePanel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <imgui.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <tinyexr.h>

#include <fitzel/render/Renderer.hpp>
#include <fitzel/graphics/Texture3D.hpp>
#include <fitzel/scene/Camera.hpp>

#include <chrono>

#include <glad/gl.h>

#include "GpuTrace.hpp"
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
    if (ui::header("What is in the render, and what is not")) {
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

void draw(State& st, lightgrid::Runtime& light,
          const std::filesystem::path& scenePath, double now) {
    if (!st.open) return;

    ImGui::SetNextWindowSize(ImVec2(420, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Render", &st.open)) {
        ImGui::End();
        return;
    }

    const bool running = st.job.running();
    if (st.focusPending && st.job.samplesDone() > 0) {
        st.focusDistance = st.job.focusDistance();
        st.focusPending  = false;
    }

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

    // --- The world beyond the render queue --------------------------------
    // All of it on by default: a render is supposed to show the scene the
    // viewport shows. The switches are for the shot that wants less -- a car
    // on an empty plain, a building without its weather.
    ImGui::Spacing();
    ui::sectionText("World");
    worldtrace::Options& w = st.world;
    ImGui::Checkbox("Trees", &w.trees);
    ImGui::SameLine(160.0f);
    ImGui::Checkbox("Flowers", &w.flowers);
    ImGui::Checkbox("Lake and rivers", &w.water);
    ImGui::SameLine(160.0f);
    ImGui::Checkbox("Far mountains", &w.farTerrain);
    ImGui::Checkbox("Birds, butterflies, fish", &w.creatures);
    ImGui::Checkbox("Particles, spray, pollen", &w.particles);
    ImGui::Checkbox("Rain", &w.rain);
    ImGui::SameLine(160.0f);
    ImGui::Checkbox("Cloud shadows", &w.clouds);
    ImGui::Checkbox("The viewport's sky as background", &w.sky);
    ImGui::BeginDisabled(!w.trees);
    ImGui::SliderFloat("Forest reach", &w.treeRadius, 50.0f, 3000.0f, "%.0f m",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!w.farTerrain);
    ImGui::SliderFloat("Mountain reach", &w.farRadius, 1000.0f, 16000.0f, "%.0f m",
                       ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();
    ui::hint("Trees are traced as the real meshes all the way out -- the\n"
             "viewport's far cards are a stand-in the tracer does not need.\n"
             "Every tree of a kind shares one copy of its mesh, so a big forest\n"
             "costs render time, not memory.");
    ImGui::EndDisabled();

    // --- Baked light ------------------------------------------------------
    ImGui::Spacing();
    ui::sectionText("Baked light");
    {
        const bool baking = st.bakeRunning.load();
        ImGui::BeginDisabled(baking);
        ImGui::SliderInt("Probe density", &st.gridSettings.resolution, 4, 64,
                         "%d across");
        ui::hint("Probes along the widest side of the world. The cost is cubic\n"
                 "in this, so it is the one number worth thinking about before\n"
                 "pressing Bake. 16 is a draft, 24 is usable, 40 is a lot.");
        ImGui::SliderInt("Rays per probe", &st.gridSettings.rays, 16, 2048, "%d",
                         ImGuiSliderFlags_Logarithmic);
        ImGui::SliderInt("Bounces##grid", &st.gridSettings.bounces, 1, 6);
        ImGui::EndDisabled();

        if (!baking) {
            if (ImGui::Button("Bake light", ImVec2(-1.0f, 0.0f))) {
                st.bakeRequested = true;
                st.gridStatus = "harvesting the scene...";
            }
        } else {
            if (ImGui::Button("Cancel bake", ImVec2(-1.0f, 0.0f)))
                st.bakeCancel.store(true);
            char bar[64];
            std::snprintf(bar, sizeof(bar), "%.0f%%",
                          st.bakeProgress.load() * 100.0f);
            ImGui::ProgressBar(st.bakeProgress.load(), ImVec2(-1.0f, 0.0f), bar);
        }

        ImGui::BeginDisabled(!light.grid.valid());
        ImGui::Checkbox("Use baked light", &light.enabled);
        ImGui::SliderFloat("Bounce strength", &light.intensity, 0.0f, 3.0f, "%.2f");
        ImGui::EndDisabled();
        ui::hint("Replaces the scene's flat ambient colour with what each place\n"
                 "actually receives: under a bridge is dark, beside a red wall\n"
                 "is red. The SUN is deliberately not in it -- the day cycle\n"
                 "moves it, and baked light has to survive that -- so the sun\n"
                 "stays dynamic and this is everything else.");
        const std::string& note = st.gridStatus.empty() ? light.status
                                                         : st.gridStatus;
        if (!note.empty()) ImGui::TextDisabled("%s", note.c_str());
    }

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
    const bool haveProject = !scenePath.empty();
    const std::filesystem::path dir =
        haveProject ? scenePath.parent_path() / "renders" : std::filesystem::path();
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

void attachToShots(State& st, shotlist::Runner& shots, int samples,
                   const std::function<void(int&, int&)>& windowSize, bool gpuToo) {
    shots.startTrace = [&st, samples, windowSize] {
        int w = 1280, h = 720;
        if (windowSize) windowSize(w, h);
        // The window's shape, at most 1280 across: this is a check to look at,
        // and a 3440-wide still at any sample count is minutes per view.
        if (w > 1280) {
            h = h * 1280 / w;
            w = 1280;
        }
        st.settings.width   = std::max(64, w);
        st.settings.height  = std::max(64, h);
        st.settings.samples = std::max(1, samples);
        // FITZEL_SHOTS_SHOW=1/2/3: the diagnostic views (base colour, normals,
        // depth) instead of the render -- the first thing to reach for when a
        // traced still and the raster one disagree about a surface.
        if (const char* show = std::getenv("FITZEL_SHOTS_SHOW"))
            st.settings.show = static_cast<pathtrace::Show>(std::clamp(std::atoi(show), 0, 3));
        st.previewSamples   = -1;
        st.captureRequested = true;
    };
    // Done once the request has been taken, the job has run and stopped, and
    // there is a picture -- in that order, or the first poll after the request
    // would see the PREVIOUS render's finished image.
    shots.traceDone = [&st] {
        return !st.captureRequested && !st.job.running() && st.job.hasImage() &&
               st.job.samplesDone() >= st.job.samplesTotal();
    };
    shots.saveTrace = [&st, gpuToo, samples](const std::string& path) {
        std::vector<unsigned char> px;
        if (!st.job.snapshotLdr(px)) return false;
        const int w = st.job.settings().width, h = st.job.settings().height;
        // What went into it, beside it: the panel's notes are the first thing
        // to read when a traced still disagrees with the raster one.
        std::string txt = path;
        const std::size_t dot = txt.find_last_of('.');
        if (dot != std::string::npos) txt.resize(dot);
        if (std::FILE* f = std::fopen((txt + ".txt").c_str(), "w")) {
            std::fprintf(f, "%dx%d, %d samples, %.1fs (build %.2fs)\n%s\n", w, h,
                         st.job.samplesDone(), st.job.elapsedSeconds(),
                         st.job.buildSeconds(), st.reportLine.c_str());
            for (const std::string& n : st.report.notes) std::fprintf(f, "- %s\n", n.c_str());
            std::fclose(f);
        }
        if (gpuToo && st.lastScene && gputrace::available()) {
            // The same scene through the GPU tracer, in dispatches of a few
            // samples (one long one trips the driver's watchdog), resolved on
            // the CPU through the same curve the still uses.
            gputrace::Tracer gpu;
            const auto t0 = std::chrono::steady_clock::now();
            bool ok = gpu.init("assets/shaders/gputrace.comp") && gpu.upload(*st.lastScene);
            if (ok) {
                gpu.setCamera(st.lastScene->camera);
                gpu.setPath(st.job.settings().maxBounces, st.job.settings().clampIndirect);
                ok = gpu.resize(w, h);
            }
            for (int done = 0; ok && done < samples; done += 4)
                ok = gpu.accumulate(std::min(4, samples - done));
            std::vector<float> hdr;
            if (ok) ok = gpu.snapshotHdr(hdr);
            glFinish();
            const double secs = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - t0).count();
            if (ok) {
                std::vector<unsigned char> g(static_cast<std::size_t>(w) * h * 4, 255);
                const pathtrace::Scene& sc = *st.lastScene;
                for (int y = 0; y < h; ++y)
                    for (int x = 0; x < w; ++x) {
                        const std::size_t i = static_cast<std::size_t>(y) * w + x;
                        glm::vec3 c = pathtrace::tonemap(
                            glm::vec3(hdr[i * 3], hdr[i * 3 + 1], hdr[i * 3 + 2]),
                            sc.exposure, sc.grade);
                        c *= pathtrace::vignette((x + 0.5f) / w, (y + 0.5f) / h,
                                                 static_cast<float>(w) / h, sc.grade.vignette);
                        c = glm::clamp(c, glm::vec3(0.0f), glm::vec3(1.0f));
                        for (int k = 0; k < 3; ++k)
                            g[i * 4 + k] = static_cast<unsigned char>(c[k] * 255.0f + 0.5f);
                    }
                std::string gp = txt + "_gpu.png";
                stbi_write_png(gp.c_str(), w, h, 4, g.data(), w * 4);
            }
            if (std::FILE* f = std::fopen((txt + ".txt").c_str(), "a")) {
                std::fprintf(f, "GPU: %s in %.1fs, %d instances, tree depth %d/%d, "
                             "%d maps (%d shrunk, %d dropped)%s%s\n",
                             ok ? "rendered" : "FAILED", secs, gpu.instanceCount(),
                             gpu.bvhDepth(), gpu.tlasDepth(), gpu.textureCount(),
                             gpu.texturesShrunk(), gpu.texturesDropped(), ok ? "" : " -- ",
                             ok ? "" : gpu.error().c_str());
                std::fclose(f);
            }
        }
        return stbi_write_png(path.c_str(), w, h, 4, px.data(), w * 4) != 0;
    };
}

State::~State() {
    // The bake thread outlives nothing. Cancelling and joining here is what
    // makes closing the editor mid-bake a close rather than a crash.
    bakeCancel.store(true);
    if (bakeThread.joinable()) bakeThread.join();
}

void service(State& st, lightgrid::Runtime& light,
             fitzel::Renderer& renderer, const fitzel::Camera& camera,
             const SceneLook& look, const std::filesystem::path& scenePath) {
    // A finished bake. The worker filled `light.grid` directly; all that is
    // left is the half that needs a GL context and the render thread.
    if (st.bakeDone.exchange(false)) {
        if (st.bakeThread.joinable()) st.bakeThread.join();
        st.bakeRunning.store(false);
        if (light.grid.valid()) {
            light.upload();
            const std::filesystem::path f = lightgrid::pathFor(scenePath);
            const bool saved = !f.empty() && lightgrid::save(light.grid, f);
            char msg[256];
            std::snprintf(msg, sizeof(msg), "%d x %d x %d probes baked%s; %d lamps (%d baked only)",
                          light.grid.nx, light.grid.ny, light.grid.nz,
                          saved ? " and saved beside the scene"
                                : " (not saved: no scene file)",
                          st.bakeLamps, st.bakeLampsBaked);
            st.gridStatus = msg;
        } else {
            // The grid was cleared by the cancel, so the scene has no baked
            // light until the next bake -- said plainly, because silently
            // falling back to the flat ambient looks like the bake did nothing.
            light.scene.clear();      // makes syncTo reload what was on disk
            st.gridStatus = "bake cancelled";
        }
    }

    if (!st.captureRequested && !st.bakeRequested) return;

    // --- The harvest, shared by both buttons ------------------------------
    const bool wantBake = st.bakeRequested;
    st.captureRequested = false;
    st.bakeRequested    = false;

    pathcapture::Options opt = st.capture;
    opt.hdriPath      = look.hdriPath;
    opt.hdriIntensity = look.hdriIntensity;
    opt.grade         = look.grade;
    if (wantBake) {
        // A bake is not a shot. It has to cover the world a car will drive
        // through, not the few hundred metres one framing can see, so the
        // camera-distance limit is lifted for it.
        opt.maxDistance = 0.0f;
    }

    std::shared_ptr<pathtrace::Scene> scenePtr =
        pathcapture::capture(renderer, camera, opt, &st.report);
    st.reportLine = st.report.summary();

    // Grass, which the harvest cannot see. Added after capture() rather than
    // inside it: capture() reads the render queue, and the field is not in it.
    if (look.grass) {
        grassfield::TraceOptions gopt;
        gopt.centerXZ = glm::vec2(camera.position().x, camera.position().z);
        gopt.radius   = look.grassRadius;
        gopt.windTime = look.grassWindTime;
        gopt.painted  = look.paintedGrass;
        gopt.paintedHeightScale = look.grassHeight;
        grassfield::TraceReport grep;
        grassfield::appendToScene(*scenePtr, *look.grass, gopt, &grep);
        followGrass(scenePtr->ground, grep.radius);
        if (grep.blades > 0) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                          "grass: %lld blades, %lld triangles, %d colours, to %.0f m%s",
                          grep.blades, grep.triangles, grep.materials, grep.radius,
                          grep.truncated ? " (cut from the field's radius to fit the "
                                           "triangle ceiling)" : "");
            st.report.notes.emplace_back(buf);
        }
    }

    // Everything else the queue does not carry. Not for a bake: a bake covers
    // the ground a car drives over, and the far mountains and a forest to the
    // horizon would stretch its grid across kilometres of nothing.
    if (look.world && !wantBake)
        look.world(*scenePtr, st.world, false, st.report.notes);
    st.report.triangles = scenePtr->triangleCount();
    st.reportLine = st.report.summary();

    if (scenePtr->triangleCount() == 0) {
        (wantBake ? st.gridStatus : st.status) =
            "nothing to work with: the frame has no geometry the tracer can read.";
        return;
    }

    if (wantBake) {
        light.grid = lightgrid::layout(*scenePtr, st.gridSettings);
        if (!light.grid.valid()) {
            st.gridStatus = "the scene is too small to put a grid over";
            return;
        }
        st.bakeCancel.store(false);
        st.bakeProgress.store(0.0f);
        st.bakeDone.store(false);
        st.bakeRunning.store(true);
        st.gridStatus = "baking...";
        st.bakeLamps = static_cast<int>(scenePtr->lamps.size());
        st.bakeLampsBaked = 0;
        for (const pathtrace::Lamp& l : scenePtr->lamps)
            if (l.bakeDirect) ++st.bakeLampsBaked;
        // The scene goes to the worker as a shared_ptr and is never touched
        // here again, so the editor is free to carry on editing the one it was
        // harvested from while the bake runs.
        lightgrid::Grid* target = &light.grid;
        st.bakeThread = std::thread([&st, target, scenePtr] {
            lightgrid::bake(*target, *scenePtr, st.gridSettings,
                            [&st](float p) {
                                st.bakeProgress.store(p);
                                return !st.bakeCancel.load();
                            });
            st.bakeDone.store(true);
        });
        return;
    }

    // --- A still ----------------------------------------------------------
    st.job.cancel();
    scenePtr->camera.apertureRadius = std::max(0.0f, st.aperture);
    // The focus is measured by the job, once its accelerator exists, rather
    // than here by a brute-force walk over every triangle before it does: on a
    // scene with a forest in it that one question cost more than the build.
    scenePtr->camera.focusDistance = std::max(0.1f, st.focusDistance);
    st.settings.autoFocus = st.aperture > 0.0f && st.autoFocus;
    st.focusPending       = st.settings.autoFocus;

    st.previewSamples = -1;
    st.lastScene = scenePtr;
    st.job.start(std::move(scenePtr), st.settings);
    st.status = "rendering";
}

} // namespace pathpanel
