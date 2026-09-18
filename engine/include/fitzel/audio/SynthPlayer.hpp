#pragma once

#include <memory>
#include <string>

#include "fitzel/audio/Midi.hpp"
#include "fitzel/audio/Synth.hpp"

namespace fitzel {

class Audio;

// A patch the game plays: several voices of it, notes from a script or from a
// MIDI file, and one voice in the mixer -- so it is positioned, attenuated and
// Doppler-shifted like any other sound in the world.
//
// Everything here may be called from the game thread while the audio thread
// renders. Notes go through a small lock-free queue and are applied at the start
// of the next block; dials are atomics; a new MIDI file is handed across by
// pointer and the old one is freed back on the game thread, never in the audio
// callback. Nothing the mixer runs allocates, locks or waits.
//
// A patch is written for one voice with "pitch", "gate" and optionally
// "velocity" dials (see synth::Poly); a patch without a gate is a drone that
// simply sounds, and its other dials are what the game turns.
//
// Move-only. The engine voice is unhooked when the player goes -- including
// when another one is assigned over it (see ~Impl).
class SynthPlayer {
public:
    SynthPlayer();
    ~SynthPlayer();
    SynthPlayer(const SynthPlayer&)            = delete;
    SynthPlayer& operator=(const SynthPlayer&) = delete;
    SynthPlayer(SynthPlayer&&) noexcept;
    SynthPlayer& operator=(SynthPlayer&&) noexcept;

    // `voices` notes at once (1 for a sound effect, 8-16 for music).
    static SynthPlayer create(Audio& audio, const synth::Patch& patch, int voices,
                              std::string* error = nullptr);

    bool isValid() const;
    // The mixer voice: running means it is rendering (and a drone is sounding).
    void start();
    void stop();
    bool isRunning() const;
    void setVolume(float volume);

    // --- Notes (any thread) -----------------------------------------------
    void noteOn(int note, float velocity = 0.8f);
    void noteOff(int note);
    void allNotesOff();

    // --- Dials (any thread) ------------------------------------------------
    int  inputIndex(const std::string& name) const;
    void setInput(int index, float value);
    void setInput(const std::string& name, float value);

    // --- A song ------------------------------------------------------------
    // Replaces the current song (and stops it). Through fitzel::vfs.
    bool loadMidi(const std::string& path, std::string* error = nullptr);
    void setMidi(synth::MidiSequence song);
    void playMidi(bool fromStart = true);   // starts the voice too if needed
    void stopMidi();
    bool midiPlaying() const;               // as of the last audio block
    double midiLength() const;              // seconds, 0 without a song
    void setMidiLoop(bool loop);
    void setTempo(float scale);             // 1 = as written
    void setChannel(int channel);           // 0..15, or -1 for all
    void setSkipDrums(bool skip);           // channel 10; see synth::Poly

    // --- In the world (same meaning as Sound's) ---------------------------
    void setSpatial(bool on);
    void setPosition(float x, float y, float z);
    void setVelocity(float x, float y, float z);
    void setAttenuation(float minDist, float maxDist, float rolloff);
    void setDopplerFactor(float factor);

    // New numbers for the patch while it plays, see synth::Graph::updateParams.
    bool updateParams(const synth::Patch& patch);
    // The last samples it produced, for a meter or a scope.
    int  peek(float* out, int maxFrames) const;

    // Public only so miniaudio's C callbacks can reach it; defined in the .cpp.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel
