#include "fitzel/audio/MusicPlayer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "fitzel/asset/Vfs.hpp"

#include "AudioInternal.hpp"

namespace fitzel {

namespace {

constexpr int    kTap = 8192;   // mono ring of what is sounding (~170 ms at 48 kHz)
constexpr int    kFft = 1024;
constexpr int    kSub = 32;     // frames between filter-coefficient updates
constexpr double kPi  = 3.14159265358979323846;

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// RBJ cookbook biquad with the Web Audio meanings of Q (the rhythm game this was
// written for was tuned in a browser): for low- and high-pass Q is a resonance
// in dB, for band-pass it is the plain Q. Transposed direct form II, one state
// pair per channel.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }

    float run(float x, int c) {
        const double y = b0 * x + z1[c];
        z1[c] = b1 * x - a1 * y + z2[c];
        z2[c] = b2 * x - a2 * y;
        return static_cast<float>(y);
    }

    void set(double nb0, double nb1, double nb2, double a0, double na1, double na2) {
        b0 = nb0 / a0; b1 = nb1 / a0; b2 = nb2 / a0; a1 = na1 / a0; a2 = na2 / a0;
    }

    void lowpass(double f, double qDb, double rate) {
        const double w = 2 * kPi * f / rate, c = std::cos(w);
        const double alpha = std::sin(w) / (2 * std::pow(10.0, qDb / 20.0));
        set((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
    }
    void highpass(double f, double qDb, double rate) {
        const double w = 2 * kPi * f / rate, c = std::cos(w);
        const double alpha = std::sin(w) / (2 * std::pow(10.0, qDb / 20.0));
        set((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
    }
    void bandpass(double f, double q, double rate) {
        const double w = 2 * kPi * f / rate, c = std::cos(w);
        const double alpha = std::sin(w) / (2 * q);
        set(alpha, 0, -alpha, 1 + alpha, -2 * c, 1 - alpha);
    }
    // Shelf slope 1, as Web Audio's lowshelf.
    void lowshelf(double f, double gainDb, double rate) {
        const double A = std::pow(10.0, gainDb / 40.0), sA = std::sqrt(A);
        const double w = 2 * kPi * f / rate, c = std::cos(w);
        const double alpha = std::sin(w) / 2 * std::sqrt(2.0);
        set(A * ((A + 1) - (A - 1) * c + 2 * sA * alpha),
            2 * A * ((A - 1) - (A + 1) * c),
            A * ((A + 1) - (A - 1) * c - 2 * sA * alpha),
            (A + 1) + (A - 1) * c + 2 * sA * alpha,
            -2 * ((A - 1) + (A + 1) * c),
            (A + 1) + (A - 1) * c - 2 * sA * alpha);
    }
};

// In-place radix-2 FFT, n a power of two.
void fft(std::vector<double>& re, std::vector<double>& im) {
    const std::size_t n = re.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * kPi / static_cast<double>(len);
        const double wr = std::cos(ang), wi = std::sin(ang);
        for (std::size_t i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::size_t a = i + k, b = a + len / 2;
                const double tr = re[b] * cr - im[b] * ci, ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
                const double nr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = nr;
            }
        }
    }
}

} // namespace

struct MusicPlayer::Impl {
    // miniaudio casts the data source pointer straight to its base: first member.
    struct Source {
        ma_data_source_base base;
        Impl*               owner = nullptr;
    };

    Source    source{};
    ma_sound  sound{};
    bool      sourceOk = false;
    bool      soundOk  = false;
    ma_uint32 rate     = 48000;
    double    latency  = 0.03;   // what the device buffers after a block is rendered

