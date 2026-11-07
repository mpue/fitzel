#pragma once

#include <functional>
#include <memory>
#include <string>

#include "fitzel/audio/SongEngine.hpp"

namespace fitzel {

class Audio;
class Mixer;

// A song engine (SongEngine.hpp) in the mixer: the song sequencer's preview in
// the editor, a Song Player component in the game.
//
// ONE NODE, MANY OUTPUTS. The player is a node of miniaudio's graph with an
// output per engine output: [0] the song's own mix, then one per mixer channel
// its tracks name. All of them are rendered by the same call, the way a desk
// channel's sends are -- miniaudio processes a node with several outputs once
// per step and hands each output to whatever reads it -- so a track in its own
// channel stays on the sample with the rest of the song.
//
// The node renders for as long as it runs -- stopped transport included, so a
// released chord rings out and a key pressed in the piano roll sounds. stop()
// takes it out; a song stopped for good should be, a VST3 instrument costs its
// CPU whether it is playing or not.
//
// Move-only. The node leaves the graph before the engine goes (see ~Impl).
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

    // Where each output goes: the song's own mix into `songStrip`, an output
    // that carries a mixer channel into `stripOf(name)` (the desk's route --
    // that channel, else the song's). Cheap to call every frame: only what
    // changed is wired again.
    void route(Mixer& mixer, int songStrip,
               const std::function<int(const std::string& channel)>& stripOf = {});
    // Everything into one strip.
    void setOutput(Mixer& mixer, int strip) { route(mixer, strip); }
    void start();
    void stop();
    bool isRunning() const;
    void setVolume(float volume);   // on every output

    // Where the song is in ticks as it plays: the engine's position, moved on
    // smoothly between audio blocks (blocks come in bursts; a playhead drawn
    // from them alone would stutter).
    double ticks() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel
