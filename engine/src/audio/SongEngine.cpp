#include "fitzel/audio/SongEngine.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "fitzel/audio/Synth.hpp"
#include "fitzel/audio/SynthDsp.hpp"
#include "fitzel/audio/SynthPoly.hpp"
#include "fitzel/audio/Vst3.hpp"

namespace fitzel::song {

using synth::ModuleType;
using vst3::NoteEvent;

// --- Factory patches ------------------------------------------------------------
//
// Written for one voice of synth::Poly: dials "pitch", "gate" and "velocity",
// none of them smoothed (a smoothed gate would open late, a smoothed pitch
// glide into every note).
namespace {

struct Build {
    synth::Patch p;
    int add(ModuleType t, std::initializer_list<std::pair<const char*, float>> params,
            const char* mode = "") {
        synth::ModuleDef m;
        m.id   = p.nextId();
        m.type = t;
        m.mode = mode;
        for (const auto& kv : params) m.setParam(kv.first, kv.second);
        p.modules.push_back(std::move(m));
        return p.modules.back().id;
    }
    void wire(int from, int to, int port = 0) { p.wires.push_back({from, to, port}); }

    int gate = 0, vel = 0, hz = 0;   // the dials, and the note's frequency

    // The dials and the note's frequency.
    void voice(const char* name) {
        p.name   = name;
        p.inputs = {{"pitch", 0.0f, 127.0f, 60.0f}, {"gate", 0.0f, 1.0f, 0.0f}, {"velocity", 0.0f, 1.0f, 0.8f}};
        const int pitch = add(ModuleType::Param, {{"smooth", 0.0f}}, "pitch");
        gate = add(ModuleType::Param, {{"smooth", 0.0f}}, "gate");
        vel  = add(ModuleType::Param, {{"smooth", 0.0f}}, "velocity");
        hz   = add(ModuleType::Note, {});
        wire(pitch, hz);
    }
    // The note's frequency times `ratio`.
    int harmonic(float ratio) {
        const int g = add(ModuleType::Gain, {{"gain", ratio}});
        wire(hz, g);
        return g;
    }
    int osc(const char* wave, float level, int freqFrom, float fine = 0.0f, float width = 0.5f) {
        const int o = add(ModuleType::Osc, {{"freq", 0.0f}, {"fine", fine}, {"pulseWidth", width}, {"level", level}}, wave);
        wire(freqFrom, o);
        return o;
    }
    int mix(std::initializer_list<int> ins) {
        const int m = add(ModuleType::Mix, {{"gain", 1.0f}});
        int port = 0;
        for (int i : ins) wire(i, m, port++);
        return m;
    }
    // A low-pass, its cutoff opened by an envelope of its own when `envHz` > 0.
    int lowpass(int in, float cutoff, float q, float envHz = 0.0f, float a = 0.0f, float d = 0.3f,
                float s = 0.0f, float r = 0.3f) {
        const int f = add(ModuleType::Filter, {{"cutoff", cutoff}, {"q", q}}, "lp");
        wire(in, f);
        if (envHz > 0.0f) {
            const int env = add(ModuleType::Adsr, {{"attack", a}, {"decay", d}, {"sustain", s}, {"release", r}});
            wire(gate, env);
            const int amt = add(ModuleType::Gain, {{"gain", envHz}});
            wire(env, amt);
            wire(amt, f, 1);
        }
        return f;
    }
    // Envelope, velocity, out: the end of every voice.
    int amp(int in, float a, float d, float s, float r) {
        const int env = add(ModuleType::Adsr, {{"attack", a}, {"decay", d}, {"sustain", s}, {"release", r}});
        wire(gate, env);
        const int g = add(ModuleType::Gain, {{"gain", 1.0f}});
        wire(in, g);
        wire(env, g, 1);
        // Soft notes quieter, never silent.
        const int curve = add(ModuleType::Map, {{"inMin", 0.0f}, {"inMax", 1.0f}, {"outMin", 0.15f}, {"outMax", 1.0f}, {"curve", 1.0f}});
        wire(vel, curve);
        const int v = add(ModuleType::Gain, {{"gain", 1.0f}});
        wire(g, v);
        wire(curve, v, 1);
        return v;
    }
    int fx(ModuleType t, int in, std::initializer_list<std::pair<const char*, float>> params) {
        const int m = add(t, params);
        wire(in, m);
        return m;
    }
    void out(int in, float level) {
        const int o = add(ModuleType::Out, {{"gain", level}});
        wire(in, o);
    }
};

synth::Patch makeFactory(const std::string& name) {
    Build b;
    b.voice(name.c_str());
    if (name == "Saw Lead") {
        const int m = b.mix({b.osc("saw", 0.5f, b.hz), b.osc("saw", 0.5f, b.hz, 1.3f)});
        const int f = b.lowpass(m, 1600.0f, 1.1f, 2800.0f, 0.003f, 0.35f, 0.35f, 0.3f);
        b.out(b.amp(f, 0.004f, 0.2f, 0.75f, 0.25f), 0.9f);
    } else if (name == "Square Lead") {
        const int m = b.mix({b.osc("pulse", 0.55f, b.hz), b.osc("pulse", 0.4f, b.hz, -0.9f, 0.25f)});
        const int f = b.lowpass(m, 3200.0f, 0.9f);
        const int a = b.amp(f, 0.008f, 0.1f, 0.8f, 0.2f);
        b.out(b.fx(ModuleType::Delay, a, {{"time", 0.3f}, {"feedback", 0.25f}, {"mix", 0.18f}}), 0.53f);
    } else if (name == "Bass") {
        const int sub = b.add(ModuleType::Note, {{"transpose", -12.0f}});
        b.wire(b.p.modules[0].id, sub);   // the pitch dial is the first module
        const int m = b.mix({b.osc("saw", 0.6f, b.hz), b.osc("pulse", 0.5f, sub)});
        const int f = b.lowpass(m, 360.0f, 2.0f, 1500.0f, 0.001f, 0.18f, 0.0f, 0.1f);
        b.out(b.amp(f, 0.002f, 0.3f, 0.6f, 0.08f), 0.5f);
    } else if (name == "Pluck") {
        const int m = b.mix({b.osc("saw", 0.5f, b.hz), b.osc("pulse", 0.3f, b.hz, 0.7f)});
        const int f = b.lowpass(m, 250.0f, 1.5f, 4800.0f, 0.001f, 0.22f, 0.0f, 0.2f);
        b.out(b.amp(f, 0.001f, 0.6f, 0.0f, 0.35f), 0.66f);
    } else if (name == "Soft Pad") {
        const int sub = b.add(ModuleType::Note, {{"transpose", -12.0f}});
        b.wire(b.p.modules[0].id, sub);
        const int m = b.mix({b.osc("saw", 0.4f, b.hz, -2.0f), b.osc("saw", 0.4f, b.hz, 2.1f), b.osc("sine", 0.3f, sub)});
        const int f = b.lowpass(m, 1200.0f, 0.8f);
        const int a = b.amp(f, 0.7f, 0.6f, 0.85f, 1.4f);
        const int c = b.fx(ModuleType::Chorus, a, {{"rate", 0.4f}, {"depth", 0.003f}, {"mix", 0.5f}});
        b.out(b.fx(ModuleType::Reverb, c, {{"size", 0.8f}, {"damp", 0.4f}, {"mix", 0.35f}}), 2.1f);
    } else if (name == "Strings") {
        const int m = b.mix({b.osc("saw", 0.33f, b.hz, -1.5f), b.osc("saw", 0.33f, b.hz), b.osc("saw", 0.33f, b.hz, 1.7f)});
        const int f = b.lowpass(m, 2800.0f, 0.7f);
        const int a = b.amp(f, 0.3f, 0.4f, 0.85f, 0.7f);
        const int c = b.fx(ModuleType::Chorus, a, {{"rate", 0.6f}, {"depth", 0.002f}, {"mix", 0.5f}});
        b.out(b.fx(ModuleType::Reverb, c, {{"size", 0.6f}, {"damp", 0.5f}, {"mix", 0.25f}}), 3.1f);
    } else if (name == "Organ") {
        const int m = b.mix({b.osc("sine", 0.5f, b.hz), b.osc("sine", 0.35f, b.harmonic(2.0f)),
                             b.osc("sine", 0.25f, b.harmonic(3.0f)), b.osc("sine", 0.3f, b.harmonic(0.5f))});
        const int a = b.amp(m, 0.006f, 0.05f, 1.0f, 0.07f);
        b.out(b.fx(ModuleType::Chorus, a, {{"rate", 5.8f}, {"depth", 0.0012f}, {"mix", 0.35f}}), 0.8f);
    } else if (name == "Bell") {
        const int m = b.mix({b.osc("sine", 0.55f, b.hz), b.osc("sine", 0.3f, b.harmonic(2.76f)),
                             b.osc("sine", 0.15f, b.harmonic(5.4f))});
        const int a = b.amp(m, 0.001f, 2.2f, 0.0f, 1.8f);
        b.out(b.fx(ModuleType::Reverb, a, {{"size", 0.7f}, {"damp", 0.3f}, {"mix", 0.3f}}), 1.0f);
    } else {   // Keys
        const int tone = b.lowpass(b.osc("pulse", 0.12f, b.hz), 2000.0f, 0.7f);
        const int m = b.mix({b.osc("sine", 0.6f, b.hz), b.osc("sine", 0.25f, b.harmonic(2.0f)), tone});
        const int a = b.amp(m, 0.002f, 1.4f, 0.2f, 0.45f);
        b.out(b.fx(ModuleType::Chorus, a, {{"rate", 0.8f}, {"depth", 0.002f}, {"mix", 0.3f}}), 0.87f);
    }
    return b.p;
}

} // namespace

const std::vector<std::string>& factoryPatchNames() {
    static const std::vector<std::string> names = {"Keys", "Saw Lead", "Square Lead", "Bass", "Pluck",
                                                   "Soft Pad", "Strings", "Organ", "Bell"};
    return names;
}

bool factoryPatch(const std::string& name, synth::Patch& out) {
    const std::string n = name.rfind("builtin:", 0) == 0 ? name.substr(8) : name;
    const auto& all = factoryPatchNames();
    if (std::find(all.begin(), all.end(), n) == all.end()) return false;
    out = makeFactory(n);
    return true;
}

// --- Instruments ------------------------------------------------------------------
namespace {

constexpr int kEvCap   = 2048;
constexpr int kHeldCap = 512;

// Events in frame order, a note-off before a note-on at the same frame (a
// repeated note strikes again instead of being let go as it starts). An
// insertion sort: the list arrives almost sorted, and it must not allocate.
void sortEvents(NoteEvent* e, int n) {
    for (int i = 1; i < n; ++i) {
        const NoteEvent x = e[i];
        int j = i - 1;
        while (j >= 0 && (e[j].frame > x.frame || (e[j].frame == x.frame && e[j].on && !x.on))) {
            e[j + 1] = e[j];
            --j;
        }
        e[j + 1] = x;
    }
}

// What the engine plays on an Instrument track. Made on the main thread,
// played on the audio thread; the bookkeeping below is the audio thread's.
class Instr {
public:
    virtual ~Instr() = default;
    // `frames` stereo frames into `out` (overwritten), consuming this block's
    // events.
    virtual void render(float* out, int frames, const vst3::Transport& t) = 0;

    void push(const NoteEvent& e) {
        if (evCount < kEvCap) ev[static_cast<std::size_t>(evCount++)] = e;
    }
    // The song's notes that are sounding: when each ends, in plan samples.
    struct Held { std::int64_t end; std::uint8_t ch, pitch; };
    bool hold(const Held& h) {
        if (heldCount >= kHeldCap) return false;
        held[static_cast<std::size_t>(heldCount++)] = h;
        return true;
    }
    // Every song note let go at `frame`.
    void releaseSong(std::uint32_t frame) {
        for (int i = 0; i < heldCount; ++i) {
            const Held& h = held[static_cast<std::size_t>(i)];
            push({frame, h.ch, h.pitch, 0.0f, false});
        }
        heldCount = 0;
    }
    // ...and every note held by hand.
    void releaseAll(std::uint32_t frame) {
        releaseSong(frame);
        for (int c = 0; c < 16; ++c)
            for (int p = 0; p < 128; ++p)
                if (live[c][p]) {
                    push({frame, static_cast<std::uint8_t>(c), static_cast<std::uint8_t>(p), 0.0f, false});
                    live[c][p] = 0;
                }
    }

    std::array<NoteEvent, kEvCap> ev{};
    int                           evCount = 0;
    std::array<Held, kHeldCap>    held{};
    int                           heldCount = 0;
    std::uint8_t                  live[16][128] = {};
};

class SilentInstr final : public Instr {
public:
    void render(float* out, int frames, const vst3::Transport&) override {
        std::fill(out, out + static_cast<std::size_t>(frames) * 2, 0.0f);
        evCount = 0;
    }
};

// Splits a block at its events: render up to the next one, apply it, go on.
// What makes a note land on its sample with an instrument that only takes
// notes between blocks (Poly, the drum kit).
template <class Apply, class Run>
void splitAtEvents(const NoteEvent* ev, int count, int frames, Apply apply, Run run) {
    int pos = 0, e = 0;
    while (pos < frames) {
        while (e < count && static_cast<int>(ev[e].frame) <= pos) apply(ev[e++]);
        // Every event at or before `pos` is applied, so the next is after it.
        const int next = e < count ? std::min(frames, static_cast<int>(ev[e].frame)) : frames;
        run(pos, next - pos);
        pos = next;
    }
    while (e < count) apply(ev[e++]);
}

class SynthInstr final : public Instr {
public:
    synth::Poly        poly;
    std::vector<float> mono;

    void render(float* out, int frames, const vst3::Transport&) override {
        splitAtEvents(ev.data(), evCount, frames,
            [&](const NoteEvent& e) {
                if (e.on) poly.noteOn(e.pitch, e.velocity);
                else      poly.noteOff(e.pitch);
            },
            [&](int at, int n) { poly.process(mono.data() + at, n); });
        evCount = 0;
        for (int i = 0; i < frames; ++i) out[i * 2] = out[i * 2 + 1] = mono[static_cast<std::size_t>(i)];
    }
};

class PluginInstr final : public Instr {
public:
    std::shared_ptr<vst3::Instance> plugin;

    void render(float* out, int frames, const vst3::Transport& t) override {
        std::fill(out, out + static_cast<std::size_t>(frames) * 2, 0.0f);
        plugin->processNotes(out, static_cast<unsigned>(frames), 2, ev.data(), evCount, &t);
        evCount = 0;
        // A plugin is somebody else's code: what it hands back is checked
        // before it reaches the mix.
        for (int i = 0; i < frames * 2; ++i)
            if (!std::isfinite(out[i])) out[i] = 0.0f;
    }
};

// --- The drum kit ---------------------------------------------------------------
//
// General MIDI's drum map, synthesised: a sine falling in pitch for the kick,
// tone and noise for the snare, the 808's six detuned squares for hats and
// cymbals. Not a sampler -- it needs no files, so a song made with it plays in
// any project and in the shipped game as it did in the editor.
class DrumInstr final : public Instr {
public:
    explicit DrumInstr(double rate) : m_rate(rate) {
        for (Voice& v : m_voices) {
            v.f1.prepare(rate);
            v.f2.prepare(rate);
        }
    }

    void render(float* out, int frames, const vst3::Transport&) override {
        std::fill(out, out + static_cast<std::size_t>(frames) * 2, 0.0f);
        splitAtEvents(ev.data(), evCount, frames,
            [&](const NoteEvent& e) { if (e.on) trigger(e.pitch, e.velocity); },
            [&](int at, int n) { run(out + static_cast<std::size_t>(at) * 2, n); });
        evCount = 0;
    }

private:
    enum Kind { Kick, Snare, Clap, HatClosed, HatPedal, HatOpen, Tom, Crash, Ride, RideBell, Cowbell, Rim, Shaker };

    struct Voice {
        bool          active = false;
        Kind          kind   = Kick;
        float         vel = 1.0f, pan = 0.0f, base = 100.0f, life = 1.0f;
        double        t = 0.0, phase = 0.0;
        double        sq[6] = {};
        std::uint32_t rng = 0x9E3779B9u;
        std::uint64_t started = 0;
        synth::Biquad f1, f2;
    };

    double                m_rate;
    std::array<Voice, 24> m_voices;
    std::uint64_t         m_stamp = 0;

