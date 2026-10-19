#pragma once

#include <fitzel/audio/Mixer.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <string>

#include <fitzel/audio/MusicPlayer.hpp>

// The game's one song, for the `music` table (music.load, music.play,
// music.time, ...): a rhythm game's soundtrack, which unlike an AudioSource
// can start at any second, say where it is and be filtered while it plays.
//
// Built on the first call that needs it, dropped when Play ends. Song names
// resolve like every sound a script names (the asset database first, then
// content/sounds), so a song in the project's music/ folder is found by its
// file name.
class MusicSystem {
public:
    // `audio` outlives this (both live in main, this one declared after it).
    void bind(fitzel::Audio& audio, std::function<std::string(const std::string&)> resolve) {
        m_audio   = &audio;
        m_resolve = std::move(resolve);
    }

    // The voice, created on first use. Null without an audio device.
    fitzel::MusicPlayer* player() {
        if (!m_player.isValid() && m_audio) {
            m_player = fitzel::MusicPlayer::create(*m_audio, &m_error);
            if (m_mixer && m_player.isValid()) m_player.setOutput(*m_mixer, m_strip);
        }
        return m_player.isValid() ? &m_player : nullptr;
    }
    // Only if something already made it -- for reads that should not create one.
    fitzel::MusicPlayer* existing() { return m_player.isValid() ? &m_player : nullptr; }

    std::string resolve(const std::string& name) const {
        return m_resolve ? m_resolve(name) : name;
    }

    // music.crossfade: the song now playing fades out over `sec` while `path` fades in from
    // silence over the same time -- two voices for that while, then the old one is let go.
    // The new song loops if the old one did. False (and why) if it cannot be loaded; the old
    // one then plays on.
    bool crossfade(const std::string& path, double sec, std::string* error) {
        if (!m_audio) { if (error) *error = "no audio"; return false; }
        fitzel::MusicPlayer next = fitzel::MusicPlayer::create(*m_audio, error);
        if (!next.isValid()) return false;
        if (m_mixer) next.setOutput(*m_mixer, m_strip);
        if (!next.load(path, error)) return false;
        const bool loop = m_player.isValid() && m_player.looping();
        next.setLoop(loop);
        next.setVolume(0.0f);
        next.play(0.0);
        sec = std::max(sec, 0.01);
        if (m_player.isValid() && m_player.playing()) {
            m_player.fadeOut(sec);
            m_old      = std::move(m_player);
            m_oldLeft  = sec + 0.5;
        }
        m_player  = std::move(next);
        m_fadeIn  = sec;
        m_fadeT   = 0.0;
        return true;
    }

    // Once a frame: the script's volume under the mixer's music/ambient level (and a
    // crossfade's share of it).
    void update(float mixGain) {
        const auto now = std::chrono::steady_clock::now();
        const double dt = m_lastTick.time_since_epoch().count() == 0
            ? 0.0 : std::min(0.1, std::chrono::duration<double>(now - m_lastTick).count());
        m_lastTick = now;
        float k = 1.0f;
        if (m_fadeIn > 0.0) {
            m_fadeT += dt;
            k = static_cast<float>(std::min(1.0, m_fadeT / m_fadeIn));
            if (k >= 1.0f) m_fadeIn = 0.0;
        }
        if (m_player.isValid()) m_player.setVolume(m_volume * mixGain * k);
        if (m_old.isValid()) {
            m_oldLeft -= dt;
            if (m_oldLeft <= 0.0) m_old = fitzel::MusicPlayer{};
        }
    }
    // The mixer strip the song feeds, now and when the voice is made later.
    void route(fitzel::Mixer& mixer, int strip) {
        m_mixer = &mixer;
        m_strip = strip;
        if (m_player.isValid()) m_player.setOutput(mixer, strip);
    }
    // Play ended: silence and free the voice.
    void clear() {
        m_player = fitzel::MusicPlayer{};
        m_old    = fitzel::MusicPlayer{};
        m_volume = 1.0f;
        m_fadeIn = 0.0;
    }

    void setVolume(float v) { m_volume = v < 0.0f ? 0.0f : v; }
    std::string&       error() { return m_error; }

private:
    fitzel::Audio*                                   m_audio = nullptr;
    fitzel::Mixer*                                   m_mixer = nullptr;
    int                                              m_strip = -1;
    std::function<std::string(const std::string&)>   m_resolve;
    fitzel::MusicPlayer                              m_player;
    fitzel::MusicPlayer                              m_old;       // a crossfade's outgoing song
    double                                           m_oldLeft = 0.0;
    double                                           m_fadeIn = 0.0, m_fadeT = 0.0;
    std::chrono::steady_clock::time_point            m_lastTick{};
    float                                            m_volume = 1.0f;
    std::string                                      m_error;
};
