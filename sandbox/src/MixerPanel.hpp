#pragma once

#include <algorithm>
#include <cmath>

// The audio mixer: a small desk with two buses and a master.
//
// The STATE here is runtime -- the gains are read in the frame loop, in the
// editor and in the shipped player alike. Only the panel that draws it
// (MixerPanel.cpp) is editor-only.
//
// What the buses mean, because the names alone do not say it: Ambient scales the
// looping weather and zone voices, SFX the one-shot bus and the vehicle voices,
// Master the device itself. The routing lives in main.cpp; this file owns the
// numbers it routes with, so "what is this fader worth right now" has exactly
// one answer (busGain) instead of one per call site.
namespace mixerui {

// Decibels are the unit a mixer is read in; miniaudio takes a linear gain. These
// two are the whole conversion, and -96 dB is treated as silence (below the last
// step of 16-bit audio, so nothing is lost by calling it off).
inline constexpr float kMinDb = -96.0f;

inline float toDb(float gain) {
    return gain <= 0.000016f ? kMinDb
                             : std::max(kMinDb, 20.0f * std::log10(gain));
}
inline float fromDb(float db) {
    return db <= kMinDb ? 0.0f : std::pow(10.0f, db * 0.05f);
}

// One channel strip.
struct Channel {
    float level = 1.0f;   // the fader, as a LINEAR gain (1.0 = unity = 0 dB)
    bool  mute  = false;
    bool  solo  = false;

    // --- What the bus is being asked to play, for the meter -------------------
    // `ask` is set by the caller every frame: the loudest voice the mixer is
    // currently commanding on this bus, after its own fader. `hit` pulses it for
    // a one-shot, which has no voice to read a level off afterwards.
    //
    // This is a meter of what the desk ASKS for, not a measurement of the signal
    // coming back out of the device -- there is no tap on the output to measure
    // (see the note in MixerPanel.cpp). It moves with the rain fading in and the
    // engine coming up, which is what the meter is looked at for.
    float ask  = 0.0f;
    float ping = 0.0f;
    void  hit(float gain) { ping = std::max(ping, gain); }

    // Meter ballistics + clip latch, owned and advanced by the panel.
    float showDb  = kMinDb;
    float holdDb  = kMinDb;
    float holdAge = 0.0f;
    bool  clipped = false;
};

struct Desk {
    Channel ambient;
    Channel sfx;
    Channel master;      // master.solo is unused

    Desk() { master.level = 0.8f; }

    // Solo is exclusive by subtraction: while anything is soloed, everything
    // that is not is silent. One channel soloed is the normal case; both soloed
    // is the same as neither, which is what a desk does too.
    bool anySolo() const { return ambient.solo || sfx.solo; }

    float busGain(const Channel& c) const {
        if (c.mute) return 0.0f;
        if (anySolo() && !c.solo) return 0.0f;
        return c.level;
    }
    float ambientGain() const { return busGain(ambient); }
    float sfxGain()     const { return busGain(sfx); }
    float masterGain()  const { return master.mute ? 0.0f : master.level; }
};

// What the panel touches in main. References rather than a back-pointer, the
// same shape weatherui::PanelState uses.
struct PanelState {
    bool& show;          // the window's own open flag
    Desk& desk;

    float dt      = 0.0f;   // seconds since the last frame, for the ballistics
    bool  audioOk = true;   // false when no output device came up
    bool  playing = false;  // the editor stays silent; the meters say so
};

void drawPanel(const PanelState& s);

} // namespace mixerui