    static float noise(std::uint32_t& s) {
        s ^= s << 13; s ^= s >> 17; s ^= s << 5;
        return static_cast<float>(static_cast<std::int32_t>(s)) * (1.0f / 2147483648.0f);
    }
    // The 808's metal: six squares at frequencies that share no harmonics.
    float metal(Voice& v) {
        static const double hz[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
        float s = 0.0f;
        for (int i = 0; i < 6; ++i) {
            v.sq[i] += hz[i] * 1.0 / m_rate;
            if (v.sq[i] >= 1.0) v.sq[i] -= 1.0;
            s += v.sq[i] < 0.5 ? 1.0f : -1.0f;
        }
        return s * (1.0f / 6.0f);
    }

    void trigger(int pitch, float vel) {
        Kind kind;
        float base = 100.0f, pan = 0.0f;
        switch (pitch) {
        case 35: case 36:          kind = Kick; break;
        case 37:                   kind = Rim; break;
        case 38: case 40:          kind = Snare; break;
        case 39:                   kind = Clap; break;
        case 42:                   kind = HatClosed; pan = 0.25f; break;
        case 44:                   kind = HatPedal;  pan = 0.25f; break;
        case 46:                   kind = HatOpen;   pan = 0.25f; break;
        case 41: case 43: case 45: case 47: case 48: case 50: {
            kind = Tom;
            static const int order[6] = {41, 43, 45, 47, 48, 50};
            int i = 0;
            while (order[i] != pitch) ++i;
            base = 78.0f * std::pow(1.19f, static_cast<float>(i));
            pan  = -0.45f + 0.18f * static_cast<float>(i);
            break;
        }
        case 49: case 52: case 55: case 57: kind = Crash; pan = -0.3f; break;
        case 51: case 59:          kind = Ride; pan = 0.35f; break;
        case 53:                   kind = RideBell; pan = 0.35f; break;
        case 56:                   kind = Cowbell; pan = -0.15f; break;
        case 54: case 69: case 70: case 82: kind = Shaker; pan = 0.4f; break;
        default:
            if (pitch < 35) { kind = Kick; break; }
            // Bongos, congas, timbales, blocks: a tom tuned by the key.
            kind = Tom;
            base = 110.0f * std::pow(2.0f, static_cast<float>(pitch - 60) / 24.0f);
            pan  = ((pitch % 5) - 2) * 0.15f;
            break;
        }
        // A closed hat chokes an open one, as the pedal does.
        if (kind == HatClosed || kind == HatPedal)
            for (Voice& v : m_voices)
                if (v.active && v.kind == HatOpen) v.life = std::min(v.life, static_cast<float>(v.t) + 0.02f);

        Voice* pick = nullptr;
        for (Voice& v : m_voices)
            if (!v.active) { pick = &v; break; }
        if (!pick) {
            pick = &m_voices[0];
            for (Voice& v : m_voices)
                if (v.started < pick->started) pick = &v;
        }
        Voice& v = *pick;
        v.active  = true;
        v.kind    = kind;
        v.vel     = std::clamp(vel, 0.0f, 1.0f);
        v.pan     = pan;
        v.base    = base;
        v.t       = 0.0;
        v.phase   = 0.0;
        v.started = ++m_stamp;
        v.rng     = 0x9E3779B9u ^ static_cast<std::uint32_t>(m_stamp * 2654435761u);
        for (double& s : v.sq) s = 0.0;
        v.f1.reset();
        v.f2.reset();
        using M = synth::Biquad::Mode;
        switch (kind) {
        case Kick:      v.life = 0.9f; break;
        case Snare:     v.life = 0.45f; v.f1.setMode(M::HighPass); v.f1.set(1400.0f, 0.7f); break;
        case Clap:      v.life = 0.5f;  v.f1.setMode(M::BandPass); v.f1.set(1100.0f, 1.4f); break;
        case HatClosed: v.life = 0.12f; v.f1.setMode(M::HighPass); v.f1.set(7500.0f, 0.8f); break;
        case HatPedal:  v.life = 0.09f; v.f1.setMode(M::HighPass); v.f1.set(7000.0f, 0.8f); break;
        case HatOpen:   v.life = 0.9f;  v.f1.setMode(M::HighPass); v.f1.set(7000.0f, 0.8f); break;
        case Tom:       v.life = 0.8f; break;
        case Crash:     v.life = 2.6f;  v.f1.setMode(M::HighPass); v.f1.set(4200.0f, 0.7f); break;
        case Ride:      v.life = 3.0f;  v.f1.setMode(M::BandPass); v.f1.set(5200.0f, 0.9f); break;
        case RideBell:  v.life = 1.6f;  v.f1.setMode(M::BandPass); v.f1.set(2600.0f, 2.0f); break;
        case Cowbell:   v.life = 0.5f;  v.f1.setMode(M::BandPass); v.f1.set(900.0f, 2.5f); break;
        case Rim:       v.life = 0.08f; v.f1.setMode(M::BandPass); v.f1.set(1700.0f, 3.0f); break;
        case Shaker:    v.life = 0.2f;  v.f1.setMode(M::HighPass); v.f1.set(6500.0f, 0.7f); break;
        }
        v.f2.setMode(M::LowPass);
        v.f2.set(9000.0f, 0.7f);
    }

    float sample(Voice& v) {
        const float t  = static_cast<float>(v.t);
        const float tw = 6.2831853f;
        float s = 0.0f;
        switch (v.kind) {
        case Kick: {
            const double hz = 44.0 + 120.0 * std::exp(-t * 32.0f);
            v.phase += hz / m_rate;
            s = std::sin(tw * static_cast<float>(v.phase)) * std::exp(-t * 6.5f) * 0.68f +
                noise(v.rng) * std::exp(-t * 400.0f) * 0.2f;
            break;
        }
        case Snare: {
            const float tone = std::sin(tw * 185.0f * t) * std::exp(-t * 22.0f) * 0.3f +
                               std::sin(tw * 330.0f * t) * std::exp(-t * 28.0f) * 0.16f;
            s = tone + v.f2.process(v.f1.process(noise(v.rng))) * std::exp(-t * 13.0f) * 0.58f;
            break;
        }
        case Clap: {
            float env = 0.0f;
            for (int i = 0; i < 3; ++i) {
                const float ti = t - 0.011f * static_cast<float>(i);
                if (ti >= 0.0f) env += std::exp(-ti * 190.0f);
            }
            if (t > 0.03f) env += std::exp(-(t - 0.03f) * 15.0f) * 0.7f;
            s = v.f1.process(noise(v.rng)) * env * 1.4f;
            break;
        }
        case HatClosed: case HatPedal: case HatOpen: {
            const float decay = v.kind == HatOpen ? 4.5f : v.kind == HatPedal ? 60.0f : 42.0f;
            const float raw   = metal(v) * 0.6f + noise(v.rng) * 0.4f;
            s = v.f1.process(raw) * std::exp(-t * decay) * 0.55f;
            break;
        }
        case Tom: {
            const double hz = v.base * (1.0 + 0.6 * std::exp(-t * 18.0f));
            v.phase += hz / m_rate;
            s = std::sin(tw * static_cast<float>(v.phase)) * std::exp(-t * 6.0f) * 0.6f +
                noise(v.rng) * std::exp(-t * 60.0f) * 0.06f;
            break;
        }
        case Crash: {
            const float raw = metal(v) * 0.5f + noise(v.rng) * 0.5f;
            s = v.f2.process(v.f1.process(raw)) * std::exp(-t * 1.6f) * 0.7f;
            break;
        }
        case Ride: {
            const float raw = metal(v) * 0.7f + noise(v.rng) * 0.3f;
            s = v.f1.process(raw) * std::exp(-t * 1.2f) * 0.55f;
            break;
        }
        case RideBell:
            s = v.f1.process(metal(v)) * std::exp(-t * 2.8f) * 0.65f;
            break;
        case Cowbell: {
            v.sq[0] += 540.0 / m_rate; if (v.sq[0] >= 1.0) v.sq[0] -= 1.0;
            v.sq[1] += 800.0 / m_rate; if (v.sq[1] >= 1.0) v.sq[1] -= 1.0;
            const float raw = (v.sq[0] < 0.5 ? 0.5f : -0.5f) + (v.sq[1] < 0.5 ? 0.5f : -0.5f);
            s = v.f1.process(raw) * std::exp(-t * 11.0f) * 0.6f;
            break;
        }
        case Rim:
            s = (std::sin(tw * 1700.0f * t) * 0.6f + v.f1.process(noise(v.rng)) * 0.6f) * std::exp(-t * 110.0f);
            break;
        case Shaker:
            s = v.f1.process(noise(v.rng)) * std::exp(-t * 28.0f) * (t < 0.004f ? t / 0.004f : 1.0f) * 0.5f;
            break;
        }
        return s * v.vel;
    }

    void run(float* out, int n) {
        const double dt = 1.0 / m_rate;
        for (Voice& v : m_voices) {
            if (!v.active) continue;
            const float l = std::sqrt(0.5f * (1.0f - v.pan));
            const float r = std::sqrt(0.5f * (1.0f + v.pan));
            for (int i = 0; i < n; ++i) {
                float s = sample(v);
                // The last 20 ms fade out: a choked hat or a voice at the end
                // of its life ends without a click.
                const float left = v.life - static_cast<float>(v.t);
                if (left < 0.02f) s *= std::max(0.0f, left / 0.02f);
                out[i * 2]     += s * l * 1.41421f;
                out[i * 2 + 1] += s * r * 1.41421f;
                v.t += dt;
                if (v.t >= v.life) { v.active = false; break; }
            }
        }
    }
};

// --- Sound files --------------------------------------------------------------
struct SampleEntry {
    std::string        path;
    std::atomic<int>   state{0};   // 0 loading, 1 ready, 2 failed
    std::vector<float> data;       // stereo, interleaved, at the engine's rate
    std::int64_t       frames = 0;
    std::string        error;
    std::filesystem::file_time_type stamp{};
};

// The file at the engine's rate, as two channels.
bool decodeTo(const std::string& path, double rate, SampleEntry& e) {
    SampleData sd;
    if (!loadSampleData(path, sd, &e.error)) return false;
    if (sd.channels <= 0 || sd.frames() == 0) { e.error = "no samples"; return false; }
    const std::int64_t n = sd.frames();
    std::vector<float> st(static_cast<std::size_t>(n) * 2);
    for (std::int64_t i = 0; i < n; ++i) {
        const float l = sd.samples[static_cast<std::size_t>(i * sd.channels)];
        const float r = sd.channels > 1 ? sd.samples[static_cast<std::size_t>(i * sd.channels + 1)] : l;
        st[static_cast<std::size_t>(i) * 2]     = l;
        st[static_cast<std::size_t>(i) * 2 + 1] = r;
    }
    const auto outRate = static_cast<ma_uint32>(std::lround(rate));
    if (static_cast<ma_uint32>(sd.sampleRate) == outRate) {
        e.data   = std::move(st);
        e.frames = n;
        return true;
    }
    ma_resampler_config cfg = ma_resampler_config_init(ma_format_f32, 2, static_cast<ma_uint32>(sd.sampleRate),
                                                       outRate, ma_resample_algorithm_linear);
    cfg.linear.lpfOrder = 8;
    ma_resampler rs;
    if (ma_resampler_init(&cfg, nullptr, &rs) != MA_SUCCESS) { e.error = "no resampler"; return false; }
    ma_uint64 expect = 0;
    ma_resampler_get_expected_output_frame_count(&rs, static_cast<ma_uint64>(n), &expect);
    std::vector<float> out(static_cast<std::size_t>(expect + 16) * 2);
    ma_uint64 inFrames = static_cast<ma_uint64>(n), outFrames = expect + 16;
    ma_resampler_process_pcm_frames(&rs, st.data(), &inFrames, out.data(), &outFrames);
    ma_resampler_uninit(&rs, nullptr);
    out.resize(static_cast<std::size_t>(outFrames) * 2);
    e.frames = static_cast<std::int64_t>(outFrames);
    e.data   = std::move(out);
    return true;
}

// --- The plan: a song in samples ------------------------------------------------
struct TrackState {
    std::atomic<float> peak{0.0f};
    float gl = 0.0f, gr = 0.0f;   // the audio thread's: gains as of the last block
    bool  primed = false;
};

struct PlanNote {
    std::int64_t at = 0, end = 0;
    std::uint8_t ch = 0, pitch = 60;
    float        vel = 0.8f;
};

struct PlanClip {
    std::int64_t at = 0, end = 0, src = 0;   // samples; `src` = file frame at `at`
    float        gain = 1.0f;
    std::int64_t fadeIn = 0, fadeOut = 0;
    std::shared_ptr<SampleEntry> sample;
};

struct PlanTrack {
    int                         uid  = 0;
    int                         out  = 0;   // the output slot it is heard through
    TrackKind                   kind = TrackKind::Instrument;
    float                       gl = 0.0f, gr = 0.0f;
    std::shared_ptr<Instr>      instr;
    std::shared_ptr<TrackState> state;
    std::vector<PlanNote>       notes;   // sorted by `at`
    std::vector<PlanClip>       clips;   // sorted by `at`
    std::vector<float>          buf;     // a block, stereo
};

// A hand-played note's way: the track a script or a key names, to the plan
// track whose instrument sounds it.
struct PlanRoute {
    int          uid = 0, index = -1, transpose = 0;
    std::uint8_t ch = 0;
};

struct Plan {
    double       spt = 1.0;            // samples per tick
    double       bpm = 120.0;
    int          bpb = 4, unit = 4;
    double       beatTicks = kPpq;
    bool         loop = false;
    std::int64_t loopStart = 0, loopEnd = 0;   // samples
    std::int64_t end = 0;                       // samples; the song's end
    float        master = 1.0f;
    std::vector<PlanTrack> tracks;
    std::vector<PlanRoute> routes;
};

struct Cmd {
    enum Kind : std::uint8_t { Play, Stop, Seek, Live, AllOff, StopAtEnd, Metronome };
    Kind         kind  = Play;
    int          track = 0;
    int          pitch = 0;
    float        vel   = 0.0f;
    bool         on    = false;
    std::int64_t tick  = 0;
};

constexpr int kQueue = 1024;

} // namespace

struct Engine::Impl {
    double rate     = 48000.0;
    int    maxBlock = 512;
    Files  files;

