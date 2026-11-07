#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The Video Sequencer's film: what is cut together, as data (VideoSeqPanel.hpp
// is the window, VideoSeqRender.hpp makes pictures and sound of it).
//
// A FILM IS TRACKS OF CLIPS ON ONE CLOCK, the way every editing program lays it
// out: video tracks stacked over each other (the top one covers the ones below,
// V1 is the bottom of the pile), audio tracks under them. A clip shows a piece
// of a source -- a video file, a picture, a sound, a song of the Song Sequencer
// -- or makes its own picture: a title, a plain colour.
//
// TIMES ARE SECONDS. Every edit lands on a frame of the film's rate (the panel
// snaps), but the numbers stay seconds so a film changed from 30 to 25 fps
// keeps its cuts where they were in time, not where they were in frames.
//
// TWO CLIPS OVERLAPPING ON ONE TRACK CROSS-FADE: over the overlap the later one
// comes up over the earlier (picture) and the two sounds trade places (equal
// power). That is the whole transition system -- drag a clip onto its
// neighbour and the cut becomes a dissolve, drag it off and it is a cut again.
//
// Sources are named the way the project names them: a path relative to the
// project folder for anything inside it, absolute for anything outside (a
// video is not copied into the project -- recordings are gigabytes).
//
// Nothing here needs ffmpeg; the files it names do (VideoSeqMedia.hpp).
namespace vseq {

enum class TrackKind { Video, Audio };
enum class ClipKind { Video, Image, Audio, Text, Color };
// How a picture fills the frame at scale 1: all of it inside (black bars where
// the shapes differ), the frame covered (the overhang cut off), or pulled to
// the frame's shape.
enum class Fit { Fit = 0, Fill = 1, Stretch = 2 };

const char* kindName(ClipKind k);
const char* fitName(Fit f);

// Where a picture sits. x/y move its centre, in fractions of the frame (0 is
// the middle, 0.5 the right/bottom edge), so a film rendered at another size
// keeps its layout. Scale 1 is the Fit's size; rotation in degrees, clockwise.
struct Look {
    float x = 0.0f, y = 0.0f;
    float scale    = 1.0f;
    float rotation = 0.0f;
    float opacity  = 1.0f;
    bool operator==(const Look&) const = default;
};
// The look `f` (0..1) of the way from a to b.
Look mix(const Look& a, const Look& b, float f);

struct Rgba {
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 1.0f;
    bool operator==(const Rgba&) const = default;
};

// A title's words and how they are set. Size is a fraction of the frame's
// height (0.08: a line is 8% of the picture tall) so it scales with the film.
struct TextStyle {
    std::string text = "Title";
    std::string font;                 // a .ttf path; "" = the first system font
    float size    = 0.08f;
    Rgba  color{1.0f, 1.0f, 1.0f, 1.0f};
    int   align   = 1;                // 0 left, 1 centred, 2 right (the lines among each other)
    bool  bold    = false;            // a heavier stroke (an outline in the text's own colour)
    float outline = 0.0f;             // width, a fraction of the size
    Rgba  outlineColor{0.0f, 0.0f, 0.0f, 1.0f};
    bool  shadow  = true;
    float box     = 0.0f;             // a plate behind the words: its opacity (0: none)
    Rgba  boxColor{0.0f, 0.0f, 0.0f, 1.0f};
    bool operator==(const TextStyle&) const = default;
};

struct Clip {
    int         uid  = 0;
    ClipKind    kind = ClipKind::Video;
    std::string name;
    std::string file;                 // Video, Image, Audio: the source (Audio may be a song's .json)
    double      start  = 0.0;         // on the film's clock
    double      length = 1.0;         // how long it is on the film
    double      in     = 0.0;         // where in the source it starts
    double      speed  = 1.0;         // source seconds per film second (Video, Audio)

    Look look;                        // pictures: where and how solid
    bool animate = false;             // the look moves from `look` to `lookEnd` over the clip
    Look lookEnd;
    Fit  fit = Fit::Fit;

    float fadeIn  = 0.0f;             // seconds: the picture up from nothing, the sound up from silence
    float fadeOut = 0.0f;
    float volume  = 1.0f;             // the sound of a Video or Audio clip
    bool  muted   = false;            // a Video clip's own sound off

