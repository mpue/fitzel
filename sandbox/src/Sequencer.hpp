#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "AnimSystem.hpp"

struct Entity;
struct CamKey;
namespace fitzel { class Audio; class Sound; }

// The sequencer: the scene's recorded animations, cameras and sounds laid out
// on one clock, the way an edit is laid out in a video editor.
//
// WHAT IT ADDS TO THE TIMELINE. The Timeline makes a clip -- keys on
// properties, in seconds from the clip's own start. It cannot say "this clip,
// then that one", "watch it through that camera", or "and the music starts on
// the cut". A sequence can: it places clips as STRIPS on tracks, and the
// track's kind says what a strip there does.
//
//   Camera  -- a shot. The strip names a camera entity, and while the playhead
//              is inside it that camera IS the view. Two shots side by side on
//              a camera track are a cut; nothing has to be scripted to switch.
//              A shot may also carry a clip -- the recorded move of that very
//              camera -- which plays inside the shot, so a camera animation and
//              the camera it moves sit in one block.
//   Anim    -- a clip from the scene's animation list, played from where the
//              strip starts: a door opening at 4 s, a lift at 12.
//   Audio   -- a sound file, like a region in a DAW: trimmed, faded, on its own
//              track with its own level and its own mixer channel.
//
// STRIPS REFER, THEY DO NOT COPY. A strip stores the clip's NAME and the
// camera's entity id, the same addresses an Animator and a CameraSwitcher use,
// so a clip re-recorded in the Timeline is the new take everywhere it is
// placed, and a sequence never holds a stale copy of an animation.
//
// THE SEQUENCE IS SCENE DATA, like the clips it places. It is saved into the
// .fitzel and the shipped player runs it: a cutscene is part of the level.
namespace seq {

enum class Kind { Camera = 0, Anim = 1, Audio = 2 };

const char* kindName(Kind k);

// One block on a track.
//
// Its place on the sequence is [start, start + length). What it plays from its
// source begins `offset` seconds in -- trimming the head of a strip moves both
// `start` and `offset` so the material under the playhead stays put, which is
// what trimming means in every editor anyone has used.
struct Strip {
    int   uid    = 0;     // stable within the sequence (voices, selection)
    float start  = 0.0f;  // seconds on the sequence
    float length = 4.0f;  // seconds on the sequence
    float offset = 0.0f;  // seconds into the source at `start`

    // Camera + Anim: the clip played under the strip ("" on a camera strip =
    // a held shot, the camera standing where it stands).
    std::string clip;
    float speed = 1.0f;   // clip seconds per sequence second
    // Run the clip round again when the strip outlasts it, rather than holding
    // its last pose. A turning windmill is a loop; a door opening is not.
    bool  loop  = false;

    // Camera: the entity whose view this shot is (-1 = none chosen yet).
    int   camera = -1;

    // Audio.
    std::string sound;      // file name under the project's sounds
    float gain    = 1.0f;   // linear, 0..2
    float fadeIn  = 0.0f;   // seconds
    float fadeOut = 0.0f;   // seconds

    float end() const { return start + length; }
    bool  covers(float t) const { return t >= start && t < end(); }
    // The source time under sequence time `t` (clip seconds or file seconds).
    float sourceTime(float t) const;
    // 0..1 envelope of the fades at sequence time `t`.
    float fadeAt(float t) const;
};

struct Track {
    int         uid  = 0;
    Kind        kind = Kind::Anim;
    std::string name;
    bool        mute = false;
    bool        solo = false;   // audio: only soloed tracks sound while any is
    bool        lock = false;   // strips cannot be moved or trimmed
    // Anim/Camera: past the end of a strip, keep its last pose until the next
    // strip on the track starts. On by default because the alternative -- the
    // object snapping back to its authored place the frame a clip ends -- is
    // never what a cutscene wants, and it is what "nothing is playing" would
    // otherwise look like.
    bool        hold = true;
    float       gain = 1.0f;          // audio: the track fader, linear 0..2
    std::string channel = "Music";    // audio: mixer channel by name
    std::vector<Strip> strips;        // kept sorted by start

