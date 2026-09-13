#include "Soundscape.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>

namespace {

// How much a species sings at `hour`: its dawn, day and dusk factors blended
// across the day, and next to nothing at night.
float timeFactor(float hour, float dawn, float day, float dusk) {
    const auto bump = [](float h, float c, float w) {
        const float d = (h - c) / w;
        return std::exp(-d * d);
    };
    const float night = 0.02f;
    return night + dawn * bump(hour, 6.2f, 1.3f) + day * 0.45f * bump(hour, 12.5f, 4.0f)
         + dusk * 0.6f * bump(hour, 19.2f, 1.2f);
}

} // namespace

bool Soundscape::init(fitzel::Audio& audio, const std::string& soundDir) {
    if (!audio.ok()) return false;
    const char* files[] = {"bird_chaffinch_a", "bird_chaffinch_b", "bird_blackbird_a",
                           "bird_blackbird_b", "bird_robin_a", "bird_robin_b",
                           "bird_tit", "bird_chiffchaff", "bird_cuckoo",
                           "bird_woodpecker"};
    for (const char* f : files) {
        Pool p;
        for (int v = 0; v < 3; ++v) {
            fitzel::Sound s = fitzel::Sound::fromFile(audio, soundDir + "/" + f + ".wav", false);
            if (!s.isValid()) break;
            s.setSpatial(true);
            // A cuckoo is heard across the valley; a tit in the next tree.
            if (std::string(f) == "bird_cuckoo") s.setAttenuation(40.0f, 700.0f, 1.0f);
            else                                 s.setAttenuation(10.0f, 260.0f, 1.0f);
            s.setDopplerFactor(0.0f);
            p.voices.push_back(std::move(s));
        }
        m_pools.push_back(std::move(p));
    }
    // Species: which files, how common, how often a singer repeats, and when.
    const auto add = [&](std::vector<int> fs, float w, float g0, float g1, float dawn,
                         float day, float dusk, bool far = false) {
        Species s;
        s.files = std::move(fs);
        s.weight = w; s.gapMin = g0; s.gapMax = g1;
        s.dawn = dawn; s.day = day; s.dusk = dusk; s.far = far;
        m_species.push_back(s);
    };
    add({0, 1}, 3.0f, 5.0f, 9.0f, 1.0f, 1.0f, 0.6f);     // chaffinch
    add({2, 3}, 2.2f, 6.0f, 12.0f, 1.6f, 0.5f, 1.8f);    // blackbird: dawn and dusk
    add({4, 5}, 1.8f, 4.0f, 9.0f, 1.4f, 0.7f, 1.2f);     // robin
    add({6}, 1.8f, 7.0f, 14.0f, 1.0f, 1.0f, 0.4f);       // great tit
    add({7}, 1.3f, 6.0f, 11.0f, 1.0f, 0.9f, 0.5f);       // chiffchaff
    add({8}, 0.5f, 9.0f, 16.0f, 1.2f, 0.6f, 0.2f, true); // cuckoo, far off
    add({9}, 0.4f, 12.0f, 25.0f, 0.6f, 1.0f, 0.3f);      // woodpecker
    m_singers.assign(7, Singer{});

    m_leaves   = fitzel::Sound::fromFile(audio, soundDir + "/leaves_rustle.wav", true);
    m_meadow   = fitzel::Sound::fromFile(audio, soundDir + "/insects_meadow.wav", true);
    m_crickets = fitzel::Sound::fromFile(audio, soundDir + "/insects_crickets.wav", true);
    for (fitzel::Sound* s : {&m_leaves, &m_meadow, &m_crickets})
        if (s->isValid()) { s->setVolume(0.0f); s->play(); }
    m_ok = !m_pools.empty() && !m_pools[0].voices.empty();
    return m_ok;
}

bool Soundscape::sing(int file, const glm::vec3& pos, float volume, float pitch) {
    if (file < 0 || file >= static_cast<int>(m_pools.size())) return false;
    for (fitzel::Sound& s : m_pools[file].voices) {
        if (s.isPlaying()) continue;
        s.setPosition(pos.x, pos.y, pos.z);
        s.setVolume(volume);
        s.setPitch(pitch);
        s.play();
        ++m_phrases;
        return true;
    }
    return false;   // every voice of this song is busy: this phrase is skipped
}

glm::vec3 Soundscape::perch(const Frame& f, bool far) {
    // A tree near the listener, up in its crown -- or, far off or with no
    // tree about, a point in the air over the land.
    if (!far && f.trees && f.trees->size() >= 5) {
        const std::size_t n = f.trees->size() / 5;
        for (int tries = 0; tries < 16; ++tries) {
            const std::size_t i = static_cast<std::size_t>(uni() * n) % n;
            const float* t = &(*f.trees)[i * 5];
            const float d = glm::length(glm::vec2(t[0] - f.eye.x, t[2] - f.eye.z));
            if (d < 12.0f || d > 110.0f) continue;
            return glm::vec3(t[0], t[1] + t[4] * (0.55f + 0.3f * uni()), t[2]);
        }
    }
    const float a = uni() * 6.2831853f;
    const float d = far ? 180.0f + uni() * 150.0f : 25.0f + uni() * 60.0f;
    glm::vec3 p(f.eye.x + std::cos(a) * d, 0.0f, f.eye.z + std::sin(a) * d);
    p.y = (f.ground ? f.ground(p.x, p.z) : f.eye.y) + (far ? 15.0f : 6.0f + uni() * 6.0f);
    return p;
}

