#pragma once

#include <memory>
#include <string>

#include "fitzel/audio/SongEngine.hpp"

namespace fitzel {

class Audio;
class Mixer;

// A song engine (SongEngine.hpp) as a voice in the mixer: the song sequencer's
// preview in the editor, a Song Player component in the game.
//
// The voice is stereo and renders for as long as it runs -- stopped transport
// included, so a released chord rings out and a key pressed in the piano roll
// sounds. stop() takes it out of the mixer; a song stopped for good should be,
// a VST3 instrument costs its CPU whether it is playing or not.
//
// Move-only. The engine voice is unhooked before the engine goes (see ~Impl).
class SongPlayer {
public:
    SongPlayer();
    ~SongPlayer();
    SongPlayer(const SongPlayer&)            = delete;
    SongPlayer& operator=(const SongPlayer&) = delete;
    SongPlayer(SongPlayer&&) noexcept;
    SongPlayer& operator=(SongPlayer&&) noexcept;

    static SongPlayer create(Audio& audio, std::string* error = nullptr);
    bool isValid() const;

    // The song, the transport, the instruments. Only while isValid().
    song::Engine&       engine();
    const song::Engine& engine() const;

    // Which strip of the desk the voice feeds (see Mixer.hpp).
    void setOutput(Mixer& mixer, int strip);
    void start();
    void stop();
    bool isRunning() const;
    void setVolume(float volume);

    // Where the song is in ticks as it plays: the engine's position, moved on
    // smoothly between audio blocks (blocks come in bursts; a playhead drawn
    // from them alone would stutter).
    double ticks() const;

    // --- In the world (same meaning as Sound's) ---------------------------
    void setSpatial(bool on);
    void setPosition(float x, float y, float z);
    void setAttenuation(float minDist, float maxDist, float rolloff);

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel
