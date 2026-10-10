#include "fitzel/audio/Mixer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "fitzel/audio/SynthDsp.hpp"

#include "AudioInternal.hpp"

namespace fitzel {

// --- The effects' descriptions ------------------------------------------------
namespace mixfx {
namespace {

const std::vector<Param> kEq = {
    {"Low freq",  30.0f,   1000.0f,  120.0f, "%.0f Hz", true},
    {"Low gain", -18.0f,     18.0f,    0.0f, "%+.1f dB"},
    {"Mid freq", 150.0f,   8000.0f, 1000.0f, "%.0f Hz", true},
    {"Mid gain", -18.0f,     18.0f,    0.0f, "%+.1f dB"},
    {"Mid Q",      0.3f,      6.0f,    0.9f, "%.2f", true},
    {"High freq", 1500.0f, 16000.0f, 6000.0f, "%.0f Hz", true},
    {"High gain", -18.0f,    18.0f,    0.0f, "%+.1f dB"},
};
const std::vector<Param> kFilter = {
    {"Mode",       0.0f,     2.0f,    0.0f,   "%.0f", false, "Low pass|High pass|Band pass"},
    {"Cutoff",    20.0f, 20000.0f, 2000.0f,   "%.0f Hz", true},
    {"Resonance",  0.3f,    10.0f,    0.707f, "%.2f", true},
};
const std::vector<Param> kComp = {
    {"Threshold", -60.0f,    0.0f, -18.0f, "%.1f dB"},
    {"Ratio",       1.0f,   20.0f,   4.0f, "%.1f:1", true},
    {"Attack",      0.1f,  100.0f,  10.0f, "%.1f ms", true},
    {"Release",    10.0f, 1000.0f, 120.0f, "%.0f ms", true},
    {"Makeup",      0.0f,   24.0f,   0.0f, "%+.1f dB"},
};
const std::vector<Param> kDelay = {
    {"Time",      10.0f,  2000.0f,  350.0f, "%.0f ms", true},
    {"Feedback",   0.0f,     0.95f,   0.35f, "%.2f"},
    {"Tone",     500.0f, 18000.0f, 6000.0f, "%.0f Hz", true},
    {"Mix",        0.0f,     1.0f,    0.3f,  "%.2f"},
};
const std::vector<Param> kReverb = {
    {"Size",    0.0f, 1.0f, 0.6f,  "%.2f"},
    {"Damping", 0.0f, 1.0f, 0.4f,  "%.2f"},
    {"Mix",     0.0f, 1.0f, 0.35f, "%.2f"},
};
const std::vector<Param> kChorus = {
    {"Rate",  0.05f, 5.0f, 0.6f, "%.2f Hz", true},
    {"Depth", 0.1f,  8.0f, 2.0f, "%.1f ms"},
    {"Mix",   0.0f,  1.0f, 0.5f, "%.2f"},
};
const std::vector<Param> kDrive = {
    {"Shape",   0.0f,  3.0f,  0.0f, "%.0f", false, "Soft|Hard|Arctan|Fold"},
    {"Drive",   1.0f, 30.0f,  3.0f, "%.1f", true},
    {"Mix",     0.0f,  1.0f,  1.0f, "%.2f"},
    {"Output", -24.0f, 6.0f, -3.0f, "%+.1f dB"},
};

struct Info { const char* name; const char* key; const std::vector<Param>* params; };
const Info kInfo[] = {
    {"EQ", "eq", &kEq},             {"Filter", "filter", &kFilter},
    {"Compressor", "compressor", &kComp}, {"Delay", "delay", &kDelay},
    {"Reverb", "reverb", &kReverb}, {"Chorus", "chorus", &kChorus},
    {"Drive", "drive", &kDrive},
};
static_assert(sizeof(kInfo) / sizeof(kInfo[0]) == static_cast<int>(Type::Count));

const Info& info(Type t) {
    const int i = std::clamp(static_cast<int>(t), 0, static_cast<int>(Type::Count) - 1);
    return kInfo[i];
}

} // namespace

const char* name(Type t) { return info(t).name; }
const char* key(Type t)  { return info(t).key; }
const std::vector<Param>& params(Type t) { return *info(t).params; }
bool fromKey(const std::string& k, Type& out) {
    for (int i = 0; i < static_cast<int>(Type::Count); ++i)
        if (k == kInfo[i].key) { out = static_cast<Type>(i); return true; }
    return false;
}

} // namespace mixfx

namespace {

constexpr float kPi = 3.14159265358979f;
inline float dbToGain(float db) { return std::pow(10.0f, db * 0.05f); }

// --- A biquad with the RBJ cookbook's shapes, two channels of state -----------
struct Biquad2 {
    enum Mode { LowPass, HighPass, BandPass, LowShelf, HighShelf, Peak };
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    float z1[2] = {0, 0}, z2[2] = {0, 0};

