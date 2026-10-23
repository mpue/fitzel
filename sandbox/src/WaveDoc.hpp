#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <fitzel/audio/SampleData.hpp>

// The wave editor's document: a sound as samples (fitzel::SampleData -- every
// channel, its rate, its loop points), the operations that change it and the
// questions asked of it. No ImGui, no audio device: WaveEditPanel is the window
// and SamplePlayer the ears, and wavecheck runs all of this headless.
//
// Ranges are frames [a, b), clamped to the sound. Operations that do not change
// the length take a channel mask (bit 0 = left/mono, bit 1 = right), so a
// stereo file can be faded or silenced on one side. Every operation that
// changes the length keeps the loop points on the material they marked: a cut
// before the loop moves it, a cut through it shrinks it, a cut of all of it
// takes it away.
namespace wave {

using Frames = std::int64_t;
constexpr unsigned kAllChannels = ~0u;

enum class FadeCurve { Linear, EqualPower, Exponential, Logarithmic, SCurve, Count };
const char* fadeCurveName(FadeCurve c);

// A piece of sound on the editor's clipboard.
struct Clip {
    int                channels = 0;
    int                rate     = 0;
    std::vector<float> samples;
    Frames frames() const { return channels > 0 ? static_cast<Frames>(samples.size()) / channels : 0; }
};

// How loud a range is, per channel (dBFS; -inf as -200).
struct Stats {
    float peakDb[2] = {-200.0f, -200.0f};
    float rmsDb[2]  = {-200.0f, -200.0f};
    float dc[2]     = {0.0f, 0.0f};       // mean, -1..1
    int   clipped   = 0;                  // samples at or past full scale
};

// The drawing pyramid: min, max and mean square per block of 2^k frames for
// k = 6, 10, 14 -- so a column of a zoomed-out view reads a handful of numbers
// rather than a hundred thousand samples.
struct Lod {
    int shift = 0;                                   // block = 1 << shift frames
    std::vector<std::vector<float>> mn, mx, ms;      // [channel][block]
};

class Document {
public:
    fitzel::SampleData data;
    std::string        path;        // "" until saved
    bool               dirty = false;

    bool load(const std::string& file, std::string* error);
    void newSilence(int channels, int rate, double seconds);
    bool save(const std::string& file, fitzel::WavFormat fmt, std::string* error);

    Frames frames() const { return data.frames(); }
    int    channels() const { return data.channels; }
    int    rate() const { return data.sampleRate; }
    float  sample(Frames f, int ch) const;          // 0 outside
    std::string name() const;                       // file name, or "Untitled"

    // --- Undo ---------------------------------------------------------------
    // Call BEFORE an operation: the state as it is now becomes one step back.
    void checkpoint(const char* label);
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    const std::string& undoLabel() const;
    const std::string& redoLabel() const;
    bool undo();
    bool redo();

    // Bumped by every change; the panel re-reads the pyramid and the player's
    // copy when it moves.
    std::uint64_t version() const { return m_version; }
    void          touched();                        // after changing `data` by hand
    const std::vector<Lod>& lods() const { return m_lods; }

private:
    struct Step {
        fitzel::SampleData data;
        std::string        label;
    };
    std::deque<Step>   m_undo, m_redo;
    std::vector<Lod>   m_lods;
    std::uint64_t      m_version = 1;
    void rebuildLods();
};

// --- Operations (one undo step each when called through the panel) ---------
Clip copy(const Document& d, Frames a, Frames b);
void erase(Document& d, Frames a, Frames b);
// `c` converted to the document's channels and rate first.
void insert(Document& d, Frames at, const Clip& c);
void crop(Document& d, Frames a, Frames b);
void insertSilence(Document& d, Frames at, Frames n);
void silence(Document& d, Frames a, Frames b, unsigned mask);
void gain(Document& d, Frames a, Frames b, float db, unsigned mask);
// Peak to `targetDb`; returns the gain applied (dB).
float normalize(Document& d, Frames a, Frames b, float targetDb, unsigned mask);
void fade(Document& d, Frames a, Frames b, bool in, FadeCurve curve, unsigned mask);
void reverse(Document& d, Frames a, Frames b, unsigned mask);
void invert(Document& d, Frames a, Frames b, unsigned mask);
void removeDc(Document& d, Frames a, Frames b, unsigned mask);
void toMono(Document& d);
void toStereo(Document& d);
void resample(Document& d, int newRate);
// Drop leading and trailing material quieter than `thresholdDb`; returns the
// frames removed at the start.
Frames trimSilence(Document& d, float thresholdDb);

// --- Loops --------------------------------------------------------------------
// The nearest zero crossing to `f` (mixed channels, rising or falling), within
// `reach` frames; `f` itself when there is none.
Frames nearestZeroCrossing(const Document& d, Frames f, Frames reach = 4096);
// Where to END a loop that starts at `start` so the wrap is least audible: the
// candidate in [from, to) whose next `window` frames best match the `window`
// frames after `start` (normalised cross-correlation, weighted by how alike the
// two are in level). Returns -1 when the range is empty; `score` gets 0..1.
Frames bestLoopEnd(const Document& d, Frames start, Frames from, Frames to, Frames window,
                   float* score = nullptr);
// Blend the `len` frames before the loop end towards the `len` frames before
// the loop start (equal power), so running off the end into the start is no
// seam at all. Needs a loop with `len` frames in front of its start; false
// otherwise.
bool crossfadeLoop(Document& d, Frames len);

// --- Analysis -------------------------------------------------------------------
Stats stats(const Document& d, Frames a, Frames b);
// Magnitude spectrum (dB, `bins` log-spaced bands from 20 Hz to Nyquist) of a
// range, averaged over Hann windows of 2048 frames.
std::vector<float> spectrum(const Document& d, Frames a, Frames b, int bins);

float toDb(float linear);
float fromDb(float db);

} // namespace wave
