#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/audio/SynthPlayer.hpp>

namespace fitzel { class Audio; }
class Document;
class SynthComponent;
struct Entity;

// The running half of the Synth component: one SynthPlayer per object that has
// been started, kept here by entity id rather than on the component, so copying,
// undoing or saving an entity never touches a live voice.
//
// A player is built the first time something asks for it -- Play starting with
// "play on start", a script's first synth.noteOn, the Inspector's preview -- and
// rebuilt only when the patch or the voice count changes. Everything else the
// component says (volume, tempo, channel, loop) is read again every frame, so it
// can be turned while it plays.
//
// Files are found where the Synth panel saves them: a patch under
// <project>/content/patches/, a song under <project>/content/midi/, both through
// fitzel::vfs so an exported game reads them out of its .fpak.
class SynthSystem {
public:
    // The things it reads and never owns; all of them outlive it in main.
    void bind(fitzel::Audio& audio, Document& doc, const std::string& projectFile) {
        m_audio   = &audio;
        m_doc     = &doc;
        m_project = &projectFile;
    }

    // Start the object's synth, and its song if it has one. False (and a reason
    // in lastError) when there is no Synth on it or its patch will not load.
    // `reload` reads the patch and song from disk again even if nothing about
    // the component changed -- the Inspector's preview, after an edit in the
    // Synth panel was saved under the same name.
    bool play(int id, bool reload = false);
    // Silence it: the song stops, held notes are let go, the voice leaves the
    // mixer.
    void stop(int id);
    // Every player gone -- Play ended, or a scene is being replaced.
    void clear() {
        m_players.clear();
        m_broken.clear();
    }

    // --- What a script can do ------------------------------------------------
    bool noteOn(int id, int note, float velocity);
    bool noteOff(int id, int note);
    bool allNotesOff(int id);
    bool setDial(int id, const std::string& dial, float value);
    // `file` empty: the song on the component. `loop` < 0: the component's loop.
    bool playMidi(int id, const std::string& file, int loop);
    bool stopMidi(int id);
    bool midiPlaying(int id) const;
    bool setTempo(int id, float scale);

    // Once a frame: level (the component's volume, the mix, and distance from
    // `listener` when spatial), the song settings, and players whose object is
    // gone are dropped.
    void update(const glm::vec3& listener, float ambientGain);

    // A patch or song name to the file it means (see the class comment).
    std::string resolvePatch(const std::string& name) const;
    std::string resolveMidi(const std::string& name) const;

    const std::string& lastError() const { return m_error; }
    bool running(int id) const;

private:
    struct Running {
        fitzel::SynthPlayer player;
        std::string         patch;   // what it was built from, to know when to rebuild
        int                 voices = 0;
        std::string         midi;    // the song loaded into it ("" none)
    };

    // The player for an object, built or rebuilt as its component asks.
    Running* ensure(int id);
    SynthComponent* component(int id) const;
    std::string resolve(const std::string& name, const char* folder, const char* ext) const;
    bool fail(const std::string& why);

    fitzel::Audio*            m_audio   = nullptr;
    Document*                 m_doc     = nullptr;
    const std::string*        m_project = nullptr;
    std::unordered_map<int, Running> m_players;
    // Objects whose patch would not load, and the name that failed. A script
    // that calls synth.noteOn every frame on a broken patch would otherwise
    // read the disk (and log the same line) sixty times a second; this answers
    // no until the name changes or play(id, true) asks for a real retry.
    std::unordered_map<int, std::string> m_broken;
    std::string               m_error;
};
