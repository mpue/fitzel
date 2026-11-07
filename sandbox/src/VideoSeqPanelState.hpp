#pragma once

// The Video Sequencer panel's insides (VideoSeqPanel.hpp), shared by
// VideoSeqPanel.cpp -- the menus, transport, bin, viewer, inspector, export and
// files -- and VideoSeqTimeline.cpp, the ruler, the track headers and the lanes.

#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

#include <fitzel/audio/SamplePlayer.hpp>
#include <fitzel/audio/SongEngine.hpp>
#include <fitzel/graphics/Texture.hpp>

#include "VideoSeq.hpp"
#include "VideoSeqMedia.hpp"
#include "VideoSeqPanel.hpp"
#include "VideoSeqRender.hpp"

namespace vsequi {

namespace vs = vseq;

// A press on the timeline or in the viewer, and what it became.
struct Drag {
    enum Mode { None, Move, TrimL, TrimR, FadeIn, FadeOut, Rubber, Scrub, MarkIn, MarkOut, HScroll,
                ViewMove, ViewScale, ViewRotate, Split };
    Mode     mode = None;
    vs::Film base;                  // the film as it was when the press began
    int      anchor = -1;           // the clip pressed on
    int      row = -1;              // the track row pressed on
    double   pressT = 0.0;          // film time under the press
    ImVec2   press{};
    bool     moved = false, copy = false;
    bool     endLook = false;       // the viewer drag edits the clip's end look
    std::vector<int> before;        // the selection before a rubber band
    std::vector<int> items;         // the clips a Move drags (picked at the press)
    float    f0 = 0.0f, f1 = 0.0f;  // what a viewer drag started from
    int      splitter = 0;          // Split: 0 the top/bottom, 1 the bin's edge, 2 the inspector's
};

// A source's thumbnails on the GPU, for the clips on the lanes and the bin.
struct ThumbTex {
    std::shared_ptr<const vs::ThumbStrip> strip;
    fitzel::Texture tex;
};

struct Panel::State {
    // --- The film -----------------------------------------------------------------
    vs::Film      film = vs::Film::starter();
    std::string   path;                    // the file it came from ("" = not saved yet)
    bool          dirty = false;
    std::uint64_t rev = 1;
    struct Step {
        vs::Film         film;
        std::vector<int> sel;
    };
    std::vector<Step> undo, redo;

    // --- Sources and pictures -------------------------------------------------------
    vs::Library                     lib;
    std::unique_ptr<vs::Preview>    preview;
    std::shared_ptr<const vs::Film> snapshot;      // the film as the preview draws it
    std::uint64_t                   snapRev = 0;
    std::map<std::string, ThumbTex> thumbs;        // by file
    std::map<std::string, fitzel::Texture> pictures;   // the bin's thumbnails of pictures

    // --- The viewer -------------------------------------------------------------------
    fitzel::Texture viewTex;
    int           viewW = 0, viewH = 0;            // the texture's size
    double        shownT = -1.0;                   // what it shows
    std::uint64_t shownRev = 0;
    bool          shownComplete = false;
    double        askedT = -1.0;                   // what was last asked for
    std::uint64_t askedRev = 0;
    int           askedW = 0, askedH = 0;
    int           previewDiv = 0;                  // 0 auto, 1 full, 2 half, 4 quarter
    bool          safeAreas = false;
    ImVec2        viewA{}, viewB{};                // where the picture was drawn
    std::vector<std::string> problems;             // the preview's, last frame

    // --- The transport ------------------------------------------------------------------
    double playhead = 0.0;
    bool   playing = false;
    bool   loop = false;
    double playFrom = 0.0, playWall = 0.0;   // film time and clock when play began (or was resynced)
    float  shuttle = 1.0f;                   // L pressed again: faster
    fitzel::SamplePlayer player;
    bool   playerTried = false;
    // The sound for the preview, mixed on a worker after every edit.
    struct Mix { std::vector<float> pcm; bool whole = true; };
    std::future<Mix> mixJob;
    double mixAt = -100.0;                   // when the last mix began (wall clock)
    std::uint64_t mixRev = 0, mixedRev = 0;  // the revision being mixed; the one the player has
    double mixFrom = 0.0, mixJobFrom = 0.0;  // where the player's samples (the job's) begin
    double mixSpan = 0.0;
    bool   mixWhole = true;                  // the last mix had every sound it needed

