#pragma once

#include <memory>
#include <string>
#include <vector>

namespace fitzel {

class Audio;

// The mixing desk, as a graph in the audio engine: every sound feeds exactly one
// strip, a strip runs its insert effects, then its fader and pan, and goes on to
// the master. Channels can also send to aux buses -- each send its own level, pre
// or post the fader -- and an aux bus is a strip of its own: inserts (a reverb,
// usually), fader, master. The master is a strip too, the last one before the
// device.
//
// The DSP runs on the audio thread inside miniaudio's node graph; everything here
// is called from the game thread. Levels, pans and sends are plain atomics. The
// insert chains are swapped under a short lock that the audio thread only ever
// tries: a block that finds it held plays dry rather than waiting.
//
// Strips are named by an id this class hands out. Ids are never reused within one
// Mixer, so a stale id is simply unknown (every call ignores it) rather than
// quietly meaning another strip.
namespace mixfx {

// Plugin: a VST3 plugin (fitzel/audio/Vst3.hpp) -- its parameters are its own,
// so params(Plugin) is empty, and it is not one of the effects a menu offers.
enum class Type { Eq, Filter, Compressor, Delay, Reverb, Chorus, Drive, Plugin, Count };

struct Param {
    const char* name;
    float       min, max, def;
    const char* fmt;              // printf format for the value
    bool        log    = false;   // a frequency or a time: sweep it logarithmically
    const char* labels = nullptr; // "LP|HP|BP": a stepped choice, value = index
};

const char*               name(Type t);
const char*               key(Type t);          // stable name for files ("eq", "reverb", ...)
bool                      fromKey(const std::string& k, Type& out);
const std::vector<Param>& params(Type t);

// An insert whose DSP lives outside the mixer: a VST3 plugin. process() is called
// on the audio thread, on the strip's interleaved block, while it sits in a chain;
// the chain holds a reference, so it cannot go away in the middle of one.
struct External {
    virtual ~External() = default;
    virtual void process(float* interleaved, unsigned frames, unsigned channels) = 0;
};

// One place in an insert chain. `external` only for Type::Plugin (null: the
// slot passes the signal through, as a plugin that is not installed does).
struct Slot {
    Type                      type = Type::Eq;
    std::shared_ptr<External> external;
};

} // namespace mixfx

class Mixer {
public:
    enum class Kind { Channel, Aux, Master };
    static constexpr int kMaxAux = 16;   // send slots per channel

    explicit Mixer(Audio& audio);
    ~Mixer();
    Mixer(const Mixer&)            = delete;
    Mixer& operator=(const Mixer&) = delete;

    bool  ok() const;
    int   master() const;                // the one master strip
    float sampleRate() const;            // what the engine renders at

    // A new channel or aux bus, routed to the master. -1 when the engine is down
    // or every aux slot is taken.
    int  addStrip(Kind kind);
    // Gone, with everything attached to it: sounds that fed it go silent until
    // they are routed elsewhere (route them first). The master stays.
    void removeStrip(int id);
    bool has(int id) const;
    Kind kind(int id) const;

    void setGain(int id, float linear);  // fader, mute folded in (0 = silent)
    void setPan(int id, float pan);      // -1 left .. 1 right (equal power)
    // Channel -> aux bus. 0 removes the send. Pre-fader sends ignore the
    // channel's fader and pan (a monitor mix); post-fader ones follow them.
    void setSend(int channel, int aux, float linear, bool preFader = false);

    // The insert chain, in order. Effects whose type and position are unchanged
    // keep their state (a reverb tail does not cut off because a later slot was
    // added); everything else starts fresh with default parameters.
    void setInserts(int id, const std::vector<mixfx::Type>& chain);
    // The same with plugins in it: a Plugin slot is kept only while it holds the
    // same External.
    void setInserts(int id, const std::vector<mixfx::Slot>& chain);
    void setInsertParam(int id, int slot, int param, float value);
    void setInsertBypass(int id, int slot, bool bypass);

    // Peak level per side since the last call, linear, after the fader.
    void takePeak(int id, float& left, float& right);

    // Fire-and-forget one-shot into a strip (the master for -1 / unknown).
    void playOneShot(const std::string& path, int id);

    struct Impl;
    Impl* impl() const { return m_impl.get(); }

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel