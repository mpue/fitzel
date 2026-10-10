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

enum class Type { Eq, Filter, Compressor, Delay, Reverb, Chorus, Drive, Count };

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

} // namespace mixfx

class Mixer {
public:
    enum class Kind { Channel, Aux, Master };
    static constexpr int kMaxAux = 16;   // send slots per channel

    explicit Mixer(Audio& audio);
    ~Mixer();
    Mixer(const Mixer&)            = delete;
    Mixer& operator=(const Mixer&) = delete;

    bool ok() const;
    int  master() const;                 // the one master strip

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