    // --- Owned by whoever holds `lock`. The game thread takes it to load and
    // seek; the audio thread only TRIES it and plays silence for one block if it
    // cannot have it, so the mixer never waits on the game.
    std::mutex                lock;
    std::vector<std::uint8_t> bytes;
    ma_decoder                dec{};
    bool                      decOk  = false;
    double                    length = 0.0;
    std::int64_t              pos    = 0;       // next frame to render, < 0 = lead-in
    bool                      ended  = false;
    Biquad                    lp, shelfF;
    float                     cut = 20000.0f, gain = 1.0f, shelf = 0.0f;
    double                    fadeGain = 1.0, fadeStep = 0.0;

    // --- Game thread -> audio thread.
    std::atomic<float> tCut{20000.0f}, tGain{1.0f}, tShelf{0.0f}, tTc{0.12f};
    std::atomic<bool>  snap{true};
    std::atomic<float> fadeReq{0.0f};   // > 0: start a fade of that many seconds
    std::atomic<bool>  fadeDone{false};

    // --- Audio thread -> game thread: the block clock, under a sequence lock.
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::int64_t>  blockPos{0}, blockStamp{0};

    // --- Game thread only.
    enum class State { Stopped, Playing, Paused };
    State        state      = State::Stopped;
    double       frozen     = 0.0;    // the time while nothing new is known
    std::int64_t startedNs  = 0;      // blocks older than this belong to before
    bool         anchored   = false;
    double       anchorRaw  = 0.0;
    std::int64_t anchorNs   = 0;
    std::vector<float> smoothed = std::vector<float>(kFft / 2, 0.0f);

    float            tap[kTap] = {};
    std::atomic<int> tapWrite{0};

    ~Impl() {
        if (soundOk)  ma_sound_uninit(&sound);
        if (sourceOk) ma_data_source_uninit(&source.base);
        if (decOk)    ma_decoder_uninit(&dec);
    }

    void publish(std::int64_t p, std::int64_t stamp) {
        seq.fetch_add(1);
        blockPos.store(p);
        blockStamp.store(stamp);
        seq.fetch_add(1);
    }
    void readClock(std::int64_t& p, std::int64_t& stamp) const {
        for (;;) {
            const std::uint32_t s1 = seq.load();
            if (s1 & 1u) continue;
            p     = blockPos.load();
            stamp = blockStamp.load();
            if (seq.load() == s1) return;
        }
    }

    void updateFilters(bool instant, int frames) {
        const float tc = tTc.load();
        float k = 1.0f;
        if (!instant && tc > 0.0f)
            k = 1.0f - std::exp(-static_cast<float>(frames) / (tc * static_cast<float>(rate)));
        const float maxCut = 0.45f * static_cast<float>(rate);
        const float target = std::clamp(tCut.load(), 20.0f, maxCut);
        cut   += (target - cut) * k;
        gain  += (tGain.load() - gain) * k;
        // The shelf moves at half the speed, as the game this was made for had it.
        shelf += (tShelf.load() - shelf) * (instant ? 1.0f : k * 0.5f);
        lp.lowpass(cut, 0.9, rate);
        shelfF.lowshelf(120.0, shelf, rate);
    }
};

namespace {

ma_result musicRead(ma_data_source* pDataSource, void* pFramesOut,
                    ma_uint64 frameCount, ma_uint64* pFramesRead) {
    auto* src = reinterpret_cast<MusicPlayer::Impl::Source*>(pDataSource);
    float* out = static_cast<float*>(pFramesOut);
    if (pFramesRead) *pFramesRead = frameCount;
    if (!src || !src->owner) {
        std::memset(out, 0, sizeof(float) * 2 * frameCount);
        return MA_SUCCESS;
    }
    MusicPlayer::Impl& im = *src->owner;
    std::unique_lock<std::mutex> guard(im.lock, std::try_to_lock);
    if (!guard.owns_lock() || !im.decOk) {
        std::memset(out, 0, sizeof(float) * 2 * frameCount);
        return MA_SUCCESS;
    }
    const std::int64_t stamp = nowNs();
    const std::int64_t start = im.pos;

    // --- The song (or the silence before and after it) ---
    ma_uint64 done = 0;
    while (done < frameCount) {
        float*          dst  = out + done * 2;
        const ma_uint64 want = frameCount - done;
        if (im.pos < 0 || im.ended) {
            const ma_uint64 take = im.pos < 0
                ? std::min<ma_uint64>(want, static_cast<ma_uint64>(-im.pos)) : want;
            std::memset(dst, 0, sizeof(float) * 2 * take);
            im.pos += static_cast<std::int64_t>(take);
            done   += take;
            continue;
        }
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&im.dec, dst, want, &got);
        if (got == 0) { im.ended = true; continue; }
        im.pos += static_cast<std::int64_t>(got);
        done   += got;
    }

    // --- The analyser's tap: before the filter, like a meter on the source ---
    int w = im.tapWrite.load(std::memory_order_relaxed);
    for (ma_uint64 i = 0; i < frameCount; ++i) {
        im.tap[w] = 0.5f * (out[i * 2] + out[i * 2 + 1]);
        w = (w + 1) % kTap;
    }
    im.tapWrite.store(w, std::memory_order_relaxed);

    // --- Fade ---
    const float fr = im.fadeReq.exchange(0.0f);
    if (fr > 0.0f) im.fadeStep = im.fadeGain / (static_cast<double>(fr) * im.rate);

    // --- Filter, shelf, gain ---
    const bool instant = im.snap.exchange(false);
    for (ma_uint64 i = 0; i < frameCount; i += kSub) {
        const ma_uint64 end = std::min<ma_uint64>(frameCount, i + kSub);
        im.updateFilters(instant && i == 0, static_cast<int>(end - i));
        for (ma_uint64 j = i; j < end; ++j) {
            if (im.fadeStep > 0.0) {
                im.fadeGain = std::max(0.0, im.fadeGain - im.fadeStep);
                if (im.fadeGain <= 0.0) { im.fadeStep = 0.0; im.fadeDone.store(true); }
            }
            const float g = im.gain * static_cast<float>(im.fadeGain);
            for (int c = 0; c < 2; ++c) {
                float& s = out[j * 2 + c];
                s = im.shelfF.run(im.lp.run(s, c), c) * g;
            }
        }
    }

    im.publish(start, stamp);
    return MA_SUCCESS;
}

ma_result musicSeek(ma_data_source*, ma_uint64) { return MA_SUCCESS; }

ma_result musicFormat(ma_data_source* pDataSource, ma_format* pFormat,
                      ma_uint32* pChannels, ma_uint32* pSampleRate,
                      ma_channel* pChannelMap, size_t channelMapCap) {
    auto* src = reinterpret_cast<MusicPlayer::Impl::Source*>(pDataSource);
    if (pFormat)     *pFormat     = ma_format_f32;
    if (pChannels)   *pChannels   = 2;
    if (pSampleRate) *pSampleRate = src && src->owner ? src->owner->rate : 48000;
    if (pChannelMap && channelMapCap > 0)
        ma_channel_map_init_standard(ma_standard_channel_map_default, pChannelMap,
                                     channelMapCap, 2);
    return MA_SUCCESS;
}

ma_result musicCursor(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }
ma_result musicLength(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }

const ma_data_source_vtable kMusicVtable = {
    musicRead, musicSeek, musicFormat, musicCursor, musicLength, nullptr, 0,
};

} // namespace

MusicPlayer::MusicPlayer()  = default;
MusicPlayer::~MusicPlayer() = default;   // the Impl unhooks itself
MusicPlayer::MusicPlayer(MusicPlayer&&) noexcept            = default;
MusicPlayer& MusicPlayer::operator=(MusicPlayer&&) noexcept = default;

MusicPlayer MusicPlayer::create(Audio& audio, std::string* error) {
    MusicPlayer p;
    if (!audio.ok()) {
        if (error) *error = "no audio device";
        return p;
    }
    auto impl  = std::make_unique<Impl>();
    ma_engine& eng = audio.impl()->engine;
    impl->rate = ma_engine_get_sample_rate(&eng);
    if (ma_device* dev = ma_engine_get_device(&eng)) {
        const double frames = static_cast<double>(dev->playback.internalPeriodSizeInFrames) *
                              std::max<ma_uint32>(1, dev->playback.internalPeriods);
        const double r = dev->playback.internalSampleRate ? dev->playback.internalSampleRate
                                                          : impl->rate;
        if (frames > 0) impl->latency = std::clamp(frames / r, 0.005, 0.25);
    }

    ma_data_source_config cfg = ma_data_source_config_init();
    cfg.vtable                = &kMusicVtable;
    impl->source.owner        = impl.get();
    if (ma_data_source_init(&cfg, &impl->source.base) != MA_SUCCESS) {
        if (error) *error = "could not create the data source";
        return p;
    }
    impl->sourceOk = true;
    if (ma_sound_init_from_data_source(&eng, &impl->source.base, 0, nullptr,
                                       &impl->sound) != MA_SUCCESS) {
        if (error) *error = "could not create the voice";
        return p;
    }
    impl->soundOk = true;
    ma_sound_set_spatialization_enabled(&impl->sound, MA_FALSE);
    p.m_impl = std::move(impl);
    return p;
}

bool MusicPlayer::isValid() const { return m_impl && m_impl->soundOk; }

bool MusicPlayer::load(const std::string& path, std::string* error) {
    if (!isValid()) {
        if (error) *error = "no music voice";
        return false;
    }
    Impl& im = *m_impl;
    std::vector<std::uint8_t> data = vfs::read(path);
    if (data.empty()) {
        if (error) *error = "cannot read '" + path + "'";
        return false;
    }
    stop();
    std::lock_guard<std::mutex> guard(im.lock);
    if (im.decOk) { ma_decoder_uninit(&im.dec); im.decOk = false; }
    im.bytes  = std::move(data);
    im.length = 0.0;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 2, im.rate);
    if (ma_decoder_init_memory(im.bytes.data(), im.bytes.size(), &cfg, &im.dec) != MA_SUCCESS) {
        im.bytes.clear();
        if (error) *error = "cannot decode '" + path + "'";
        return false;
    }
    im.decOk = true;
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&im.dec, &frames) == MA_SUCCESS)
        im.length = static_cast<double>(frames) / im.rate;
    im.pos   = 0;
    im.ended = false;
    return true;
}

bool MusicPlayer::loaded() const { return m_impl && m_impl->decOk; }
double MusicPlayer::duration() const { return m_impl ? m_impl->length : 0.0; }

void MusicPlayer::play(double fromSec) {
    if (!isValid() || !m_impl->decOk) return;
    Impl& im = *m_impl;
    {
        std::lock_guard<std::mutex> guard(im.lock);
        const std::int64_t f = std::llround(fromSec * im.rate);
        ma_decoder_seek_to_pcm_frame(&im.dec, f > 0 ? static_cast<ma_uint64>(f) : 0);
        im.pos      = f;
        im.ended    = false;
        im.fadeGain = 1.0;
        im.fadeStep = 0.0;
        im.lp.reset();
        im.shelfF.reset();
        im.fadeReq.store(0.0f);
        im.fadeDone.store(false);
    }
    im.frozen    = fromSec - im.latency;
    im.startedNs = nowNs();
    im.anchored  = false;
    im.state     = Impl::State::Playing;
    ma_sound_start(&im.sound);
}

void MusicPlayer::stop() {
    if (!isValid()) return;
    ma_sound_stop(&m_impl->sound);
    m_impl->state = Impl::State::Stopped;
}

void MusicPlayer::pause() {
    if (!isValid() || m_impl->state != Impl::State::Playing) return;
    m_impl->frozen = time();
    ma_sound_stop(&m_impl->sound);
    m_impl->state = Impl::State::Paused;
}

void MusicPlayer::resume() {
    if (!isValid() || m_impl->state != Impl::State::Paused) return;
    m_impl->startedNs = nowNs();
    m_impl->anchored  = false;
    m_impl->state     = Impl::State::Playing;
    ma_sound_start(&m_impl->sound);
}

bool MusicPlayer::playing() const { return m_impl && m_impl->state == Impl::State::Playing; }
bool MusicPlayer::paused() const { return m_impl && m_impl->state == Impl::State::Paused; }

void MusicPlayer::fadeOut(double sec) {
    if (!isValid()) return;
    m_impl->fadeReq.store(static_cast<float>(std::max(0.01, sec)));
}

double MusicPlayer::time() {
    if (!m_impl) return 0.0;
    Impl& im = *m_impl;
    if (im.state == Impl::State::Playing && im.fadeDone.load()) {
        ma_sound_stop(&im.sound);
        im.state = Impl::State::Stopped;
    }
    if (im.state != Impl::State::Playing) return im.frozen;

    std::int64_t p = 0, stamp = 0;
    im.readClock(p, stamp);
    if (stamp < im.startedNs) return im.frozen;   // no block rendered since (re)start

    const std::int64_t now = nowNs();
    const double raw = static_cast<double>(p) / im.rate + (now - stamp) * 1e-9 - im.latency;
    double out = raw;
    if (!im.anchored) {
        im.anchored  = true;
        im.anchorRaw = raw;
        im.anchorNs  = now;
    } else {
        // Run on the steady clock, pulled gently towards the audio clock; a big
        // disagreement (a hitch, a device change) is taken at once instead.
        const double est = im.anchorRaw + (now - im.anchorNs) * 1e-9;
        const double err = raw - est;
        if (std::abs(err) > 0.04) {
            im.anchorRaw = raw;
            im.anchorNs  = now;
        } else {
            im.anchorRaw += err * 0.05;
            out = est + err * 0.05;
        }
    }
    // Never backwards by a hair: a note judged at t must not be judged again.
    if (out < im.frozen && im.frozen - out < 0.05) out = im.frozen;
    im.frozen = out;
    return out;
}

void MusicPlayer::setFilter(float cutoffHz, float gain, float shelfDb, float smoothSec) {
    if (!m_impl) return;
    m_impl->tCut.store(cutoffHz);
    m_impl->tGain.store(gain);
    m_impl->tShelf.store(shelfDb);
    m_impl->tTc.store(std::max(0.0f, smoothSec));
    if (smoothSec <= 0.0f) m_impl->snap.store(true);
}

void MusicPlayer::setVolume(float volume) {
    if (isValid()) ma_sound_set_volume(&m_impl->sound, std::max(0.0f, volume));
}

int MusicPlayer::sampleRate() const { return m_impl ? static_cast<int>(m_impl->rate) : 48000; }

namespace {
// The kFft samples that are sounding NOW: the tap runs ahead of the speakers by
// the device's buffering.
void heardWindow(const MusicPlayer::Impl& im, const float* tap, int write, float* out) {
    int r = write - static_cast<int>(im.latency * im.rate) - kFft;
    while (r < 0) r += kTap;
    for (int i = 0; i < kFft; ++i) out[i] = tap[(r + i) % kTap];
}
} // namespace

void MusicPlayer::spectrum(std::vector<float>& out) {
    out.assign(kFft / 2, 0.0f);
    if (!m_impl || m_impl->state != Impl::State::Playing) return;
    Impl& im = *m_impl;
    float win[kFft];
    heardWindow(im, im.tap, im.tapWrite.load(std::memory_order_relaxed), win);
    std::vector<double> re(kFft), imag(kFft, 0.0);
    for (int i = 0; i < kFft; ++i) {
        const double a = 2 * kPi * i / kFft;
        re[i] = win[i] * (0.42 - 0.5 * std::cos(a) + 0.08 * std::cos(2 * a));
    }
    fft(re, imag);
    for (int k = 0; k < kFft / 2; ++k) {
        const float mag = static_cast<float>(std::sqrt(re[k] * re[k] + imag[k] * imag[k]) / kFft);
        float& s = im.smoothed[k];
        s = 0.3f * s + 0.7f * mag;
        const float db = 20.0f * std::log10(std::max(s, 1e-12f));
        out[k] = std::clamp((db + 100.0f) / 70.0f, 0.0f, 1.0f);
    }
}

void MusicPlayer::waveform(std::vector<float>& out, int n) {
    n = std::clamp(n, 1, kFft);
    out.assign(n, 0.0f);
    if (!m_impl || m_impl->state != Impl::State::Playing) return;
    float win[kFft];
    heardWindow(*m_impl, m_impl->tap, m_impl->tapWrite.load(std::memory_order_relaxed), win);
    for (int k = 0; k < n; ++k) out[k] = win[k * kFft / n];
}

// --- Analysis ----------------------------------------------------------------

bool music::analyzeBands(const std::string& path, Bands& out, std::string* error) {
    std::vector<std::uint8_t> data = vfs::read(path);
    if (data.empty()) {
        if (error) *error = "cannot read '" + path + "'";
        return false;
    }
    const int rate = out.rate;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, static_cast<ma_uint32>(rate));
    ma_decoder dec;
    if (ma_decoder_init_memory(data.data(), data.size(), &cfg, &dec) != MA_SUCCESS) {
        if (error) *error = "cannot decode '" + path + "'";
        return false;
    }
    std::vector<float> pcm;
    std::vector<float> chunk(65536);
    for (;;) {
        ma_uint64 got = 0;
        ma_decoder_read_pcm_frames(&dec, chunk.data(), chunk.size(), &got);
        if (got == 0) break;
        pcm.insert(pcm.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(got));
    }
    ma_decoder_uninit(&dec);

    const int hop = out.hop;
    const long long nFrames = static_cast<long long>(pcm.size() / hop) - 1;
    if (nFrames < 16) {
        if (error) *error = "song too short";
        return false;
    }
    out.duration = static_cast<double>(pcm.size()) / rate;

    Biquad low1, low2, mid, high;
    low1.lowpass(140, 0.8, rate);
    low2.lowpass(140, 0.8, rate);
    mid.bandpass(1800, 0.6, rate);
    high.highpass(7000, 0.7, rate);

    std::vector<float> band[3];
    for (auto& b : band) b.resize(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); ++i) {
        const float x = pcm[i];
        band[0][i] = low2.run(low1.run(x, 0), 0);
        band[1][i] = mid.run(x, 0);
        band[2][i] = high.run(x, 0);
    }

    for (int b = 0; b < 3; ++b) {
        std::vector<float>& rms  = out.rms[b];
        std::vector<float>& flux = out.flux[b];
        rms.assign(static_cast<std::size_t>(nFrames), 0.0f);
        flux.assign(static_cast<std::size_t>(nFrames), 0.0f);
        std::vector<double> env(static_cast<std::size_t>(nFrames));
        for (long long f = 0; f < nFrames; ++f) {
            double s = 0;
            const float* p = band[b].data() + f * hop;
            for (int i = 0; i < hop; ++i) s += static_cast<double>(p[i]) * p[i];
            rms[f] = static_cast<float>(std::sqrt(s / hop));
            env[f] = std::log(1.0 + 200.0 * rms[f]);
        }
        double mean = 0;
        for (long long f = 1; f < nFrames; ++f) {
            flux[f] = static_cast<float>(std::max(0.0, env[f] - env[f - 1]));
            mean += flux[f];
        }
        mean = mean / static_cast<double>(nFrames) + 1e-9;
        for (auto& v : flux) v = static_cast<float>(v / mean);
    }
    return true;
}

} // namespace fitzel
