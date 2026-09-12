#pragma once

#include <functional>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/audio/Audio.hpp>

// --- What the landscape sounds like -----------------------------------------------
//
// A valley that looks alive and is silent reads as a model railway. Real
// countryside is a layered sound: birds singing from their perches -- each bird
// the same few phrases from the same tree, again and again, which is what makes
// a wood sound inhabited rather than randomised -- a dawn chorus that dwarfs the
// midday lull, grasshoppers in the warm grass, crickets once the light goes, and
// under all of it the wind in the leaves, rising and falling with the gusts you
// can see going through the crowns.
//
// The birds are spatial one-shots (content/sounds/bird_*.wav, synthesised by
// gen_nature_sounds.py) sung by a handful of SINGERS: each settles on a nearby
// tree, sings its species' song every few seconds for a minute or two, and
// moves on. The insects and leaves are 2D loops whose levels follow the hour,
// the wind, the rain and what is around the listener -- trees for the rustle,
// open meadow for the grasshoppers.
//
// Voices are pooled and only ever re-triggered, never reloaded: re-loading a
// Sound the mixer is still reading tears its decoder away from the audio thread.
class Soundscape {
public:
    bool init(fitzel::Audio& audio, const std::string& soundDir);

    struct Frame {
        glm::vec3 eye{0.0f};
        float hour      = 12.0f;   // time of day
        float daylight  = 1.0f;    // 0 night .. 1 day
        float wind      = 0.3f;    // mean wind strength
        float gust      = 1.0f;    // the gust at the listener (Wind.hpp)
        float rain      = 0.0f;    // 0..1
        float storm     = 0.0f;    // 0..1
        float gain      = 1.0f;    // the ambient bus
        const std::vector<float>* trees = nullptr;   // 5 floats a tree: pos, yaw, scale
        std::function<float(float, float)> ground;
    };
    // Call once a frame in Play; `active` false fades everything out.
    void update(float dt, const Frame& f, bool active);
    // Phrases sung since start, and singers on a perch now (for the log).
    int phrases() const { return m_phrases; }
    int singing() const {
        int n = 0;
        for (const Singer& s : m_singers) n += s.species >= 0 ? 1 : 0;
        return n;
    }

private:
    struct Species {
        std::vector<int> files;      // indices into m_pools
        float weight = 1.0f;
        float gapMin = 4.0f, gapMax = 10.0f;   // between phrases of one singer
        float dawn = 1.0f, day = 1.0f, dusk = 1.0f;
        bool  far = false;           // sung from well away (the cuckoo)
    };
    struct Singer {
        int species = -1;
        glm::vec3 pos{0.0f};
        float next = 0.0f;           // seconds until the next phrase
        float stay = 0.0f;           // seconds left on this perch
        float pitch = 1.0f;          // this bird's own voice
    };
    struct Pool {
        std::vector<fitzel::Sound> voices;
    };

    bool sing(int file, const glm::vec3& pos, float volume, float pitch);
    glm::vec3 perch(const Frame& f, bool far);

    bool m_ok = false;
    int  m_phrases = 0;
    std::vector<Pool>    m_pools;    // one per bird file
    std::vector<Species> m_species;
    std::vector<Singer>  m_singers;
    fitzel::Sound m_leaves, m_meadow, m_crickets;
    float m_leavesVol = 0.0f, m_meadowVol = 0.0f, m_cricketVol = 0.0f;
    std::mt19937 m_rng{4242u};
    float uni() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng); }
};
