#include "fitzel/audio/SynthPlayer.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "AudioInternal.hpp"
#include "fitzel/audio/SynthPoly.hpp"

namespace fitzel {

namespace {
constexpr int kGraphBlock  = 256;
constexpr int kScopeFrames = 4096;
constexpr int kQueue       = 512;   // notes between two audio blocks; a chord flood fits

struct Cmd {
    enum Kind : std::uint8_t { NoteOn, NoteOff, AllOff, SeqPlay, SeqRestart, SeqStop };
    Kind  kind = NoteOn;
    int   note = 0;
    float vel  = 0.0f;
};
} // namespace

struct SynthPlayer::Impl {
    // miniaudio reads from this like a decoded file. The base has to come first:
    // miniaudio casts the pointer it is handed straight to it.
    struct Source {
        ma_data_source_base base;
        Impl*               owner = nullptr;
    };

    Source     source{};
    ma_sound   sound{};
    synth::Poly poly;
    ma_uint32  sampleRate = 48000;
    bool       sourceOk = false, soundOk = false;

    // Notes from the game thread, one producer and one consumer: a ring the
    // audio thread drains at the top of each block.
    Cmd                      queue[kQueue];
    std::atomic<int>         head{0}, tail{0};

    // Settings, read by the audio thread once a block.
    std::atomic<bool>        loop{false}, skipDrums{true};
    std::atomic<float>       tempo{1.0f};
    std::atomic<int>         channel{-1};
    std::atomic<bool>        seqPlaying{false};
    double                   seqLength = 0.0;   // game-thread copy, for midiLength()

    // A song crosses to the audio thread by pointer. `pending` is set here and
    // taken there; the one it replaces goes to `retired`, which only the game
    // thread frees. The audio thread takes a new song only once `retired` is
    // empty, so a pointer is never dropped and nothing is freed in the callback.
    std::atomic<synth::MidiSequence*> pending{nullptr};
    std::atomic<synth::MidiSequence*> retired{nullptr};
    synth::MidiSequence*              current = nullptr;   // audio thread's

    float            scope[kScopeFrames] = {};
    std::atomic<int> scopeWrite{0};

    // Unhooked from the mixer when the Impl goes, not when the SynthPlayer does:
    // a defaulted move assignment frees the Impl it overwrites without running
    // the outer destructor (the bug that crashed the Synth panel -- see
    // SynthVoice.cpp). Sound first, the mixer reads through it; then the source;
    // and only then the songs, which nothing can be reading any more.
    ~Impl() {
        if (soundOk)  ma_sound_uninit(&sound);
        if (sourceOk) ma_data_source_uninit(&source.base);
        delete current;
        delete pending.exchange(nullptr);
        delete retired.exchange(nullptr);
    }

    void push(const Cmd& c) {
        const int h    = head.load(std::memory_order_relaxed);
        const int next = (h + 1) % kQueue;
        // Full: the audio thread has not run for a whole queue of notes. Drop
        // this one rather than wait -- the game thread must never block on
        // the mixer.
        if (next == tail.load(std::memory_order_acquire)) return;
        queue[h] = c;
        head.store(next, std::memory_order_release);
    }

    void freeRetired() { delete retired.exchange(nullptr); }
};

namespace {

using Impl = SynthPlayer::Impl;

ma_result playerRead(ma_data_source* ds, void* framesOut, ma_uint64 frameCount,
                     ma_uint64* framesRead) {
    auto* src = reinterpret_cast<Impl::Source*>(ds);
    if (!src || !src->owner) {
        if (framesRead) *framesRead = 0;
        return MA_INVALID_ARGS;
    }
    Impl& im = *src->owner;

    // A new song, if one is waiting and the last one has been collected.
    if (im.retired.load(std::memory_order_acquire) == nullptr) {
        if (synth::MidiSequence* next = im.pending.exchange(nullptr)) {
            synth::MidiSequence* old = im.current;
            im.current = next;
            im.poly.setSequence(next);
            im.retired.store(old, std::memory_order_release);
        }
    }

    im.poly.setLoop(im.loop.load(std::memory_order_relaxed));
    im.poly.setTempo(im.tempo.load(std::memory_order_relaxed));
    im.poly.setChannel(im.channel.load(std::memory_order_relaxed));
    im.poly.setSkipDrums(im.skipDrums.load(std::memory_order_relaxed));

    // Everything the game asked for since the last block, in order.
    int t = im.tail.load(std::memory_order_relaxed);
    while (t != im.head.load(std::memory_order_acquire)) {
        const Cmd& c = im.queue[t];
        switch (c.kind) {
        case Cmd::NoteOn:     im.poly.noteOn(c.note, c.vel); break;
        case Cmd::NoteOff:    im.poly.noteOff(c.note); break;
        case Cmd::AllOff:     im.poly.allNotesOff(); break;
        case Cmd::SeqPlay:    im.poly.play(false); break;
        case Cmd::SeqRestart: im.poly.play(true); break;
        case Cmd::SeqStop:    im.poly.stop(); break;
        }
        t = (t + 1) % kQueue;
    }
    im.tail.store(t, std::memory_order_release);

    float*    out = static_cast<float*>(framesOut);
    const int n   = static_cast<int>(frameCount);
    im.poly.process(out, n);
    im.seqPlaying.store(im.poly.playing(), std::memory_order_relaxed);

    int w = im.scopeWrite.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        im.scope[w] = out[i];
        w = (w + 1) % kScopeFrames;
    }
    im.scopeWrite.store(w, std::memory_order_relaxed);

