#include "Autosave.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include <fitzel/Version.hpp>

#ifndef FITZEL_PLAYER
#include <imgui.h>
#include "UiStyle.hpp"
#endif

namespace fs = std::filesystem;

namespace autosave {
namespace {

// The two files a snapshot is made of. The scene carries the work; the sidecar
// carries the handful of strings the startup dialog needs, so deciding whether
// there is anything to recover never means parsing a scene.
std::string sceneFileIn(const std::string& dir)   { return dir + "/session.fitzel"; }
std::string sidecarFileIn(const std::string& dir) { return dir + "/session.json"; }

std::int64_t nowEpoch() {
    return static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

// "2026-08-28 14:32" in local time -- the form a person reads off a dialog.
std::string stampFrom(std::int64_t epoch) {
    const std::time_t t = static_cast<std::time_t>(epoch);
    std::tm tmv{};
#if defined(_WIN32)
    if (localtime_s(&tmv, &t) != 0) return std::string();
#else
    if (!localtime_r(&t, &tmv)) return std::string();
#endif
    char buf[32];
    if (!std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmv)) return std::string();
    return std::string(buf);
}

std::string clockFrom(std::int64_t epoch) {
    const std::string full = stampFrom(epoch);
    return full.size() >= 16 ? full.substr(11) : full; // just "14:32"
}

// Write `text` to `path` by way of <path>.tmp, so a crash mid-write leaves the
// previous file intact rather than a half-written one. Everything the snapshot
// writes goes through here -- a recovery file you cannot trust is worse than
// none, because it is offered with the same confidence as a good one.
bool writeAtomic(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { // some filesystems refuse to replace: clear the way and retry once
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) { std::error_code rec; fs::remove(tmp, rec); return false; }
    return true;
}

// The same move, for a file a caller has already produced at `tmp`.
bool moveIntoPlace(const std::string& tmp, const std::string& path) {
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) { std::error_code rec; fs::remove(tmp, rec); return false; }
    return true;
}

} // namespace

Snapshot pending(const std::string& dir) {
    Snapshot s;
    const std::string sidecar = sidecarFileIn(dir);
    std::error_code ec;
    if (!fs::exists(sidecar, ec)) return s;
    std::ifstream f(sidecar);
    if (!f) return s;
    nlohmann::json j;
    try { f >> j; }
    catch (const nlohmann::json::exception&) { return s; }

    const std::string file = j.value("file", sceneFileIn(dir));
    if (!fs::exists(file, ec)) return s; // sidecar without its scene: nothing to offer
    const std::string folder = j.value("project", std::string());
    if (folder.empty() || !fs::is_directory(folder, ec)) return s; // project gone

    s.file          = file;
    s.scenePath     = j.value("scene", std::string());
    s.projectFolder = folder;
    s.projectName   = j.value("projectName", std::string());
    s.sceneName     = j.value("sceneName", std::string());
    s.app           = j.value("app", std::string());
    const std::int64_t written = j.value("epoch", static_cast<std::int64_t>(0));
    s.writtenAt = written ? stampFrom(written) : std::string();
    s.ageMinutes = written
        ? static_cast<int>((nowEpoch() - written) / 60)
        : 0;
    if (s.ageMinutes < 0) s.ageMinutes = 0; // a clock that moved backwards
    return s;
}

void discard(const std::string& dir) {
    std::error_code ec;
    // The sidecar first: it is what makes a snapshot visible, so removing it
    // first means an interrupted discard leaves an ignored file, never a live
    // pointer to a scene that is already gone.
    fs::remove(sidecarFileIn(dir), ec);
    fs::remove(sceneFileIn(dir), ec);
    fs::remove(sceneFileIn(dir) + ".tmp", ec);
    fs::remove(sidecarFileIn(dir) + ".tmp", ec);
}

void Autosave::clear() {
    discard(m_dir);
    m_status.clear();
    m_nextDue   = -1.0;
    m_lastWrite = -1.0;
    m_haveRev   = false;
    m_touched   = false;
}

