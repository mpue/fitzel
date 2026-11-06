#pragma once

// The Song Sequencer panel's insides (SongPanel.hpp), shared by SongPanel.cpp
// -- transport, tracks, arrangement, files -- and SongPianoRoll.cpp, the note
// editor and the audio clip's pane under the lanes.

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include <fitzel/audio/Song.hpp>
#include <fitzel/audio/SongEngine.hpp>
#include <fitzel/audio/SongPlayer.hpp>
#include <fitzel/audio/Waveform.hpp>

#include "SongPanel.hpp"

namespace songui {

namespace sg = fitzel::song;

enum class Tool { Select = 0, Draw = 1, Erase = 2, Split = 3 };

// A grid: whole bars (ticks 0) or a fixed number of ticks.
struct SnapDef {
    const char*  label;
    std::int64_t ticks;
};
const std::vector<SnapDef>& snaps();

// A press on the lanes and what it became.
struct ArrDrag {
    enum Mode { None, Move, TrimL, TrimR, FadeIn, FadeOut, Rubber, DrawClip, Scrub, Loop, LoopL, LoopR,
                LoopMove, Height, Splitter, HScroll };
    Mode         mode = None;
    sg::Song     base;           // the song as it was when the press began
    int          anchor = -1;    // the clip pressed on
    int          track  = -1;    // the track (uid) pressed on
    int          lane   = -1;    // its row
    std::int64_t pressTick = 0, v0 = 0, v1 = 0;
    double       f0 = 0.0;
    ImVec2       press{};
    bool         moved = false, copy = false;
    std::vector<int> before;     // the selection before a rubber band
};

// A press in the piano roll.
struct PrDrag {
    enum Mode { None, Move, ResizeR, ResizeL, Rubber, Draw, Velocity, Erase, ClipEnd, Keys };
    Mode         mode = None;
    sg::Song     base;
    int          note = -1;      // index into the clip's notes in `base`
    std::int64_t pressTick = 0;
    int          pressPitch = 0;
    ImVec2       press{};
    bool         moved = false, copy = false;
    int          lastPitch = -1; // the last pitch auditioned while dragging
};

struct WaveSlot {
    std::future<fitzel::Waveform> job;
    fitzel::Waveform              wave;
    bool                          ready = false;
};

struct MidiIn;   // a hardware MIDI input (Linux raw MIDI), SongMidiIn.hpp

struct Panel::State {
    // --- The song -------------------------------------------------------------
    sg::Song      song;
    std::string   path;               // the file it came from ("" = not saved yet)
    bool          dirty = false;
    std::uint64_t rev = 1, sentRev = 0;   // edits; what the preview last got
    // An undo step: the song as it was, and what was selected then.
    struct Step {
        sg::Song         song;
        std::vector<int> sel;
    };
    std::vector<Step> undo, redo;

    // --- The preview --------------------------------------------------------------
    fitzel::SongPlayer player;
    bool               playerTried = false;
    std::string        playerError;
    fitzel::Mixer*     routedMixer = nullptr;
    int                routedStrip = -2;
    float              peakHold[64] = {};      // meters, by track row (decaying)
    float              masterHold = 0.0f;

    // --- The arrangement's view ---------------------------------------------------
    double ppt   = 0.0;      // pixels per tick; 0 = fit on the next frame
    double view0 = 0.0;      // tick at the lanes' left edge
    float  laneW = 600.0f;   // last frame's
    float  paneH = 0.0f;     // the editor pane under the lanes (0 = default)
    bool   follow = true;
    bool   metronome = false;
    int    arrSnap = 0;      // index into snaps(): Bar
    int    prSnap  = 4;      // 1/16
    Tool   tool = Tool::Select;
    std::vector<int> sel;    // selected clips (uids)
    int    selTrack = -1;
    int    editClip = -1;    // in the piano roll / audio pane
    bool   prFocus = false;  // the last press was in the pane under the lanes
    ArrDrag drag;

    // --- The piano roll's view ------------------------------------------------------
    double prPpt   = 0.0;    // 0 = fit the clip on the next frame
    double prView0 = 0.0;    // content tick at the grid's left edge
    float  prTop   = 84.0f;  // pitch of the top row
    float  rowH    = 12.0f;
    int    prFitFor = -1;    // the clip the view was last fitted to
    PrDrag pr;
    std::int64_t lastLen = sg::kPpq / 4;   // a drawn note's length: the last one drawn or picked
    int    lastVel = 100;
    struct Audition { int track = -1, pitch = -1; double until = 0.0; } heard;   // the note audition() is sounding

