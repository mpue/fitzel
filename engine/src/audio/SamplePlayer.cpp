#include "fitzel/audio/SamplePlayer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

#include "AudioInternal.hpp"

namespace fitzel {

namespace {
constexpr int kFadeIn  = 96;    // frames: 2 ms at 48 kHz
constexpr int kFadeOut = 240;   // 5 ms
} // namespace

struct SamplePlayer::Impl {
    struct Buffer {
        std::vector<float> s;
    };
    // miniaudio casts the data source pointer straight to its base: first.
    struct Source {
        ma_data_source_base base;
        Impl*               owner = nullptr;
    };

    Source    source{};
    ma_sound  sound{};
    bool      sourceOk = false, soundOk = false, started = false;
    int       channels = 1, rate = 48000;

    // --- Handed across -------------------------------------------------------
    // Every buffer ever handed over lives in `owned` until the audio thread is
    // provably done with it: it is neither the one waiting (`pending`) nor the
    // one being read (`inUse`). The audio thread only ever swaps pointers.
    std::vector<std::unique_ptr<Buffer>> owned;
    std::atomic<Buffer*> pending{nullptr};
    std::atomic<Buffer*> inUse{nullptr};

    std::atomic<int>          cmd{0};          // 1 play, 2 stop
    std::atomic<std::int64_t> reqFrom{0}, reqTo{-1};
    std::atomic<bool>         loopOn{false};
    std::atomic<std::int64_t> loopStart{0}, loopEnd{0};
    std::atomic<float>        gain{1.0f};
    std::atomic<bool>         playing{false};
    std::atomic<std::int64_t> pos{0};
    std::atomic<float>        peakL{0.0f}, peakR{0.0f};

    // --- The audio thread's own -----------------------------------------------
    Buffer*      cur      = nullptr;
    std::int64_t at       = 0;
    std::int64_t to       = -1;
    bool         running  = false;
    int          fadeIn   = 0;
    int          fadeOut  = -1;   // >= 0: counting down to silence

    void collect() {
        const Buffer* p = pending.load();
        const Buffer* u = inUse.load();
        owned.erase(std::remove_if(owned.begin(), owned.end(),
                                   [&](const std::unique_ptr<Buffer>& b) {
                                       return b.get() != p && b.get() != u;
                                   }),
                    owned.end());
    }

    ~Impl() {
        if (soundOk)  ma_sound_uninit(&sound);
        if (sourceOk) ma_data_source_uninit(&source.base);
    }
};

namespace {

ma_result spRead(ma_data_source* ds, void* framesOut, ma_uint64 frameCount, ma_uint64* framesRead) {
    auto* src = reinterpret_cast<SamplePlayer::Impl::Source*>(ds);
    float* out = static_cast<float*>(framesOut);
    const int ch = src && src->owner ? src->owner->channels : 1;
    std::fill(out, out + frameCount * ch, 0.0f);
    if (framesRead) *framesRead = frameCount;
    if (!src || !src->owner) return MA_SUCCESS;
    SamplePlayer::Impl& im = *src->owner;

    // A new buffer: published as in use BEFORE it is taken off `pending`, so the
    // game thread never sees it in neither place (see collect()).
    if (SamplePlayer::Impl::Buffer* p = im.pending.load()) {
        im.inUse.store(p);
        im.cur = p;
        SamplePlayer::Impl::Buffer* expect = p;
        im.pending.compare_exchange_strong(expect, nullptr);
    }
    switch (im.cmd.exchange(0)) {
        case 1:
            im.at      = std::max<std::int64_t>(0, im.reqFrom.load());
            im.to      = im.reqTo.load();
            im.running = true;
            im.fadeIn  = kFadeIn;
            im.fadeOut = -1;
            break;
        case 2:
            if (im.running && im.fadeOut < 0) im.fadeOut = kFadeOut;
            break;
        default: break;
    }
    if (!im.running || !im.cur) {
        im.playing.store(im.running && im.cur != nullptr);
        return MA_SUCCESS;
    }
    const std::vector<float>& s = im.cur->s;
    const std::int64_t frames = static_cast<std::int64_t>(s.size()) / ch;
    const bool loop = im.loopOn.load();
    const std::int64_t ls = im.loopStart.load(), le = std::min(im.loopEnd.load(), frames);
    const bool looping = loop && le > ls && ls >= 0;
    const std::int64_t end = (im.to < 0 || im.to > frames) ? frames : im.to;
    const float g = im.gain.load();
    float pl = 0.0f, pr = 0.0f;
    for (ma_uint64 i = 0; i < frameCount; ++i) {
        if (looping && im.at >= le) im.at = ls;   // round the loop
        if (!looping && im.at >= end) { im.running = false; break; }
        if (im.at >= frames) { im.running = false; break; }
        float k = g;
        if (im.fadeIn > 0) { k *= 1.0f - static_cast<float>(im.fadeIn) / kFadeIn; --im.fadeIn; }
        if (im.fadeOut >= 0) {
            k *= static_cast<float>(im.fadeOut) / kFadeOut;
            if (im.fadeOut-- == 0) { im.running = false; break; }
        }
        const float* f = &s[static_cast<std::size_t>(im.at * ch)];
        for (int c = 0; c < ch; ++c) out[i * ch + c] = f[c] * k;
        pl = std::max(pl, std::abs(out[i * ch]));
        pr = std::max(pr, std::abs(out[i * ch + (ch > 1 ? 1 : 0)]));
        ++im.at;
    }
    im.pos.store(im.at);
    im.playing.store(im.running);
    if (pl > im.peakL.load()) im.peakL.store(pl);
    if (pr > im.peakR.load()) im.peakR.store(pr);
    return MA_SUCCESS;
}

ma_result spSeek(ma_data_source*, ma_uint64) { return MA_SUCCESS; }

ma_result spFormat(ma_data_source* ds, ma_format* format, ma_uint32* channels, ma_uint32* rate,
                   ma_channel* map, size_t mapCap) {
    auto* src = reinterpret_cast<SamplePlayer::Impl::Source*>(ds);
    const ma_uint32 ch = src && src->owner ? static_cast<ma_uint32>(src->owner->channels) : 1;
    if (format)   *format   = ma_format_f32;
    if (channels) *channels = ch;
    if (rate)     *rate     = src && src->owner ? static_cast<ma_uint32>(src->owner->rate) : 48000;
    if (map && mapCap > 0)
        ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, ch);
    return MA_SUCCESS;
}

ma_result spCursor(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }
ma_result spLength(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }

const ma_data_source_vtable kSampleVtable = {spRead, spSeek, spFormat, spCursor, spLength,
                                             nullptr, 0};

} // namespace