    // --- Main thread -------------------------------------------------------
    struct Slot {
        Instrument             def;
        std::shared_ptr<Instr> instr;
        std::string            error;
    };
    std::map<int, Slot>                        slots;    // Instrument tracks by uid
    std::vector<std::string>                   outputs = std::vector<std::string>(Engine::kMaxOutputs);
    std::map<int, std::shared_ptr<TrackState>> states;   // every track by uid
    mutable std::mutex                         sampleMutex;   // guards `samples` against the loader
    std::map<std::string, std::shared_ptr<SampleEntry>> samples;
    std::deque<std::shared_ptr<SampleEntry>>   loadQueue;
    std::condition_variable                    loadCv;
    std::thread                                loader;
    bool                                       loaderStop = false;
    std::atomic<float>                         masterPeak{0.0f};

    // --- Handover --------------------------------------------------------------
    std::atomic<Plan*> pending{nullptr};
    std::atomic<Plan*> retired{nullptr};
    Plan*              current = nullptr;   // the audio thread's

    Cmd              queue[kQueue];
    std::atomic<int> head{0}, tail{0};
    std::mutex       pushMutex;

    // --- Audio thread ------------------------------------------------------------
    std::int64_t pos = 0;   // samples on the current plan
    bool         playing = false, stopAtEnd = false, metronome = false;
    struct Click { int frame; bool accent; };
    Click        clicks[8];
    int          clickCount = 0;
    double       clickT = -1.0;   // seconds into the click sounding, < 0 none
    bool         clickAccent = false;
    // Hand-played notes. A key pressed and let go between two blocks arrives as
    // an on and an off in one; both at the block's start, the off would be
    // sorted before the on (the rule that makes a repeated SONG note strike
    // again) and the note would never end. So an off for a note struck in the
    // same block waits for the next one: the shortest note there is is a
    // block long (~11 ms), and none is left hanging.
    struct LiveKey { int track, pitch; };
    LiveKey      struck[256];
    int          struckCount = 0;
    LiveKey      deferred[256];
    int          deferredCount = 0;