    void sort();
};

struct Sequence {
    std::string name = "Sequence";
    float duration    = 30.0f;  // seconds; where the sequence ends
    float fps         = 30.0f;  // the snap grid and the timecode's frames
    bool  loop        = false;
    bool  playOnStart = false;
    // When the sequence is over, hand the view back to whatever had it before
    // the first shot. Off, the last shot stays the view -- an intro that ends
    // on the camera the game then plays from.
    bool  releaseCamera = true;
    std::vector<Track> tracks;
    int   nextUid = 1;

    int    newUid() { return nextUid++; }
    Track& addTrack(Kind k, const std::string& name = std::string());
    Track*       trackByUid(int uid);
    const Track* trackByUid(int uid) const;
    // The strip with this uid and the track it is on, or null.
    Strip* stripByUid(int uid, Track** owner = nullptr);
    // Last strip end on any track: what "the sequence is this long" really is.
    float  contentEnd() const;
    float  snap(float t) const;
};

// What the sequence says about one moment.
struct Frame {
    // The camera the cut is on (-1 = no shot covers or precedes this time).
    int  camera = -1;
    // The strip that cut is (its uid; -1 = none). Lets the panel light the live
    // shot without working the cut out a second time.
    int  shot   = -1;
};

// Which camera the sequence is cut to at `t`. Camera tracks are stacked like
// video tracks: the TOPMOST track with a shot under the playhead wins, so a
// second camera track is an overlay -- an insert shot dropped over the master
// without cutting the master into pieces. Between shots the last one holds.
Frame cutAt(const Sequence& s, float t);

// Pose the scene at sequence time `t`: every Anim strip, and every Camera strip
// that carries a clip, applied at its own source time. Tracks muted are skipped.
void applyAnim(const Sequence& s, const std::vector<anim::Clip>& clips,
               std::vector<Entity>& entities, float t);

// The clip names a sequence plays, so a preview can capture what they are about
// to overwrite.
std::vector<std::string> clipsUsed(const Sequence& s);

// The first camera a clip moves -- the camera a dropped camera animation is a
// shot OF. -1 when the clip moves no entity carrying a Camera.
int cameraOfClip(const anim::Clip& c, const std::vector<Entity>& entities);

// Length of a clip as a strip should get it by default, in sequence seconds.
float clipLength(const anim::Clip& c);

// --- Recorded camera paths ----------------------------------------------------
// The Camera path window records the FLY camera, which is no entity and so
// cannot be a shot. This turns such a recording into one: keys on camera entity
// `cameraId`'s position, rotation and field of view, so the recorded move plays
// as a clip on a real camera and can be cut against any other.
anim::Clip cameraClipFromPath(const std::vector<CamKey>& keys, int cameraId,
                              const std::string& name);
// The entity rotation (Euler degrees) that makes a Static camera look the way
// the fly camera does at this yaw/pitch (yaw 0 = +X, 90 = +Z).
glm::vec3 cameraRotationFor(float yawDeg, float pitchDeg);

// Split the strip at sequence time `t` (strictly inside it). Returns the new
// right half's uid, or -1 when `t` is not inside.
int split(Sequence& s, int stripUid, float t);

// --- Persistence ------------------------------------------------------------
// Under one "sequences" array in the scene. A scene without one has none.
void save(nlohmann::json& j, const std::vector<Sequence>& seqs);
void load(const nlohmann::json& j, std::vector<Sequence>& seqs);
int  find(const std::vector<Sequence>& seqs, const std::string& name); // -1 if none

// --- Running one ------------------------------------------------------------
// The playhead and everything it owns while it moves: the restore list of an
// editor preview, the camera it took, and the voices of the audio strips.
//
// One Runner, one sequence at a time. The editor previews through it and the
// game plays through it; the difference is `preview`, which makes it hand the
// scene back when it stops.
class Runner {
public:
    Runner();
    ~Runner();
    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    int   sequence = -1;    // index into the scene's list; -1 = idle
    float time     = 0.0f;
    bool  playing  = false;
    // An editor preview: captures what it overwrites and gives it back on stop.
    bool  preview  = false;

