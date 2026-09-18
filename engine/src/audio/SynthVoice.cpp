#include "fitzel/audio/SynthVoice.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "AudioInternal.hpp"

namespace fitzel {

namespace {
constexpr int kScopeFrames = 4096;   // about 85 ms at 48 kHz: a scope's worth
constexpr int kGraphBlock  = 256;
} // namespace

struct SynthVoice::Impl {
    // miniaudio reads from this like it reads from a decoded file. The base has
    // to come first: miniaudio casts the pointer it is given straight to it.
    struct Source {
        ma_data_source_base base;
        Impl*               owner = nullptr;
    };

    Source        source{};
    ma_sound      sound{};
    synth::Graph  graph;
    ma_uint32     sampleRate = 48000;
    bool          sourceOk   = false;
    bool          soundOk    = false;

    // What the audio thread last produced, for the editor's scope. Written
    // without a lock and read without one: see the header.
    float                 scope[kScopeFrames] = {};
    std::atomic<int>      scopeWrite{0};

    // Unhooked from the mixer HERE, when the Impl goes -- not in ~SynthVoice.
    // A defaulted move assignment frees the Impl it overwrites without ever
    // running ~SynthVoice on it, so an uninit that lived there was skipped
    // exactly when an editor swapped a playing voice for a rebuilt one: the
    // old voice stayed in the graph pointing at freed memory, and the next
    // audio callback crashed reading it.
    //
    // Order matters: the sound is what the mixer reads THROUGH the data source,
    // so it goes first.
    ~Impl() {
        if (soundOk)  ma_sound_uninit(&sound);
        if (sourceOk) ma_data_source_uninit(&source.base);
    }
};

namespace {

ma_result synthRead(ma_data_source* pDataSource, void* pFramesOut,
                    ma_uint64 frameCount, ma_uint64* pFramesRead) {
    auto* src = reinterpret_cast<SynthVoice::Impl::Source*>(pDataSource);
    if (!src || !src->owner) {
        if (pFramesRead) *pFramesRead = 0;
        return MA_INVALID_ARGS;
    }
    SynthVoice::Impl& im  = *src->owner;
    float*            out = static_cast<float*>(pFramesOut);
    const int         n   = static_cast<int>(frameCount);
    im.graph.process(out, n);

    // Into the scope ring, so the editor can draw what is actually playing
    // rather than a second render that might disagree with it.
    int w = im.scopeWrite.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        im.scope[w] = out[i];
        w = (w + 1) % kScopeFrames;
    }
    im.scopeWrite.store(w, std::memory_order_relaxed);

    if (pFramesRead) *pFramesRead = frameCount;
    // Never MA_AT_END: a patch is a sound that goes on until it is stopped.
    return MA_SUCCESS;
}

ma_result synthSeek(ma_data_source*, ma_uint64) {
    // A synth has no position to seek to. Reporting success rather than failure
    // keeps miniaudio's own "rewind before playing" path quiet.
    return MA_SUCCESS;
}

ma_result synthFormat(ma_data_source* pDataSource, ma_format* pFormat,
                      ma_uint32* pChannels, ma_uint32* pSampleRate,
                      ma_channel* pChannelMap, size_t channelMapCap) {
    auto* src = reinterpret_cast<SynthVoice::Impl::Source*>(pDataSource);
    if (pFormat)     *pFormat     = ma_format_f32;
    if (pChannels)   *pChannels   = 1;   // mono, so the spatializer can place it
    if (pSampleRate) *pSampleRate = src && src->owner ? src->owner->sampleRate : 48000;
    if (pChannelMap && channelMapCap > 0)
        ma_channel_map_init_standard(ma_standard_channel_map_default, pChannelMap,
                                     channelMapCap, 1);
    return MA_SUCCESS;
}

ma_result synthCursor(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }
ma_result synthLength(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }

const ma_data_source_vtable kSynthVtable = {
    synthRead, synthSeek, synthFormat, synthCursor, synthLength,
    nullptr,   // onSetLooping: nothing to loop, it never ends
    0,
};

} // namespace

SynthVoice::SynthVoice() = default;

SynthVoice::~SynthVoice() = default;   // the Impl unhooks itself, see above

SynthVoice::SynthVoice(SynthVoice&&) noexcept            = default;
SynthVoice& SynthVoice::operator=(SynthVoice&&) noexcept = default;