    void set(Mode m, float sr, float freq, float q, float gainDb) {
        freq = std::clamp(freq, 10.0f, sr * 0.45f);
        q    = std::max(q, 0.05f);
        const float w0 = 2.0f * kPi * freq / sr;
        const float c = std::cos(w0), s = std::sin(w0);
        const float A = std::pow(10.0f, gainDb / 40.0f);
        float alpha = s / (2.0f * q);
        float nb0 = 1, nb1 = 0, nb2 = 0, na0 = 1, na1 = 0, na2 = 0;
        switch (m) {
        case LowPass:
            nb0 = (1 - c) * 0.5f; nb1 = 1 - c; nb2 = nb0;
            na0 = 1 + alpha; na1 = -2 * c; na2 = 1 - alpha; break;
        case HighPass:
            nb0 = (1 + c) * 0.5f; nb1 = -(1 + c); nb2 = nb0;
            na0 = 1 + alpha; na1 = -2 * c; na2 = 1 - alpha; break;
        case BandPass:
            nb0 = alpha; nb1 = 0; nb2 = -alpha;
            na0 = 1 + alpha; na1 = -2 * c; na2 = 1 - alpha; break;
        case Peak:
            nb0 = 1 + alpha * A; nb1 = -2 * c; nb2 = 1 - alpha * A;
            na0 = 1 + alpha / A; na1 = -2 * c; na2 = 1 - alpha / A; break;
        case LowShelf: {
            alpha = s * 0.5f * std::sqrt(2.0f);           // shelf slope 1
            const float k = 2.0f * std::sqrt(A) * alpha;
            nb0 = A * ((A + 1) - (A - 1) * c + k);
            nb1 = 2 * A * ((A - 1) - (A + 1) * c);
            nb2 = A * ((A + 1) - (A - 1) * c - k);
            na0 = (A + 1) + (A - 1) * c + k;
            na1 = -2 * ((A - 1) + (A + 1) * c);
            na2 = (A + 1) + (A - 1) * c - k; break;
        }
        case HighShelf: {
            alpha = s * 0.5f * std::sqrt(2.0f);
            const float k = 2.0f * std::sqrt(A) * alpha;
            nb0 = A * ((A + 1) + (A - 1) * c + k);
            nb1 = -2 * A * ((A - 1) + (A + 1) * c);
            nb2 = A * ((A + 1) + (A - 1) * c - k);
            na0 = (A + 1) - (A - 1) * c + k;
            na1 = 2 * ((A - 1) - (A + 1) * c);
            na2 = (A + 1) - (A - 1) * c - k; break;
        }
        }
        const float inv = 1.0f / na0;
        b0 = nb0 * inv; b1 = nb1 * inv; b2 = nb2 * inv; a1 = na1 * inv; a2 = na2 * inv;
    }
    float run(int ch, float x) {
        const float y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }
};

// --- The effects ---------------------------------------------------------------
// Interleaved float blocks; the first two channels are processed (the engine
// renders stereo, see Audio's constructor), any others pass through.
struct Effect {
    Effect(mixfx::Type t, float sampleRate) : type(t), sr(sampleRate) {
        for (const mixfx::Param& p : mixfx::params(t)) v.push_back(p.def);
    }
    virtual ~Effect() = default;
    void set(int i, float x) {
        if (i < 0 || i >= static_cast<int>(v.size())) return;
        const mixfx::Param& p = mixfx::params(type)[i];
        v[i] = std::clamp(x, p.min, p.max);
        update();
    }
    virtual void update() {}
    virtual void process(float* buf, ma_uint32 frames, ma_uint32 ch) = 0;

