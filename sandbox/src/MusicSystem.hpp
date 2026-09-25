#pragma once

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
        if (!m_player.isValid() && m_audio)
            m_player = fitzel::MusicPlayer::create(*m_audio, &m_error);
        return m_player.isValid() ? &m_player : nullptr;
    }
    // Only if something already made it -- for reads that should not create one.
    fitzel::MusicPlayer* existing() { return m_player.isValid() ? &m_player : nullptr; }

    std::string resolve(const std::string& name) const {
        return m_resolve ? m_resolve(name) : name;
    }

    // Once a frame: the script's volume under the mixer's music/ambient level.
    void update(float mixGain) {
        if (m_player.isValid()) m_player.setVolume(m_volume * mixGain);
    }
    // Play ended: silence and free the voice.
    void clear() { m_player = fitzel::MusicPlayer{}; m_volume = 1.0f; }

    void setVolume(float v) { m_volume = v < 0.0f ? 0.0f : v; }
    std::string&       error() { return m_error; }

private:
    fitzel::Audio*                                   m_audio = nullptr;
    std::function<std::string(const std::string&)>   m_resolve;
    fitzel::MusicPlayer                              m_player;
    float                                            m_volume = 1.0f;
    std::string                                      m_error;
};
