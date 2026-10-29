#include "ScriptSfx.hpp"

#include <algorithm>

void ScriptSfx::play(const std::string& path, float volume, float pitch, const glm::vec3* pos,
                     float nearM, float farM, int strip) {
    if (path.empty() || !m_audio.ok()) return;
    std::vector<Voice>& pool = m_voices[path];
    Voice* pick = nullptr;
    for (Voice& v : pool)
        if (!v.sound.isPlaying()) { pick = &v; break; }
    if (!pick && static_cast<int>(pool.size()) < kVoicesPerFile) {
        Voice v;
        v.sound = fitzel::Sound::fromFile(m_audio, path, false);
        if (!v.sound.isValid()) return;
        pool.push_back(std::move(v));
        pick = &pool.back();
    }
    if (!pick) {   // all busy: cut the one that has played longest
        pick = &*std::min_element(pool.begin(), pool.end(),
                                  [](const Voice& a, const Voice& b) { return a.started < b.started; });
    }
    fitzel::Sound& s = pick->sound;
    if (pick->strip != strip) {   // re-routed only when it changes: a re-attach per shot is not free
        s.setOutput(m_audio.mixer(), strip);
        pick->strip = strip;
    }
    place(s, volume, pitch, pos, nearM, farM);
    pick->started = ++m_count;
    s.play();
}

void ScriptSfx::place(fitzel::Sound& s, float volume, float pitch, const glm::vec3* pos,
                      float nearM, float farM) {
    s.setVolume(glm::clamp(volume, 0.0f, 4.0f));
    s.setPitch(glm::clamp(pitch, 0.2f, 3.0f));
    if (pos) {
        s.setSpatial(true);
        s.setDopplerFactor(0.0f);
        s.setPosition(pos->x, pos->y, pos->z);
        const float n = glm::max(nearM, 0.1f);
        s.setAttenuation(n, glm::max(farM, n + 1.0f), 1.0f);
    } else {
        s.setSpatial(false);
    }
}

void ScriptSfx::loop(const std::string& key, const std::string& path, float volume, float pitch,
                     const glm::vec3* pos, float nearM, float farM, int strip) {
    if (key.empty() || path.empty() || !m_audio.ok()) return;
    auto it = m_loops.find(key);
    if (it != m_loops.end() && it->second.path != path) {
        // Another sound under this name: the old voice goes first, then the
        // new one is made -- never one assigned over a voice still playing.
        it->second.sound.stop();
        m_loops.erase(it);
        it = m_loops.end();
    }
    if (it == m_loops.end()) {
        Loop l;
        l.path  = path;
        l.sound = fitzel::Sound::fromFile(m_audio, path, true);
        if (!l.sound.isValid()) return;
        it = m_loops.emplace(key, std::move(l)).first;
    }
    Loop& l = it->second;
    if (l.strip != strip) {
        l.sound.setOutput(m_audio.mixer(), strip);
        l.strip = strip;
    }
    place(l.sound, volume, pitch, pos, nearM, farM);
    if (!l.sound.isPlaying()) l.sound.play();
}

void ScriptSfx::stopLoop(const std::string& key) {
    auto it = m_loops.find(key);
    if (it == m_loops.end()) return;
    it->second.sound.stop();
    m_loops.erase(it);
}

void ScriptSfx::clear() {
    for (auto& [path, pool] : m_voices)
        for (Voice& v : pool) v.sound.stop();
    m_voices.clear();
    for (auto& [key, l] : m_loops) l.sound.stop();
    m_loops.clear();
}