    mixfx::Type        type;
    float              sr;
    std::vector<float> v;
    bool               bypass = false;
};

struct EqFx : Effect {
    Biquad2 lo, mid, hi;
    explicit EqFx(float s) : Effect(mixfx::Type::Eq, s) { update(); }
    void update() override {
        lo.set(Biquad2::LowShelf, sr, v[0], 0.707f, v[1]);
        mid.set(Biquad2::Peak, sr, v[2], v[4], v[3]);
        hi.set(Biquad2::HighShelf, sr, v[5], 0.707f, v[6]);
    }
    void process(float* b, ma_uint32 n, ma_uint32 ch) override {
        const ma_uint32 m = std::min<ma_uint32>(ch, 2);
        for (ma_uint32 f = 0; f < n; ++f)
            for (ma_uint32 c = 0; c < m; ++c) {
                float& x = b[f * ch + c];
                x = hi.run(c, mid.run(c, lo.run(c, x)));
            }
    }
};

struct FilterFx : Effect {
    Biquad2 bq;
    explicit FilterFx(float s) : Effect(mixfx::Type::Filter, s) { update(); }
    void update() override {
        const int mode = static_cast<int>(std::lround(v[0]));
        bq.set(mode == 1 ? Biquad2::HighPass : mode == 2 ? Biquad2::BandPass : Biquad2::LowPass,
               sr, v[1], v[2], 0.0f);
    }
    void process(float* b, ma_uint32 n, ma_uint32 ch) override {
        const ma_uint32 m = std::min<ma_uint32>(ch, 2);
        for (ma_uint32 f = 0; f < n; ++f)
            for (ma_uint32 c = 0; c < m; ++c) b[f * ch + c] = bq.run(c, b[f * ch + c]);
    }
};

// Stereo-linked: both sides get the same gain, so the image does not lean
// toward whichever side happened to be louder.
struct CompFx : Effect {
    float env = -120.0f;   // dB
    explicit CompFx(float s) : Effect(mixfx::Type::Compressor, s) {}
    void process(float* b, ma_uint32 n, ma_uint32 ch) override {
        const float thr = v[0], ratio = v[1];
        const float att = std::exp(-1.0f / (sr * v[2] * 0.001f));
        const float rel = std::exp(-1.0f / (sr * v[3] * 0.001f));
        const float makeup = v[4];
        const ma_uint32 m = std::min<ma_uint32>(ch, 2);
        for (ma_uint32 f = 0; f < n; ++f) {
            float pk = 0.0f;
            for (ma_uint32 c = 0; c < m; ++c) pk = std::max(pk, std::fabs(b[f * ch + c]));
            const float db = pk > 1e-6f ? 20.0f * std::log10(pk) : -120.0f;
            const float k = db > env ? att : rel;
            env = db + k * (env - db);
            const float over = env - thr;
            const float gr = over > 0.0f ? over / ratio - over : 0.0f;
            const float g = dbToGain(gr + makeup);
            for (ma_uint32 c = 0; c < m; ++c) b[f * ch + c] *= g;
        }
    }
};

struct DelayFx : Effect {
    std::vector<float> line[2];
    int   write = 0;
    float tone[2] = {0, 0};
    explicit DelayFx(float s) : Effect(mixfx::Type::Delay, s) {
        const int len = static_cast<int>(s * 2.1f) + 4;
        line[0].assign(len, 0.0f); line[1].assign(len, 0.0f);
    }
    void process(float* b, ma_uint32 n, ma_uint32 ch) override {
        const int   len = static_cast<int>(line[0].size());
        const float d   = std::clamp(v[0] * 0.001f * sr, 1.0f, static_cast<float>(len - 2));
        const float fb = v[1], mix = v[3];
        const float k  = 1.0f - std::exp(-2.0f * kPi * v[2] / sr);   // one-pole on the repeats
        const ma_uint32 m = std::min<ma_uint32>(ch, 2);
        for (ma_uint32 f = 0; f < n; ++f) {
            float rp = static_cast<float>(write) - d;
            if (rp < 0) rp += static_cast<float>(len);
            const int   i0 = static_cast<int>(rp);
            const int   i1 = (i0 + 1) % len;
            const float fr = rp - static_cast<float>(i0);
            for (ma_uint32 c = 0; c < m; ++c) {
                const float x   = b[f * ch + c];
                const float wet = line[c][i0] + (line[c][i1] - line[c][i0]) * fr;
                tone[c] += k * (wet - tone[c]);
                line[c][write] = x + tone[c] * fb;
                b[f * ch + c]  = x * (1.0f - mix) + wet * mix;
            }
            if (++write >= len) write = 0;
        }
    }
};

struct ReverbFx : Effect {
    synth::Reverb rv[2];
    explicit ReverbFx(float s) : Effect(mixfx::Type::Reverb, s) {
        rv[0].prepare(s); rv[1].prepare(s);
        update();
    }
    void update() override {
        // The right side a touch smaller: two identical rooms would be mono.
        rv[0].set(v[0], v[1], v[2]);
        rv[1].set(v[0] * 0.94f, v[1], v[2]);
    }
    void process(float* b, ma_uint32 n, ma_uint32 ch) override {
        const ma_uint32 m = std::min<ma_uint32>(ch, 2);
        for (ma_uint32 f = 0; f < n; ++f)
            for (ma_uint32 c = 0; c < m; ++c) b[f * ch + c] = rv[c].process(b[f * ch + c]);
    }
};

struct ChorusFx : Effect {
    synth::Chorus ch_[2];
    explicit ChorusFx(float s) : Effect(mixfx::Type::Chorus, s) {
        ch_[0].prepare(s); ch_[1].prepare(s);
        update();
    }
    void update() override {
        ch_[0].set(v[0], v[1] * 0.001f, v[2]);
        ch_[1].set(v[0] * 1.13f, v[1] * 0.001f, v[2]);   // a slower twin: width
    }
    void process(float* b, ma_uint32 n, ma_uint32 ch) override {
        const ma_uint32 m = std::min<ma_uint32>(ch, 2);
        for (ma_uint32 f = 0; f < n; ++f)
            for (ma_uint32 c = 0; c < m; ++c) b[f * ch + c] = ch_[c].process(b[f * ch + c]);
    }
};

struct DriveFx : Effect {
    synth::Drive dr;
    float out = 1.0f;
    explicit DriveFx(float s) : Effect(mixfx::Type::Drive, s) { update(); }
    void update() override {
        dr.setShape(static_cast<synth::Drive::Shape>(std::clamp(static_cast<int>(std::lround(v[0])), 0, 3)));
        dr.set(v[1], v[2]);
        out = dbToGain(v[3]);
    }
    void process(float* b, ma_uint32 n, ma_uint32 ch) override {
        const ma_uint32 m = std::min<ma_uint32>(ch, 2);
        for (ma_uint32 f = 0; f < n; ++f)
            for (ma_uint32 c = 0; c < m; ++c) b[f * ch + c] = dr.process(b[f * ch + c]) * out;
    }
};

std::unique_ptr<Effect> makeEffect(mixfx::Type t, float sr) {
    switch (t) {
    case mixfx::Type::Eq:         return std::make_unique<EqFx>(sr);
    case mixfx::Type::Filter:     return std::make_unique<FilterFx>(sr);
    case mixfx::Type::Compressor: return std::make_unique<CompFx>(sr);
    case mixfx::Type::Delay:      return std::make_unique<DelayFx>(sr);
    case mixfx::Type::Reverb:     return std::make_unique<ReverbFx>(sr);
    case mixfx::Type::Chorus:     return std::make_unique<ChorusFx>(sr);
    case mixfx::Type::Drive:      return std::make_unique<DriveFx>(sr);
    default:                      return nullptr;
    }
}

// --- A strip: one node in miniaudio's graph -------------------------------------
struct Strip;
// What miniaudio holds: its base first (it casts the node pointer to this), and
// the way back to the strip, which is not a standard-layout type.
struct NodeShell {
    ma_node_base base;
    Strip*       owner;
};

struct Strip {
    NodeShell          node{};
    Mixer::Kind        kind = Mixer::Kind::Channel;
    int                id = 0;
    int                slot = 0;      // aux: which send output of every channel feeds it
    ma_uint32          channels = 2;
    ma_uint32          outputs = 1;
    bool               inited = false;

