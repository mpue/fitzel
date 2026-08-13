#pragma once

#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>

namespace fitzel {
class Shader;
class Renderer;
class Input;
struct PointLight;
}

// Homing missiles with a seeker lock, for the hover racer.
//
// The whole weapon lives in here -- acquisition, the missiles' flight, the
// effects they leave and every pixel of their HUD -- because a weapon is one
// feature and it should be one file. main only tells it who is shooting and who
// is out there; everything else, including which button fires, is decided here.
//
// How it plays:
//   * Aim: hold an enemy craft inside the seeker cone ahead of the nose. A lock
//     takes `lockTime` seconds to build, so a missile is earned by flying a line,
//     not by tapping a button. The lock survives `lockHold` seconds outside the
//     cone, which is what lets you shoot around a corner.
//   * Fire (F / pad X) launches one at the locked craft; T / pad Y steps to
//     another target inside the cone. There is no free-fire: without a lock the
//     HUD says so and nothing leaves the rail. That is the point of the lock.
//   * The missile flies out of its rail sideways, then the seeker takes over and
//     pulls lead on where the target is GOING (a racer at 300 km/h is nowhere
//     near where it looks). It detonates on proximity, on the ground, or burns
//     out.
//
// The marked target is unmissable, by design and in three places at once: a
// billboarded bracket cage that closes in as the lock builds, a ring spinning
// around the craft in the world, and a screen reticle with its name and range.
// A pilot has to know which craft the next missile is going to, and one cue in
// the corner of the screen is not enough when both hands are busy.
//
// Damage is NOT applied here: update() collects `Hit`s and the caller decides
// what a hit costs (main knocks the AI off its line and drops its speed). The
// system has no idea what an opponent is, which is what keeps it reusable for
// whatever else ends up flying.
class WeaponSystem {
public:
    explicit WeaponSystem(fitzel::Shader& lit);

    // One craft the seeker can see. Built by the caller from whatever it
    // considers a rival; velocity is derived in here from frame to frame, so a
    // caller that has no velocity handy (the AI racers move kinematically) does
    // not have to invent one.
    struct Racer {
        int         id = -1;
        glm::vec3   pos{0.0f};
        float       radius = 2.4f;  // hull radius, for the proximity fuse
        std::string name;
    };

    // The shooter's frame: who is flying, where, and what the camera sees (the
    // world markers billboard toward it).
    struct Frame {
        float     dt = 0.0f;
        bool      armed = false;         // a glider is being flown -> weapons live
        bool      mayFire = true;        // false on the grid / after the flag
        glm::vec3 pos{0.0f};
        glm::vec3 fwd{0.0f, 0.0f, 1.0f};
        glm::vec3 up{0.0f, 1.0f, 0.0f};
        glm::vec3 vel{0.0f};
        glm::vec3 camPos{0.0f};
        const fitzel::Input* input = nullptr; // null = no firing this frame
    };

    // One craft caught by a detonation. `dir` is the missile's travel direction,
    // so the caller can shove the victim the way it was hit.
    struct Hit {
        int       targetId = -1;
        glm::vec3 pos{0.0f};
        glm::vec3 dir{0.0f, 0.0f, 1.0f};
        float     damage = 0.0f;   // 0..1 of a full hit (blast falls off)
        bool      direct = false;  // took it on the nose vs. caught in the blast
    };

    // Advance acquisition, every missile in the air and every effect left over.
    // `racers` is this frame's field (the shooter must not be in it).
    void update(const Frame& f, const std::vector<Racer>& racers);

    // Missiles, their trails, the blasts and the target's world marker.
    void render(fitzel::Renderer& renderer);

    // Motor + detonation lights, appended to the frame's point lights (the
    // caller's budget: it stops when the renderer's limit is reached).
    void collectLights(std::vector<fitzel::PointLight>& out) const;

    // Reticle, lock brackets, target readout and the launcher's ammo row.
    void drawHud(ImDrawList* dl, const ImVec2& vmin, const ImVec2& vsize,
                 const glm::mat4& viewProj);

    // A "pick a sound asset" combo, supplied by the caller (the picker walks the
    // project's asset database, which lives in main). Same shape the Glider and
    // Boost Pad inspectors use, so the weapon's SFX are chosen exactly like
    // every other sound in the editor.
    using SoundPicker = std::function<void(const char*, std::string&)>;

