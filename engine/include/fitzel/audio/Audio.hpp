#pragma once

#include <memory>
#include <string>

namespace fitzel {

// A minimal audio engine (wraps miniaudio's high-level engine). Owns the output
// device and mixer; create one and keep it alive while sounds play.
class Audio {
public:
    Audio();
    ~Audio();

    Audio(const Audio&)            = delete;
    Audio& operator=(const Audio&) = delete;

    bool ok() const;
    void setMasterVolume(float volume);

    // Fire-and-forget one-shot (loads + plays + auto-frees).
    void playOneShot(const std::string& path);

    struct Impl;
    Impl* impl() const { return m_impl.get(); }

private:
    std::unique_ptr<Impl> m_impl;
};

// A single loaded sound: a looping ambient layer or a one-shot effect.
// Move-only.
class Sound {
public:
    Sound();
    ~Sound();

    Sound(const Sound&)            = delete;
    Sound& operator=(const Sound&) = delete;
    Sound(Sound&& other) noexcept;
    Sound& operator=(Sound&& other) noexcept;

    static Sound fromFile(Audio& audio, const std::string& path,
                          bool loop = false);

    bool isValid() const;

    void play();   // (re)start from the beginning
    void stop();
    void setVolume(float volume);
    void setPitch(float pitch);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel
