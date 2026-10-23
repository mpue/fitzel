#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace fitzel {

// A sound file as samples: every channel, at the file's own rate, as floats --
// what an editor cuts, fades and loops, and what it writes back.
//
// Loop points travel with it. In a WAV they live in the "smpl" chunk (the one
// samplers and every wave editor read), in sample frames; here the end is
// EXCLUSIVE, the way miniaudio and every range in this engine count, and the
// chunk's inclusive end is converted on the way in and out. A looping Sound
// made from a WAV that has them plays the start once and then repeats the loop
// (see Sound::fromFile) -- an engine hum with its spin-up in front.
struct SampleData {
    int                channels   = 0;
    int                sampleRate = 0;
    std::vector<float> samples;          // interleaved, frames() * channels
    std::int64_t       loopStart  = -1;  // frames; -1 = no loop
    std::int64_t       loopEnd    = -1;  // exclusive
    // What the file was (for "save as it came"): bits per sample, float or
    // integer. 0 for a compressed source (ogg, mp3, flac).
    int                sourceBits  = 0;
    bool               sourceFloat = false;

    std::int64_t frames() const {
        return channels > 0 ? static_cast<std::int64_t>(samples.size()) / channels : 0;
    }
    double seconds() const {
        return sampleRate > 0 ? static_cast<double>(frames()) / sampleRate : 0.0;
    }
    bool hasLoop() const { return loopStart >= 0 && loopEnd > loopStart; }
};

// Decode any sound the engine plays (wav, ogg, mp3, flac -- through the VFS)
// into `out`, keeping its channels and rate, and its loop points when it is a
// WAV with a smpl chunk. False, with `error` set, when it cannot be read.
bool loadSampleData(const std::string& path, SampleData& out, std::string* error = nullptr);

// The first loop of a WAV's smpl chunk, as frames [start, end). False when the
// bytes are not a WAV or carry no loop.
bool readWavLoop(const std::vector<std::uint8_t>& wav, std::int64_t& start, std::int64_t& end);
bool readWavLoop(const std::string& path, std::int64_t& start, std::int64_t& end);

enum class WavFormat { Pcm16, Pcm24, Float32 };

// Write `data` as a WAV (straight to disk, not the VFS), its loop points as a
// smpl chunk. 16-bit is dithered (triangular, one LSB) so a quiet fade does not
// turn into a staircase. False, with `error` set, when the file cannot be written.
bool saveWav(const std::string& path, const SampleData& data, WavFormat format,
             std::string* error = nullptr);

} // namespace fitzel
