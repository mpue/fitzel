#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/audio/Audio.hpp>

struct Entity;
namespace city { struct District; }

// The world's own noises: the things that are somewhere ELSE than the player.
//
// Two effects, one reason to share a module. The rivals get engine voices, which
// is what makes an overtake audible -- the Doppler shift comes out of the
// spatializer for free once a source has a position and a velocity, and it is
// the relative velocity that shifts, so a rival held alongside is not shifted at
// all and one being reeled in drops in pitch as it goes past. And a close pass
// at speed fires a swoosh: a tower going by at two hundred, a rival's wake. Both
// need the same three things -- a listener, a pool of positioned voices, and a
// per-object memory of where the thing was last frame -- so they live together
// rather than solving that three times.
//
// Everything here is a VOICE POOL and never a load: a sample is loaded once per
// voice at startup and afterwards only moved, re-pitched and restarted. Loading
// into a Sound that is still being mixed pulls its decoder out from under the
// audio thread, which is a crash, not a glitch.
//
// The player's own craft is deliberately NOT in here. Its engine is not in the
// world, it is on the player's head (see GliderAudio), and spatializing it would
// make the one sound that must never wander do exactly that.
class WorldAudio {
public:
    // --- Tuning, all public so a scene or a panel can move it ----------------
    // How many rivals can be heard at once. Four is the number where a pack
    // still sounds like a pack; past that they smear into one tone and cost
    // voices for nothing.
    static constexpr int kRivalVoices = 4;
    // Concurrent swooshes. A close pass through a city street can retrigger
    // several times a second, and a pool that runs dry simply drops the quietest
    // pass, which nobody can hear anyway.
    static constexpr int kPassVoices  = 6;

    float rivalGain    = 0.55f; // rival engines, relative to the player's own
    float rivalRange   = 140.0f;// past this a rival is not worth a voice (m)
    float passGain     = 0.9f;  // swoosh level
    // Below this closing speed a pass is not a pass, it is a drive-by, and a
    // swoosh on it sounds like the wind is being blown by hand.
    float passMinSpeed = 22.0f; // m/s
    // How wide the swoosh's trigger reaches past an object's own radius. A
    // building answers with its own size; this is the margin around it.
    float passReach    = 14.0f; // m
    float doppler      = 1.0f;  // 0 = off, 1 = physical, higher = exaggerated

    // Load the samples from `soundDir`. Re-loading is a no-op. A missing file
    // leaves that layer silent rather than failing -- a project without a swoosh
    // sample should still race.
    void load(fitzel::Audio& audio, const std::string& soundDir);
    bool loaded() const { return m_loaded; }

    // Silence everything and forget where things were. Call when play stops or a
    // scene is loaded: the object ids in the memory below belong to the scene
    // that was running, and a rival's id in the next one is somebody else.
    void reset();

    // One frame. `listenerPos/Fwd/Up` place the ears (the eye that is rendering),
    // `listenerVel` is its velocity in m/s -- smoothed by the caller, because a
    // velocity divided by a stuttering frame time pitches the whole world.
    // `district` may be null (no city, no building passes).
    void update(float dt,
                const glm::vec3& listenerPos, const glm::vec3& listenerFwd,
                const glm::vec3& listenerUp, const glm::vec3& listenerVel,
                const std::vector<Entity>& entities,
                const city::District* district,
                int skipIdA, int skipIdB,   // the craft in the seats: not rivals
                float masterGain);

private:
    // One rival engine voice plus what it is currently following.
    struct RivalVoice {
        fitzel::Sound sound;
        int           entity = -1;    // whose engine this is, -1 = idle
        glm::vec3     lastPos{0.0f};
        bool          hasLast = false;
    };
    // What a tracked object was doing last frame, so a pass can be spotted at
    // the moment it happens rather than guessed at from a distance.
    struct Track {
        float side = 0.0f;   // signed distance along the listener's forward axis
        bool  seen = false;  // still in the scene this frame (else it is pruned)
    };

    // Fire a swoosh at `pos`, if a voice is free. `strength` 0..1 sets level and
    // pitch together: a faster, closer pass is louder AND brighter, which is
    // most of what makes one read as fast.
    void firePass(const glm::vec3& pos, const glm::vec3& vel, float strength);

    fitzel::Audio* m_audio = nullptr;
    RivalVoice     m_rivals[kRivalVoices];
    fitzel::Sound  m_pass[kPassVoices];
    // Keyed by entity id for rivals, and by ~(building index) for buildings, so
    // the two cannot collide in one map.
    std::unordered_map<int, Track> m_tracks;
    bool m_loaded = false;
};