    if (framesRead) *framesRead = frameCount;
    return MA_SUCCESS;   // never at end: it plays until it is stopped
}

ma_result playerSeek(ma_data_source*, ma_uint64) { return MA_SUCCESS; }

ma_result playerFormat(ma_data_source* ds, ma_format* format, ma_uint32* channels,
                       ma_uint32* rate, ma_channel* map, size_t mapCap) {
    auto* src = reinterpret_cast<Impl::Source*>(ds);
    if (format)   *format   = ma_format_f32;
    if (channels) *channels = 1;   // mono, so the spatializer can place it
    if (rate)     *rate     = src && src->owner ? src->owner->sampleRate : 48000;
    if (map && mapCap > 0)
        ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, 1);
    return MA_SUCCESS;
}

ma_result playerCursor(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }
ma_result playerLength(ma_data_source*, ma_uint64*) { return MA_NOT_IMPLEMENTED; }

const ma_data_source_vtable kPlayerVtable = {
    playerRead, playerSeek, playerFormat, playerCursor, playerLength, nullptr, 0,
};

} // namespace

SynthPlayer::SynthPlayer()  = default;
SynthPlayer::~SynthPlayer() = default;   // the Impl unhooks itself
SynthPlayer::SynthPlayer(SynthPlayer&&) noexcept            = default;
SynthPlayer& SynthPlayer::operator=(SynthPlayer&&) noexcept = default;

SynthPlayer SynthPlayer::create(Audio& audio, const synth::Patch& patch, int voices,
                                std::string* error) {
    SynthPlayer p;
    if (!audio.ok()) {
        if (error) *error = "no audio device";
        return p;
    }
    auto impl        = std::make_unique<Impl>();
    impl->sampleRate = ma_engine_get_sample_rate(&audio.impl()->engine);
    if (!impl->poly.compile(patch, static_cast<double>(impl->sampleRate), voices,
                            kGraphBlock, error))
        return p;
    // Headroom that grows with the voice count: one voice plays at the level
    // its patch sets, a sixteen-note chord does not clip the moment it lands.
    impl->poly.setGain(1.0f / std::sqrt(static_cast<float>(std::max(1, voices))));

    ma_data_source_config cfg = ma_data_source_config_init();
    cfg.vtable                = &kPlayerVtable;
    impl->source.owner        = impl.get();
    if (ma_data_source_init(&cfg, &impl->source.base) != MA_SUCCESS) {
        if (error) *error = "could not create the data source";
        return p;
    }
    impl->sourceOk = true;
    if (ma_sound_init_from_data_source(&audio.impl()->engine, &impl->source.base, 0,
                                       nullptr, &impl->sound) != MA_SUCCESS) {
        if (error) *error = "could not create the voice";
        return p;   // ~Impl unhooks the source
    }
    impl->soundOk = true;
    ma_sound_set_spatialization_enabled(&impl->sound, MA_FALSE);   // 2D unless asked
    p.m_impl = std::move(impl);
    return p;
}

bool SynthPlayer::isValid() const { return m_impl && m_impl->soundOk; }

void SynthPlayer::start() {
    if (isValid() && !isRunning()) ma_sound_start(&m_impl->sound);
}

void SynthPlayer::stop() {
    if (!isValid()) return;
    m_impl->push({Cmd::SeqStop, 0, 0.0f});
    ma_sound_stop(&m_impl->sound);
}

