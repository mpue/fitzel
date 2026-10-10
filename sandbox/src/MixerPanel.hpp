#pragma once

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include <fitzel/audio/Mixer.hpp>

// The audio mixer: a desk of channel strips, aux buses and a master.
//
// Every sound in the game feeds exactly one CHANNEL (an Audio Source picks it by
// name; weather and zone loops use "Ambient", game.sound and the vehicles "SFX",
// songs and synths "Music"). A channel runs its insert effects, its fader and pan,
// and goes to the master -- and it can SEND to any aux bus, each send its own
// level, before or after the fader. An aux bus is a strip of its own (inserts,
// fader, pan), typically a reverb or a delay that several channels share.
//
// The STATE here is runtime -- the shipped player builds the same graph from the
// same scene -- and lives in this header plus MixerDesk.cpp. Only the panel that
// draws it (MixerPanel.cpp) is editor-only. The engine side is fitzel::Mixer;
// Desk::sync() makes it follow whatever the desk says, every frame.
namespace mixerui {

// Decibels are the unit a mixer is read in; the engine takes a linear gain. These
// two are the whole conversion, and -96 dB is treated as silence.
inline constexpr float kMinDb = -96.0f;

inline float toDb(float gain) {
    return gain <= 0.000016f ? kMinDb
                             : std::max(kMinDb, 20.0f * std::log10(gain));
}
inline float fromDb(float db) {
    return db <= kMinDb ? 0.0f : std::pow(10.0f, db * 0.05f);
}

using Kind = fitzel::Mixer::Kind;

// One insert effect in a strip's chain.
struct Insert {
    fitzel::mixfx::Type type   = fitzel::mixfx::Type::Eq;
    bool                bypass = false;
    std::vector<float>  values;            // one per mixfx::params(type)

    // What the engine was last told, so a frame only sends what changed.
    std::vector<float>  sent;
    bool                sentBypass = false;
};

struct Send {
    float level = 0.0f;   // linear; 0 = no send
    bool  pre   = false;  // before the fader (ignores fader, pan and mute)
};

// Meter ballistics + clip latch, advanced by the panel.
struct Meter {
    float showDb  = kMinDb;
    float holdDb  = kMinDb;
    float holdAge = 0.0f;
    bool  clipped = false;
};

struct Strip {
    int                 uid  = 0;          // the desk's own id: saved, and what sends name
    Kind                kind = Kind::Channel;
    std::string         name;
    float               level = 1.0f;      // the fader, LINEAR (1.0 = 0 dB)
    float               pan   = 0.0f;      // -1 .. 1
    bool                mute  = false;
    bool                solo  = false;     // channels only; not saved
    std::vector<Insert> inserts;
    std::map<int, Send> sends;             // aux uid -> send (channels only)

    // --- runtime ---
    int                 engine = -1;       // fitzel::Mixer strip id
    std::vector<fitzel::mixfx::Type> built;
    bool                builtValid = false;
    float               peak[2] = {0.0f, 0.0f};   // since the panel last looked
    Meter               meter[2];
};

struct Desk {
    std::vector<Strip> strips;     // channels and aux buses, in desk order
    Strip              master;
    int                nextUid  = 1;
    // Bumps whenever the name -> engine strip mapping changes (a strip comes,
    // goes, is renamed or rebuilt): whoever routes voices by name re-routes then.
    int                revision = 0;
    std::vector<int>   retired;    // engine ids to remove on the next sync

    Desk();

    // The desk a scene starts with: Ambient, SFX and Music, and a Reverb bus.
    void reset();
    // A scene from before the desk had strips: its old three faders.
    void loadLegacy(float ambient, bool ambientMute, float sfx, bool sfxMute,
                    float master, bool masterMute);

    Strip*       find(int uid);
    const Strip* find(int uid) const;
    Strip&       add(Kind kind, const std::string& name);
    void         remove(int uid);
    void         rename(int uid, const std::string& name);
    std::string  uniqueName(const std::string& base) const;

    bool  anySolo() const;
    float gainOf(const Strip& s) const;    // fader with mute and solo folded in

    std::vector<std::string> channelNames() const;
    // The engine strip for a channel by name; `fallback` when there is no such
    // channel, the master when neither exists.
    int route(const std::string& name, const std::string& fallback = "Ambient") const;

    // The engine follows the desk: strips created and removed, faders, pans,
    // sends and insert chains pushed, meters collected. Every frame.
    void sync(fitzel::Mixer& mixer);

    nlohmann::json toJson() const;
    void           fromJson(const nlohmann::json& j);
};

// The channel names an Audio Source can pick in the Inspector. Kept by main
// (refreshed whenever the desk changes), read by the property's dropdown.
inline std::vector<std::string>& channelList() {
    static std::vector<std::string> names{"Ambient", "SFX", "Music"};
    return names;
}

// What the panel touches in main.
struct PanelState {
    bool& show;          // the window's own open flag
    Desk& desk;

    float dt      = 0.0f;   // seconds since the last frame, for the ballistics
    bool  audioOk = true;   // false when no output device came up
    bool  playing = false;  // the editor stays silent; the meters say so
};

void drawPanel(const PanelState& s);
// Open an insert's parameters under the desk (strip by desk uid, slot 0..).
void selectInsert(int stripUid, int slot);

} // namespace mixerui