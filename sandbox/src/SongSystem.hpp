#pragma once

#include <functional>
#include <string>
#include <unordered_map>

#include <glm/glm.hpp>

#include <fitzel/audio/Song.hpp>
#include <fitzel/audio/SongPlayer.hpp>

namespace fitzel { class Audio; class Mixer; }
class Document;
class SongPlayerComponent;

// The running half of the Song Player component: one fitzel::SongPlayer per
// object that has been started, kept here by entity id (as SynthSystem keeps
// its players), so copying, undoing or saving an entity never touches a voice.
//
// A song is read from <project>/content/songs/ (through fitzel::vfs, so an
// exported game reads it out of its .fpak) when the object is started, and
// held in memory while it plays: a script muting a layer or changing the tempo
// changes that copy and hands it to the engine again -- adaptive music is one
// song with its layers switched, not several songs crossfaded.
//
// What the component says every frame (volume, channel, distance) is applied
// in update(); what it says once (the song, the loop) when it starts.
class SongSystem {
public:
    // The things it reads and never owns; all of them outlive it in main.
    void bind(fitzel::Audio& audio, Document& doc, const std::string& projectFile,
              std::function<std::string(const std::string&)> resolveSound,
              std::function<std::string(const std::string&)> resolvePatch) {
        m_audio        = &audio;
        m_doc          = &doc;
        m_project      = &projectFile;
        m_resolveSound = std::move(resolveSound);
        m_resolvePatch = std::move(resolvePatch);
    }
    // The desk, and how a channel name becomes one of its strips (the mixer's
    // route()). Called again when the desk changes: every player is re-routed.
    void route(fitzel::Mixer& mixer, std::function<int(const std::string&)> strip);

    // Start the object's song from the top. False (and why, in lastError) when
    // it has no Song Player, no song, or the song will not load. `reload` reads
    // the file again even if it is the one already loaded -- the Inspector's
    // preview, after the sequencer saved it.
    bool play(int id, bool reload = false);
    void stop(int id);
    // Every player gone: Play ended, or a scene is being replaced.
    void clear() { m_players.clear(); }

    bool   playing(int id) const;
    bool   running(int id) const { return playing(id); }
    double seconds(int id) const;            // -1: not started
    double beats(int id) const;              // beats of the song's meter, from 0 (-1: not started)
    // Bar and beat in it, both from 1 (0, 0 when not started).
    void   barBeat(int id, int& bar, int& beat) const;
    bool   seek(int id, double beat);
    // A track by name (the first of that name): its mute, its level.
    bool   setTrackMute(int id, const std::string& track, bool mute);
    bool   setTrackVolume(int id, const std::string& track, float volume);
    bool   setTempo(int id, double bpm);
    // The loudest sample the song put out since the last call (0..1) -- a meter.
    float  takePeak(int id);

    // Once a frame: levels (the component's volume, `gain`, distance from
    // `listener` when spatial), the channel, and players whose object is gone
    // or whose song has ended (and rung out) are let go.
    void update(const glm::vec3& listener, float gain);

    std::string resolveSong(const std::string& name) const;
    const std::string& lastError() const { return m_error; }

private:
    struct Running {
        fitzel::SongPlayer player;
        std::string        name;       // what the component named when it started
        fitzel::song::Song song;       // the copy a script changes
        std::string        channel;
        int                strip = -2;
        bool               loop = true;
        double             endedFor = 0.0;   // seconds since it ran out
    };

    SongPlayerComponent* component(int id) const;
    bool fail(const std::string& why);
    Running* find(int id);
    const Running* find(int id) const;

    fitzel::Audio*     m_audio = nullptr;
    fitzel::Mixer*     m_mixer = nullptr;
    Document*          m_doc   = nullptr;
    const std::string* m_project = nullptr;
    std::function<std::string(const std::string&)> m_resolveSound, m_resolvePatch;
    std::function<int(const std::string&)>         m_strip;
    std::unordered_map<int, Running> m_players;
    std::string m_error;
};