bool SynthPlayer::isRunning() const {
    return isValid() && ma_sound_is_playing(&m_impl->sound) == MA_TRUE;
}

void SynthPlayer::setVolume(float v) {
    if (isValid()) ma_sound_set_volume(&m_impl->sound, v);
}

void SynthPlayer::noteOn(int note, float velocity) {
    if (!m_impl) return;
    start();   // a note to a stopped voice would wait silently for ever
    m_impl->push({Cmd::NoteOn, std::clamp(note, 0, 127), velocity});
}
void SynthPlayer::noteOff(int note) {
    if (m_impl) m_impl->push({Cmd::NoteOff, std::clamp(note, 0, 127), 0.0f});
}
void SynthPlayer::allNotesOff() {
    if (m_impl) m_impl->push({Cmd::AllOff, 0, 0.0f});
}

int SynthPlayer::inputIndex(const std::string& name) const {
    return m_impl ? m_impl->poly.inputIndex(name) : -1;
}
void SynthPlayer::setInput(int index, float value) {
    // Graph::setInput is an atomic store, so this is safe against the render.
    if (m_impl) m_impl->poly.setInput(index, value);
}
void SynthPlayer::setInput(const std::string& name, float value) {
    setInput(inputIndex(name), value);
}

bool SynthPlayer::loadMidi(const std::string& path, std::string* error) {
    synth::MidiSequence song;
    if (!synth::MidiSequence::load(path, song, error)) return false;
    setMidi(std::move(song));
    return true;
}

void SynthPlayer::setMidi(synth::MidiSequence song) {
    if (!m_impl) return;
    m_impl->freeRetired();
    m_impl->seqLength = song.length;
    auto* fresh = new synth::MidiSequence(std::move(song));
    // One the audio thread never got round to taking is ours to free.
    delete m_impl->pending.exchange(fresh);
}

void SynthPlayer::playMidi(bool fromStart) {
    if (!m_impl) return;
    m_impl->freeRetired();
    start();
    m_impl->push({fromStart ? Cmd::SeqRestart : Cmd::SeqPlay, 0, 0.0f});
    // Set here as well as by the audio thread, so a script that starts a song
    // and asks straight away is told it is playing.
    m_impl->seqPlaying.store(true, std::memory_order_relaxed);
}

void SynthPlayer::stopMidi() {
    if (!m_impl) return;
    m_impl->push({Cmd::SeqStop, 0, 0.0f});
    m_impl->seqPlaying.store(false, std::memory_order_relaxed);
}

bool SynthPlayer::midiPlaying() const {
    return m_impl && m_impl->seqPlaying.load(std::memory_order_relaxed);
}
double SynthPlayer::midiLength() const { return m_impl ? m_impl->seqLength : 0.0; }

void SynthPlayer::setMidiLoop(bool loop) { if (m_impl) m_impl->loop.store(loop); }
void SynthPlayer::setTempo(float scale)  { if (m_impl) m_impl->tempo.store(scale); }
void SynthPlayer::setChannel(int ch)     { if (m_impl) m_impl->channel.store(ch); }
void SynthPlayer::setSkipDrums(bool s)   { if (m_impl) m_impl->skipDrums.store(s); }

void SynthPlayer::setSpatial(bool on) {
    if (isValid())
        ma_sound_set_spatialization_enabled(&m_impl->sound, on ? MA_TRUE : MA_FALSE);
}
void SynthPlayer::setPosition(float x, float y, float z) {
    if (isValid()) ma_sound_set_position(&m_impl->sound, x, y, z);
}
void SynthPlayer::setVelocity(float x, float y, float z) {
    if (isValid()) ma_sound_set_velocity(&m_impl->sound, x, y, z);
}
void SynthPlayer::setAttenuation(float minDist, float maxDist, float rolloff) {
    if (!isValid()) return;
    ma_sound_set_min_distance(&m_impl->sound, minDist);
    ma_sound_set_max_distance(&m_impl->sound, maxDist);
    ma_sound_set_rolloff(&m_impl->sound, rolloff);
}
void SynthPlayer::setDopplerFactor(float f) {
    if (isValid()) ma_sound_set_doppler_factor(&m_impl->sound, f);
}

bool SynthPlayer::updateParams(const synth::Patch& patch) {
    return m_impl && m_impl->poly.updateParams(patch);
}

int SynthPlayer::peek(float* out, int maxFrames) const {
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