    // --- Clipboard ------------------------------------------------------------------
    struct ClipCopy { sg::TrackKind kind; int trackOffset; sg::Clip clip; };
    std::vector<ClipCopy> clipBoard;
    std::vector<sg::Note> noteBoard;     // ticks from the first note

    // --- Playing in -------------------------------------------------------------------
    bool qwerty = false;                 // the computer keyboard plays notes
    int  octave = 4;
    int  qwVel  = 100;
    std::map<int, int> qwHeld;           // ImGuiKey -> the pitch it is holding
    // The track each note played in went to: its off goes there too, even if
    // another track was selected while the key was down.
    std::map<int, int> heldTrack;        // pitch -> track uid (computer keyboard, keys)
    std::atomic<int>   liveTarget{-1};   // where a MIDI keyboard plays now
    int                midiHeld[128];    // the MIDI reader's: pitch -> track uid, -1
    bool recording = false;
    int  recClip = -1, recTrack = -1;
    struct Open { std::int64_t tick; int vel; };
    std::map<int, Open> recOpen;         // pitch -> where it went down
    std::unique_ptr<MidiIn> midiIn;
    std::string midiDevice;

    // --- Files and the status line ----------------------------------------------------------
    std::string status;
    bool        statusBad = false;
    double      statusAt = -100.0;
    char        nameBuf[128] = "";
    bool        askName = false;         // the Save As popup is wanted
    int         renameTrack = -1;
    bool        renameFocus = false;
    std::unique_ptr<sg::Bounce> bounce;
    std::string bounceTo;
    std::map<std::string, std::unique_ptr<WaveSlot>> waves;   // by path
    std::map<std::string, double> fileSeconds;                // by path
    std::string projectFile;
    bool        shown = false;           // the window is open (last frame)
    // Sound names to paths, kept for a while: the editor's lookup walks every
    // asset of the project, and a big project has tens of thousands.
    mutable std::map<std::string, std::pair<std::string, double>> pathCache;
    // Where the lanes were drawn last frame (screen y of each row's top, and
    // its bottom), for a file dropped on one.
    std::vector<float> rowTop;
    std::vector<int>   rowUid;
    float       laneX0 = 0.0f;
    bool        focused = false;
    int         focusFrame = -100;

    // --- Helpers (SongPanel.cpp) ---------------------------------------------------------------
    const Host* host = nullptr;          // this frame's

    void edited();                       // the song changed: dirty, the preview follows
    void checkpoint(const sg::Song& before);   // an undo step back to `before`
    void checkpoint() { checkpoint(song); }
    std::int64_t gridOf(int snapIndex) const;
    std::int64_t snapTo(std::int64_t t, int snapIndex) const;   // Alt held: not snapped
    bool  isSel(int uid) const;
    void  select(int uid, bool add);
    double playTicks() const;
    void  seek(std::int64_t t);
    bool  playing() const;
    void  liveNote(int trackUid, int pitch, int vel, bool on);
    void  audition(int trackUid, int pitch, int vel);
    std::string soundPath(const std::string& name) const;
    const fitzel::Waveform* waveform(const std::string& name);
    double fileLength(const std::string& name);   // seconds, 0 unknown
    void  say(const std::string& msg, bool bad = false);
    std::string songsDir() const;        // "" without a project
};

ImU32 trackColor(const sg::Song& s, const sg::Track& t);
// A colour brightened or darkened by `k`, its alpha scaled by `alpha`.
inline ImU32 mul(ImU32 c, float k, float alpha = 1.0f) {
    auto ch = [&](int shift) {
        const float v = static_cast<float>((c >> shift) & 0xFF) * k;
        return static_cast<int>(v > 255.0f ? 255.0f : v);
    };
    const int a = static_cast<int>(static_cast<float>((c >> IM_COL32_A_SHIFT) & 0xFF) * alpha);
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), a);
}
const char* drumName(int pitch);

// SongPianoRoll.cpp: the pane under the lanes -- the piano roll for a MIDI
// clip, the audio clip's settings and waveform for an audio one.
void drawEditorPane(Panel::State& st, const Host& h, ImVec2 size);
// Its keys, while it has them (st.prFocus). True when one was used.
bool editorPaneKeys(Panel::State& st);

} // namespace songui
