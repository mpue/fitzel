#pragma once

#include <functional>

#include <glm/glm.hpp>

#include "CameraSystem.hpp"   // camerasys::Pose

// The multishot camera: a director that shoots ONE object from a rotating set of
// the moves an advert or a game replay is cut from -- a hero angle, an orbit, a
// crane down, a slider past, a push-in, a dolly zoom, a wheel-level pass, a
// car-to-car track, a fly-by, an overtake.
//
// WHY THIS IS NOT A CAMERA PATH. The recorder in CameraPath.hpp already plays a
// spline of authored keyframes, and that is the right tool when the subject
// stands still and the move is exactly the one drawn. It is the wrong tool for a
// car: every keyframe is a WORLD position, so the moment the subject drives
// somewhere else the whole path has to be authored again. What a car advert is
// actually made of is a handful of moves expressed RELATIVE TO THE SUBJECT --
// "three quarters front, one car length out, drifting in" -- and those hold for
// any car, anywhere on the track, at any speed. That is what this stores.
//
// WHY IT IS ALSO NOT A FOLLOW CAMERA. A follow camera is one shot held forever;
// it exists to be flown behind, not to be watched. This cuts. A shot runs for a
// few seconds, ends, and the next one starts from a different angle -- which is
// the entire difference between footage and a viewport.
//
// WHAT "INTELLIGENT" MEANS HERE, concretely, because it is easy to claim and
// easy to fake:
//   * Every distance is derived from the SUBJECT'S OWN SIZE. The same settings
//     frame a go-kart and an articulated lorry, because the numbers in a shot are
//     multiples of its bounding box, never metres.
//   * The shot list adapts to the subject's SPEED. A fly-by needs something to
//     fly by: below walking pace it is a camera watching a parked car go nowhere,
//     so it is weighted out, and the standing moves (orbit, crane, turntable
//     hero) are weighted in. Past motorway speed it is the other way round.
//   * The eye stays out of the ground. Shots that live near the tarmac -- the
//     wheel-level pass especially -- are the good ones, and they are exactly the
//     ones that end up inside a hill without a clearance check.
//   * No shot repeats back to back, and consecutive shots alternate flanks, so a
//     sequence reads as coverage rather than as one angle jittering.
//
// The result is a camerasys::Pose like every other camera in the scene, so the
// viewport, Play, the exported player and split screen all take it without
// knowing anything about shots.
namespace multishot {

// The catalogue. Order is the order the inspector lists them in and the order a
// sequential (non-shuffled) run plays them in -- roughly wide-and-calm first,
// fast-and-close last, which is also how a cut usually escalates.
enum Shot {
    Hero = 0,   // three-quarter front, held, breathing slowly inward
    Orbit,      // circles the subject -- the turntable
    Crane,      // descends from above onto the subject (jib down)
    Slider,     // straight lateral travel, aim locked: parallax past the subject
    PushIn,     // dollies in on a fixed angle
    PullOut,    // dollies out and up: the reveal
    DollyZoom,  // pulls out while the lens narrows -- the Vertigo effect
    TopDown,    // bird's eye, drifting round
    LowAngle,   // wheel height, close, looking up as it passes
    Tracking,   // car-to-car: holds a flank offset and rides along
    Nose,       // ahead of the subject looking back at it
    FlyBy,      // eye planted in the world, subject rushes past
    Overtake,   // comes past from behind and lets the subject fall away
    ShotCount
};

// "Orbit", "Dolly zoom", ... for the inspector.
const char* shotName(int shot);
// A stable key for the scene file. Separate from the label so a shot can be
// renamed, and so inserting one in the middle of the enum cannot silently turn
// every saved camera's shot list into a different one.
const char* shotKey(int shot);
// One line saying what the shot does, for the inspector's tooltip.
const char* shotHint(int shot);

// Terrain height under (x, z). Optional -- an empty function simply means the
// clearance check has no ground to check against (a scene with no terrain), not
// that it is switched off.
using GroundFn = std::function<float(float, float)>;

// What the author sets. All of it lives on the CameraComponent, so it is scene
// data: a shot list is part of the shot, not of the session.
struct Settings {
    // --- Timing ---
    float duration = 4.0f;   // seconds per shot (scaled per shot type, see below)
    float variance = 0.25f;  // +/- fraction of random length, so cuts aren't metronomic
    float blend    = 0.0f;   // 0 = hard cut (the filmic default); else cross-fade seconds

    // --- Framing ---
    // A multiplier on distances the subject's bounding box already decides, NOT a
    // distance. 1 is "a couple of car lengths out", which is what the moves were
    // tuned against; 0.6 is tight and aggressive, 2 is airy and architectural.
    float distance = 1.0f;
    float height   = 1.0f;   // same, for how high the eye rides
    float speed    = 1.0f;   // how far a move travels in its time (the move's pace)
    float lead     = 0.0f;   // aim this many seconds ahead of a moving subject

