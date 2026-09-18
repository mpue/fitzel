#pragma once

#include <memory>
#include <string>
#include <vector>

#include "fitzel/audio/Synth.hpp"

namespace fitzel {

class Audio;

// A patch, playing. It is a voice of the engine like any loaded sound -- the
// same mixer, the same spatializer, the same Doppler -- except that its samples
// are computed rather than decoded.
//
// That is the whole reason it hangs off miniaudio's data-source interface
// instead of driving an output of its own: position, distance and Doppler are
// already solved for sounds that come from a file, and a synthesised engine note
// needs every one of them. It also sidesteps the oldest trap in this engine --
// a synthesised voice is never re-loaded while it plays, it just gets new
// numbers, so there is no decoder to tear out from under the audio thread.
//
// Move-only, like Sound.
class SynthVoice {
public:
    SynthVoice();
    ~SynthVoice();
    SynthVoice(const SynthVoice&)            = delete;
    SynthVoice& operator=(const SynthVoice&) = delete;
    SynthVoice(SynthVoice&&) noexcept;
    SynthVoice& operator=(SynthVoice&&) noexcept;

    // Compiles `patch` at the engine's own sample rate and hands it to the
    // mixer, stopped. `error` says why on failure (a loop, no Out, ...).
    static SynthVoice fromPatch(Audio& audio, const synth::Patch& patch,
                                std::string* error = nullptr);

    bool isValid() const;
    void play();          // from silence: the graph is reset first
    void stop();
    bool isPlaying() const;
    void setVolume(float volume);

    // --- The dials the game turns -----------------------------------------
    const std::vector<std::string>& inputNames() const;
    int   inputIndex(const std::string& name) const;
    void  setInput(int index, float value);
    void  setInput(const std::string& name, float value);
    float input(int index) const;

    // New settings for the modules already playing -- what an editor calls
    // while a knob moves. False when the patch's shape changed; build a new
    // voice for that. See synth::Graph::updateParams.
    bool updateParams(const synth::Patch& patch);

    // --- In the world (same meaning as Sound's) ---------------------------
    void setSpatial(bool on);
    void setPosition(float x, float y, float z);
    void setVelocity(float x, float y, float z);
    void setAttenuation(float minDist, float maxDist, float rolloff);
    void setDopplerFactor(float factor);

    // The last samples the voice produced, for drawing a scope. Copies out of a
    // ring the audio thread writes without locking: a frame of staleness, or a
    // torn sample at the seam, costs a wobbly pixel and nothing else -- which is
    // a far better trade than a lock in the mixer.
    int  peek(float* out, int maxFrames) const;

    // Public only because miniaudio calls back into it through plain C function
    // pointers, which cannot be friends of anything. The definition stays in
    // the .cpp, so nothing outside the engine can do more than name it.
    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel
