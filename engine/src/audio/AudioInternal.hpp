#pragma once

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "fitzel/audio/Audio.hpp"
#include "fitzel/audio/Mixer.hpp"

#include <memory>

// What an Audio actually is, for the handful of translation units inside the
// engine that have to reach the mixer itself -- Audio.cpp, and SynthVoice.cpp,
// which hangs a computed voice off the same engine every loaded sound plays on.
//
// Not in the public header on purpose: including miniaudio.h is 90,000 lines,
// and anything that merely makes a noise should not pay for that.
namespace fitzel {

struct Audio::Impl {
    ma_engine      engine;
    ma_sound_group sfx;        // one-shot bus (mixer "SFX" channel)
    bool           sfxOk = false;
    bool           ok    = false;
    std::unique_ptr<Mixer> mixer;   // built after the engine, torn down before it
};

// Point a voice at a strip of the desk (the master when the id is unknown, the
// device when there is no desk). Every class owning an ma_sound routes through it.
void routeToStrip(ma_sound* sound, Mixer* mixer, int strip);

} // namespace fitzel