    // --- Look ---
    // The base lens. NOT authored here: it is the camera entity's own FOV,
    // copied in by whoever drives the director, so the one field the inspector
    // already shows on every camera means the same thing on this one. Shots
    // scale it (a wheel-level pass goes wider, a car-to-car track goes longer)
    // and the dolly zoom drives it outright.
    float fov   = 45.0f;
    float dutch = 0.0f;      // max tilt of the horizon, degrees (0 = level)
    float shake = 0.0f;      // handheld: 0 = locked off, 1 = shoulder-mounted

    // --- Safety ---
    float clearance = 0.6f;  // metres the eye keeps above the ground

    // --- Sequencing ---
    bool sequential = false; // play the enabled shots in list order (a storyboard)
                             // instead of picking by weight (coverage)
    int  seed       = 1;     // the same seed gives the same run -- a take you can
                             // re-record after changing a light

    // Which shots are in the rotation. All on by default: the point of the
    // component is that it works the moment it is added, and switching the ones
    // you don't want off is a faster read than switching thirteen on.
    bool use[ShotCount] = {true, true, true, true, true, true, true,
                           true, true, true, true, true, true};
};

// What the director is told about its subject each frame. Velocity is NOT here:
// the director measures it itself, because it is the only thing that remembers
// the subject between frames and a caller computing it would have to keep the
// same state twice.
struct Subject {
    glm::vec3 center{0.0f};   // world centre
    glm::vec3 half{1.0f};     // world half-extents -- every distance scales off these
    float     yaw = 0.0f;     // heading in radians (sceneHeading's convention)
};

// One camera's running shot. Live state, kept beside the camera entity rather
// than on the component, for the same reason CameraSystem keeps its easing there:
// a prefab, an undo step or Play's snapshot copies components around, and none of
// them should drag "we are 2.3 seconds into an orbit" along with them.
class Director {
public:
    // Advance the sequence by `dt` and give this frame's pose. Safe to call with
    // a subject that jumped (a respawn, a scene load): a jump only moves the
    // shot, it does not send the camera swooping, because every shot but the
    // fly-by is expressed in the subject's frame.
    camerasys::Pose update(const Subject& s, const Settings& st, float dt,
                           const GroundFn& ground);

    // Forget everything: back to the first shot, no measured speed, no blend.
    void reset();

    // End this shot now -- the next frame starts the next one. What the
    // inspector's "Cut" button does, and what a script would call to punch a
    // camera on a beat.
    void cut();

    // Jump straight to `shot` and hold it there until the next cut. Lets the
    // inspector audition one move without waiting for the rotation to offer it.
    void play(int shot);

    int   shot()      const { return m_shot; }      // what is on screen now
    float remaining() const;                        // seconds left of it
    float speed()     const { return m_speed; }     // measured subject speed (m/s)

private:
    // Pick the next shot: never the one just played, weighted by how well each
    // move suits the speed the subject is actually doing (see the file header).
    int  choose(const Settings& st);
    void start(int shot, const Subject& s, const Settings& st);
    unsigned rand32();
    float    rand01();

    int   m_shot     = -1;      // -1 = nothing started yet (also: what choose()
                                // must not pick again -- see there)
    int   m_forced   = -1;      // play() request, honoured at the next start
    float m_t        = 0.0f;    // seconds into the current shot
    float m_len      = 1.0f;    // its length
    float m_ang      = 0.0f;    // this shot's azimuth around the subject
    float m_side     = 1.0f;    // which flank it works from (+1 / -1)
    float m_clock    = 0.0f;    // free-running, for the handheld noise

    // Where the fly-by planted its tripod. World space on purpose: the whole
    // point of that shot is that the eye does NOT go with the subject.
    glm::vec3 m_anchor{0.0f};

    // Subject motion, measured. Smoothed hard: shot selection reads it, and a
    // single stuttering frame must not be able to flip the whole rotation from
    // standing moves to travelling ones.
    glm::vec3 m_lastCenter{0.0f};
    glm::vec3 m_vel{0.0f};
    float     m_speed  = 0.0f;
    bool      m_seen   = false;

    // What the last frame was pointed at, and the outgoing shot's version of the
    // same, held for a cross-fade when blend > 0.
    //
    // KEPT AS A STANDPOINT AND AN AIM POINT rather than as a finished pose,
    // because that is what makes a fade watchable. Interpolating two poses
    // interpolates two DIRECTIONS, and halfway between two shots that face each
    // other the camera faces neither -- measured at 52 degrees off the subject,
    // i.e. a second of empty scenery in the middle of every fade. Both shots aim
    // at the same object, so blending the two aims keeps the object in the middle
    // of frame the whole way across, and the fade reads as a camera move.
    glm::vec3 m_lastEye{0.0f}, m_lastAim{0.0f, 0.0f, -1.0f};
    float     m_lastFov = 45.0f, m_lastDutch = 0.0f;
    glm::vec3 m_fromEye{0.0f}, m_fromAim{0.0f, 0.0f, -1.0f};
    float     m_fromFov = 45.0f, m_fromDutch = 0.0f;
    float     m_blend = 0.0f;   // seconds left of the fade
    float     m_blendLen = 0.0f;

    unsigned m_rng = 0x9e3779b9u;
    int      m_seed = 0;              // the Settings seed the RNG was primed from
};

} // namespace multishot