void Soundscape::update(float dt, const Frame& f, bool active) {
    if (!m_ok) return;
    dt = std::clamp(dt, 0.0f, 0.1f);
    const float weather = std::clamp(1.0f - std::max(f.rain, f.storm), 0.0f, 1.0f);

    // --- Loops -------------------------------------------------------------------
    // Trees about the listener: the rustle is only as loud as there is canopy.
    float canopy = 0.0f;
    if (f.trees)
        for (std::size_t i = 0; i + 5 <= f.trees->size(); i += 5) {
            const float d = glm::length(glm::vec2((*f.trees)[i] - f.eye.x,
                                                  (*f.trees)[i + 2] - f.eye.z));
            if (d < 40.0f) canopy += 1.0f - d / 40.0f;
        }
    canopy = std::clamp(canopy / 12.0f, 0.0f, 1.0f);
    const float warm   = std::exp(-std::pow((f.hour - 14.0f) / 4.5f, 2.0f));
    const float night  = 1.0f - std::clamp(f.daylight * 1.6f, 0.0f, 1.0f);
    const float wantLeaves = active ? f.gain * std::clamp(f.wind * f.gust * 1.1f, 0.0f, 1.2f)
                                      * (0.25f + 0.75f * canopy) * 0.55f : 0.0f;
    const float wantMeadow = active ? f.gain * warm * f.daylight * (1.0f - 0.8f * canopy)
                                      * weather * 0.22f : 0.0f;
    const float wantCrick  = active ? f.gain * night * weather * 0.28f : 0.0f;
    const float k = std::min(1.0f, dt * 1.5f);   // levels ease, they never jump
    m_leavesVol  += (wantLeaves - m_leavesVol) * k;
    m_meadowVol  += (wantMeadow - m_meadowVol) * k;
    m_cricketVol += (wantCrick - m_cricketVol) * k;
    m_leaves.setVolume(m_leavesVol);
    m_leaves.setPitch(0.9f + 0.2f * std::clamp(f.gust, 0.0f, 1.5f));
    m_meadow.setVolume(m_meadowVol);
    m_crickets.setVolume(m_cricketVol);
    if (!active) return;

    // --- Birds -------------------------------------------------------------------
    float total = 0.0f;
    std::vector<float> w(m_species.size());
    for (std::size_t s = 0; s < m_species.size(); ++s) {
        const Species& sp = m_species[s];
        w[s] = sp.weight * timeFactor(f.hour, sp.dawn, sp.day, sp.dusk);
        total += w[s];
    }
    // How many singers the hour supports: the dawn chorus fills every perch,
    // the midday lull keeps two, the night none.
    const float chorus = std::clamp(total / 8.0f, 0.0f, 1.0f) * weather;
    const int   wantSingers = static_cast<int>(std::round(chorus * m_singers.size()));
    for (std::size_t i = 0; i < m_singers.size(); ++i) {
        Singer& s = m_singers[i];
        s.stay -= dt;
        s.next -= dt;
        const bool keep = static_cast<int>(i) < wantSingers;
        if (s.species >= 0 && (s.stay <= 0.0f || !keep ||
                               glm::length(glm::vec2(s.pos.x - f.eye.x, s.pos.z - f.eye.z)) > 180.0f))
            s.species = -1;                              // flew off
        if (s.species < 0 && keep && total > 0.0f) {
            float r = uni() * total;
            int pick = 0;
            for (std::size_t k2 = 0; k2 < w.size(); ++k2) { r -= w[k2]; if (r <= 0.0f) { pick = static_cast<int>(k2); break; } }
            s.species = pick;
            s.pos     = perch(f, m_species[pick].far);
            s.stay    = 40.0f + uni() * 90.0f;
            s.next    = uni() * 4.0f;
            s.pitch   = 0.93f + 0.14f * uni();
        }
        if (s.species < 0 || s.next > 0.0f) continue;
        const Species& sp = m_species[static_cast<std::size_t>(s.species)];
        const int file = sp.files[static_cast<std::size_t>(uni() * sp.files.size()) % sp.files.size()];
        sing(file, s.pos, f.gain * (0.55f + 0.45f * uni()) * (sp.far ? 1.4f : 0.8f), s.pitch);
        s.next = sp.gapMin + (sp.gapMax - sp.gapMin) * uni();
    }
}