SynthVoice SynthVoice::fromPatch(Audio& audio, const synth::Patch& patch,
                                 std::string* error) {
    SynthVoice v;
    if (!audio.ok()) {
        if (error) *error = "no audio device";
        return v;
    }
    auto impl = std::make_unique<Impl>();
    impl->sampleRate = ma_engine_get_sample_rate(&audio.impl()->engine);

    // Compiled BEFORE anything is handed to the mixer: a patch that will not
    // compile must not become a silent voice nobody can explain.
    if (!impl->graph.compile(patch, static_cast<double>(impl->sampleRate),
                             kGraphBlock, error))
        return v;

    ma_data_source_config cfg = ma_data_source_config_init();
    cfg.vtable                = &kSynthVtable;
    impl->source.owner        = impl.get();
    if (ma_data_source_init(&cfg, &impl->source.base) != MA_SUCCESS) {
        if (error) *error = "could not create the data source";
        return v;
    }
    impl->sourceOk = true;

    if (ma_sound_init_from_data_source(&audio.impl()->engine, &impl->source.base,
                                       0, nullptr, &impl->sound) != MA_SUCCESS) {
        ma_data_source_uninit(&impl->source.base);
        impl->sourceOk = false;
        if (error) *error = "could not create the voice";
        return v;
    }
    impl->soundOk = true;
    // 2D unless the caller asks, exactly as Sound::fromFile does.
    ma_sound_set_spatialization_enabled(&impl->sound, MA_FALSE);

    v.m_impl = std::move(impl);
    return v;
}

bool SynthVoice::isValid() const { return m_impl && m_impl->soundOk; }

void SynthVoice::play() {
    if (!isValid()) return;
    // From silence: an envelope half-way through its release, or a delay line
    // still full of the last run, would start the sound in the middle of itself.
    m_impl->graph.reset();
    ma_sound_start(&m_impl->sound);
}

void SynthVoice::stop() {
    if (isValid()) ma_sound_stop(&m_impl->sound);
}

bool SynthVoice::isPlaying() const {
    return isValid() && ma_sound_is_playing(&m_impl->sound) == MA_TRUE;
}

void SynthVoice::setVolume(float volume) {
    if (isValid()) ma_sound_set_volume(&m_impl->sound, volume);
}

namespace {
const std::vector<std::string> kNoInputs;
}

const std::vector<std::string>& SynthVoice::inputNames() const {
    return m_impl ? m_impl->graph.inputNames() : kNoInputs;
}

int SynthVoice::inputIndex(const std::string& name) const {
    return m_impl ? m_impl->graph.inputIndex(name) : -1;
}

void SynthVoice::setInput(int index, float value) {
    if (m_impl) m_impl->graph.setInput(index, value);
}

void SynthVoice::setInput(const std::string& name, float value) {
    if (m_impl) m_impl->graph.setInput(name, value);
}

float SynthVoice::input(int index) const {
    return m_impl ? m_impl->graph.input(index) : 0.0f;
}

bool SynthVoice::updateParams(const synth::Patch& patch) {
    return m_impl && m_impl->graph.updateParams(patch);
}

void SynthVoice::setSpatial(bool on) {
    if (isValid())
        ma_sound_set_spatialization_enabled(&m_impl->sound, on ? MA_TRUE : MA_FALSE);
}

void SynthVoice::setPosition(float x, float y, float z) {
    if (isValid()) ma_sound_set_position(&m_impl->sound, x, y, z);
}

void SynthVoice::setVelocity(float x, float y, float z) {
    if (isValid()) ma_sound_set_velocity(&m_impl->sound, x, y, z);
}

void SynthVoice::setAttenuation(float minDist, float maxDist, float rolloff) {
    if (!isValid()) return;
    ma_sound_set_min_distance(&m_impl->sound, minDist);
    ma_sound_set_max_distance(&m_impl->sound, maxDist);
    ma_sound_set_rolloff(&m_impl->sound, rolloff);
}

void SynthVoice::setDopplerFactor(float factor) {
    if (isValid()) ma_sound_set_doppler_factor(&m_impl->sound, factor);
}

int SynthVoice::peek(float* out, int maxFrames) const {
    if (!m_impl || !out || maxFrames <= 0) return 0;
    const int n = std::min(maxFrames, kScopeFrames);
    int       r = m_impl->scopeWrite.load(std::memory_order_relaxed) - n;
    while (r < 0) r += kScopeFrames;
    for (int i = 0; i < n; ++i) {
        out[i] = m_impl->scope[r];
        r = (r + 1) % kScopeFrames;
    }
    return n;
}

} // namespace fitzel
