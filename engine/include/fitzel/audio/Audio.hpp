#pragma once

#include <memory>
#include <string>

namespace fitzel {

// A minimal audio engine (wraps miniaudio's high-level engine). Owns the output
// device and mixer; create one and keep it alive while sounds play.
class Audio {
public:
    // `outputChannels` is what the MIXER renders, not what the device is. 2 is
    // the default and is a deliberate choice, not a limitation.
    //
    // Left to the device, this takes whatever Windows reports -- and a great many
    // machines report 8, because the driver offers 7.1 whether or not eight
    // speakers exist. The spatializer then dutifully pans a source that is to
    // your left into the SIDE-LEFT channel and one behind you into a REAR
    // channel, and on headphones, stereo speakers or a virtual-surround headset
    // those are channels nothing plays or something folds flat. Distance and
    // Doppler survive that; direction does not. It sounds exactly like spatial
    // audio being broken, and it is instead a panning image spread across
    // speakers that are not in the room.
    //
    // Rendering stereo and letting the OS take it to the actual hardware is what
    // games do, and it is the only output where the image is predictable on the
    // devices people really use. Pass 0 for the device's own count if the
    // machine genuinely has the speakers for it.
    explicit Audio(int outputChannels = 2);
    ~Audio();

    Audio(const Audio&)            = delete;
    Audio& operator=(const Audio&) = delete;

    bool ok() const;
    void setMasterVolume(float volume);
    // Volume of the one-shot (SFX) bus that playOneShot routes through.
    void setSfxVolume(float volume);

    // Fire-and-forget one-shot on the SFX bus (loads + plays + auto-frees).
    // Never positional: it has no handle to move, which is exactly why a
    // positioned one-shot needs a Sound of its own (see Sound::setSpatial).
    void playOneShot(const std::string& path);

    // --- The listener ------------------------------------------------------
    // Where the ears are, which is what every spatial Sound is measured
    // against. Plain floats rather than a vector type: this header is included
    // by anything that makes a noise, and it has no business dragging a maths
    // library in with it.
    //
    // `vel` is metres per second and is ONLY used for Doppler -- it does not
    // move the listener. Pass the real velocity or zero; a value derived from a
    // position delta divided by a frame time will spike on a stutter and pitch
    // the whole world with it, so smooth it before it gets here.
    void setListener(float px, float py, float pz,
                     float fx, float fy, float fz,
                     float ux, float uy, float uz,
                     float vx, float vy, float vz);
    // Speed of sound in metres per second (343 = dry air at 20 C). Lowering it
    // exaggerates every Doppler shift in the world at once, which is the dial to
    // reach for when the effect is real but too subtle to hear over an engine.
    void setSpeedOfSound(float mps);

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
    // Still sounding? A voice pool needs this to know which of its voices it is
    // allowed to reuse -- and reuse is the only safe way to fire the same sample
    // again, because re-loading a Sound that is still being mixed tears its
    // decoder out from under the audio thread.
    bool isPlaying() const;

    // --- In the world ------------------------------------------------------
    // Off by default for every Sound, so nothing that plays today changes: a
    // 2D sound is a sound the mixer leaves alone. Switching it on hands the
    // sound to the spatializer, where position, distance attenuation and
    // Doppler all start applying at once.
    void setSpatial(bool on);
    void setPosition(float x, float y, float z);
    // Metres per second, for Doppler only -- it does not move the sound. The
    // shift the listener hears comes from the RELATIVE velocity, so a source
    // travelling alongside at the same speed is not shifted at all, which is
    // what makes overtaking sound like overtaking.
    void setVelocity(float x, float y, float z);
    // Full volume within `minDist`, silent past `maxDist`, `rolloff` bends the
    // curve between them (1 = inverse-distance, higher = falls away sooner).
    void setAttenuation(float minDist, float maxDist, float rolloff);
    // 0 disables Doppler for this sound, 1 is physical, higher exaggerates it.
    void setDopplerFactor(float factor);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel
