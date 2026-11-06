#include "fitzel/audio/SongPlayer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "AudioInternal.hpp"

namespace fitzel {

struct SongPlayer::Impl {
    // miniaudio reads from this like a decoded file; the base comes first,
    // miniaudio casts the pointer straight to it.
    struct Source {
        ma_data_source_base base;
        Impl*               owner = nullptr;
    };

    std::unique_ptr<song::Engine> engine;
    Source    source{};
    ma_sound  sound{};
    ma_uint32 sampleRate = 48000;
    bool      sourceOk = false, soundOk = false;

    // The clock between blocks: where the last block started and ended, and
    // when it was rendered.
    std::atomic<double>       blockFrom{0.0}, blockTo{0.0};
    std::atomic<std::int64_t> blockAt{0};

    // The voice leaves the mixer before the engine it reads from goes -- and
    // when the Impl does, not the SongPlayer (a defaulted move assignment
    // frees the Impl it overwrites without running the outer destructor).
    ~Impl() {
        if (soundOk)  ma_sound_uninit(&sound);
        if (sourceOk) ma_data_source_uninit(&source.base);
        engine.reset();
    }
};

namespace {

using Impl = SongPlayer::Impl;

std::int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

ma_result songRead(ma_data_source* ds, void* framesOut, ma_uint64 frameCount, ma_uint64* framesRead) {
    auto* src = reinterpret_cast<Impl::Source*>(ds);
    if (!src || !src->owner || !src->owner->engine) {
        if (framesRead) *framesRead = 0;
        return MA_INVALID_ARGS;
    }
    Impl& im = *src->owner;
    const double from = im.engine->position();
    im.engine->process(static_cast<float*>(framesOut), static_cast<int>(frameCount));
    im.blockFrom.store(from, std::memory_order_relaxed);
    im.blockTo.store(im.engine->position(), std::memory_order_relaxed);
    im.blockAt.store(nowNs(), std::memory_order_relaxed);
    if (framesRead) *framesRead = frameCount;
    return MA_SUCCESS;   // never at its end: it plays until it is stopped
}

ma_result songSeek(ma_data_source*, ma_uint64) { return MA_SUCCESS; }

ma_result songFormat(ma_data_source* ds, ma_format* format, ma_uint32* channels, ma_uint32* rate,
                     ma_channel* map, size_t mapCap) {
    auto* src = reinterpret_cast<Impl::Source*>(ds);
    if (format)   *format   = ma_format_f32;
    if (channels) *channels = 2;
    if (rate)     *rate     = src && src->owner ? src->owner->sampleRate : 48000;
    if (map && mapCap > 0) ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, 2);
    return MA_SUCCESS;
}

ma_result songCursor(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }
ma_result songLength(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }

const ma_data_source_vtable kSongVtable = {
    songRead, songSeek, songFormat, songCursor, songLength, nullptr, 0,
};

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
    auto impl        = std::make_unique<Impl>();
    impl->sampleRate = ma_engine_get_sample_rate(&audio.impl()->engine);
    impl->engine     = std::make_unique<song::Engine>(static_cast<double>(impl->sampleRate), 512);

    ma_data_source_config cfg = ma_data_source_config_init();
    cfg.vtable                = &kSongVtable;
    impl->source.owner        = impl.get();
    if (ma_data_source_init(&cfg, &impl->source.base) != MA_SUCCESS) {
        if (error) *error = "could not create the data source";
        return p;
    }
    impl->sourceOk = true;
    if (ma_sound_init_from_data_source(&audio.impl()->engine, &impl->source.base, 0, nullptr,
                                       &impl->sound) != MA_SUCCESS) {
        if (error) *error = "could not create the voice";
        return p;
    }
    impl->soundOk = true;
    ma_sound_set_spatialization_enabled(&impl->sound, MA_FALSE);   // 2D unless asked
    p.m_impl = std::move(impl);
    return p;
}

bool SongPlayer::isValid() const { return m_impl && m_impl->soundOk; }

song::Engine&       SongPlayer::engine() { return *m_impl->engine; }
const song::Engine& SongPlayer::engine() const { return *m_impl->engine; }

void SongPlayer::setOutput(Mixer& mixer, int strip) {
    if (isValid()) routeToStrip(&m_impl->sound, &mixer, strip);
}

void SongPlayer::start() {
    if (isValid() && !isRunning()) ma_sound_start(&m_impl->sound);
}

void SongPlayer::stop() {
    if (!isValid()) return;
    m_impl->engine->stop();
    m_impl->engine->allNotesOff();
    ma_sound_stop(&m_impl->sound);
}

bool SongPlayer::isRunning() const {
    return isValid() && ma_sound_is_playing(&m_impl->sound) == MA_TRUE;
}

void SongPlayer::setVolume(float v) {
    if (isValid()) ma_sound_set_volume(&m_impl->sound, std::max(0.0f, v));
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
    const double secs  = static_cast<double>(nowNs() - im.blockAt.load(std::memory_order_relaxed)) * 1e-9;
    // From where the last block began, on at the song's pace, never past where
    // the audio thread has already got to.
    const double est = from + secs * im.engine->ticksPerSecond();
    return std::clamp(est, from, to);
}

void SongPlayer::setSpatial(bool on) {
    if (isValid()) ma_sound_set_spatialization_enabled(&m_impl->sound, on ? MA_TRUE : MA_FALSE);
}

void SongPlayer::setPosition(float x, float y, float z) {
    if (isValid()) ma_sound_set_position(&m_impl->sound, x, y, z);
}

void SongPlayer::setAttenuation(float minDist, float maxDist, float rolloff) {
    if (!isValid()) return;
    ma_sound_set_min_distance(&m_impl->sound, minDist);
    ma_sound_set_max_distance(&m_impl->sound, maxDist);
    ma_sound_set_rolloff(&m_impl->sound, rolloff);
}

} // namespace fitzel
