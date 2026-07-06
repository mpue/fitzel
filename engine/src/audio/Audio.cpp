#include "fitzel/audio/Audio.hpp"

#include <cstdio>
#include <utility>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace fitzel {

struct Audio::Impl {
    ma_engine       engine;
    ma_sound_group  sfx;        // one-shot bus (mixer "SFX" channel)
    bool            sfxOk = false;
    bool            ok    = false;
};

Audio::Audio() : m_impl(std::make_unique<Impl>()) {
    const ma_result r = ma_engine_init(nullptr, &m_impl->engine);
    m_impl->ok = (r == MA_SUCCESS);
    if (!m_impl->ok) {
        std::fprintf(stderr, "[Fitzel] audio engine init failed (%d)\n", r);
        return;
    }
    m_impl->sfxOk = ma_sound_group_init(&m_impl->engine, 0, nullptr, &m_impl->sfx)
                    == MA_SUCCESS;
}

Audio::~Audio() {
    if (m_impl && m_impl->ok) {
        if (m_impl->sfxOk) ma_sound_group_uninit(&m_impl->sfx);
        ma_engine_uninit(&m_impl->engine);
    }
}

bool Audio::ok() const { return m_impl && m_impl->ok; }

void Audio::setMasterVolume(float volume) {
    if (ok()) ma_engine_set_volume(&m_impl->engine, volume);
}

void Audio::setSfxVolume(float volume) {
    if (ok() && m_impl->sfxOk) ma_sound_group_set_volume(&m_impl->sfx, volume);
}

void Audio::playOneShot(const std::string& path) {
    if (ok())
        ma_engine_play_sound(&m_impl->engine, path.c_str(),
                             m_impl->sfxOk ? &m_impl->sfx : nullptr);
}

struct Sound::Impl {
    ma_sound sound;
    bool     valid = false;
};

Sound::Sound()  = default;
Sound::~Sound() {
    if (m_impl && m_impl->valid) ma_sound_uninit(&m_impl->sound);
}

Sound::Sound(Sound&& other) noexcept            = default;
Sound& Sound::operator=(Sound&& other) noexcept = default;

Sound Sound::fromFile(Audio& audio, const std::string& path, bool loop) {
    Sound s;
    if (!audio.ok()) return s;

    s.m_impl = std::make_unique<Impl>();
    const ma_result r = ma_sound_init_from_file(
        &audio.impl()->engine, path.c_str(), 0, nullptr, nullptr, &s.m_impl->sound);
    if (r != MA_SUCCESS) {
        std::fprintf(stderr, "[Fitzel] failed to load sound '%s' (%d)\n",
                     path.c_str(), r);
        s.m_impl.reset();
        return s;
    }
    ma_sound_set_looping(&s.m_impl->sound, loop ? MA_TRUE : MA_FALSE);
    s.m_impl->valid = true;
    return s;
}

bool Sound::isValid() const { return m_impl && m_impl->valid; }

void Sound::play() {
    if (isValid()) {
        ma_sound_seek_to_pcm_frame(&m_impl->sound, 0);
        ma_sound_start(&m_impl->sound);
    }
}

void Sound::stop() {
    if (isValid()) ma_sound_stop(&m_impl->sound);
}

void Sound::setVolume(float volume) {
    if (isValid()) ma_sound_set_volume(&m_impl->sound, volume);
}

void Sound::setPitch(float pitch) {
    if (isValid()) ma_sound_set_pitch(&m_impl->sound, pitch);
}

} // namespace fitzel
