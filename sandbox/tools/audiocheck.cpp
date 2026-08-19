// Spatial-audio harness: does the ENGINE put a sound in a place?
//
// The same idea as shadercheck. Spatial audio cannot be smoke-tested from inside
// the game, because "I hear nothing move" has half a dozen possible causes
// spread across three layers -- a mono output device, a listener nobody sets, a
// sound whose spatialization is off, a sample that is stereo, or game logic that
// never fires the thing in the first place. This harness removes the top three
// from the list in twenty seconds: it drives ONE sound around a stationary
// listener with the engine's own API and nothing else.
//
//   build\release\bin\audiocheck.exe [content\sounds\swoosh.wav]
//
// What you should hear, in order:
//   1. three laps around your head, left -> front -> right -> behind
//   2. a fly-past from far left to far right, close in -- the Doppler drop is
//      audible on this one and not on the laps, because a circle keeps its
//      distance and Doppler is about closing speed, not speed
//
// If both are centred and flat, the problem is below the game: check the channel
// count this prints. If they move here but not in the game, the engine is fine
// and the fault is in what the game feeds it.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

#include <fitzel/audio/Audio.hpp>

namespace {

void sleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace

int main(int argc, char** argv) {
    const std::string path =
        (argc > 1) ? argv[1] : std::string("content/sounds/swoosh.wav");

    fitzel::Audio audio;   // prints the device format itself
    if (!audio.ok()) {
        std::fprintf(stderr, "audiocheck: no audio engine\n");
        return 1;
    }

    // The listener stands still and faces +Z, which is where the game's yaw 0
    // points too. A still listener is the point: everything heard here is the
    // SOURCE moving, so there is nothing else to blame.
    audio.setListener(0, 0, 0,  0, 0, 1,  0, 1, 0,  0, 0, 0);

    fitzel::Sound s = fitzel::Sound::fromFile(audio, path, /*loop=*/true);
    if (!s.isValid()) {
        std::fprintf(stderr, "audiocheck: could not load '%s'\n", path.c_str());
        return 1;
    }
    s.setSpatial(true);
    s.setAttenuation(1.0f, 60.0f, 1.0f);
    s.setDopplerFactor(1.0f);
    s.setVolume(1.0f);
    s.play();

    std::printf("audiocheck: '%s'\n", path.c_str());

    // --- 1) Three laps around the head --------------------------------------
    std::printf("  circling (left -> front -> right -> behind)...\n");
    const float radius = 6.0f;
    for (int i = 0; i < 360; ++i) {
        const float a = static_cast<float>(i) * 3.14159265f / 60.0f; // 3 laps
        const float x = std::sin(a) * radius, z = std::cos(a) * radius;
        // Velocity is the tangent, so this is honest about how it is moving --
        // and a circle at constant radius has almost no CLOSING speed, which is
        // why this leg pans hard and barely shifts in pitch.
        const float w  = 3.14159265f / 60.0f / 0.025f;
        s.setPosition(x, 0.0f, z);
        s.setVelocity(std::cos(a) * radius * w, 0.0f, -std::sin(a) * radius * w);
        sleepMs(25);
    }

    // --- 2) A fly-past, close in --------------------------------------------
    std::printf("  fly-past (far left -> far right, 3 m away)...\n");
    const float speed = 40.0f;      // m/s
    for (int i = 0; i < 160; ++i) {
        const float t = static_cast<float>(i) * 0.025f;
        s.setPosition(-80.0f + speed * t, 0.0f, 3.0f);
        s.setVelocity(speed, 0.0f, 0.0f);
        sleepMs(25);
    }

    s.stop();
    std::printf("audiocheck: done\n");
    return 0;
}