    // Editor: the tunables, as one call. Without a picker the sound fields fall
    // back to plain text boxes.
    void settingsPanel(const SoundPicker& soundPicker = {});

    // Play start/stop and race restart: nothing in the air, nothing locked, and
    // the rack back to `ammoStart`.
    void reset();

    // Put rounds on the rail (a pickup was flown over). Returns how many were
    // actually taken, which is 0 when the rack is already full -- the caller
    // uses that to leave the pickup standing instead of spending it on nothing.
    int addAmmo(int rounds);

    // Hits collected during the last update(). Cleared at the top of each one,
    // so the caller reads them right after updating.
    const std::vector<Hit>& hits() const { return m_hits; }

    // --- Wiring the caller supplies -----------------------------------------
    // One-shot SFX by Sound-asset filename, gain and pitch -- the same sink the
    // race cues use. Unset = silent.
    std::function<void(const std::string&, float, float)> playCue;
    // Ground height under (x, z) at or below `yMax`, so a missile that misses
    // ploughs in instead of flying through the scenery -- and so one passing
    // UNDER a bridge is not detonated by the deck over its head. Same signature
    // as the glider's own ground query. Unset = missiles ignore the ground.
    std::function<float(float, float, float)> groundHeight;

    // --- Tunables ------------------------------------------------------------
    bool  enabled     = true;
    // Seeker
    float lockAngle   = 26.0f;   // half-angle of the cone ahead of the nose (deg)
    float lockRange   = 340.0f;  // how far it can see (m)
    float lockTime    = 0.85f;   // seconds of tracking before the lock is solid
    float lockHold    = 0.8f;    // seconds a solid lock survives off-cone
    // Launcher
    int   ammoMax     = 4;
    // What the rack holds at the start of a race. 0 means every round has to be
    // found on the track (see MissilePickupComponent), which is what makes the
    // pickups worth a detour.
    int   ammoStart   = 0;
    // Seconds to put one back on the rail on its own. 0 (the default) switches
    // self-reloading OFF entirely: with pickups feeding the launcher, a rack
    // that refills by waiting would make flying over them pointless.
    float reloadTime  = 0.0f;
    float fireDelay   = 0.5f;    // seconds between launches
    // Missile
    float missileSpeed = 135.0f; // top speed (m/s)
    float missileAccel = 150.0f; // how hard it gets there (m/s^2)
    float missileTurn  = 150.0f; // seeker authority (deg/s)
    float missileLife  = 9.0f;   // seconds of fuel
    float armTime      = 0.32f;  // launch arc before the seeker takes over (s)
    float fuseRadius   = 3.2f;   // proximity fuse, on top of the hull radius (m)
    float blastRadius  = 9.0f;   // how far the detonation reaches (m)
    // SFX (Sound-asset filenames; missing files are silently skipped). Every one
    // of them is scaled by `soundGain`, so the whole weapon can be turned down
    // (or off) without hunting through five fields.
    float       soundGain = 1.0f;
    std::string soundSeek = "missile_seek.wav";
    std::string soundLock = "missile_lock.wav";
    std::string soundFire = "missile_launch.wav";
    std::string soundHit  = "missile_hit.wav";
    std::string soundDeny = "missile_deny.wav";

    // --- What the HUD and the caller can ask about the lock ------------------
    int   lockedTarget() const { return m_locked ? m_trackId : -1; }
    int   trackedTarget() const { return m_trackId; }
    float lockProgress() const { return m_lockT; }  // 0..1
    int   ammo() const { return m_ammo; }

private:
    // A point on a missile's ribbon. Same shape as the contrail's, but the two
    // ribbons here (a white-hot core and the smoke around it) age at different
    // rates off one list of points.
    struct Pt { glm::vec3 pos; float age = 0.0f; };

    struct Missile {
        glm::vec3 pos{0.0f}, vel{0.0f}, dir{0.0f, 0.0f, 1.0f};
        glm::vec3 launchDir{0.0f, 0.0f, 1.0f};
        int   target = -1;
        float age = 0.0f, life = 9.0f;
        float speed = 0.0f;
        float flicker = 0.0f;
        std::deque<Pt> pts;
        std::vector<fitzel::Vertex> hotVerts, smokeVerts;
        fitzel::Mesh hotMesh, smokeMesh;
    };

