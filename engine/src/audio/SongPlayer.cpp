#include "fitzel/audio/SongPlayer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>

#include "AudioInternal.hpp"

namespace fitzel {

namespace {
constexpr int       kOutputs = song::Engine::kMaxOutputs;
constexpr ma_uint32 kScratch = 2048;   // frames, for a graph that is not stereo
} // namespace

struct SongPlayer::Impl {
    // What miniaudio holds: its base first (it casts the node pointer to it).
    struct Node {
        ma_node_base base;
        Impl*        owner = nullptr;
    };

    std::unique_ptr<song::Engine> engine;
    ma_engine* graphEngine = nullptr;
    Node       node{};
    bool       nodeOk = false;
    ma_uint32  channels = 2;   // what the graph runs at; the song is stereo
    std::vector<float> scratch[kOutputs];   // only when the graph is not stereo

    // The wiring, as last done (main thread).
    Mixer*      routedMixer = nullptr;
    int         routedStrip[kOutputs];
    std::string routedName[kOutputs];
    float       volume = 1.0f;

    // The clock between blocks: where the last block started and ended, and
    // when it was rendered.
    std::atomic<double>       blockFrom{0.0}, blockTo{0.0};
    std::atomic<std::int64_t> blockAt{0};

    Impl() { std::fill(std::begin(routedStrip), std::end(routedStrip), -2); }
    // The node leaves the graph before the engine it reads goes -- and when
    // the Impl does, not the SongPlayer (a defaulted move assignment frees the
    // Impl it overwrites without running the outer destructor).
    ~Impl() {
        if (nodeOk) ma_node_uninit(&node.base, nullptr);
        engine.reset();
    }
};

namespace {

using Impl = SongPlayer::Impl;

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void songProcess(ma_node* node, const float**, ma_uint32*, float** out, ma_uint32* frameCount) {
    Impl& im = *reinterpret_cast<Impl::Node*>(node)->owner;
    const ma_uint32 n = *frameCount;
    if (!im.engine) {
        for (int k = 0; k < kOutputs; ++k) std::fill(out[k], out[k] + n * im.channels, 0.0f);
        return;
    }
    const double from = im.engine->position();
    if (im.channels == 2) {
        im.engine->processOutputs(out, static_cast<int>(n));
    } else {
        // A graph of another width: the song in stereo, a piece at a time
        // (the scratch is sized when the player is made), laid out on it.
        float* st[kOutputs];
        for (int k = 0; k < kOutputs; ++k) st[k] = im.scratch[k].data();
        for (ma_uint32 done = 0; done < n;) {
            const ma_uint32 m = std::min<ma_uint32>(n - done, kScratch);
            im.engine->processOutputs(st, static_cast<int>(m));
            for (int k = 0; k < kOutputs; ++k)
                for (ma_uint32 f = 0; f < m; ++f) {
                    float* o = out[k] + (done + f) * im.channels;
                    if (im.channels == 1) { o[0] = 0.5f * (st[k][f * 2] + st[k][f * 2 + 1]); continue; }
                    o[0] = st[k][f * 2];
                    o[1] = st[k][f * 2 + 1];
                    for (ma_uint32 c = 2; c < im.channels; ++c) o[c] = 0.0f;
                }
            done += m;
        }
    }
    im.blockFrom.store(from, std::memory_order_relaxed);
    im.blockTo.store(im.engine->position(), std::memory_order_relaxed);
    im.blockAt.store(nowNs(), std::memory_order_relaxed);
}

// Continuous, with no input: on a node of several outputs miniaudio calls the
// callback only when it is told to run without input to wait for -- without
// these it rendered its first block and then nothing.
ma_node_vtable kSongNode = {songProcess, nullptr, 0, static_cast<ma_uint8>(kOutputs),
                            MA_NODE_FLAG_CONTINUOUS_PROCESSING | MA_NODE_FLAG_ALLOW_NULL_INPUT};

} // namespace

SongPlayer::SongPlayer()  = default;
SongPlayer::~SongPlayer() = default;
SongPlayer::SongPlayer(SongPlayer&&) noexcept            = default;
SongPlayer& SongPlayer::operator=(SongPlayer&&) noexcept = default;

SongPlayer SongPlayer::create(Audio& audio, std::string* error) {
    SongPlayer p;
    if (!audio.ok()) {
        if (error) *error = "no audio device";
        return p;
    }
    auto impl         = std::make_unique<Impl>();
    impl->graphEngine = &audio.impl()->engine;
    impl->channels    = ma_engine_get_channels(impl->graphEngine);
    impl->engine      = std::make_unique<song::Engine>(static_cast<double>(ma_engine_get_sample_rate(impl->graphEngine)), 512);
    impl->node.owner  = impl.get();
    if (impl->channels != 2)
        for (auto& sc : impl->scratch) sc.assign(static_cast<std::size_t>(kScratch) * 2, 0.0f);

    ma_uint32 outCh[kOutputs];
    std::fill(std::begin(outCh), std::end(outCh), impl->channels);
    ma_node_config nc  = ma_node_config_init();
    nc.vtable          = &kSongNode;
    nc.pOutputChannels = outCh;
    nc.initialState    = ma_node_state_stopped;
    if (ma_node_init(ma_engine_get_node_graph(impl->graphEngine), &nc, nullptr, &impl->node.base) != MA_SUCCESS) {
        if (error) *error = "could not create the voice";
        return p;
    }
    impl->nodeOk = true;
    p.m_impl = std::move(impl);
    return p;
}

bool SongPlayer::isValid() const { return m_impl && m_impl->nodeOk; }

song::Engine&       SongPlayer::engine() { return *m_impl->engine; }
const song::Engine& SongPlayer::engine() const { return *m_impl->engine; }

void SongPlayer::route(Mixer& mixer, int songStrip, const std::function<int(const std::string&)>& stripOf) {
    if (!isValid()) return;
    Impl& im = *m_impl;
    const std::vector<std::string>& names = im.engine->outputs();
    const bool newMixer = im.routedMixer != &mixer;
    im.routedMixer = &mixer;
    for (int k = 0; k < kOutputs; ++k) {
        const std::string name = k < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(k)] : std::string();
        if (k > 0 && name.empty()) {
            // An unused slot hangs on nothing.
            if (im.routedStrip[k] != -1) ma_node_detach_output_bus(&im.node.base, static_cast<ma_uint32>(k));
            im.routedStrip[k] = -1;
            im.routedName[k].clear();
            continue;
        }
        const int strip = k == 0 ? songStrip : (stripOf ? stripOf(name) : songStrip);
        if (!newMixer && im.routedStrip[k] == strip && im.routedName[k] == name) continue;
        routeNodeToStrip(&im.node.base, static_cast<ma_uint32>(k), im.graphEngine, &mixer, strip);
        ma_node_set_output_bus_volume(&im.node.base, static_cast<ma_uint32>(k), im.volume);
        im.routedStrip[k] = strip;
        im.routedName[k]  = name;
    }
}

void SongPlayer::start() {
    if (isValid() && !isRunning()) ma_node_set_state(&m_impl->node.base, ma_node_state_started);
}

void SongPlayer::stop() {
    if (!isValid()) return;
    m_impl->engine->stop();
    m_impl->engine->allNotesOff();
    ma_node_set_state(&m_impl->node.base, ma_node_state_stopped);
}

bool SongPlayer::isRunning() const {
    return isValid() && ma_node_get_state(&m_impl->node.base) == ma_node_state_started;
}

void SongPlayer::setVolume(float v) {
    if (!isValid()) return;
    v = std::max(0.0f, v);
    if (v == m_impl->volume) return;
    m_impl->volume = v;
    for (int k = 0; k < kOutputs; ++k) ma_node_set_output_bus_volume(&m_impl->node.base, static_cast<ma_uint32>(k), v);
}

double SongPlayer::ticks() const {
    if (!isValid()) return 0.0;
    const Impl& im = *m_impl;
    const double pos = im.engine->position();
    if (!im.engine->playing() || !isRunning()) return pos;
    const double from = im.blockFrom.load(std::memory_order_relaxed);
    const double to   = im.blockTo.load(std::memory_order_relaxed);
    // A loop wrapped or the playhead was moved inside the block: no glide.
    if (to < from) return to;
    // Moved since that block (a seek says where at once): where it was moved
    // to. Further than a block ahead, or behind where the block began.
    if (pos < from - 1.0 || pos > to + im.engine->ticksPerSecond() * 0.1) return pos;
    // From where the last block began, on at the song's pace, never past where
    // the audio thread has already got to.
    const double secs = static_cast<double>(nowNs() - im.blockAt.load(std::memory_order_relaxed)) * 1e-9;
    const double est  = from + secs * im.engine->ticksPerSecond();
    return std::clamp(est, from, to);
}

} // namespace fitzel