SamplePlayer::SamplePlayer()  = default;
SamplePlayer::~SamplePlayer() = default;
SamplePlayer::SamplePlayer(SamplePlayer&&) noexcept            = default;
SamplePlayer& SamplePlayer::operator=(SamplePlayer&&) noexcept = default;

SamplePlayer SamplePlayer::create(Audio& audio, int channels, int sampleRate) {
    SamplePlayer p;
    if (!audio.ok() || channels <= 0 || sampleRate <= 0) return p;
    auto im = std::make_unique<Impl>();
    im->channels = channels;
    im->rate     = sampleRate;
    ma_data_source_config cfg = ma_data_source_config_init();
    cfg.vtable                = &kSampleVtable;
    im->source.owner          = im.get();
    if (ma_data_source_init(&cfg, &im->source.base) != MA_SUCCESS) return p;
    im->sourceOk = true;
    if (ma_sound_init_from_data_source(&audio.impl()->engine, &im->source.base, 0, nullptr,
                                       &im->sound) != MA_SUCCESS)
        return p;
    im->soundOk = true;
    ma_sound_set_spatialization_enabled(&im->sound, MA_FALSE);
    p.m_impl = std::move(im);
    return p;
}

bool SamplePlayer::isValid() const { return m_impl && m_impl->soundOk; }
int  SamplePlayer::channels() const { return m_impl ? m_impl->channels : 0; }
int  SamplePlayer::sampleRate() const { return m_impl ? m_impl->rate : 0; }

void SamplePlayer::setSamples(const std::vector<float>& interleaved) {
    if (!isValid()) return;
    auto b = std::make_unique<Impl::Buffer>();
    b->s = interleaved;
    Impl::Buffer* raw = b.get();
    m_impl->owned.push_back(std::move(b));
    m_impl->pending.store(raw);
    m_impl->collect();
}

void SamplePlayer::play(std::int64_t from, std::int64_t to) {
    if (!isValid()) return;
    m_impl->reqFrom.store(from);
    m_impl->reqTo.store(to);
    m_impl->cmd.store(1);
    m_impl->playing.store(true);   // until the audio thread says otherwise
    m_impl->pos.store(from);
    // The voice runs for good once started: silence costs nothing, and a sound
    // that is started and stopped would be rewound by miniaudio on every start.
    if (!m_impl->started) {
        ma_sound_start(&m_impl->sound);
        m_impl->started = true;
    }
    m_impl->collect();
}

void SamplePlayer::stop() {
    if (isValid()) m_impl->cmd.store(2);
}

bool SamplePlayer::isPlaying() const { return isValid() && m_impl->playing.load(); }

std::int64_t SamplePlayer::position() const { return isValid() ? m_impl->pos.load() : 0; }

void SamplePlayer::setLoop(bool on, std::int64_t start, std::int64_t end) {
    if (!isValid()) return;
    m_impl->loopStart.store(start);
    m_impl->loopEnd.store(end);
    m_impl->loopOn.store(on && end > start && start >= 0);
}

void SamplePlayer::setVolume(float linear) {
    if (isValid()) m_impl->gain.store(std::max(0.0f, linear));
}

void SamplePlayer::setRate(float rate) {
    if (isValid()) ma_sound_set_pitch(&m_impl->sound, std::clamp(rate, 0.1f, 4.0f));
}

void SamplePlayer::takePeak(float& left, float& right) {
    left = right = 0.0f;
    if (!isValid()) return;
    left  = m_impl->peakL.exchange(0.0f);
    right = m_impl->peakR.exchange(0.0f);
}

} // namespace fitzel
