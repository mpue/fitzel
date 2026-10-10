#include "ShotList.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <vector>

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
        Shot s;
        {
            std::istringstream probe(line);
            std::string word;
            if (probe >> s.name >> word && word == "game") {
                s.gameView = true;
                probe >> s.hour >> s.settle >> s.frames >> s.every;
                if (s.frames < 1) s.frames = 1;
                m_shots.push_back(s);
                continue;
            }
        }
        std::istringstream ss(line);
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
        float vx = 0.0f, vz = 0.0f, turn = 0.0f;
        int shrink = 0;
        if (ss >> vx >> vz >> turn) {
            s.vel  = {vx, vz};
            s.turn = turn;
            s.shrink = (ss >> shrink && shrink != 0) ? shrink : 4;
        }
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
    if (s.gameView) {               // the game's own view: only the clock, if asked
        if (s.hour >= 0.0f) timeOfDay = s.hour;
        return;
    }
    // A moving sequence: where the eye has got to since its first picture.
    const float moved = (m_seqStart >= 0.0) ? static_cast<float>(m_now - m_seqStart) : 0.0f;
    glm::vec3 p = s.pos + glm::vec3(s.vel.x, 0.0f, s.vel.y) * moved;
    if (s.groundRel && groundAt) p.y += groundAt(p.x, p.z);
    cam.setPosition(p);
    cam.setYaw(s.yaw + s.turn * moved);
    cam.setPitch(s.pitch);
    glm::vec3 t, e;
    if (!s.name.empty() && s.name[0] == '@' && eye && eye(std::atoi(s.name.c_str() + 1), e, t) &&
        glm::length(t - e) > 0.1f) {
        cam.setPosition(e);
        const glm::vec3 d = glm::normalize(t - e);
        cam.setYaw(glm::degrees(std::atan2(d.z, d.x)));
        cam.setPitch(glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))));
    } else if (!s.name.empty() && s.name[0] == '@' && target &&
        target(std::atoi(s.name.c_str() + 1), t) && glm::length(t - p) > 0.5f) {
        const glm::vec3 d = glm::normalize(t - p);
        cam.setYaw(glm::degrees(std::atan2(d.z, d.x)));
        cam.setPitch(glm::degrees(std::asin(glm::clamp(d.y, -1.0f, 1.0f))));
    }
    cam.setFov(s.fov);
    if (s.hour >= 0.0f) timeOfDay = s.hour;
}

bool Runner::afterFrame(double now, int w, int h) {
    if (!active()) return m_done;
    m_now = now;
    if (m_index < 0) {           // the first frame after load starts the clock
        m_index = 0;
        m_frame = 0;
        m_since = now;
        return false;
    }
    const Shot& s = m_shots[static_cast<std::size_t>(m_index)];

    // A traced still of this view is rendering: hold the view until it is done,
    // then write it and move on.
    if (m_tracing) {
        if (traceDone && !traceDone()) return false;
        const std::string out = m_outDir + "/" + s.name + "_traced.png";
        const bool ok = saveTrace && saveTrace(out);
        const std::string line = s.name + "_traced  " + (ok ? "written" : "FAILED");
        std::fprintf(stderr, "shots: %s\n", line.c_str());
        if (std::FILE* f = std::fopen((m_outDir + "/shots.log").c_str(), "a")) {
            std::fprintf(f, "%s\n", line.c_str());
            std::fclose(f);
        }
        m_tracing = false;
        m_frame   = 0;
        m_since   = now;
        if (++m_index >= static_cast<int>(m_shots.size())) m_done = true;
        return m_done;
    }

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
    const int k = std::max(1, s.shrink);
    if (s.shrink < -1) {
        // The middle of the frame at full resolution.
        const int c = -s.shrink;
        const int cw = w / c, ch = h / c, x0 = (w - cw) / 2, y0 = (h - ch) / 2;
        std::vector<unsigned char> mid(static_cast<std::size_t>(cw) * ch * 4);
        for (int y = 0; y < ch; ++y)
            std::memcpy(&mid[static_cast<std::size_t>(y) * cw * 4],
                        &flipped[(static_cast<std::size_t>(y0 + y) * w + x0) * 4],
                        static_cast<std::size_t>(cw) * 4);
        stbi_write_png(out.c_str(), cw, ch, 4, mid.data(), cw * 4);
    } else if (k > 1) {
        // Box-filtered down, so a picture costs a frame and not a second.
        const int sw = w / k, sh = h / k;
        std::vector<unsigned char> small(static_cast<std::size_t>(sw) * sh * 4);
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x)
                for (int c = 0; c < 4; ++c) {
                    int sum = 0;
                    for (int j = 0; j < k; ++j)
                        for (int i = 0; i < k; ++i)
                            sum += flipped[(static_cast<std::size_t>(y * k + j) * w + (x * k + i)) * 4 + c];
                    small[(static_cast<std::size_t>(y) * sw + x) * 4 + c] =
                        static_cast<unsigned char>(sum / (k * k));
                }
        stbi_write_png(out.c_str(), sw, sh, 4, small.data(), sw * 4);
    } else {
        stbi_write_png(out.c_str(), w, h, 4, flipped.data(), static_cast<int>(row));
    }
    if (m_frame == 0) m_seqStart = now;
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
    if (startTrace && m_frame + 1 >= s.frames) {
        startTrace();
        m_tracing = true;
        return false;
    }
    if (++m_frame >= s.frames) {
        m_frame = 0;
        m_seqStart = -1.0;
        if (++m_index >= static_cast<int>(m_shots.size())) m_done = true;
    }
    return m_done;
}

} // namespace shotlist