    std::atomic<float> gain{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<float> send[Mixer::kMaxAux + 1];
    std::atomic<bool>  pre[Mixer::kMaxAux + 1];
    std::atomic<float> peak[2];

    std::atomic_flag                     lock = ATOMIC_FLAG_INIT;
    std::vector<std::unique_ptr<Effect>> fx;

    Strip() {
        for (auto& s : send) s.store(0.0f);
        for (auto& p : pre) p.store(false);
        peak[0].store(0.0f); peak[1].store(0.0f);
    }
    void acquire() { while (lock.test_and_set(std::memory_order_acquire)) {} }
    void release() { lock.clear(std::memory_order_release); }

    void process(const float* in, float** out, ma_uint32 frames) {
        const ma_uint32 ch = channels;
        float* main = out[0];
        if (in) std::memcpy(main, in, sizeof(float) * frames * ch);
        else    std::memset(main, 0, sizeof(float) * frames * ch);

        // Inserts. Only ever TRIED here: an edit in progress costs one dry block,
        // never a stalled audio thread.
        if (!lock.test_and_set(std::memory_order_acquire)) {
            for (auto& e : fx)
                if (!e->bypass) e->process(main, frames, ch);
            lock.clear(std::memory_order_release);
        }

        // Pre-fader sends see the signal after the inserts, before fader and pan.
        for (ma_uint32 k = 1; k < outputs; ++k) {
            const float lv = send[k].load(std::memory_order_relaxed);
            if (!pre[k].load(std::memory_order_relaxed)) continue;
            float* o = out[k];
            if (lv <= 0.0f) { std::memset(o, 0, sizeof(float) * frames * ch); continue; }
            for (ma_uint32 i = 0; i < frames * ch; ++i) o[i] = main[i] * lv;
        }

        // Fader and an equal-power pan, unity in the middle.
        const float g = gain.load(std::memory_order_relaxed);
        const float p = std::clamp(pan.load(std::memory_order_relaxed), -1.0f, 1.0f);
        const float a = (p + 1.0f) * 0.25f * kPi;
        const float gl = g * std::cos(a) * 1.41421356f, gr = g * std::sin(a) * 1.41421356f;
        float pk0 = 0.0f, pk1 = 0.0f;
        for (ma_uint32 f = 0; f < frames; ++f) {
            float* fr = main + f * ch;
            if (ch >= 2) {
                fr[0] *= gl; fr[1] *= gr;
                for (ma_uint32 c = 2; c < ch; ++c) fr[c] *= g;
                pk0 = std::max(pk0, std::fabs(fr[0]));
                pk1 = std::max(pk1, std::fabs(fr[1]));
            } else {
                fr[0] *= g;
                pk0 = pk1 = std::max(pk0, std::fabs(fr[0]));
            }
        }
        if (pk0 > peak[0].load(std::memory_order_relaxed)) peak[0].store(pk0, std::memory_order_relaxed);
        if (pk1 > peak[1].load(std::memory_order_relaxed)) peak[1].store(pk1, std::memory_order_relaxed);

        // Post-fader sends follow the fader and the pan.
        for (ma_uint32 k = 1; k < outputs; ++k) {
            if (pre[k].load(std::memory_order_relaxed)) continue;
            const float lv = send[k].load(std::memory_order_relaxed);
            float* o = out[k];
            if (lv <= 0.0f) { std::memset(o, 0, sizeof(float) * frames * ch); continue; }
            for (ma_uint32 i = 0; i < frames * ch; ++i) o[i] = main[i] * lv;
        }
    }
};

void stripProcess(ma_node* node, const float** ppIn, ma_uint32* /*inCount*/,
                  float** ppOut, ma_uint32* outCount) {
    Strip* s = static_cast<NodeShell*>(node)->owner;
    s->process(ppIn ? ppIn[0] : nullptr, ppOut, *outCount);
}

// Continuous: a reverb or a delay on a bus has to keep ringing after the last
// sound into it has stopped, and miniaudio would otherwise stop calling a node
// whose inputs went quiet.
ma_node_vtable g_stripVtable = {
    stripProcess, nullptr, 1, MA_NODE_BUS_COUNT_UNKNOWN,
    MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

} // namespace

struct Mixer::Impl {
    ma_engine*                             engine = nullptr;
    float                                  sampleRate = 48000.0f;
    ma_uint32                              channels = 2;
    std::map<int, std::unique_ptr<Strip>>  strips;
    int                                    nextId = 1;
    int                                    masterId = -1;

