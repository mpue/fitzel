#pragma once

#include <string>
#include <vector>

struct Entity;
namespace racesim { struct RaceState; }

// The difficulty ladder: one named step that moves the whole race at once.
//
// It is deliberately TWO halves, because a racing game gets hard in two
// different ways and only one of them is fair. The first half is the field --
// how fast it runs, how well it drives, how often it slips up. The second is the
// player's own margin for error -- how hard a crash bites, how fast the hull
// comes back, how much boost is handed out. A ladder that only moves the first
// half runs out of room: past a point the only way to make a field harder is to
// make it faster than the craft the player was given, which is not difficulty,
// it is a wall. Moving both means ELITE can ask for clean driving instead of
// impossible driving.
//
// What it deliberately does NOT touch is the driving aids. Those exist so the
// game can be played with hands that do not hold still (see the tremor-friendly
// goal), and a setting that took them away as the price of a faster field would
// make the ladder unusable for exactly the person it was built around. Aids stay
// their own axis, available on every step.
//
// Everything here is applied AFTER Play has snapshotted the scene, so a race run
// at ELITE never reaches the circuit's .fitzel on disk -- the same rule the start
// screen's other overrides follow.
namespace difficulty {

// One step of the ladder. Every field is a MULTIPLIER on what the scene's author
// entered, and PRO is all ones: a circuit played on PRO is the circuit exactly as
// it was tuned, which is what makes this ladder something laid over a track
// rather than something the track has to be built for.
struct Level {
    const char* name;

    // --- The field ---------------------------------------------------------
    float pace;      // speed / accel / brake (grip gets pace^2, see applyToField)
    // The rubber band, and the one number here that is not monotonic -- on
    // purpose. It peaks in the middle: PRO is where close racing is the point, so
    // the field is allowed to hang on. ELITE drops it almost away because a field
    // that is genuinely faster should DRIVE away from the player rather than wait
    // on an elastic -- if it cannot be dropped, a lead earned on the hardest step
    // means nothing. ROOKIE keeps it low for the opposite reason: a beginner's
    // lead should be allowed to grow instead of being reeled back in.
    float catchup;
    // Slip-ups: a multiplier on the time BETWEEN them, so a bigger number means a
    // steadier field. This is the honest way to be easy -- a rookie field that
    // occasionally runs wide reads as a field of drivers, where one that is simply
    // slowed down everywhere reads as a handbrake.
    float slip;
    // Craft: how hard the AI cuts to the apex and how well it sees the others.
    // A low step drives a visibly rougher line, which is most of why an easy
    // field feels beatable rather than merely slow.
    float craft;

    // --- The player's margin ------------------------------------------------
    float damage;    // hull damage taken, from a crash and from a warhead alike
    float heal;      // how fast the hull recharges once it is left alone
    float boost;     // boost regeneration
};

// The ladder itself. PRO (index 1) is the neutral step.
inline constexpr int kCount   = 4;
inline constexpr int kDefault = 1;

// Clamped: an index out of range gives the default step rather than a crash, so
// a profile file edited by hand cannot take the game down.
const Level& level(int i);
const char*  name(int i);

// --- The player's remembered choice -----------------------------------------
// Their own setting, not the machine's: unlike the graphics file this says how
// the player wants to be treated, so it would travel with them if the game ever
// carried a profile anywhere. Kept in the same shape as gfxmenu's file for the
// same reason -- one place per concern, written when it changes.
struct Profile {
    int level = kDefault;
};

Profile load(const std::string& file);
void    save(const std::string& file, const Profile& p);

// --- Applying ----------------------------------------------------------------
// Scale every entered rival in `entities` by the step. `skipA` / `skipB` are the
// entity ids of the craft in the two seats: player two's craft usually carries an
// Opponent component (that is how a two-craft circuit is authored), and handing
// it the AI's pace would tune the machine a human is flying.
void applyToField(std::vector<Entity>& entities, int lvl, int skipA, int skipB);

// Hand a player's craft its margin. Call once as a race starts, per seat.
void applyToPlayer(racesim::RaceState& st, int lvl);

} // namespace difficulty