    // --- The timeline's view ------------------------------------------------------------
    double pps = 0.0;        // pixels per second; 0 = fit on the next frame
    double view0 = 0.0;      // the time at the lanes' left edge
    float  scrollY = 0.0f;
    float  laneW = 600.0f;   // last frame's
    bool   snap = true;
    bool   follow = true;
    float  bottomH = 0.0f;   // the timeline's height (0 = default)
    float  binW = 0.0f, inspW = 0.0f;
    std::vector<int> sel;    // selected clips (uids)
    int    selTrack = -1;
    Drag   drag;
    double snapShown = -1.0; // where the last drag snapped (a line on the lanes)
    ImVec2 lanesA{}, lanesB{};   // where the lanes were drawn (a file dropped there lands in time)

    // --- Clipboard ------------------------------------------------------------------------
    struct ClipCopy { vs::TrackKind kind; int rowOffset; vs::Clip clip; };
    std::vector<ClipCopy> board;

    // --- Songs ----------------------------------------------------------------------------
    std::unique_ptr<fitzel::song::Bounce> bounce;
    std::string bounceFile;          // the song being bounced
    std::vector<std::string> bounceFailed;   // songs that would not load (not tried again)

    // --- Export ---------------------------------------------------------------------------
    vs::Exporter       exporter;
    vs::ExportSettings exportSet;
    bool exportDialog = false;       // the settings window is open
    bool exportPending = false;      // start once the songs are bounced
    bool exportWatch = false;        // a running export's progress is shown
    int  exportPreset = 0;           // the size row in the dialog
    int  exportRange = 0;            // 0 whole film, 1 in to out
    char exportPath[1024] = "";

    // --- Files and the status line ----------------------------------------------------------
    std::string status;
    bool        statusBad = false;
    double      statusAt = -100.0;
    char        nameBuf[128] = "";
    bool        askName = false;
    char        binFilter[64] = "";
    int         binSel = -1;         // the bin row last clicked
    bool        focused = false;
    int         focusFrame = -100;
    bool        shown = false;
    const Host* host = nullptr;
    std::string projectFile;

    // --- Helpers ------------------------------------------------------------------------------
    void edited();
    void checkpoint(const vs::Film& before);
    void checkpoint() { checkpoint(film); }
    void say(const std::string& msg, bool bad = false);
    bool isSel(int uid) const;
    void select(int uid, bool add);
    void seek(double t);
    void play();
    void stop();
    std::string projectDir() const;
    std::string filmsDir() const;
    // A path on disk as the film names it: relative inside the project.
    std::string relative(const std::string& abs) const;
    std::string absolute(const std::string& file) const;
    // How long a clip's source runs (seconds), -1 when it does not end (a
    // picture, a title) or is not known yet.
    double sourceSeconds(const vs::Clip& c);
    double end() const;          // the film's end, at least a few seconds (an empty film has room)
    ThumbTex* thumbsOf(const std::string& file);
    std::string filmName() const;
};

// Shared between the two files ---------------------------------------------------------------

// HH:MM:SS:FF at the film's rate (the frames dropped when `frames` is false).
std::string timecode(double t, double fps, bool frames = true);
ImU32 clipColor(const vs::Clip& c);
// The kind a file makes on the timeline.
vs::ClipKind kindOfFile(const std::string& file);

// A new clip of `file` at `at` on a track that takes it: `trackUid` when that
// one does, else the first fitting track (a new one when none is). Its uid, -1.
int  addMediaClip(Panel::State& st, const std::string& file, int trackUid, double at);
int  addMadeClip(Panel::State& st, vs::ClipKind kind, int trackUid, double at);   // a title or a colour
void deleteClips(Panel::State& st, const std::vector<int>& uids, bool ripple);
void splitAt(Panel::State& st, double t, std::vector<int> uids);
void copyClips(Panel::State& st);
void pasteClips(Panel::State& st, double at);
void duplicateClips(Panel::State& st);
void detachSound(Panel::State& st, int uid);
void bringIn(Panel::State& st, const std::string& path);   // into the bin (and the project's name for it)

// The ruler, the track headers and the lanes, `size` big at the cursor.
void timeline(Panel::State& st, const Host& h, ImVec2 size);

} // namespace vsequi
