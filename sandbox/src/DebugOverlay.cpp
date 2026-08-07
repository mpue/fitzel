#include "DebugOverlay.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <imgui.h>

#include "Profiler.hpp"
#include "UiStyle.hpp"

#ifdef _WIN32
// windows.h defines min/max as macros, which breaks every std::max below.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

namespace debugoverlay {
namespace {

// --- Video memory ----------------------------------------------------------
// Core GL has no way to ask how much VRAM is in use, so this leans on a vendor
// extension: NVX_gpu_memory_info on NVIDIA, ATI_meminfo on AMD. Neither is
// guaranteed, and the numbers are the driver's own accounting rather than a
// precise figure -- good enough to notice a leak or a texture flood, not to
// audit against.
constexpr GLenum kNvxDedicated = 0x9047; // TOTAL_DEDICATED_VIDMEM_NVX
constexpr GLenum kNvxAvailable = 0x9049; // CURRENT_AVAILABLE_VIDMEM_NVX
constexpr GLenum kAtiFreeTex   = 0x87FC; // TEXTURE_FREE_MEMORY_ATI

enum class VramSource { None, Nvx, Ati };

VramSource detectVram() {
    // Core-profile 3.3: the extension string list is enumerated, not one blob.
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    bool nvx = false, ati = false;
    for (GLint i = 0; i < n; ++i) {
        const char* e = reinterpret_cast<const char*>(
            glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
        if (!e) continue;
        if (std::string(e) == "GL_NVX_gpu_memory_info") nvx = true;
        if (std::string(e) == "GL_ATI_meminfo")         ati = true;
    }
    if (nvx) return VramSource::Nvx;
    if (ati) return VramSource::Ati;
    return VramSource::None;
}

// Both queries report *free* memory in kilobytes. Returns false when unavailable.
bool vramMB(double& usedMB, double& totalMB) {
    static const VramSource src = detectVram();
    if (src == VramSource::None) return false;

    if (src == VramSource::Nvx) {
        GLint total = 0, avail = 0;
        glGetIntegerv(kNvxDedicated, &total);
        glGetIntegerv(kNvxAvailable, &avail);
        // Clear any error the query raised rather than leaving it for the next
        // caller's glGetError to trip over.
        while (glGetError() != GL_NO_ERROR) {}
        if (total <= 0) return false;
        totalMB = total / 1024.0;
        usedMB  = (total - avail) / 1024.0;
        return true;
    }
    // ATI_meminfo reports a 4-element vector; the first is the free pool. There
    // is no total to pair it with, so only the free figure is meaningful.
    GLint v[4] = {0, 0, 0, 0};
    glGetIntegerv(kAtiFreeTex, v);
    while (glGetError() != GL_NO_ERROR) {}
    if (v[0] <= 0) return false;
    totalMB = 0.0;
    usedMB  = v[0] / 1024.0; // caller prints this as "free" when totalMB == 0
    return true;
}

// --- Process memory --------------------------------------------------------
bool processMemoryMB(double& workingSet, double& peak) {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS pmc{};
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return false;
    workingSet = static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    peak       = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
    return true;
#else
    (void)workingSet; (void)peak;
    return false;
#endif
}

// Green below the budget, amber approaching it, red past it. The budget is the
// frame time the display can actually show, so "red" means a dropped frame.
ImVec4 costColour(double ms, double budget) {
    if (ms > budget)         return ImVec4(1.00f, 0.42f, 0.38f, 1.0f);
    if (ms > budget * 0.6)   return ImVec4(1.00f, 0.78f, 0.35f, 1.0f);
    return ImVec4(0.55f, 0.85f, 0.55f, 1.0f);
}

} // namespace

void draw(bool* open) {
    if (!open || !*open) return;

    ImGui::SetNextWindowSize(ImVec2(380.0f, 460.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Performance", open)) { ImGui::End(); return; }

    const prof::FrameStats fs = prof::frameStats();
    // The refresh-rate budget, inferred from the typical frame rather than
    // assumed to be 60 Hz -- this runs on 144 Hz panels too, where 16.7 ms is
    // already a stutter.
    const double budget = fs.avg > 0.0f ? std::max(1000.0 / 144.0,
                                                   static_cast<double>(fs.avg))
                                        : 16.7;

    ui::sectionText("Frame");
    ImGui::TextColored(costColour(fs.last, budget * 1.5),
                       "%.2f ms   %.0f FPS", fs.last,
                       fs.last > 0.0f ? 1000.0f / fs.last : 0.0f);
    ImGui::SameLine();
    ui::hint("(avg %.2f ms)", fs.avg);

    // The two numbers that actually describe stutter. Average fps can look
    // perfect while every third second drops a frame; these cannot.
    ImGui::TextColored(costColour(fs.worst, budget * 2.0),
                       "worst %.1f ms", fs.worst);
    ImGui::SameLine();
    ImGui::Text("| 1%% low %.0f FPS", fs.low1);
    ImGui::SameLine();
    ImGui::TextColored(fs.spikes > 0 ? ImVec4(1.0f, 0.55f, 0.3f, 1.0f)
                                     : ImVec4(0.55f, 0.85f, 0.55f, 1.0f),
                       "| %d spikes", fs.spikes);
    ui::hint("Worst frame and spike count over the last ~4 s. A spike is any "
             "frame over twice the median.");

    const std::vector<float>& hist = prof::history();
    if (!hist.empty()) {
        // Scale to the worst frame, floored at ~2 frames' worth, so a flat graph
        // stays flat instead of amplifying sub-millisecond noise into mountains.
        const float top = std::max(fs.worst * 1.1f, 2.0f * fs.avg);
        ImGui::PlotLines("##frametimes", hist.data(),
                         static_cast<int>(hist.size()), 0, nullptr, 0.0f, top,
                         ImVec2(-1.0f, 70.0f));
    }
    if (ImGui::SmallButton("Reset history")) prof::reset();
    ImGui::SameLine();
    ui::hint("Clears the window after a load spike.");

    ImGui::Spacing();
    ui::sectionText("Memory");
    double ws = 0.0, peak = 0.0;
    if (processMemoryMB(ws, peak))
        ImGui::Text("RAM  %.0f MB   (peak %.0f MB)", ws, peak);
    else
        ImGui::TextDisabled("RAM  unavailable on this platform");

    double vUsed = 0.0, vTotal = 0.0;
    if (vramMB(vUsed, vTotal)) {
        if (vTotal > 0.0) ImGui::Text("VRAM %.0f / %.0f MB", vUsed, vTotal);
        else              ImGui::Text("VRAM %.0f MB free", vUsed);
    } else {
        ImGui::TextDisabled("VRAM  no driver query available");
    }

    ImGui::Spacing();
    ui::sectionText("Where the time goes");
    const std::vector<prof::ZoneStat>& zones = prof::zones();
    if (zones.empty()) {
        ImGui::TextDisabled("No zones recorded yet.");
    } else if (ImGui::BeginTable("zones", 4,
                                 ImGuiTableFlags_RowBg |
                                 ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Section");
        ImGui::TableSetupColumn("ms");
        ImGui::TableSetupColumn("avg");
        ImGui::TableSetupColumn("worst");
        ImGui::TableHeadersRow();
        // Sorted by the smoothed cost, so the expensive sections stay at the top
        // instead of the rows jumping around with every frame's noise.
        std::vector<const prof::ZoneStat*> order;
        order.reserve(zones.size());
        for (const prof::ZoneStat& z : zones) order.push_back(&z);
        std::sort(order.begin(), order.end(),
                  [](const prof::ZoneStat* a, const prof::ZoneStat* b) {
                      return a->avg > b->avg;
                  });
        for (const prof::ZoneStat* z : order) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(z->name);
            ImGui::TableNextColumn();
            ImGui::TextColored(costColour(z->last, budget), "%.2f", z->last);
            ImGui::TableNextColumn(); ImGui::Text("%.2f", z->avg);
            ImGui::TableNextColumn();
            ImGui::TextColored(costColour(z->worst, budget * 1.5), "%.2f",
                               z->worst);
        }
        ImGui::EndTable();
    }
    ui::hint("\"present\" is the buffer swap. If that is the only section "
             "spiking while the rest stays flat, the stall is on the GPU or in "
             "the driver, not in our frame -- vsync parks the wait there.");

    ImGui::End();
}

} // namespace debugoverlay
