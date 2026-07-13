#pragma once

#include <array>
#include <string>

#include <fitzel/audio/Audio.hpp>

// --- Engine sound system (RPM-layered loops + automatic gearbox) --------------
// Six looping engine samples recorded at rising RPM (idle .. full) are all kept
// playing at once; each frame their volumes crossfade and their pitch bends so
// the blend tracks a single simulated engine RPM. That RPM comes from a small
// automatic transmission model driven by road speed + throttle: it upshifts /
// downshifts at RPM thresholds, and the gear change drops the RPM (and briefly
// dips the sound) exactly like a real auto box -- an audible, authentic shift.
//
// Only the automatic mode exists for now (the task's first step). Load once,
// call start() when a car starts being driven, update() every frame, stop()
// when driving ends. Missing sample files just leave that layer silent.
class CarAudio {
public:
    static constexpr int kLayers = 6; // idle, low, med, med_high, high, full

    CarAudio() = default;

    // Load the six engine layers from `soundDir` (expects lr_idle/low/med/
    // med_high/high/full.wav). Safe to call once; re-loading is a no-op.
    void load(fitzel::Audio& audio, const std::string& soundDir);
    bool loaded() const { return loaded_; }

    // Begin / end the looping engine voices. start() resets the gearbox to
    // idle in first gear; stop() silences every layer. Both are idempotent.
    void start();
    void stop();
    bool running() const { return running_; }

    // Per-frame tick. `speedMps` is the car's road speed (>= 0), `throttle` the
    // accelerator load 0..1, `wheelRadius` feeds the RPM model, `masterGain`
    // is the final mix multiplier (0 = silent). No-op unless running.
    void update(float dt, float speedMps, float throttle,
                float wheelRadius, float masterGain);

    // Read-outs (for a HUD / debugging).
    float rpm() const { return rpm_; }
    int   gear() const { return gear_; }      // 0-based (0 = first gear)

private:
    fitzel::Audio*                     audio_ = nullptr;
    std::array<fitzel::Sound, kLayers> layer_;
    bool  loaded_  = false;
    bool  running_ = false;

    // Gearbox / engine state.
    float rpm_        = 0.0f;   // smoothed engine RPM (what the sound tracks)
    int   gear_       = 0;      // current gear index
    float shiftTimer_ = 0.0f;   // remaining clutch-dip time after a shift (s)
};