    TextStyle text;                   // Text
    Rgba      color{0.0f, 0.0f, 0.0f, 1.0f};   // Color

    double end() const { return start + length; }
    bool   visual() const { return kind != ClipKind::Audio; }
    bool   audible() const { return kind == ClipKind::Audio || (kind == ClipKind::Video && !muted); }
    // How long the source runs under this clip (length times speed).
    double sourceLength() const { return length * speed; }
    // The source's time at film time `t` (not clamped to the clip).
    double sourceAt(double t) const { return in + (t - start) * speed; }
    // The look at film time `t`.
    Look lookAt(double t) const;
};

struct Track {
    int               uid  = 0;
    TrackKind         kind = TrackKind::Video;
    std::string       name;
    bool              muted  = false; // audio: silent; video: its clips' own sound silent
    bool              hidden = false; // video: not drawn
    bool              locked = false; // nothing on it moves
    float             volume = 1.0f;
    std::vector<Clip> clips;          // kept sorted by start (sortClips)
};

struct Film {
    std::string name   = "Untitled";
    int         width  = 1920;
    int         height = 1080;
    double      fps    = 30.0;
    Rgba        background{0.0f, 0.0f, 0.0f, 1.0f};
    // In display order: the video tracks top to bottom (the top one is drawn
    // last, over everything), then the audio tracks.
    std::vector<Track>       tracks;
    std::vector<std::string> media;   // the bin: every file brought in, used or not
    double markIn = -1.0, markOut = -1.0;   // the range to export (-1: none)
    int    nextUid = 1;

    int newUid() { return nextUid++; }

    // A video and an audio track, empty.
    static Film starter();

    double duration() const;          // the end of the last clip
    double frame() const { return fps > 0.0 ? 1.0 / fps : 1.0 / 30.0; }
    // `t` on the nearest frame of the film's rate.
    double snap(double t) const;
    std::int64_t frameOf(double t) const;

    Track*       track(int uid);
    const Track* track(int uid) const;
    Clip*        clip(int uid);
    const Clip*  clip(int uid) const;
    Track*       trackOf(int clipUid);
    const Track* trackOf(int clipUid) const;
    int          rowOf(int trackUid) const;   // index into tracks, -1

    // A new track of `kind`: video tracks go on top of the video pile, audio
    // tracks under the last one. Returns its uid (not a reference: a later add
    // would leave one dangling).
    int addTrack(TrackKind kind, const std::string& name = {});
    // `c` onto track `trackUid` (a uid is given when it has none). Its uid, or
    // -1 when the track is not there.
    int addClip(int trackUid, Clip c);
    bool removeClip(int uid);
    void sortClips();

    // The clips at film time `t` on track `row`, in drawing order (the later
    // starting on top), and how far each has come up over the one before
    // (1 = fully; under 1 during the overlap with its predecessor).
    struct Active { const Clip* clip; float dissolve; };
    std::vector<Active> activeAt(std::size_t row, double t) const;

    // Edits -------------------------------------------------------------------
    // Cut clip `uid` in two at film time `t`; the right half's uid, -1 when `t`
    // is not inside it (a frame from either end).
    int split(int uid, double t);
    // Take clips out and close the gap each leaves on its track (everything
    // later on that track moves left by the clip's length, as far as the gap
    // goes).
    void rippleDelete(const std::vector<int>& uids);
    // Every cut point (clip starts and ends, 0), sorted, unique.
    std::vector<double> editPoints() const;

    // Files ------------------------------------------------------------------
    std::string toJson() const;
    static bool fromJson(const std::string& text, Film& out, std::string* error = nullptr);
    bool        save(const std::string& path, std::string* error = nullptr) const;
    static bool load(const std::string& path, Film& out, std::string* error = nullptr);
};

// Is `path` a source the sequencer takes, and as what?
bool isVideoFile(const std::string& path);
bool isImageFile(const std::string& path);
bool isAudioFile(const std::string& path);   // sounds, and a Song Sequencer song (.json under songs/)
bool isSongFile(const std::string& path);

} // namespace vseq
