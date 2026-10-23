#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace fitzel {

class Audio;

// Plays samples held in memory -- the wave editor's preview: what is being
// edited, as it is now, without saving it first. A voice of the engine like any
// Sound (miniaudio resamples it to the device and converts the channels), fed
// from a buffer the editor hands over instead of a decoder.
//
// It plays [from, to) once, or -- with a loop set -- runs on into the loop and
// round it until stopped, the way a looping Sound plays a WAV with loop points:
// what is heard here is what the game will do. Starting and stopping fade over
// a few milliseconds, so neither clicks wherever the waveform happens to be.
//
// Every call is from the game thread; the audio thread picks the changes up at
// its next block. A new buffer is handed across by pointer and the old one is
// freed here, never in the audio callback -- nothing the mixer runs allocates,
// locks or waits. Move-only.
class SamplePlayer {
public:
    SamplePlayer();
    ~SamplePlayer();
    SamplePlayer(const SamplePlayer&)            = delete;
    SamplePlayer& operator=(const SamplePlayer&) = delete;
    SamplePlayer(SamplePlayer&&) noexcept;
    SamplePlayer& operator=(SamplePlayer&&) noexcept;

    // A player for interleaved float samples of this shape, stopped. Invalid
    // when the engine has no device.
    static SamplePlayer create(Audio& audio, int channels, int sampleRate);

    bool isValid() const;
    int  channels() const;
    int  sampleRate() const;

    // What to play (copied). Takes effect at the next audio block; a playing
    // preview carries on at the same frame (clamped to the new length).
    void setSamples(const std::vector<float>& interleaved);

    void play(std::int64_t from, std::int64_t to = -1);   // to -1: the end
    void stop();                                           // faded
    bool isPlaying() const;
    std::int64_t position() const;                         // frame being played

    // Loop [start, end): reaching `end` goes back to `start`. Off or empty: none.
    void setLoop(bool on, std::int64_t start, std::int64_t end);
    void setVolume(float linear);
    // Playback speed (and pitch with it): 1 = as recorded.
    void setRate(float rate);

    // Peak per side since the last call (linear); mono reports the same twice.
    void takePeak(float& left, float& right);

    struct Impl;   // public for miniaudio's C callbacks; defined in the .cpp

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel
