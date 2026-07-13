#include "CarAudio.hpp"

#include <algorithm>
#include <cmath>

namespace {

// Engine model constants. Tuned for a generic road car; the samples are the
// Land-Rover-style "lr_*" loops (idle .. full throttle).
constexpr float kIdleRpm     = 850.0f;
constexpr float kRedlineRpm  = 6800.0f;
constexpr float kUpShiftRpm  = 6000.0f;  // auto-box upshifts here
constexpr float kDownShiftRpm= 2500.0f;  // ...and downshifts here (wide hysteresis)
constexpr float kShiftTime   = 0.32f;    // clutch-dip duration (s)

// A 5-speed gearbox + final drive. Ratios fall as gears rise (taller gearing).
constexpr float kFinalDrive = 3.7f;
constexpr float kGearRatio[] = {3.6f, 2.15f, 1.5f, 1.1f, 0.85f};
constexpr int   kNumGears    = static_cast<int>(sizeof(kGearRatio) / sizeof(float));

// The RPM each sample layer was recorded at (its "sweet spot"). The blend picks
// the two layers bracketing the live RPM and crossfades between them.
constexpr float kLayerRpm[CarAudio::kLayers] =
    {kIdleRpm, 2100.0f, 3300.0f, 4500.0f, 5600.0f, kRedlineRpm};

constexpr float kTwoPi = 6.2831853f;

// Engine RPM the given gear produces at this road speed (before the idle floor).
float rpmForGear(float speedMps, float wheelRadius, int gear) {
    const float wr = std::max(wheelRadius, 0.05f);
    const float wheelRevPerSec = speedMps / (kTwoPi * wr);
    return wheelRevPerSec * kGearRatio[gear] * kFinalDrive * 60.0f;
}

} // namespace

void CarAudio::load(fitzel::Audio& audio, const std::string& soundDir) {
    if (loaded_) return;
    audio_ = &audio;
    static const char* kNames[kLayers] = {
        "lr_idle.wav", "lr_low.wav", "lr_med.wav",
        "lr_med_high.wav", "lr_high.wav", "lr_full.wav"};
    for (int i = 0; i < kLayers; ++i)
        layer_[i] = fitzel::Sound::fromFile(audio, soundDir + "/" + kNames[i], true);
    loaded_ = true;
}

void CarAudio::start() {
    if (!loaded_ || running_) return;
    rpm_        = kIdleRpm;
    gear_       = 0;
    shiftTimer_ = 0.0f;
    // Every layer loops continuously; only volume/pitch move (so there are no
    // restart clicks). Start them silent -- the first update sets real levels.
    for (auto& s : layer_)
        if (s.isValid()) { s.setVolume(0.0f); s.play(); }
    running_ = true;
}

void CarAudio::stop() {
    if (!running_) return;
    for (auto& s : layer_)
        if (s.isValid()) s.stop();
    running_ = false;
}

void CarAudio::update(float dt, float speedMps, float throttle,
                      float wheelRadius, float masterGain) {
    if (!running_) return;
    dt       = std::clamp(dt, 0.0f, 0.1f);
    speedMps = std::max(speedMps, 0.0f);
    throttle = std::clamp(throttle, 0.0f, 1.0f);

    // --- Automatic gearbox: shift on RPM thresholds (not while mid-shift) -----
    if (shiftTimer_ > 0.0f) shiftTimer_ = std::max(0.0f, shiftTimer_ - dt);
    float geared = std::max(kIdleRpm, rpmForGear(speedMps, wheelRadius, gear_));
    if (shiftTimer_ == 0.0f) {
        if (geared > kUpShiftRpm && gear_ < kNumGears - 1) {
            ++gear_;
            shiftTimer_ = kShiftTime;
        } else if (geared < kDownShiftRpm && gear_ > 0) {
            --gear_;
            shiftTimer_ = kShiftTime;
        }
        geared = std::max(kIdleRpm, rpmForGear(speedMps, wheelRadius, gear_));
    }

    // During a shift the clutch opens: the engine briefly sags toward idle and
    // the note softens -- the audible "gap" that sells an authentic change.
    const float shiftBlend = shiftTimer_ / kShiftTime;               // 1 -> 0
    const float targetRpm  = geared - 700.0f * shiftBlend;
    const float clutchDuck = 1.0f - 0.4f * shiftBlend;

    // Smooth toward the target so throttle blips and shifts glide, not snap.
    rpm_ += (targetRpm - rpm_) * std::min(1.0f, dt * 10.0f);
    rpm_  = std::clamp(rpm_, kIdleRpm * 0.9f, kRedlineRpm);

    // --- Crossfade the two layers bracketing the live RPM --------------------
    float vol[kLayers] = {0};
    if (rpm_ <= kLayerRpm[0]) {
        vol[0] = 1.0f;
    } else if (rpm_ >= kLayerRpm[kLayers - 1]) {
        vol[kLayers - 1] = 1.0f;
    } else {
        for (int i = 0; i < kLayers - 1; ++i)
            if (rpm_ >= kLayerRpm[i] && rpm_ < kLayerRpm[i + 1]) {
                const float t = (rpm_ - kLayerRpm[i]) /
                                (kLayerRpm[i + 1] - kLayerRpm[i]);
                vol[i]     = 1.0f - t;
                vol[i + 1] = t;
                break;
            }
    }

    // Slightly louder on throttle (engine load), softer coasting; idle stays
    // audible. Overall level times the clutch duck and the caller's mix gain.
    const float load = 0.68f + 0.32f * throttle;
    const float gain = masterGain * clutchDuck * load;

    for (int i = 0; i < kLayers; ++i) {
        if (!layer_[i].isValid()) continue;
        layer_[i].setVolume(vol[i] * gain);
        // Bend each layer's pitch toward the live RPM around its recorded RPM,
        // clamped so it never sounds sped-up/chipmunk or dragged-down/muddy.
        const float pitch = std::clamp(rpm_ / kLayerRpm[i], 0.72f, 1.38f);
        layer_[i].setPitch(pitch);
    }
}
