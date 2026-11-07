#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <imgui.h>

namespace fitzel { class Audio; }
namespace vseq { struct Film; }

// The editor's "Video Sequencer": a film cut together from the project's
// recordings, pictures, titles and songs (VideoSeq.hpp), watched while it is
// cut, and written out as a video through the machine's ffmpeg
// (VideoSeqRender.hpp) -- a trailer, an intro, a clip for a billboard in the
// game (.fvid).
//
// IT IS LAID OUT LIKE EVERY EDITING PROGRAM: the media brought in on the left,
// the picture in the middle, the selected clip's numbers on the right, the
// tracks across the bottom under a time ruler. And driven like one: drag media
// onto a track, drag a clip along it or onto another, pull its edges to trim,
// pull its top corners to fade, drag it onto its neighbour for a dissolve, S
// to cut at the playhead, Space to play, J K L, I and O for the range.
//
// THE EDITOR'S RULE HOLDS: every drag lands on a frame and snaps to the cuts
// near it, and every number a drag sets is in the inspector too, stepped a
// frame a click (and typed, when a number is known).
//
// Needs ffmpeg for anything with video or for any export but .fvid and WAV:
// without one the panel says how to get it and still cuts pictures, titles and
// sound.
namespace vsequi {

struct Host {
    fitzel::Audio* audio = nullptr;   // the preview's sound plays on it (null: silent)
    std::string    projectFile;       // the open .fitzel ("" none): films go in <project>/content/films
    // For bouncing the project's songs into the film (the Song Sequencer's own).
    std::function<std::string(const std::string&)> resolveSound;
    std::function<std::string(const std::string&)> resolvePatch;
    // A thing dragged in from the Assets panel (its GUID) to its file, "" when
    // it is nothing a film can show or play.
    std::function<std::string(const std::string& guid)> fileOfAsset;
    // Files dropped on the editor's window, and where (window pixels); the
    // panel takes the ones over it.
    std::vector<std::string>* drops = nullptr;
    const float* dropX = nullptr;
    const float* dropY = nullptr;
    // An export wrote into the project (a .fvid into videos/, a WAV into sounds/).
    std::function<void()> assetsChanged;
    // "Edit in the Song Sequencer" on a song's clip.
    std::function<void(const std::string& songName)> openSong;
    // Optional: where the panel drew its parts ("clip:<uid>", "lane:<track uid>",
    // "header:<track uid>", "ruler", "viewer", "bin:<n>", "lanes"), for
    // videoseqpanelcheck to press them as a person would.
    std::function<void(const std::string&, ImVec2, ImVec2)> probe;
};

// The film last open, kept in imgui.ini beside the area layout. Installed before
// the first frame reads the file (like areas::install).
void install();

class Panel {
public:
    Panel();
    ~Panel();
    Panel(const Panel&)            = delete;
    Panel& operator=(const Panel&) = delete;

    void draw(bool& show, const Host& h);

    // The window has the keyboard: Space, J K L, S, Del, the arrows, I/O and
    // Ctrl+Z/C/V/S are the film's, not the scene's.
    bool hasKeyboard() const;
    // Wants the editor drawn at a smooth rate: playing, something dragged, a
    // picture still coming, a song bouncing, an export running.
    bool wantsSmoothFrames() const;
    // Play is starting: the preview stops and the open film is saved.
    void beforePlay(const Host& h);
    // Another project: the open film is saved where it was and a new one begun.
    void projectChanged(const Host& h);
    // A film of the project by name (content/films/<name>, ".json" optional).
    bool open(const std::string& name, const Host& h);

    // --- For videoseqpanelcheck ---------------------------------------------
    vseq::Film& film();
    void   edited();                  // the film was changed from outside
    double playhead() const;
    void   seek(double t);
    bool   playing() const;
    const std::vector<int>& selection() const;
    void   select(const std::vector<int>& uids);
    bool   canUndo() const;
    // The picture in the viewer is the film at the playhead, all of it decoded.
    bool   viewerSettled() const;
    // The last thing said in the status line.
    std::string status() const;
    // Import files into the bin (as a drop would); placed on the timeline at
    // the playhead too when `place`.
    void   importFiles(const std::vector<std::string>& paths, bool place);
    bool   exporting() const;
    std::string exportMessage() const;
    void   exportTo(const std::string& path, int format);   // vseq::Format, as the dialog's Export button
    // The whole timeline fits the lanes on the next frame.
    void   fitTimeline();

    struct State;

private:
    std::unique_ptr<State> m;
};

} // namespace vsequi
