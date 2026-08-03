#include "GliderAudio.hpp"

#include <algorithm>
#include <cmath>

void GliderAudio::load(fitzel::Audio& audio, const std::string& soundDir) {
    if (loaded_) return;
    audio_  = &audio;
    whine_  = fitzel::Sound::fromFile(audio, soundDir + "/jet_whine.wav", true);
    thrust_ = fitzel::Sound::fromFile(audio, soundDir + "/jet_thrust.wav", true);
    loaded_ = true;
}

void GliderAudio::start() {
    if (!loaded_ || running_) return;
    spool_ = 0.0f;
    // Both layers loop continuously; only volume/pitch move (no restart clicks).
    // Start silent -- the first update sets real levels.
    if (whine_.isValid())  { whine_.setVolume(0.0f);  whine_.play();  }
    if (thrust_.isValid()) { thrust_.setVolume(0.0f); thrust_.play(); }
    running_ = true;
}

void GliderAudio::stop() {
    if (!running_) return;
    if (whine_.isValid())  whine_.stop();
    if (thrust_.isValid()) thrust_.stop();
    running_ = false;
}

void GliderAudio::update(float dt, float speedMps, float topSpeed, float throttle,
                         float masterGain) {
    if (!running_) return;
    dt       = std::clamp(dt, 0.0f, 0.1f);
    throttle = std::clamp(throttle, 0.0f, 1.0f);
    const float speedNorm = std::clamp(speedMps / std::max(topSpeed, 1.0f), 0.0f, 1.0f);

    // Turbine spool: the "power" chases the throttle with lag, so stabbing the
    // thrust winds the engine up (and easing off winds it down) instead of the
    // volume snapping -- this lag is most of the perceived dynamics. Spooling up
    // is a touch quicker than spooling down (like a real jet).
    const float targetPower = 0.18f + 0.82f * throttle;
    const float rate = (targetPower > spool_) ? 3.2f : 1.8f;
    spool_ += (targetPower - spool_) * std::min(1.0f, dt * rate);
    spool_  = std::clamp(spool_, 0.0f, 1.0f);

    // Whine: bright turbine tone, pitch rises mostly with airspeed (with a little
    // throttle push); always audible so a hovering craft still hums.
    const float whinePitch = std::clamp(0.85f + 0.80f * speedNorm + 0.12f * spool_,
                                        0.7f, 1.9f);
    const float whineVol = masterGain * (0.20f + 0.45f * speedNorm) *
                           (0.6f + 0.4f * spool_);
    if (whine_.isValid()) {
        whine_.setPitch(whinePitch);
        whine_.setVolume(std::clamp(whineVol, 0.0f, masterGain));
    }

    // Thrust: broadband roar, driven by throttle spool and airspeed; pitch bends
    // up more gently so it reads as "more air", not "faster tape".
    const float thrustPitch = std::clamp(0.70f + 0.55f * speedNorm + 0.10f * spool_,
                                         0.6f, 1.6f);
    const float thrustVol = masterGain * (0.10f + 0.85f * speedNorm) *
                            (0.45f + 0.6f * spool_);
    if (thrust_.isValid()) {
        thrust_.setPitch(thrustPitch);
        thrust_.setVolume(std::clamp(thrustVol, 0.0f, masterGain * 1.1f));
    }
}
