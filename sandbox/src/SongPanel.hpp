#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

namespace fitzel { class Audio; class Mixer; }
namespace fitzel::song { struct Song; }

// The editor's "Song Sequencer": songs made of audio, instrument and MIDI
// tracks (fitzel/audio/Song.hpp), heard while they are made, saved into the
// project's content/songs/ for a Song Player component to play in the game.
//
// IT IS LAID OUT LIKE A DAW because that picture is understood before anything
// is explained: a transport across the top, track headers on the left, clips
// on lanes under a bar ruler, a piano roll for the clip being edited below. And
// it is driven like one -- drag a clip, pull its edge, Alt-drag a copy, draw
// notes with the pencil, Space to play, the computer keyboard as a keyboard.
//
// THE EDITOR'S RULE HOLDS ALL THE SAME: nothing needs a steady hand. Every
// drag lands on the grid, and every amount a drag sets has a number beside it
// -- a clip's start and length, a note's velocity, the loop -- in the bar under
// the lanes and the piano roll's own, stepped a grid unit a click.
//
// The panel owns its song, its undo history and the voice it previews through
// (one fitzel::SongPlayer on the mixer's Music channel). It does not run the
// game: the Song Player components do (SongSystem.hpp).
namespace songui {

struct Host {
    fitzel::Audio* audio = nullptr;   // the preview plays on it (null: edit in silence)
    fitzel::Mixer* mixer = nullptr;
    int            strip = -1;        // the strip the preview feeds
    std::string    projectFile;       // the open .fitzel ("" none): songs go in <project>/content/songs
    // A clip's file name to a path, as every sound in the project resolves.
    std::function<std::string(const std::string&)> resolveSound;
    // A Synth instrument's patch name to a path (content/patches).
    std::function<std::string(const std::string&)> resolvePatch;
    // What an audio clip can name; the project's own synth patches.
    std::function<std::vector<std::string>()>      soundNames;
    std::function<std::vector<std::string>()>      patchNames;
    // A sound dragged in from the Assets panel (its GUID) to its name, "" when
    // it is not a sound.
    std::function<std::string(const std::string& guid)> soundOfAsset;
    // Files dropped on the editor's window; the panel takes the sounds and MIDI
    // files that land on it.
    std::vector<std::string>* drops = nullptr;
    // A sound from outside the project copied into it; its name ("" failed).
    std::function<std::string(const std::string& path)> importSound;
    // A bounce wrote a new sound into the project.
    std::function<void()> soundsChanged;
    // Optional: where the panel drew its parts ("clip:<uid>", "lane:<track
    // uid>", "header:<uid>", "ruler", "pr", "pr:keys", "pr:vel"), so
    // songpanelcheck can press them the way a person would.
    std::function<void(const std::string&, ImVec2, ImVec2)> probe;
};

// The song last opened or saved, kept in imgui.ini beside the area layout, so
// the panel opens it again next time the project is. Installed before the
// first frame reads the file (like areas::install).
void install();

class Panel {
public:
    Panel();
    ~Panel();
    Panel(const Panel&)            = delete;
    Panel& operator=(const Panel&) = delete;

    void draw(bool& show, const Host& h);

    // The window has the keyboard: Space, Del, the arrows, Ctrl+Z/C/V/S and
    // the computer keyboard's notes are its own, not the scene's.
    bool hasKeyboard() const;
    // Wants the editor drawn at a smooth rate, not the idle one: the song is
    // playing or recording (the playhead, the meters), something in it is being
    // dragged, or the computer keyboard is playing it.
    bool wantsSmoothFrames() const;
    // Play is starting: the preview stops (the game's Song Players take over)
    // and the open song is saved, so the game plays what is on screen.
    void beforePlay(const Host& h);
    // Another project: the open song is saved where it was and a new one begun.
    void projectChanged(const Host& h);
    // A song of the project by name (content/songs/<name>, ".json" optional),
    // the one open before saved first. False (and why, in the status line)
    // when it cannot be read.
    bool open(const std::string& songName, const Host& h);

    // --- For songpanelcheck --------------------------------------------------
    fitzel::song::Song& song();
    // The song was changed from outside (as an edit is: undo, the preview).
    void edited();
    int    editedClip() const;      // the clip in the piano roll / audio pane, -1
    void   editClip(int uid);
    double playhead() const;        // ticks
    bool   canUndo() const;
    const std::vector<int>& selection() const;

    struct State;

private:
    std::unique_ptr<State> m;
};

} // namespace songui
