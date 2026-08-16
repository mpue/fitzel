#include "fitzel/audio/Audio.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "fitzel/asset/Vfs.hpp"

namespace fitzel {

namespace {

// miniaudio opens sounds by path, which an exported game cannot offer: the .wav
// is a range inside the mounted archive. Its own VFS hook is the way in -- one
// object handed to the engine, and every load after that (ambient loops,
// one-shots, the resource manager's own streaming reads) comes through here
// without a single call site changing.
//
// An open "file" is just the decrypted bytes plus a cursor. Sounds are small,
// and the alternative -- decrypting a range per read -- would mean seeking the
// keystream on every call.
struct VfsFile {
    std::vector<std::uint8_t> data;
    std::size_t               pos = 0;
};

struct FitzelVfs {
    ma_vfs_callbacks cb; // must stay first: miniaudio casts ma_vfs* to this
};

ma_result vfsOpen(ma_vfs*, const char* path, ma_uint32 openMode, ma_vfs_file* out) {
    if (openMode & MA_OPEN_MODE_WRITE) return MA_NOT_IMPLEMENTED;
    auto file = std::make_unique<VfsFile>();
    file->data = vfs::read(path);
    if (file->data.empty()) return MA_DOES_NOT_EXIST;
    *out = file.release();
    return MA_SUCCESS;
}

ma_result vfsClose(ma_vfs*, ma_vfs_file file) {
    delete static_cast<VfsFile*>(file);
    return MA_SUCCESS;
}

ma_result vfsRead(ma_vfs*, ma_vfs_file file, void* dst, std::size_t bytes,
                  std::size_t* readOut) {
    auto* f = static_cast<VfsFile*>(file);
    const std::size_t take = std::min(bytes, f->data.size() - f->pos);
    std::memcpy(dst, f->data.data() + f->pos, take);
    f->pos += take;
    if (readOut) *readOut = take;
    return take == 0 && bytes > 0 ? MA_AT_END : MA_SUCCESS;
}

ma_result vfsSeek(ma_vfs*, ma_vfs_file file, ma_int64 offset, ma_seek_origin origin) {
    auto*     f    = static_cast<VfsFile*>(file);
    ma_int64  base = 0;
    if (origin == ma_seek_origin_current)   base = static_cast<ma_int64>(f->pos);
    else if (origin == ma_seek_origin_end)  base = static_cast<ma_int64>(f->data.size());
    const ma_int64 to = base + offset;
    if (to < 0 || to > static_cast<ma_int64>(f->data.size())) return MA_BAD_SEEK;
    f->pos = static_cast<std::size_t>(to);
    return MA_SUCCESS;
}

ma_result vfsTell(ma_vfs*, ma_vfs_file file, ma_int64* cursor) {
    *cursor = static_cast<ma_int64>(static_cast<VfsFile*>(file)->pos);
    return MA_SUCCESS;
}

ma_result vfsInfo(ma_vfs*, ma_vfs_file file, ma_file_info* info) {
    info->sizeInBytes = static_cast<VfsFile*>(file)->data.size();
    return MA_SUCCESS;
}

// One instance, alive as long as the process: the engine keeps the pointer.
FitzelVfs g_vfs{{vfsOpen, nullptr, vfsClose, vfsRead, nullptr, vfsSeek, vfsTell,
                 vfsInfo}};

} // namespace

struct Audio::Impl {
    ma_engine       engine;
    ma_sound_group  sfx;        // one-shot bus (mixer "SFX" channel)
    bool            sfxOk = false;
    bool            ok    = false;
};

Audio::Audio() : m_impl(std::make_unique<Impl>()) {
    ma_engine_config cfg    = ma_engine_config_init();
    cfg.pResourceManagerVFS = &g_vfs;
    const ma_result r       = ma_engine_init(&cfg, &m_impl->engine);
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