    // --- Published ------------------------------------------------------------------
    std::atomic<double> posTicks{0.0};
    std::atomic<double> sptPub{1.0};
    std::atomic<bool>   playingPub{false}, endedPub{false};

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(sampleMutex);
            loaderStop = true;
        }
        loadCv.notify_all();
        if (loader.joinable()) loader.join();
        delete current;
        delete pending.exchange(nullptr);
        delete retired.exchange(nullptr);
    }

    void push(const Cmd& c) {
        std::lock_guard<std::mutex> lock(pushMutex);
        const int h    = head.load(std::memory_order_relaxed);
        const int next = (h + 1) % kQueue;
        if (next == tail.load(std::memory_order_acquire)) return;   // the audio has stalled: drop
        queue[h] = c;
        head.store(next, std::memory_order_release);
    }
    void freeRetired() { delete retired.exchange(nullptr); }

    // --- Sound files: decoded on a thread of their own ----------------------------
    std::shared_ptr<SampleEntry> sample(const std::string& path) {
        std::filesystem::file_time_type stamp{};
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) stamp = std::filesystem::last_write_time(path, ec);
        std::lock_guard<std::mutex> lock(sampleMutex);
        auto it = samples.find(path);
        // Saved again since (the wave editor): decoded again. The old entry
        // lives on in any plan still holding it.
        if (it != samples.end() && (it->second->state.load() == 0 || it->second->stamp == stamp))
            return it->second;
        auto e   = std::make_shared<SampleEntry>();
        e->path  = path;
        e->stamp = stamp;
        samples[path] = e;
        loadQueue.push_back(e);
        if (!loader.joinable()) loader = std::thread([this] { loadLoop(); });
        loadCv.notify_one();
        return e;
    }
    void loadLoop() {
        for (;;) {
            std::shared_ptr<SampleEntry> e;
            {
                std::unique_lock<std::mutex> lock(sampleMutex);
                loadCv.wait(lock, [&] { return loaderStop || !loadQueue.empty(); });
                if (loaderStop) return;
                e = loadQueue.front();
                loadQueue.pop_front();
            }
            const bool ok = decodeTo(e->path, rate, *e);
            if (!ok) std::fprintf(stderr, "[song] %s: %s\n", e->path.c_str(), e->error.c_str());
            e->state.store(ok ? 1 : 2, std::memory_order_release);
        }
    }

    // --- Instruments -----------------------------------------------------------------
    Slot makeSlot(const Instrument& def) {
        Slot s;
        s.def = def;
        switch (def.kind) {
        case InstrumentKind::None:
            s.instr = std::make_shared<SilentInstr>();
            break;
        case InstrumentKind::Drums:
            s.instr = std::make_shared<DrumInstr>(rate);
            break;
        case InstrumentKind::Synth: {
            synth::Patch patch;
            std::string  err;
            bool ok = factoryPatch(def.patch, patch);
            if (!ok) {
                const std::string path = files.patch ? files.patch(def.patch) : def.patch;
                ok = synth::Patch::load(path, patch, &err);
            }
            auto in = std::make_shared<SynthInstr>();
            const int voices = std::clamp(def.voices, 1, 32);
            if (ok) ok = in->poly.compile(patch, rate, voices, maxBlock, &err);
            if (!ok) {
                s.error = "patch '" + def.patch + "': " + err;
                s.instr = std::make_shared<SilentInstr>();
                break;
            }
            // Headroom that grows with the voices, as SynthPlayer gives it.
            in->poly.setGain(1.6f / std::sqrt(static_cast<float>(voices)));
            in->mono.assign(static_cast<std::size_t>(maxBlock), 0.0f);
            s.instr = in;
            break;
        }
        case InstrumentKind::Vst3: {
            const vst3::PluginInfo* info = vst3::find(def.pluginUid);
            std::string err;
            std::shared_ptr<vst3::Instance> plug;
            if (!info) err = vst3::supported() ? "not installed on this machine" : "VST3 is not supported here";
            else plug = vst3::Instance::create(*info, rate, &err);
            if (plug && !def.state.empty() && !plug->setState(def.state))
                std::fprintf(stderr, "[song] %s: the saved state was not taken\n", info->name.c_str());
            if (!plug) {
                s.error = (def.pluginName.empty() ? def.pluginUid : def.pluginName) + ": " + err;
                s.instr = std::make_shared<SilentInstr>();
                break;
            }
            auto in = std::make_shared<PluginInstr>();
            in->plugin = std::move(plug);
            s.instr = in;
            break;
        }
        }
        if (!s.error.empty()) std::fprintf(stderr, "[song] %s\n", s.error.c_str());
        return s;
    }

    void build(const Song& song) {
        freeRetired();
        auto P = std::make_unique<Plan>();
        Plan& p = *P;
        p.spt       = rate * song.secondsPerTick();
        p.bpm       = song.bpm;
        p.bpb       = song.beatsPerBar;
        p.unit      = song.beatUnit;
        p.beatTicks = static_cast<double>(song.ticksPerBeat());
        p.master    = std::clamp(song.masterVolume, 0.0f, 2.0f);
        p.loop      = song.loop && song.loopEnd > song.loopStart;
        p.loopStart = std::llround(static_cast<double>(song.loopStart) * p.spt);
        p.loopEnd   = std::llround(static_cast<double>(song.loopEnd) * p.spt);
        p.end       = std::llround(static_cast<double>(song.length()) * p.spt);

        // Solo: any soloed track silences the others -- except what a soloed
        // track needs to be heard: the instrument a soloed MIDI track plays,
        // and the MIDI tracks playing a soloed instrument.
        bool anySolo = false;
        for (const Track& t : song.tracks) anySolo = anySolo || t.solo;
        auto heard = [&](const Track& t) {
            if (t.mute) return false;
            if (!anySolo || t.solo) return true;
            for (const Track& o : song.tracks) {
                if (!o.solo || o.mute) continue;
                if (t.kind == TrackKind::Instrument && o.kind == TrackKind::Midi && o.target == t.uid) return true;
                if (t.kind == TrackKind::Midi && o.uid == t.target) return true;
            }
            return false;
        };

        // Output slots: a channel a track names keeps the slot it had; a new
        // one takes a free slot; one no track names any more frees its own.
        {
            std::vector<std::string> wanted;
            for (const Track& t : song.tracks)
                if (t.makesSound() && !t.output.empty() &&
                    std::find(wanted.begin(), wanted.end(), t.output) == wanted.end())
                    wanted.push_back(t.output);
            std::vector<std::string> next(Engine::kMaxOutputs);
            for (int k = 1; k < Engine::kMaxOutputs; ++k)
                if (std::find(wanted.begin(), wanted.end(), outputs[static_cast<std::size_t>(k)]) != wanted.end())
                    next[static_cast<std::size_t>(k)] = outputs[static_cast<std::size_t>(k)];
            for (const std::string& w : wanted) {
                if (std::find(next.begin(), next.end(), w) != next.end()) continue;
                for (int k = 1; k < Engine::kMaxOutputs; ++k)
                    if (next[static_cast<std::size_t>(k)].empty()) { next[static_cast<std::size_t>(k)] = w; break; }
            }
            outputs = std::move(next);
        }
        auto slotOf = [&](const std::string& name) {
            if (name.empty()) return 0;
            for (int k = 1; k < Engine::kMaxOutputs; ++k)
                if (outputs[static_cast<std::size_t>(k)] == name) return k;
            return 0;   // more channels than slots: the song's own mix
        };

        std::map<int, Slot> keep;
        std::map<int, std::shared_ptr<TrackState>> keepStates;
        std::map<int, int> indexOf;   // instrument track uid -> plan index
        for (const Track& t : song.tracks) {
            auto st = states.count(t.uid) ? states[t.uid] : std::make_shared<TrackState>();
            keepStates[t.uid] = st;
            if (t.kind == TrackKind::Midi) continue;
            PlanTrack pt;
            pt.uid   = t.uid;
            pt.out   = slotOf(t.output);
            pt.kind  = t.kind;
            pt.state = st;
            if (heard(t)) {
                const float v = std::clamp(t.volume, 0.0f, 2.0f);
                const float a = (std::clamp(t.pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265f;
                pt.gl = v * std::cos(a) * 1.41421356f;
                pt.gr = v * std::sin(a) * 1.41421356f;
            }
            if (t.kind == TrackKind::Instrument) {
                auto it = slots.find(t.uid);
                if (it != slots.end() && it->second.def.sameEngine(t.instrument)) {
                    keep[t.uid] = it->second;
                    keep[t.uid].def.state = t.instrument.state;
                } else {
                    keep[t.uid] = makeSlot(t.instrument);
                }
                pt.instr = keep[t.uid].instr;
            }
            pt.buf.assign(static_cast<std::size_t>(maxBlock) * 2, 0.0f);
            indexOf[t.uid] = static_cast<int>(p.tracks.size());
            p.tracks.push_back(std::move(pt));
        }
        slots  = std::move(keep);
        states = std::move(keepStates);

        // Notes: a track's own, and a MIDI track's on the instrument it plays.
        std::vector<Note> played;
        for (const Track& t : song.tracks) {
            if (!t.hasNotes()) continue;
            const int dest = t.kind == TrackKind::Midi ? t.target : t.uid;
            const auto idx = indexOf.find(dest);
            if (idx == indexOf.end()) continue;
            PlanTrack& pt = p.tracks[static_cast<std::size_t>(idx->second)];
            const std::uint8_t ch = static_cast<std::uint8_t>(t.kind == TrackKind::Midi ? t.channel : 0);
            p.routes.push_back({t.uid, idx->second, t.transpose, ch});
            if (!heard(t)) continue;
            if (t.kind == TrackKind::Midi) {
                // A MIDI track feeding an instrument that is not heard is not heard either.
                const Track* target = song.track(dest);
                if (!target || !heard(*target)) continue;
            }
            for (const Clip& c : t.clips) {
                if (c.mute) continue;
                played.clear();
                playedNotes(c, played);
                for (const Note& n : played) {
                    PlanNote pn;
                    pn.at    = std::llround(static_cast<double>(n.tick) * p.spt);
                    pn.end   = std::max(pn.at + 1, static_cast<std::int64_t>(std::llround(static_cast<double>(n.end()) * p.spt)));
                    pn.ch    = ch;
                    pn.pitch = static_cast<std::uint8_t>(std::clamp(n.pitch + t.transpose, 0, 127));
                    pn.vel   = std::clamp(static_cast<float>(n.velocity) / 127.0f, 0.0f, 1.0f);
                    pt.notes.push_back(pn);
                }
            }
        }
        for (PlanTrack& pt : p.tracks)
            std::stable_sort(pt.notes.begin(), pt.notes.end(),
                             [](const PlanNote& a, const PlanNote& b) { return a.at < b.at; });

        // Audio clips, with their files.
        for (const Track& t : song.tracks) {
            if (t.kind != TrackKind::Audio) continue;
            PlanTrack& pt = p.tracks[static_cast<std::size_t>(indexOf[t.uid])];
            for (const Clip& c : t.clips) {
                if (c.mute || c.file.empty()) continue;
                PlanClip pc;
                pc.at      = std::llround(static_cast<double>(c.start) * p.spt);
                pc.end     = std::llround(static_cast<double>(c.end()) * p.spt);
                pc.src     = std::llround(c.sourceOffset * rate);
                pc.gain    = std::clamp(c.gain, 0.0f, 4.0f);
                pc.fadeIn  = std::llround(std::max(0.0f, c.fadeIn) * rate);
                pc.fadeOut = std::llround(std::max(0.0f, c.fadeOut) * rate);
                pc.sample  = sample(files.sound ? files.sound(c.file) : c.file);
                if (pc.end > pc.at) pt.clips.push_back(std::move(pc));
            }
            std::stable_sort(pt.clips.begin(), pt.clips.end(),
                             [](const PlanClip& a, const PlanClip& b) { return a.at < b.at; });
        }

        delete pending.exchange(P.release());
    }

    // --- The audio thread --------------------------------------------------------------
    void adopt() {
        if (retired.load(std::memory_order_acquire) != nullptr) return;
        Plan* next = pending.exchange(nullptr, std::memory_order_acq_rel);
        if (!next) return;
        Plan* old = current;
        // A new tempo: the playhead stays on its beat, and notes still
        // sounding end on theirs.
        if (old && std::fabs(old->spt - next->spt) > 1e-9) {
            const double k = next->spt / old->spt;
            const std::int64_t p0 = pos;
            pos = std::llround(static_cast<double>(pos) * k);
            for (PlanTrack& t : next->tracks)
                if (t.instr)
                    for (int i = 0; i < t.instr->heldCount; ++i) {
                        auto& h = t.instr->held[static_cast<std::size_t>(i)];
                        h.end = pos + std::llround(static_cast<double>(h.end - p0) * k);
                    }
        }
        current = next;
        retired.store(old, std::memory_order_release);
    }

    void releaseSong(Plan& p, std::uint32_t frame) {
        for (PlanTrack& t : p.tracks)
            if (t.instr) t.instr->releaseSong(frame);
    }

    // A note played by hand, on the instrument its track routes to.
    void live(Plan& p, int track, int pitch0, float vel, bool on) {
        for (const PlanRoute& r : p.routes) {
            if (r.uid != track || r.index < 0) continue;
            Instr* in = p.tracks[static_cast<std::size_t>(r.index)].instr.get();
            if (!in) return;
            const int pitch = std::clamp(pitch0 + r.transpose, 0, 127);
            std::uint8_t& held = in->live[r.ch & 15][pitch];
            if (on) {
                if (held < 255) ++held;
                in->push({0, r.ch, static_cast<std::uint8_t>(pitch), std::clamp(vel, 0.0f, 1.0f), true});
                if (struckCount < 256) struck[struckCount++] = {track, pitch0};
                return;
            }
            for (int i = 0; i < struckCount; ++i)
                if (struck[i].track == track && struck[i].pitch == pitch0) {
                    if (deferredCount < 256) {
                        deferred[deferredCount++] = {track, pitch0};
                        return;
                    }
                    break;   // no room to wait: let it go now rather than never
                }
            if (held) {
                --held;
                in->push({0, r.ch, static_cast<std::uint8_t>(pitch), 0.0f, false});
            }
            return;
        }
    }

    void commands(Plan* p) {
        // The offs that waited a block, first.
        struckCount = 0;
        const int waiting = deferredCount;
        deferredCount = 0;
        if (p)
            for (int i = 0; i < waiting; ++i) live(*p, deferred[i].track, deferred[i].pitch, 0.0f, false);
        int t = tail.load(std::memory_order_relaxed);
        while (t != head.load(std::memory_order_acquire)) {
            const Cmd& c = queue[t];
            switch (c.kind) {
            case Cmd::Play:
                playing = true;
                endedPub.store(false, std::memory_order_relaxed);
                break;
            case Cmd::Stop:
                playing = false;
                if (p) releaseSong(*p, 0);
                break;
            case Cmd::Seek:
                if (p) {
                    releaseSong(*p, 0);
                    pos = std::max<std::int64_t>(0, std::llround(static_cast<double>(c.tick) * p->spt));
                }
                break;
            case Cmd::AllOff:
                if (p)
                    for (PlanTrack& tr : p->tracks)
                        if (tr.instr) tr.instr->releaseAll(0);
                break;
            case Cmd::StopAtEnd: stopAtEnd = c.on; break;
            case Cmd::Metronome: metronome = c.on; break;
            case Cmd::Live:
                if (p) live(*p, c.track, c.pitch, c.vel, c.on);
                break;
            }
            t = (t + 1) % kQueue;
        }
        tail.store(t, std::memory_order_release);
    }

    // Song notes due in [s0, s1), at frame `fb` + their offset into it.
    void schedule(Plan& p, std::int64_t s0, std::int64_t s1, int fb) {
        for (PlanTrack& t : p.tracks) {
            if (!t.instr) continue;
            Instr& in = *t.instr;
            auto it = std::lower_bound(t.notes.begin(), t.notes.end(), s0,
                                       [](const PlanNote& n, std::int64_t v) { return n.at < v; });
            for (; it != t.notes.end() && it->at < s1; ++it) {
                if (!in.hold({it->end, it->ch, it->pitch})) continue;
                in.push({static_cast<std::uint32_t>(fb + (it->at - s0)), it->ch, it->pitch, it->vel, true});
            }
            for (int h = 0; h < in.heldCount;) {
                const Instr::Held& hd = in.held[static_cast<std::size_t>(h)];
                if (hd.end < s1) {
                    in.push({static_cast<std::uint32_t>(fb + std::max<std::int64_t>(0, hd.end - s0)), hd.ch, hd.pitch, 0.0f, false});
                    in.held[static_cast<std::size_t>(h)] = in.held[static_cast<std::size_t>(--in.heldCount)];
                } else {
                    ++h;
                }
            }
        }
        // Audio clips straight into their track's block.
        for (PlanTrack& t : p.tracks) {
            for (const PlanClip& c : t.clips) {
                if (c.at >= s1) break;
                if (c.end <= s0) continue;
                const SampleEntry* e = c.sample.get();
                if (!e || e->state.load(std::memory_order_acquire) != 1) continue;
                const std::int64_t a = std::max(s0, c.at), b = std::min(s1, c.end);
                // Fades as drawn, and 2 ms at each edge whatever is drawn: a
                // clip cut in the middle of a waveform must not click.
                const std::int64_t edge = std::max<std::int64_t>(1, std::llround(rate * 0.002));
                for (std::int64_t s = a; s < b; ++s) {
                    const std::int64_t f = c.src + (s - c.at);
                    if (f < 0) continue;
                    if (f >= e->frames) break;
                    float g = c.gain;
                    const std::int64_t in = s - c.at, out = c.end - s;
                    if (c.fadeIn > 0 && in < c.fadeIn) g *= static_cast<float>(in) / static_cast<float>(c.fadeIn);
                    if (c.fadeOut > 0 && out < c.fadeOut) g *= static_cast<float>(out) / static_cast<float>(c.fadeOut);
                    if (in < edge) g *= static_cast<float>(in) / static_cast<float>(edge);
                    if (out < edge) g *= static_cast<float>(out) / static_cast<float>(edge);
                    const std::size_t o = static_cast<std::size_t>(fb + (s - s0)) * 2;
                    t.buf[o]     += e->data[static_cast<std::size_t>(f) * 2] * g;
                    t.buf[o + 1] += e->data[static_cast<std::size_t>(f) * 2 + 1] * g;
                }
            }
        }
        // The metronome's beats.
        if (metronome && clickCount < 8) {
            const double spb = p.beatTicks * p.spt;
            if (spb > 1.0) {
                std::int64_t k = static_cast<std::int64_t>(std::ceil(static_cast<double>(s0) / spb - 1e-9));
                for (;; ++k) {
                    const std::int64_t at = std::llround(static_cast<double>(k) * spb);
                    if (at >= s1 || clickCount >= 8) break;
                    if (at < s0) continue;
                    clicks[clickCount++] = {fb + static_cast<int>(at - s0), k % std::max(1, p.bpb) == 0};
                }
            }
        }
    }

    void block(float* const* outs, int n) {
        adopt();
        Plan* p = current;
        commands(p);
        float* out = outs[0];
        for (int k = 0; k < Engine::kMaxOutputs; ++k)
            if (outs[k]) std::fill(outs[k], outs[k] + static_cast<std::size_t>(n) * 2, 0.0f);
        if (!p) return;
        for (PlanTrack& t : p->tracks) std::fill(t.buf.begin(), t.buf.begin() + n * 2, 0.0f);

        vst3::Transport tr;
        tr.tempo       = p->bpm;
        tr.beatsPerBar = p->bpb;
        tr.beatUnit    = p->unit;
        tr.playing     = playing;
        tr.samples     = pos;
        tr.ppq         = static_cast<double>(pos) / p->spt / kPpq;
        const double barQ = static_cast<double>(p->bpb) * 4.0 / std::max(1, p->unit);
        tr.barPpq      = std::floor(tr.ppq / barQ) * barQ;
        tr.cycle       = p->loop;
        tr.cycleStart  = static_cast<double>(p->loopStart) / p->spt / kPpq;
        tr.cycleEnd    = static_cast<double>(p->loopEnd) / p->spt / kPpq;

        clickCount = 0;
        if (playing) {
            int done = 0;
            while (done < n && playing) {
                std::int64_t s1 = pos + (n - done);
                const bool wraps = p->loop && pos < p->loopEnd && s1 >= p->loopEnd;
                if (wraps) s1 = p->loopEnd;
                const bool ends = !p->loop && stopAtEnd && p->end > 0 && s1 >= p->end;
                if (ends) s1 = std::max(pos, p->end);
                schedule(*p, pos, s1, done);
                done += static_cast<int>(s1 - pos);
                pos = s1;
                if (wraps) {
                    // Round again: what the song was holding stops at the loop's end.
                    releaseSong(*p, static_cast<std::uint32_t>(std::min(done, n - 1)));
                    pos = p->loopStart;
                } else if (ends) {
                    releaseSong(*p, static_cast<std::uint32_t>(std::min(done, n - 1)));
                    playing = false;
                    endedPub.store(true, std::memory_order_relaxed);
                }
            }
        }

        for (PlanTrack& t : p->tracks) {
            if (!t.instr) continue;
            sortEvents(t.instr->ev.data(), t.instr->evCount);
            t.instr->render(t.buf.data(), n, tr);
        }

        // The mix: each track's fader and pan, glided across the block.
        for (PlanTrack& t : p->tracks) {
            TrackState& st = *t.state;
            if (!st.primed) { st.gl = t.gl; st.gr = t.gr; st.primed = true; }
            const float dl = (t.gl - st.gl) / static_cast<float>(n);
            const float dr = (t.gr - st.gr) / static_cast<float>(n);
            float gl = st.gl, gr = st.gr, peak = 0.0f;
            float* dst = t.out > 0 && t.out < Engine::kMaxOutputs && outs[t.out] ? outs[t.out] : out;
            for (int i = 0; i < n; ++i) {
                gl += dl;
                gr += dr;
                const float l = t.buf[static_cast<std::size_t>(i) * 2] * gl;
                const float r = t.buf[static_cast<std::size_t>(i) * 2 + 1] * gr;
                dst[i * 2]     += l;
                dst[i * 2 + 1] += r;
                peak = std::max(peak, std::max(std::fabs(l), std::fabs(r)));
            }
            st.gl = t.gl;
            st.gr = t.gr;
            if (peak > st.peak.load(std::memory_order_relaxed)) st.peak.store(peak, std::memory_order_relaxed);
        }

        // The click: 30 ms of sine, higher on the bar.
        int ci = 0;
        for (int i = 0; i < n; ++i) {
            while (ci < clickCount && clicks[ci].frame <= i) {
                clickT = 0.0;
                clickAccent = clicks[ci].accent;
                ++ci;
            }
            if (clickT < 0.0) continue;
            const double hz = clickAccent ? 1760.0 : 1320.0;
            const float s = static_cast<float>(std::sin(6.283185307 * hz * clickT) * std::exp(-clickT * 90.0)) *
                            (clickAccent ? 0.45f : 0.3f);
            out[i * 2] += s;
            out[i * 2 + 1] += s;
            clickT += 1.0 / rate;
            if (clickT > 0.06) clickT = -1.0;
        }

        // The master: its fader on every output, and on the song's own mix a
        // knee from 0.9 that bends an over into the last tenth instead of
        // squaring it off. The others go on into a mixer channel, which has
        // headroom of its own.
        float mp = 0.0f;
        for (int k = 0; k < Engine::kMaxOutputs; ++k) {
            float* o = outs[k];
            if (!o) continue;
            for (int i = 0; i < n * 2; ++i) {
                float v = o[i] * p->master;
                if (!std::isfinite(v)) v = 0.0f;
                mp = std::max(mp, std::fabs(v));
                if (k == 0) {
                    const float a = std::fabs(v);
                    if (a > 0.9f) v = std::copysign(0.9f + 0.1f * std::tanh((a - 0.9f) * 10.0f), v);
                }
                o[i] = v;
            }
        }
        if (mp > masterPeak.load(std::memory_order_relaxed)) masterPeak.store(mp, std::memory_order_relaxed);

        posTicks.store(static_cast<double>(pos) / p->spt, std::memory_order_relaxed);
        sptPub.store(p->spt, std::memory_order_relaxed);
        playingPub.store(playing, std::memory_order_relaxed);
    }
};

// --- The public face -----------------------------------------------------------------

Engine::Engine(double sampleRate, int maxBlock) : m(std::make_unique<Impl>()) {
    m->rate     = sampleRate > 1000.0 ? sampleRate : 48000.0;
    m->maxBlock = std::clamp(maxBlock, 32, 4096);
}

Engine::~Engine() = default;

double Engine::sampleRate() const { return m->rate; }

void Engine::setFiles(Files files) { m->files = std::move(files); }
void Engine::setSong(const Song& song) { m->build(song); }

void Engine::reloadInstrument(int trackUid) {
    m->slots.erase(trackUid);
}

void Engine::play() {
    m->freeRetired();
    m->push({Cmd::Play});
    m->playingPub.store(true, std::memory_order_relaxed);
    m->endedPub.store(false, std::memory_order_relaxed);
}

void Engine::stop() {
    m->freeRetired();
    m->push({Cmd::Stop});
    m->playingPub.store(false, std::memory_order_relaxed);
}

void Engine::seek(std::int64_t tick) {
    m->freeRetired();
    Cmd c;
    c.kind = Cmd::Seek;
    c.tick = std::max<std::int64_t>(0, tick);
    m->push(c);
    // Said at once, so a playhead dragged in the editor does not jump back
    // for the block until the audio thread has seen it.
    m->posTicks.store(static_cast<double>(c.tick), std::memory_order_relaxed);
}

bool Engine::playing() const { return m->playingPub.load(std::memory_order_relaxed); }
double Engine::position() const { return m->posTicks.load(std::memory_order_relaxed); }
double Engine::positionSeconds() const {
    return position() * m->sptPub.load(std::memory_order_relaxed) / m->rate;
}
double Engine::ticksPerSecond() const {
    return m->rate / std::max(1e-9, m->sptPub.load(std::memory_order_relaxed));
}
bool Engine::ended() const { return m->endedPub.load(std::memory_order_relaxed); }

void Engine::setStopAtEnd(bool on) {
    Cmd c;
    c.kind = Cmd::StopAtEnd;
    c.on   = on;
    m->push(c);
}

void Engine::setMetronome(bool on) {
    Cmd c;
    c.kind = Cmd::Metronome;
    c.on   = on;
    m->push(c);
}

void Engine::liveNote(int trackUid, int pitch, float velocity, bool on) {
    Cmd c;
    c.kind  = Cmd::Live;
    c.track = trackUid;
    c.pitch = std::clamp(pitch, 0, 127);
    c.vel   = velocity;
    c.on    = on;
    m->push(c);
}

void Engine::allNotesOff() { m->push({Cmd::AllOff}); }

float Engine::takePeak(int trackUid) {
    auto it = m->states.find(trackUid);
    return it == m->states.end() ? 0.0f : it->second->peak.exchange(0.0f, std::memory_order_relaxed);
}

float Engine::takeMasterPeak() { return m->masterPeak.exchange(0.0f, std::memory_order_relaxed); }

std::shared_ptr<vst3::Instance> Engine::plugin(int trackUid) const {
    auto it = m->slots.find(trackUid);
    if (it == m->slots.end()) return nullptr;
    auto* p = dynamic_cast<PluginInstr*>(it->second.instr.get());
    return p ? p->plugin : nullptr;
}

std::string Engine::instrumentError(int trackUid) const {
    auto it = m->slots.find(trackUid);
    return it == m->slots.end() ? std::string() : it->second.error;
}

void Engine::captureStates(Song& song) const {
    for (Track& t : song.tracks) {
        if (t.kind != TrackKind::Instrument || t.instrument.kind != InstrumentKind::Vst3) continue;
        if (auto p = plugin(t.uid)) t.instrument.state = p->state();
    }
}

bool Engine::samplesPending() const {
    std::lock_guard<std::mutex> lock(m->sampleMutex);
    for (const auto& [path, e] : m->samples)
        if (e->state.load(std::memory_order_acquire) == 0) return true;
    return false;
}

std::vector<std::string> Engine::sampleErrors() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lock(m->sampleMutex);
    for (const auto& [path, e] : m->samples)
        if (e->state.load(std::memory_order_acquire) == 2) out.push_back(path + ": " + e->error);
    return out;
}