void Autosave::tick(double now, bool editable, const std::string& scenePath,
                    unsigned revision,
                    const std::function<bool(const std::string&)>& write) {
    if (!m_enabled) return;
    // No project, or a document that is not the user's work right now (play mode
    // rolls its changes back on Stop; a load in flight has half a scene). Snapshot
    // either of those and the offer after a crash would be worse than none.
    if (!editable || scenePath.empty()) return;

    if (m_nextDue < 0.0) { // first eligible frame: start the clock, take a baseline
        m_nextDue   = now + m_interval * 60.0;
        m_lastWrite = now;
        m_lastRev   = revision;
        m_haveRev   = true;
        return;
    }
    if (now < m_nextDue) return;
    m_nextDue = now + m_interval * 60.0;

    const bool edited = !m_haveRev || revision != m_lastRev || m_touched;
    // Not every edit reaches the undo history: terrain sculpting, grass and tree
    // painting and the scene's look are written as scene *settings*, and someone
    // can spend an hour in there without pushing a single command. So a snapshot
    // is taken anyway once one is this old -- the periodic backstop under the
    // cheap check, rather than a dirty flag threaded through every panel.
    const bool overdue = m_lastWrite >= 0.0 &&
                         (now - m_lastWrite) >= 3.0 * m_interval * 60.0;
    if (!edited && !overdue) return;

    std::error_code ec;
    fs::create_directories(m_dir, ec);
    const std::string scene = sceneFileIn(m_dir);
    const std::string tmp   = scene + ".tmp";
    if (!write(tmp)) { fs::remove(tmp, ec); return; }
    if (!moveIntoPlace(tmp, scene)) return;

    const fs::path sp(scenePath);
    const std::string folder = sp.parent_path().generic_string();
    const std::int64_t epoch = nowEpoch();
    nlohmann::json j;
    j["file"]        = scene;
    j["scene"]       = fs::path(scenePath).generic_string();
    j["project"]     = folder;
    j["projectName"] = fs::path(folder).filename().string();
    j["sceneName"]   = sp.stem().string();
    j["epoch"]       = epoch;
    j["app"]         = fitzel::kVersionFull;
    // The sidecar last, and only now: it is the flag that says "a snapshot is
    // here", and it must never be raised over a scene file that is not finished.
    if (!writeAtomic(sidecarFileIn(m_dir), j.dump(2) + "\n")) return;

    m_lastWrite = now;
    m_lastRev   = revision;
    m_haveRev   = true;
    m_touched   = false;
    m_status    = "Autosave " + clockFrom(epoch);
}

#ifndef FITZEL_PLAYER

Choice drawRecoveryModal(const Snapshot& s) {
    const char* kTitle = "Recover unsaved work";
    if (!ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);

    ImGui::SetNextWindowSize(ImVec2(560.0f, 0.0f), ImGuiCond_Appearing);
    Choice choice = Choice::None;
    if (ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped(
            "The last session ended without closing the editor. Changes that were "
            "never saved to the project are still here.");
        ImGui::Spacing();
        ui::sectionText("Snapshot");
        ImGui::Text("Project:  %s", s.projectName.c_str());
        ImGui::Text("Scene:    %s", s.sceneName.c_str());
        if (!s.writtenAt.empty()) {
            if (s.ageMinutes < 60)
                ImGui::Text("Taken:    %s  (%d min ago)", s.writtenAt.c_str(),
                            s.ageMinutes);
            else
                ImGui::Text("Taken:    %s", s.writtenAt.c_str());
        }
        ImGui::Spacing();
        ui::hint("Restoring opens the project and loads these changes instead of "
                 "the saved scene. Nothing is written to the project until you "
                 "save, so you can look first and still decide against it.");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Wide, well-separated buttons: this dialog is the one place where a
        // mis-click costs work that cannot be undone, so the discard button is
        // nowhere near the one the user reaches for.
        const ImVec2 big(200.0f, 40.0f);
        if (ImGui::Button("Restore", big)) { choice = Choice::Restore; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine(0.0f, 100.0f);
        if (ImGui::Button("Discard", big)) { choice = Choice::Discard; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    return choice;
}

#endif // !FITZEL_PLAYER

} // namespace autosave
