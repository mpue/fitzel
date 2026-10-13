#pragma once

#include <string>
#include <vector>

namespace fitzel {

// A sound file boiled down to what a timeline draws of it: how loud it is,
// bucket by bucket.
//
// Two numbers per bucket because a waveform drawn from peaks alone is a solid
// slab for anything mastered loud -- every bucket of a song touches full scale
// somewhere. The RMS under it is what still shows the verse from the chorus, so
// a strip draws the peak as the outline and the RMS as the body, the way every
// DAW does.
//
// Mono on purpose: a strip is a few dozen pixels tall and two channels drawn in
// half that each would show nothing either of them says.
struct Waveform {
    float              seconds = 0.0f;   // the file's length
    float              rate    = 100.0f; // buckets per second
    std::vector<float> peak;             // 0..1, max |sample| in the bucket
    std::vector<float> rms;              // 0..1, root mean square in the bucket

    bool empty() const { return peak.empty(); }
    // The bucket under `t` seconds, or 0 outside the file.
    float peakAt(float t) const;
    float rmsAt(float t) const;
    // The loudest peak between two times -- what one pixel column shows when it
    // covers more than one bucket (zoomed far out). Without it a zoomed-out
    // waveform aliases: every column samples one bucket and skips the hit
    // between them.
    float peakOver(float t0, float t1) const;
    float rmsOver(float t0, float t1) const;
};

// Decode `path` (through the VFS, so an exported game's archive works) and
// measure it at `bucketsPerSecond`. Slow for a long file -- a whole decode --
// so call it off the main thread. False when the file cannot be read or decoded.
bool readWaveform(const std::string& path, Waveform& out, float bucketsPerSecond = 100.0f);

// Just the length, in seconds (0 when unknown). Cheaper than a waveform for
// formats whose length is in the header; an mp3 is scanned.
float soundFileSeconds(const std::string& path);

} // namespace fitzel