void Engine::process(float* stereo, int frames) {
    float* outs[kMaxOutputs] = {};
    for (int done = 0; done < frames;) {
        const int n = std::min(m->maxBlock, frames - done);
        outs[0] = stereo + static_cast<std::size_t>(done) * 2;   // the rest null: all into it
        m->block(outs, n);
        done += n;
    }
}

void Engine::processOutputs(float* const* out, int frames) {
    float* outs[kMaxOutputs] = {};
    for (int done = 0; done < frames;) {
        const int n = std::min(m->maxBlock, frames - done);
        for (int k = 0; k < kMaxOutputs; ++k)
            outs[k] = out[k] ? out[k] + static_cast<std::size_t>(done) * 2 : nullptr;
        m->block(outs, n);
        done += n;
    }
}

const std::vector<std::string>& Engine::outputs() const { return m->outputs; }

bool Engine::render(const Song& song, const Files& files, double sampleRate, SampleData& out,
                    std::string* error, double tailSeconds, const std::function<bool(float)>& progress) {
    Bounce b;
    b.start(song, files, sampleRate, tailSeconds);
    // The files decode on the loader thread; a bounce waits for every one
    // (for thirty seconds at most, see Bounce::step).
    while (!b.done()) {
        if (!b.step(4096)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        if (progress && !progress(b.progress())) {
            if (error) *error = "cancelled";
            return false;
        }
    }
    if (error) *error = b.problems();
    out = b.take();
    return true;
}

// --- Bounce ------------------------------------------------------------------------------

struct Bounce::Impl {
    std::unique_ptr<Engine> engine;
    SampleData              out;
    std::int64_t            frames = 0, at = 0;
    std::string             problems;
    bool                    started = false;   // the files are in, the transport rolls
    Song                    copy;
    std::chrono::steady_clock::time_point began;
};

Bounce::Bounce() : m(std::make_unique<Impl>()) {}
Bounce::~Bounce() = default;

void Bounce::start(const Song& song, const Files& files, double sampleRate, double tailSeconds) {
    m = std::make_unique<Impl>();
    m->copy      = song;
    m->copy.loop = false;
    m->engine    = std::make_unique<Engine>(sampleRate, 512);
    m->engine->setFiles(files);
    m->engine->setSong(m->copy);
    m->began = std::chrono::steady_clock::now();
    const double seconds = m->copy.seconds(m->copy.length()) + std::max(0.0, tailSeconds);
    m->frames = static_cast<std::int64_t>(std::ceil(seconds * sampleRate));
    m->out.channels   = 2;
    m->out.sampleRate = static_cast<int>(std::lround(sampleRate));
    m->out.samples.assign(static_cast<std::size_t>(m->frames) * 2, 0.0f);
}

bool Bounce::step(int frames) {
    if (!m->engine || done()) return true;
    if (!m->started) {
        // Thirty seconds for the files is generous; past that, what has not
        // decoded is a problem reported, not a bounce that never ends.
        const bool waited = std::chrono::steady_clock::now() - m->began > std::chrono::seconds(30);
        if (m->engine->samplesPending() && !waited) return false;
        for (const Track& t : m->copy.tracks) {
            const std::string why = m->engine->instrumentError(t.uid);
            if (!why.empty()) m->problems += (m->problems.empty() ? "" : "; ") + t.name + ": " + why;
        }
        for (const std::string& why : m->engine->sampleErrors())
            m->problems += (m->problems.empty() ? "" : "; ") + why;
        m->engine->setStopAtEnd(true);
        m->engine->play();
        m->started = true;
    }
    const int n = static_cast<int>(std::min<std::int64_t>(std::max(frames, 1), m->frames - m->at));
    m->engine->process(m->out.samples.data() + static_cast<std::size_t>(m->at) * 2, n);
    m->at += n;
    return true;
}

bool Bounce::done() const { return m->engine && m->at >= m->frames; }
float Bounce::progress() const {
    return m->frames > 0 ? static_cast<float>(static_cast<double>(m->at) / static_cast<double>(m->frames)) : 0.0f;
}
const std::string& Bounce::problems() const { return m->problems; }

SampleData Bounce::take() {
    SampleData s = std::move(m->out);
    m->out = SampleData{};
    return s;
}

} // namespace fitzel::song
