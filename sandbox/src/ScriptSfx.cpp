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
    pick->started = ++m_count;
    s.play();
}

void ScriptSfx::clear() {
    for (auto& [path, pool] : m_voices)
        for (Voice& v : pool) v.sound.stop();
    m_voices.clear();
}
