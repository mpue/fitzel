#include "ShotList.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <glad/gl.h>
#include <stb_image_write.h>

#include <fitzel/scene/Camera.hpp>

namespace shotlist {

bool Runner::load(const std::string& file, const std::string& outDir) {
    std::ifstream in(file);
    if (!in) {
        std::fprintf(stderr, "shots: cannot read %s\n", file.c_str());
        return false;
    }
    m_outDir = outDir.empty()
                   ? std::filesystem::path(file).parent_path().generic_string()
                   : outDir;
    std::string line;
    while (std::getline(in, line)) {
        if (const auto hash = line.find('#'); hash != std::string::npos)
            line.erase(hash);
        std::istringstream ss(line);
        Shot s;
        std::string y;
        if (!(ss >> s.name >> s.pos.x >> y >> s.pos.z >> s.yaw >> s.pitch))
            continue;   // blank, comment, or not a view
        if (!y.empty() && y[0] == '~') {
            s.groundRel = true;
            y.erase(0, 1);
        }
        s.pos.y = static_cast<float>(std::atof(y.c_str()));
        ss >> s.fov >> s.hour >> s.settle >> s.frames >> s.every;
        if (s.frames < 1) s.frames = 1;
        m_shots.push_back(s);
    }
    std::fprintf(stderr, "shots: %zu views from %s\n", m_shots.size(), file.c_str());
    return !m_shots.empty();
}

void Runner::applyCamera(fitzel::Camera& cam,
                         const std::function<float(float, float)>& groundAt,
                         float& timeOfDay) {
    if (!active()) return;
    const Shot& s = m_shots[static_cast<std::size_t>(m_index < 0 ? 0 : m_index)];
    glm::vec3 p = s.pos;
    if (s.groundRel && groundAt) p.y += groundAt(p.x, p.z);
    cam.setPosition(p);
    cam.setYaw(s.yaw);
    cam.setPitch(s.pitch);
    cam.setFov(s.fov);
    if (s.hour >= 0.0f) timeOfDay = s.hour;
}

bool Runner::afterFrame(double now, int w, int h) {
    if (!active()) return m_done;
    if (m_index < 0) {           // the first frame after load starts the clock
        m_index = 0;
        m_frame = 0;
        m_since = now;
        return false;
    }
    const Shot& s = m_shots[static_cast<std::size_t>(m_index)];
    const double due = (m_frame == 0) ? s.settle : s.every;
    if (now - m_since < due) return false;

    const auto wroteStart = std::chrono::steady_clock::now();
    // After the swap the front buffer holds the finished frame, post chain and
    // all. Flipped: GL counts rows from the bottom.
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    std::vector<unsigned char> flipped(px.size());
    const std::size_t row = static_cast<std::size_t>(w) * 4;
    for (int y = 0; y < h; ++y)
        std::memcpy(&flipped[static_cast<std::size_t>(y) * row],
                    &px[static_cast<std::size_t>(h - 1 - y) * row], row);
    // Opaque: the alpha channel of the back buffer is whatever the last pass
    // left there, and a viewer that honours it shows holes.
    for (std::size_t i = 3; i < flipped.size(); i += 4) flipped[i] = 255;

    std::string name = s.name;
    if (s.frames > 1) {
        char suf[16];
        std::snprintf(suf, sizeof suf, "_%02d", m_frame);
        name += suf;
    }
    const std::string out = m_outDir + "/" + name + ".png";
    stbi_write_png(out.c_str(), w, h, 4, flipped.data(), static_cast<int>(row));
    // To a log beside the pictures as well: the release build writes its
    // console to the window it attaches, which a redirect never sees.
    const std::string line = name + "  " + (status ? status() : std::string());
    std::fprintf(stderr, "shots: %s\n", line.c_str());
    if (std::FILE* f = std::fopen((m_outDir + "/shots.log").c_str(), "a")) {
        std::fprintf(f, "%s\n", line.c_str());
        std::fclose(f);
    }

    // The clock restarts AFTER the picture is on disk: writing a 5 MB PNG
    // takes most of a second, and a wait measured from before it would be
    // over by the next frame -- every short settle silently became "one frame".
    m_since = now + std::chrono::duration<double>(std::chrono::steady_clock::now() - wroteStart).count();
    if (++m_frame >= s.frames) {
        m_frame = 0;
        if (++m_index >= static_cast<int>(m_shots.size())) m_done = true;
    }
    return m_done;
}

} // namespace shotlist
