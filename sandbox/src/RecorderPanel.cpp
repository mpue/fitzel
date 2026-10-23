#include "RecorderPanel.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include <imgui.h>

#include "UiStyle.hpp"

namespace recorderui {
namespace {

namespace fs = std::filesystem;

std::string clock(double s) {
    const int m = static_cast<int>(s / 60.0);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%d:%05.2f", m, s - m * 60.0);
    return buf;
}

std::string megabytes(std::size_t b) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f MB", static_cast<double>(b) / (1024.0 * 1024.0));
    return buf;
}

// The name field: the saved take's stem, or a dated default for a fresh one.
// Remembered across frames, re-seeded whenever the take in hand changes.
char        g_name[128] = "";
std::string g_nameKey;

void seedName(const gamerec::Session& ss) {
    const std::string key = ss.takePath() + "|" + std::to_string(ss.take().frames.size()) + "|" +
                            std::to_string(ss.take().duration());
    if (g_nameKey == key && g_name[0]) return;
    g_nameKey = key;
    std::string stem;
    if (!ss.takePath().empty()) {
        stem = fs::path(ss.takePath()).stem().string();
    } else {
        std::time_t t = std::time(nullptr);
        char buf[64];
        std::strftime(buf, sizeof buf, "take_%Y-%m-%d_%H%M%S", std::localtime(&t));
        stem = buf;
    }
    std::snprintf(g_name, sizeof g_name, "%s", stem.c_str());
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    gamerec::Session& ss = s.session;
    ImGui::SetNextWindowSize(ImVec2(420.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Recorder", &s.show)) { ImGui::End(); return; }

    ui::hint("Records what a Play run did -- every object's motion, the camera, the clock -- "
             "so it can be watched again from any angle and rendered to a video.");

    // --- Record -----------------------------------------------------------------
    ui::sectionText("Record");
    const bool exporting = ss.exporting();
    ImGui::BeginDisabled(exporting);
    if (ss.recording()) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("Stop recording", ImVec2(160.0f, 0.0f))) ss.stopRecording();
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const bool blink = std::fmod(ImGui::GetTime(), 1.0) < 0.6;
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, blink ? 1.0f : 0.35f), "REC");
        ImGui::SameLine();
        ImGui::Text("%s  %zu frames  %s", clock(ss.recorder().time()).c_str(),
                    ss.recorder().frames(), megabytes(ss.recorder().bytes()).c_str());
    } else {
        if (ImGui::Button(s.playMode ? "Record" : "Play && record", ImVec2(160.0f, 0.0f))) {
            if (s.playMode) {
                ss.startRecording();
            } else if (s.startPlay) {
                // Play's start runs onPlayStart, which records when asked to.
                const bool was = ss.autoRecord;
                ss.autoRecord = true;
                s.startPlay();
                ss.autoRecord = was;
            }
        }
        ImGui::SetItemTooltip("F8 in Play starts and stops recording.");
    }
    ImGui::Checkbox("Record every Play run", &ss.autoRecord);
    ImGui::EndDisabled();

    // --- Take -------------------------------------------------------------------
    ui::sectionText("Take");
    if (ss.hasTake()) {
        seedName(ss);
        const gamerec::Take& t = ss.take();
        ImGui::Text("%s, %zu frames, %zu spawned, %s", clock(t.duration()).c_str(), t.frames.size(),
                    t.spawns.size(), megabytes(t.bytes()).c_str());
        if (!t.scene.empty()) ImGui::TextDisabled("recorded in %s", t.scene.c_str());
        ImGui::SetNextItemWidth(-90.0f);
        ImGui::InputText("##takename", g_name, sizeof g_name);
        ImGui::SameLine();
        if (ImGui::Button(ss.takeSaved() ? "Save" : "Save*", ImVec2(-1.0f, 0.0f))) ss.saveTake(g_name);
    } else {
        ImGui::TextDisabled("No take yet: record a Play run, or load one below.");
    }

    // The folder, looked at every couple of seconds rather than every frame.
    static std::vector<std::string> takes;
    static double listedAt = -1e9;
    static std::string listedFor;
    if (ImGui::GetTime() - listedAt > 2.0 || listedFor != ss.takePath()) {
        takes = ss.listTakes();
        listedAt = ImGui::GetTime();
        listedFor = ss.takePath();
    }
    if (!takes.empty() && ImGui::BeginListBox("##takes", ImVec2(-1.0f, 110.0f))) {
        for (const std::string& p : takes) {
            const bool cur = p == ss.takePath();
            const std::string label = fs::path(p).stem().string();
            if (ImGui::Selectable(label.c_str(), cur, ImGuiSelectableFlags_AllowDoubleClick) &&
                !cur && !exporting)
                ss.loadTake(p);
        }
        ImGui::EndListBox();
    }

    // --- Replay -----------------------------------------------------------------
    ui::sectionText("Replay");
    ImGui::BeginDisabled(!ss.hasTake() || s.playMode || ss.recording() || exporting);
    if (!ss.replaying()) {
        if (ImGui::Button("Replay", ImVec2(110.0f, 0.0f))) ss.beginReplay();
        ImGui::SetItemTooltip("Plays the take over the scene. The scene is put back when it stops.");
    } else {
        if (ImGui::Button(ss.paused ? "Resume" : "Pause", ImVec2(80.0f, 0.0f))) {
            if (ss.paused && ss.head >= ss.take().duration()) ss.head = 0.0;
            ss.paused = !ss.paused;
        }
        ImGui::SameLine();
        if (ImGui::Button("Stop", ImVec2(80.0f, 0.0f))) ss.endReplay();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &ss.loop);
    ImGui::SameLine();
    ImGui::Checkbox("Free camera", &ss.freeCam);
    ImGui::SetItemTooltip("Fly the editor camera through the replay instead of riding the "
                          "recorded one.");

    if (ss.replaying() || ss.hasTake()) {
        float head = static_cast<float>(ss.head);
        const float len = static_cast<float>(ss.take().duration());
        ImGui::SetNextItemWidth(-1.0f);
        const std::string fmt = clock(ss.head) + " / " + clock(len);
        if (ImGui::SliderFloat("##head", &head, 0.0f, std::max(len, 0.001f), fmt.c_str())) {
            ss.head = head;
            if (!ss.replaying()) ss.beginReplay();
            ss.paused = true;   // scrubbing holds the picture where it is put
        }
    }
    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("Speed", &ss.speed, 0.1f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
    ImGui::EndDisabled();

    // --- Video ------------------------------------------------------------------
    ui::sectionText("Video");
    const bool ffmpeg = videoout::ffmpegAvailable();
    ImGui::BeginDisabled(exporting);
    {
        int fmt = ss.exportFormat == videoout::Format::Mp4 ? 0 : 1;
        const char* names[] = {ffmpeg ? "MP4 (H.264)" : "MP4 (needs ffmpeg)", "AVI (MJPEG)"};
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Format", &fmt, names, 2))
            ss.exportFormat = fmt == 0 ? videoout::Format::Mp4 : videoout::Format::Avi;
        if (!ffmpeg) ImGui::SetItemTooltip("MP4 is encoded by ffmpeg, which isn't installed "
                                           "(apt install ffmpeg). AVI needs nothing.");
        const int rates[] = {24, 25, 30, 50, 60};
        int ri = 2;
        for (int i = 0; i < 5; ++i) if (rates[i] == ss.exportFps) ri = i;
        const char* rateNames[] = {"24", "25", "30", "50", "60"};
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("fps", &ri, rateNames, 5)) ss.exportFps = rates[ri];
        ImGui::SetNextItemWidth(160.0f);
        ImGui::SliderInt("Quality", &ss.exportQuality, 30, 100);
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("Size: the Scene view's, as it is now.");

    if (exporting) {
        const float p = ss.exportProgress();
        char over[64];
        std::snprintf(over, sizeof over, "%.0f%%  %s", p * 100.0f, clock(ss.head).c_str());
        ImGui::ProgressBar(p, ImVec2(-90.0f, 0.0f), over);
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(-1.0f, 0.0f))) ss.cancelExport();
    } else {
        ImGui::BeginDisabled(!ss.hasTake() || s.playMode || ss.recording() ||
                             (ss.exportFormat == videoout::Format::Mp4 && !ffmpeg));
        if (ImGui::Button("Export video", ImVec2(160.0f, 0.0f))) {
            if (!ss.takeSaved() && g_name[0]) ss.saveTake(g_name);   // the video is named after it
            ss.beginExport(ss.defaultVideoPath());
        }
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Renders the take frame by frame at a fixed rate and writes it to "
                              "the recordings folder next to the take.");
    }

    if (!ss.status.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", ss.status.c_str());
    }
    ImGui::End();
}

} // namespace recorderui
