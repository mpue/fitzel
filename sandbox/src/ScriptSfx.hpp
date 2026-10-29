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
    // `strip` is the mixer strip it plays into (see Mixer.hpp).
    void play(const std::string& path, float volume, float pitch, const glm::vec3* pos,
              float nearM, float farM, int strip);
    // game.loop: a looping voice a script holds by a name of its own (an engine
    // hum, the wind, a rolling ball). Every call sets its volume, pitch and --
    // with `pos` -- where it is heard from, so a script calls it each frame
    // with the level it wants; a new `path` under the same key swaps the sound.
    void loop(const std::string& key, const std::string& path, float volume, float pitch,
              const glm::vec3* pos, float nearM, float farM, int strip);
    void stopLoop(const std::string& key);
    // Stop and free every voice, loops too (Play stopped).
    void clear();

private:
    struct Voice {
        fitzel::Sound sound;
        unsigned long long started = 0;
        int strip = -2;   // where it is routed now (-2: not yet)
    };
    struct Loop {
        std::string   path;
        fitzel::Sound sound;
        int strip = -2;
    };
    // Volume, pitch and place of a voice, as play() and loop() both set them.
    static void place(fitzel::Sound& s, float volume, float pitch, const glm::vec3* pos,
                      float nearM, float farM);
    fitzel::Audio&                                      m_audio;
    std::unordered_map<std::string, std::vector<Voice>> m_voices;
    std::unordered_map<std::string, Loop>               m_loops;
    unsigned long long                                  m_count = 0;
};