    // The camera cut and the strip it is, as of the last tick.
    Frame frame;

    // Audio. Without it the runner is silent (a check tool, a muted editor).
    // `resolve` turns a strip's file name into a path; `route` a track's mixer
    // channel name into an engine strip id (-1 = master).
    void bindAudio(fitzel::Audio* audio,
                   std::function<std::string(const std::string&)> resolve,
                   std::function<int(const std::string&)> route);
    float masterGain = 1.0f;

    // Start an editor preview of `seqIndex` (ending any other first). Captures
    // the camera that has the view so it comes back too.
    void beginPreview(int seqIndex, const std::vector<Sequence>& seqs,
                      const std::vector<anim::Clip>& clips,
                      std::vector<Entity>& entities, int activeCam);
    // End it: properties and camera back to what they were.
    void endPreview(const std::vector<anim::Clip>& clips,
                    std::vector<Entity>& entities, int& activeCam);

    // The game's own run: from the top, no restore.
    void start(int seqIndex, int activeCam);
    // Stop and fall silent. In the game, gives the view back if the sequence
    // asks for that; a preview is ended with endPreview instead.
    void stop(const std::vector<Sequence>& seqs, int& activeCam);

    // Move the playhead (editor scrub). Voices re-seek on the next tick.
    void seek(float t) { time = std::max(0.0f, t); m_jumped = true; }

    // One frame: advance if playing, pose the scene, take the cut, feed the
    // voices. Returns false when a non-looping game run has just finished.
    //
    // `dt` must be REAL time, not a frame time clamped for the physics: the
    // sound plays on the sound card's clock whatever the frame rate does, and
    // a sequence clock that loses time on a slow frame drifts away from it.
    // While a voice plays, the clock is also eased onto that voice's position,
    // so picture and sound stay locked over minutes without the sound ever
    // being yanked back to meet the picture.
    bool tick(const std::vector<Sequence>& seqs, const std::vector<anim::Clip>& clips,
              std::vector<Entity>& entities, float dt, int& activeCam);

    // Silence every voice (stop pressed, scene unloaded).
    void silence();
    // Forget everything: a scene was loaded or Play ended, and what this runner
    // captured or took belongs to a scene that is gone.
    void reset();

    bool active() const { return sequence >= 0 && (playing || preview); }

    // Diagnostics (seqaudiocheck): how many times a playing voice was reseated
    // since the runner was made, and how far the clock stood from the sound at
    // the last tick that had a voice to compare with (seconds, + = sound ahead).
    int   reseats() const { return m_reseats; }
    float skew()    const { return m_skew; }

private:
    struct Captured {
        anim::Track addr;   // just the address (keys left empty)
        float       value = 0.0f;
    };
    void capture(const Sequence& s, const std::vector<anim::Clip>& clips,
                 std::vector<Entity>& entities);
    void restore(std::vector<Entity>& entities);
    void feedAudio(const Sequence& s, float dt);
    void lockToAudio(float dt);

    std::vector<Captured> m_captured;
    int  m_camBefore   = -1;   // the view before the first shot took it
    int  m_camSet      = -2;   // what this runner last wrote into activeCam
    bool m_camReleased = false;// someone else took the view: stop fighting for it
    bool m_jumped      = false;
    int   m_reseats    = 0;
    float m_skew       = 0.0f;

    struct Voice;
    std::map<int, std::unique_ptr<Voice>> m_voices;   // strip uid -> voice
    std::map<std::string, float>          m_lengths;  // file -> seconds, asked once
    // Voices fading out after a stop: kept alive until the fade has played,
    // because a Sound destroyed mid-fade is the click the fade was for.
    std::vector<std::pair<std::unique_ptr<Voice>, float>> m_dying;
    void retireVoice(std::unique_ptr<Voice> v);
    fitzel::Audio* m_audio = nullptr;
    std::function<std::string(const std::string&)> m_resolve;
    std::function<int(const std::string&)>         m_route;
};

} // namespace seq