    // A detonation: the fireball, the shock ring, the sparks and the smoke it
    // throws. All of it dies inside `life`, so a blast leaves nothing behind.
    struct Blast {
        struct Spark { glm::vec3 pos, vel; float life = 0.0f, life0 = 1.0f; };
        struct Puff  { glm::vec3 pos, vel; float r0 = 1.0f, life = 0.0f, life0 = 1.0f; };
        glm::vec3 pos{0.0f}, dir{0.0f, 0.0f, 1.0f};
        float age = 0.0f, life = 1.4f;
        float spin = 0.0f;
        std::vector<Spark> sparks;
        std::vector<Puff>  puffs;
    };

    // One SFX through the caller's sink, scaled by `soundGain`. Everything the
    // weapon makes a noise about goes through here, so the volume knob has no
    // holes in it.
    void cue(const std::string& file, float gain, float pitch) const {
        if (playCue && soundGain > 0.001f) playCue(file, gain * soundGain, pitch);
    }

    void updateLock(const Frame& f, const std::vector<Racer>& racers);
    void fire(const Frame& f);
    void detonate(const Missile& m, const std::vector<Racer>& racers, bool onTarget);
    void rebuildRibbons(Missile& m, const glm::vec3& camPos);

    // The next material out of the pool, set up for a flat self-lit surface.
    // Pooled because the renderer keeps a POINTER to the material until the
    // frame is drawn: one shared instance would give every missile, spark and
    // ring the colour of whichever was submitted last.
    fitzel::Material& mat(const glm::vec3& albedo, const glm::vec3& emission,
                          float glow);

    float rnd();                      // 0..1
    float rnd(float a, float b) { return a + (b - a) * rnd(); }

    fitzel::Shader* m_lit = nullptr;
    std::deque<fitzel::Material> m_mats;   // pool (deque: references stay valid)
    std::size_t m_matUsed = 0;

    // Static geometry, built once.
    fitzel::Mesh m_dart;     // the missile body, nose along +Z
    fitzel::Mesh m_flame;    // motor plume, blowing down -Z
    fitzel::Mesh m_ring;     // unit annulus in the XY plane (rings, shockwaves)
    fitzel::Mesh m_gyro;     // segmented ring: the marker that orbits a target
    fitzel::Mesh m_bracket;  // four corner brackets in the XY plane, half-size 1
    fitzel::Mesh m_sphere;   // unit sphere (fireball, smoke)
    fitzel::Mesh m_shard;    // a spike trailing down -Z from the origin (sparks)

    std::vector<Missile> m_missiles;
    std::vector<Blast>   m_blasts;
    std::vector<Hit>     m_hits;

    // Acquisition
    int   m_trackId = -1;     // the craft the seeker is on (-1 = none)
    bool  m_locked  = false;
    float m_lockT   = 0.0f;   // 0..1 toward a solid lock
    float m_offCone = 0.0f;   // seconds the tracked craft has been out of the cone
    float m_seekBlip = 0.0f;  // re-arm guard on the acquisition dit (s)
    int   m_seekAnnounced = -1; // which target the dit has already been played for
    float m_lockFlash = 0.0f; // counts down after the lock snapped shut (HUD)
    float m_denyFlash = 0.0f; // counts down after a shot with no lock (HUD)
    float m_hitFlash  = 0.0f; // counts down after one of ours connected (HUD)
    float m_pulse     = 0.0f; // the locked marker's outgoing ring, 0..1

    // Where the marked craft is this frame (the world marker and the reticle
    // both need it, and it is gone by the time either draws).
    bool      m_haveMark = false;
    glm::vec3 m_markPos{0.0f};
    glm::vec3 m_markVel{0.0f};
    float     m_markRadius = 2.4f;
    std::string m_markName;
    float     m_markDist = 0.0f;

    // Launcher
    int   m_ammo = 4;
    float m_reload = 0.0f;    // 0..1 toward the next round on the rail
    float m_fireT  = 0.0f;    // launch cooldown left (s)
    int   m_rail   = 0;       // which side the next one leaves from
    bool  m_prevFire = false, m_prevCycle = false;

    // Frame state kept for render()/drawHud(), which run long after update().
    glm::vec3 m_camPos{0.0f};
    bool      m_armed = false;
    float     m_time  = 0.0f;

    // Per-target velocity, differentiated from the positions we are handed.
    struct Track { glm::vec3 pos{0.0f}, vel{0.0f}; bool seen = false, init = false; };
    std::unordered_map<int, Track> m_tracks;

    unsigned m_rngState = 0x9E3779B9u;
};
