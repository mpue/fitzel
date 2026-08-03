#pragma once

#include <string>

#include <fitzel/audio/Audio.hpp>

// --- Glider jet-thruster sound -----------------------------------------------
// Two looping layers give the hover racer a dynamic turbine voice:
//   * whine  -- a bright harmonic turbine tone; airspeed bends its pitch up
//   * thrust -- a broadband roar; throttle + airspeed drive its volume
// A throttle "spool" (a slow-follow power value) makes the engine wind up and
// down with a turbine's lag rather than snapping, which is what sells the
// dynamics. Load once, start() when a glider starts being flown, update() every
// frame with its speed/throttle, stop() when flight ends. Missing sample files
// just leave a layer silent. Mirrors CarAudio's lifecycle.
class GliderAudio {
public:
    GliderAudio() = default;

    // Load jet_whine.wav / jet_thrust.wav from `soundDir`. Re-loading is a no-op.
    void load(fitzel::Audio& audio, const std::string& soundDir);
    bool loaded() const { return loaded_; }

    void start();  // begin the looping voices (idempotent)
    void stop();   // silence them (idempotent)
    bool running() const { return running_; }

    // Per-frame tick. `speedMps` is the craft's speed (>= 0), `topSpeed` its max
    // (for normalising airspeed), `throttle` the 0..1 thrust input, `masterGain`
    // the final mix multiplier (0 = silent). No-op unless running.
    void update(float dt, float speedMps, float topSpeed, float throttle,
                float masterGain);

private:
    fitzel::Audio* audio_ = nullptr;
    fitzel::Sound  whine_;
    fitzel::Sound  thrust_;
    bool  loaded_  = false;
    bool  running_ = false;
    float spool_   = 0.0f; // smoothed engine power (throttle spool-up lag)
};
