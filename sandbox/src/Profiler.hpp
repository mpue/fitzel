#pragma once

#include <vector>

// Per-frame CPU timing, built for one job: finding hitches.
//
// Averages hide stutter -- a single 60 ms frame every two seconds barely moves a
// mean, but it is exactly what you feel. So everything here is oriented around
// the *worst* frame in a recent window rather than the typical one, and the
// history is kept frame by frame so a spike can be seen instead of inferred.
//
// Single-threaded by design: every zone is opened and closed on the render
// thread, so there are no locks and a zone costs two clock reads.
namespace prof {

// How many frames of history to keep -- about four seconds at 60 fps. Long
// enough that a periodic hitch falls inside it, short enough that the "worst"
// figure still reflects what is happening now rather than a stall a minute ago.
inline constexpr int kHistory = 240;

// Open a timing zone that closes at the end of the enclosing scope. `name` must
// be a string literal (or otherwise outlive the program): zones are identified
// by pointer, which is what keeps this cheap enough to leave switched on.
struct Scope {
    explicit Scope(const char* name);
    ~Scope();
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

private:
    const char* m_name;
    long long   m_start;
};

#define FZ_ZONE_CAT2(a, b) a##b
#define FZ_ZONE_CAT(a, b)  FZ_ZONE_CAT2(a, b)
// Time the enclosing scope under `name`. Wrap a block in braces to time part of
// a function: { FZ_ZONE("shadows"); ... }
#define FZ_ZONE(name) ::prof::Scope FZ_ZONE_CAT(fzZone_, __LINE__)(name)

// Record a zone measured by hand (for spans that don't fit a scope).
void add(const char* name, double ms);

// Stopwatch for spans that straddle braces, which is most of a long frame loop:
//   const long long m = prof::mark();
//   ... work ...
//   prof::addSince("shadows", m);
long long mark();
void      addSince(const char* name, long long from);

// Times one frame and closes it on destruction.
//
// Construct it *after* event polling, not before: when the editor is idling or
// capping its frame rate it sleeps in there, and counting that sleep as frame
// cost would report a comfortable 25 fps editor as a 40 ms disaster. What this
// measures is the work, which in play mode (uncapped) is also the frame period.
struct Frame {
    Frame();
    ~Frame();
    Frame(const Frame&)            = delete;
    Frame& operator=(const Frame&) = delete;

private:
    long long m_start;
};

// Close the frame: pushes `frameMs` into the history and rolls the per-zone
// windowed maxima. Called by ~Frame; only needed directly for a custom clock.
void endFrame(double frameMs);

struct ZoneStat {
    const char* name  = nullptr;
    double      last  = 0.0;  // milliseconds, most recent frame
    double      avg   = 0.0;  // smoothed
    double      worst = 0.0;  // worst single frame in the current window
};

// Zones seen so far this run, in first-seen order.
const std::vector<ZoneStat>& zones();

// Frame times in milliseconds, oldest first. Shorter than kHistory until the
// buffer has filled once.
const std::vector<float>& history();

// Summary over the history window. `worst` and `low1` are the stutter-relevant
// numbers: `low1` is the 99th-percentile frame time expressed as fps, i.e. how
// bad the worst one percent of frames feel.
struct FrameStats {
    float last   = 0.0f;  // ms
    float avg    = 0.0f;  // ms
    float worst  = 0.0f;  // ms
    float low1   = 0.0f;  // fps at the 99th-percentile frame time
    int   spikes = 0;     // frames in the window over twice the median
};
FrameStats frameStats();

// Forget the history and the windowed maxima (after a load, or on demand -- a
// level load leaves a 500 ms frame that would dominate the scale for seconds).
void reset();

} // namespace prof
