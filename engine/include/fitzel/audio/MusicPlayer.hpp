#pragma once

#include <memory>
#include <string>
#include <vector>

namespace fitzel {

class Audio;
class Mixer;

// One song, played the way a rhythm game needs it played: from any second
// (negative = that much silence first), with a clock that says where the song is
// AS HEARD, a filter the game can close and open while it plays, and a tap of
// what is sounding for meters, spectra and scopes.
//
// Why not a Sound: a Sound can only start from the top, and nothing can ask it
// where it is. Here the song is decoded on the audio thread from the file's bytes
// in memory (a four-minute song fully decoded would be ~90 MB of floats), and the
// position is counted in samples by the one thread that knows it.
//
// The clock: every audio block records which song sample it started at and when
// (steady clock). time() extrapolates from the newest block, subtracts the
// device's buffering (what was rendered is not yet what is heard), and is then
// smoothed and kept monotonic -- blocks arrive in bursts, a game frame must not
// see the song stand still and then jump.
//
// Game thread only, except for what the mixer calls internally. Move-only.
class MusicPlayer {
public:
    MusicPlayer();
    ~MusicPlayer();
    MusicPlayer(const MusicPlayer&)            = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;
    MusicPlayer(MusicPlayer&&) noexcept;
    MusicPlayer& operator=(MusicPlayer&&) noexcept;

    // The voice in the mixer, silent until a song is loaded and played.
    static MusicPlayer create(Audio& audio, std::string* error = nullptr);
    bool isValid() const;
    // Which strip of the desk the voice feeds (see Mixer.hpp).
    void setOutput(Mixer& mixer, int strip);

    // Replaces the song (stopping the old one). Through fitzel::vfs.
    bool   load(const std::string& path, std::string* error = nullptr);
    bool   loaded() const;
    double duration() const;   // seconds, 0 without a song

    void play(double fromSec = 0.0);   // negative: -fromSec of silence first
    void stop();
    void pause();
    void resume();
    bool playing() const;              // started, not paused/stopped, not run out
    bool paused() const;
    // Fade to silence over `sec`, then stop.
    void fadeOut(double sec);

    // Where the song is as it comes out of the speakers, in seconds.
    double time();

    // The shaping the game plays with: a low-pass (Hz), a gain (linear) and a
    // low shelf at 120 Hz (dB). Each glides to its new value with time constant
    // `smoothSec` (0 = at once).
    void setFilter(float cutoffHz, float gain, float shelfDb, float smoothSec = 0.12f);
    void setVolume(float volume);      // the mix's level, on top of the gain

    int sampleRate() const;
    // What is sounding now (before the filter), as an analyser would see it:
    // 512 bins of a 1024-point FFT (Blackman window, smoothed 0.3), each mapped
    // from -100..-30 dB to 0..1. Smoothing advances per call: call once a frame.
    void spectrum(std::vector<float>& out);
    // `n` samples of the last 1024, -1..1.
    void waveform(std::vector<float>& out, int n);

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

namespace music {

// The heavy half of finding the beat in a song, done once per song: decoded to
// mono at 22050 Hz, split into bass (<140 Hz), mids (1.8 kHz band) and highs
// (>7 kHz), and reduced to one number per 256 samples (~11.6 ms). For each band
// the RMS of that stretch and its "flux" -- how much the log-energy rose since
// the stretch before, normalised to a mean of 1. Everything musical (tempo,
// beats, what becomes a note) is the game's business.
struct Bands {
    double             duration  = 0.0;   // seconds
    int                rate      = 22050;
    int                hop       = 256;
    std::vector<float> flux[3];            // bass, mids, highs
    std::vector<float> rms[3];
};

bool analyzeBands(const std::string& path, Bands& out, std::string* error = nullptr);

} // namespace music

} // namespace fitzel
