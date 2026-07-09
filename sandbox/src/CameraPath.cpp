#include "CameraPath.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>

#include <imgui.h>

#include <fitzel/scene/Camera.hpp>

void samplePath(const std::vector<CamKey>& k, float time,
                glm::vec3& pos, float& yaw, float& pitch, float& fov) {
    const int n = static_cast<int>(k.size());
    if (n == 0) return;
    if (n == 1 || time <= k.front().t) {
        pos = k.front().pos; yaw = k.front().yaw;
        pitch = k.front().pitch; fov = k.front().fov; return;
    }
    if (time >= k.back().t) {
        pos = k.back().pos; yaw = k.back().yaw;
        pitch = k.back().pitch; fov = k.back().fov; return;
    }
    int i = 0;
    while (i < n - 1 && k[i + 1].t <= time) ++i;
    const float seg = k[i + 1].t - k[i].t;
    const float u   = seg > 1e-6f ? (time - k[i].t) / seg : 0.0f;
    const int i0 = std::max(0, i - 1), i1 = i, i2 = i + 1, i3 = std::min(n - 1, i + 2);
    pos   = catmull(k[i0].pos,   k[i1].pos,   k[i2].pos,   k[i3].pos,   u);
    yaw   = catmull(k[i0].yaw,   k[i1].yaw,   k[i2].yaw,   k[i3].yaw,   u);
    pitch = catmull(k[i0].pitch, k[i1].pitch, k[i2].pitch, k[i3].pitch, u);
    fov   = catmull(k[i0].fov,   k[i1].fov,   k[i2].fov,   k[i3].fov,   u);
}

// --- CameraPathRecorder -----------------------------------------------------

namespace { const char* kPathFile = "campath.txt"; }

void CameraPathRecorder::append(fitzel::Camera& cam, float t) {
    // Unwrap yaw so it stays continuous with the previous key (no 360 spin).
    float y = cam.yaw();
    if (!m_keys.empty()) {
        const float prev = m_keys.back().yaw;
        while (y - prev >  180.0f) y -= 360.0f;
        while (y - prev < -180.0f) y += 360.0f;
    }
    m_keys.push_back({t, cam.position(), y, cam.pitch(), cam.fov()});
}

void CameraPathRecorder::save() const {
    std::ofstream f(kPathFile);
    for (const CamKey& k : m_keys)
        f << k.t << ' ' << k.pos.x << ' ' << k.pos.y << ' ' << k.pos.z << ' '
          << k.yaw << ' ' << k.pitch << ' ' << k.fov << '\n';
}

void CameraPathRecorder::load() {
    std::ifstream f(kPathFile);
    if (!f) return;
    std::vector<CamKey> loaded;
    CamKey k;
    while (f >> k.t >> k.pos.x >> k.pos.y >> k.pos.z >> k.yaw >> k.pitch >> k.fov)
        loaded.push_back(k);
    if (!loaded.empty()) { m_keys = std::move(loaded); m_playing = false; m_time = 0.0f; }
}

void CameraPathRecorder::update(fitzel::Camera& cam, float dt, bool allowPlay) {
    if (m_recording) {
        m_time       += dt;
        m_recordAccum += dt;
        if (m_recordAccum >= m_recordInterval) {
            m_recordAccum -= m_recordInterval;
            append(cam, m_time);
        }
    } else if (allowPlay && m_playing && m_keys.size() >= 2) {
        m_time += dt * m_speed;
        const float tmax = m_keys.back().t;
        if (m_time >= tmax) {
            if (m_loop) m_time = std::fmod(m_time, tmax);
            else { m_time = tmax; m_playing = false; }
        }
        glm::vec3 p; float y, pi, fv;
        samplePath(m_keys, m_time, p, y, pi, fv);
        cam.setPosition(p);
        cam.setYaw(y);
        cam.setPitch(pi);
        cam.setFov(fv);
    }
}

void CameraPathRecorder::panel(fitzel::Camera& cam) {
    ImGui::Text("Keyframes: %d", static_cast<int>(m_keys.size()));
    if (m_recording) ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1),
                                        "RECORDING  %.1fs", m_time);
    else if (m_playing) ImGui::TextColored(ImVec4(0.4f, 1, 0.5f, 1),
                                           "PLAYING  %.1fs", m_time);
    else ImGui::TextDisabled("idle");

    // Record continuously samples the camera while you fly; Add keyframe snapshots
    // the current pose. Both build the same spline path.
    if (ImGui::Button(m_recording ? "Stop recording" : "Record")) {
        m_recording = !m_recording;
        if (m_recording) {
            m_keys.clear();
            m_playing = false;
            m_time = 0.0f;
            m_recordAccum = 0.0f;
            append(cam, 0.0f); // anchor at the start pose
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add keyframe")) {
        const float t = m_keys.empty() ? 0.0f : m_keys.back().t + m_keySpacing;
        append(cam, t);
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        m_keys.clear();
        m_playing = m_recording = false;
        m_time = 0.0f;
    }

    ImGui::BeginDisabled(m_keys.size() < 2);
    if (ImGui::Button(m_playing ? "Stop" : "Play")) {
        m_playing = !m_playing;
        m_recording = false;
        if (m_playing) m_time = 0.0f;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Checkbox("Loop", &m_loop);

    ImGui::SliderFloat("Speed", &m_speed, 0.1f, 4.0f, "%.2fx");
    ImGui::SliderFloat("Key spacing", &m_keySpacing, 0.5f, 10.0f, "%.1f s");
    ImGui::SliderFloat("Rec interval", &m_recordInterval, 0.05f, 1.0f, "%.2f s");

    if (m_keys.size() >= 2) {
        const float tmax = m_keys.back().t;
        ImGui::Text("Duration: %.1f s", tmax);
        // Scrubbing previews the pose and pauses playback.
        if (ImGui::SliderFloat("Scrub", &m_time, 0.0f, tmax, "%.2f s")) {
            glm::vec3 p; float y, pi, fv;
            samplePath(m_keys, m_time, p, y, pi, fv);
            cam.setPosition(p);
            cam.setYaw(y);
            cam.setPitch(pi);
            cam.setFov(fv);
            m_playing = false;
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Save")) save();
    ImGui::SameLine();
    if (ImGui::Button("Load")) load();
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", kPathFile);
}