    Strip* find(int id) {
        auto it = strips.find(id);
        return it == strips.end() ? nullptr : it->second.get();
    }
    Strip* auxAt(int slot) {
        for (auto& [id, s] : strips)
            if (s->kind == Mixer::Kind::Aux && s->slot == slot) return s.get();
        return nullptr;
    }
    ma_node* masterNode() {
        Strip* m = find(masterId);
        return m ? &m->node.base : nullptr;
    }
};

Mixer::Mixer(Audio& audio) : m_impl(std::make_unique<Impl>()) {
    if (!audio.ok()) return;
    m_impl->engine     = &audio.impl()->engine;
    m_impl->sampleRate = static_cast<float>(ma_engine_get_sample_rate(m_impl->engine));
    m_impl->channels   = ma_engine_get_channels(m_impl->engine);
    m_impl->masterId   = addStrip(Kind::Master);
}

Mixer::~Mixer() {
    if (!m_impl) return;
    // Channels and buses first, the master last: nothing is left feeding a node
    // that is already gone.
    std::vector<int> ids;
    for (auto& [id, s] : m_impl->strips) if (id != m_impl->masterId) ids.push_back(id);
    for (int id : ids) removeStrip(id);
    if (Strip* m = m_impl->find(m_impl->masterId)) {
        if (m->inited) ma_node_uninit(&m->node.base, nullptr);
        m_impl->strips.clear();
    }
}

bool Mixer::ok() const { return m_impl && m_impl->engine && m_impl->masterId >= 0; }
int  Mixer::master() const { return m_impl ? m_impl->masterId : -1; }

int Mixer::addStrip(Kind kind) {
    Impl& I = *m_impl;
    if (!I.engine) return -1;
    if (kind == Kind::Master && I.masterId >= 0) return I.masterId;

    auto s = std::make_unique<Strip>();
    s->kind     = kind;
    s->channels = I.channels;
    s->node.owner = s.get();
    if (kind == Kind::Aux) {
        for (int k = 1; k <= kMaxAux; ++k)
            if (!I.auxAt(k)) { s->slot = k; break; }
        if (s->slot == 0) return -1;   // every slot taken
    }
    s->outputs = kind == Kind::Channel ? 1 + kMaxAux : 1;

    ma_uint32 inCh[1] = {I.channels};
    ma_uint32 outCh[1 + kMaxAux];
    for (auto& c : outCh) c = I.channels;
    ma_node_config nc     = ma_node_config_init();
    nc.vtable             = &g_stripVtable;
    nc.inputBusCount      = 1;
    nc.outputBusCount     = s->outputs;
    nc.pInputChannels     = inCh;
    nc.pOutputChannels    = outCh;
    if (ma_node_init(ma_engine_get_node_graph(I.engine), &nc, nullptr, &s->node.base) != MA_SUCCESS)
        return -1;
    s->inited = true;

    // Where it goes: the master into the device, everything else into the master.
    if (kind == Kind::Master)
        ma_node_attach_output_bus(&s->node.base, 0, ma_engine_get_endpoint(I.engine), 0);
    else if (ma_node* m = I.masterNode())
        ma_node_attach_output_bus(&s->node.base, 0, m, 0);

    // The sends: every channel's output k feeds the aux bus in slot k.
    if (kind == Kind::Channel) {
        for (auto& [oid, o] : I.strips)
            if (o->kind == Kind::Aux)
                ma_node_attach_output_bus(&s->node.base, static_cast<ma_uint32>(o->slot), &o->node.base, 0);
    } else if (kind == Kind::Aux) {
        for (auto& [oid, o] : I.strips)
            if (o->kind == Kind::Channel)
                ma_node_attach_output_bus(&o->node.base, static_cast<ma_uint32>(s->slot), &s->node.base, 0);
    }

    const int id = I.nextId++;
    s->id = id;
    I.strips[id] = std::move(s);
    return id;
}

void Mixer::removeStrip(int id) {
    Impl& I = *m_impl;
    Strip* s = I.find(id);
    if (!s || id == I.masterId) return;
    if (s->kind == Kind::Aux) {
        for (auto& [oid, o] : I.strips)
            if (o->kind == Kind::Channel) {
                ma_node_detach_output_bus(&o->node.base, static_cast<ma_uint32>(s->slot));
                o->send[s->slot].store(0.0f);
            }
    }
    if (s->inited) ma_node_uninit(&s->node.base, nullptr);
    I.strips.erase(id);
}

bool Mixer::has(int id) const { return m_impl && m_impl->find(id) != nullptr; }

Mixer::Kind Mixer::kind(int id) const {
    Strip* s = m_impl ? m_impl->find(id) : nullptr;
    return s ? s->kind : Kind::Channel;
}

void Mixer::setGain(int id, float linear) {
    if (Strip* s = m_impl->find(id)) s->gain.store(std::max(0.0f, linear));
}

void Mixer::setPan(int id, float pan) {
    if (Strip* s = m_impl->find(id)) s->pan.store(std::clamp(pan, -1.0f, 1.0f));
}

void Mixer::setSend(int channel, int aux, float linear, bool preFader) {
    Strip* c = m_impl->find(channel);
    Strip* a = m_impl->find(aux);
    if (!c || !a || c->kind != Kind::Channel || a->kind != Kind::Aux) return;
    c->pre[a->slot].store(preFader);
    c->send[a->slot].store(std::max(0.0f, linear));
}

void Mixer::setInserts(int id, const std::vector<mixfx::Type>& chain) {
    Strip* s = m_impl->find(id);
    if (!s) return;
    // Build the new chain outside the lock (allocation, delay lines), keeping the
    // effects that already sit in the same place with the same type.
    std::vector<std::unique_ptr<Effect>> next(chain.size());
    s->acquire();
    for (std::size_t i = 0; i < chain.size() && i < s->fx.size(); ++i)
        if (s->fx[i] && s->fx[i]->type == chain[i]) next[i] = std::move(s->fx[i]);
    s->release();
    for (std::size_t i = 0; i < chain.size(); ++i)
        if (!next[i]) next[i] = makeEffect(chain[i], m_impl->sampleRate);
    std::vector<std::unique_ptr<Effect>> old;
    s->acquire();
    old.swap(s->fx);
    s->fx.swap(next);
    s->release();
    // `old` (what was not kept) is freed here, outside the lock.
}

void Mixer::setInsertParam(int id, int slot, int param, float value) {
    Strip* s = m_impl->find(id);
    if (!s) return;
    s->acquire();
    if (slot >= 0 && slot < static_cast<int>(s->fx.size()) && s->fx[slot]) s->fx[slot]->set(param, value);
    s->release();
}

void Mixer::setInsertBypass(int id, int slot, bool bypass) {
    Strip* s = m_impl->find(id);
    if (!s) return;
    s->acquire();
    if (slot >= 0 && slot < static_cast<int>(s->fx.size()) && s->fx[slot]) s->fx[slot]->bypass = bypass;
    s->release();
}

void Mixer::takePeak(int id, float& left, float& right) {
    left = right = 0.0f;
    if (Strip* s = m_impl->find(id)) {
        left  = s->peak[0].exchange(0.0f);
        right = s->peak[1].exchange(0.0f);
    }
}

void Mixer::playOneShot(const std::string& path, int id) {
    if (!ok()) return;
    Strip* s = m_impl->find(id);
    if (!s) s = m_impl->find(m_impl->masterId);
    ma_engine_play_sound_ex(m_impl->engine, path.c_str(), s ? &s->node.base : nullptr, 0);
}

// The one door from a voice to the desk, for every class that owns an ma_sound.
void routeToStrip(ma_sound* sound, Mixer* mixer, int strip) {
    if (!sound) return;
    ma_engine* eng = ma_sound_get_engine(sound);
    ma_node*   to  = nullptr;
    if (mixer && mixer->impl())
        if (Strip* s = mixer->impl()->find(strip)) to = &s->node.base;
    if (!to && mixer && mixer->impl())
        if (Strip* m = mixer->impl()->find(mixer->master())) to = &m->node.base;
    if (!to && eng) to = ma_engine_get_endpoint(eng);
    if (to) ma_node_attach_output_bus(sound, 0, to, 0);
}

} // namespace fitzel