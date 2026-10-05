#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/audio/Audio.hpp>

// game.sound: one-shots a script fires with a volume, a pitch and -- for a game
// seen from above, where a cannon on the far side of the map must not sound
// like one at your feet -- a place in the world.
//
// Audio::playOneShot can do none of that (it has no handle to move), so this
// keeps a small pool of voices per file and re-plays them: the load-once,
// replay-forever pattern every sound in the sandbox follows, because loading a
// file per shot is a disk read per shot. When every voice of a file is busy
// the oldest is cut off; a battle with forty guns sounds like forty guns at
// MAX voices, and nobody counts.
class ScriptSfx {
public:
    static constexpr int kVoicesPerFile = 6;

    explicit ScriptSfx(fitzel::Audio& audio) : m_audio(audio) {}

    // `path` is a resolved file. `pos` null = a plain 2D sound (interface
    // clicks, announcements); otherwise heard from where it is, full volume
    // within `nearM`, silent past `farM`.
    void play(const std::string& path, float volume, float pitch, const glm::vec3* pos,
              float nearM, float farM);
    // Stop and free every voice (Play stopped).
    void clear();

private:
    struct Voice {
        fitzel::Sound sound;
        unsigned long long started = 0;
    };
    fitzel::Audio&                                      m_audio;
    std::unordered_map<std::string, std::vector<Voice>> m_voices;
    unsigned long long                                  m_count = 0;
};
