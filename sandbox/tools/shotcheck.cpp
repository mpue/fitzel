// The multishot check: does the shot director produce shots you could put in a
// cut, or poses that merely exist?
//
// A camera fault is not like a geometry fault. A tower in the road is visible in
// one screenshot; a camera that spends 0.4 seconds of every fly-by inside the
// tarmac, or that quietly points 30 degrees off the car for the second half of a
// crane, looks fine in the editor because the eye is where the picture comes
// from -- you cannot see the mistake, you are standing in it. And it is the kind
// of fault that survives review: every frame is plausible on its own.
//
// So measure it, over the whole sequence, for a subject standing still and for
// one doing motorway speed:
//   * is the subject actually IN FRAME, every frame, of every shot?
//   * does the eye stay out of the ground, on the flat and on a slope?
//   * is the framing distance sane for the size of the thing being shot?
//   * does the edit CUT -- several different shots, none repeated back to back?
//   * does the speed weighting do what it claims: no fly-bys of a parked car,
//     and travelling shots when there is travel?
//   * does a seed reproduce a take exactly?
//
// Console program, like citycheck and shadercheck, and for the same reason: the
// editor is /SUBSYSTEM:WINDOWS in Release and has nowhere to print to.
//   build/release/bin/shotcheck.exe
// Exits non-zero if any of the above fails.

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "../src/MultiShot.hpp"

namespace {

constexpr float kDt = 1.0f / 60.0f;

bool finite3(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// What one run of the director produced.
struct Result {
    int   frames      = 0;
    int   cuts        = 0;              // shot changes
    int   repeats     = 0;              // a shot immediately following itself
    int   offFrame    = 0;              // frames with the subject outside the lens
    int   underground = 0;              // frames with the eye below the clearance
    int   broken      = 0;              // NaN / non-unit basis / absurd lens
    int   tooFar      = 0;              // frames absurdly far from the subject
    float worstOff    = 0.0f;           // worst overshoot in degrees
    int   worstShot   = -1;             // ...and which shot was running then
    float worstDig    = 0.0f;           // deepest breach of the clearance, metres
    float maxDist     = 0.0f;
    int   used[multishot::ShotCount] = {0};   // frames spent in each shot
    std::vector<glm::vec3> eyes;              // for the determinism check
};

// Run the director for `seconds` over a subject of `half` extents moving at
// `speed` along its own nose.
Result run(const multishot::Settings& st, const glm::vec3& half, float speed,
           float seconds, const multishot::GroundFn& ground) {
    multishot::Director dir;
    multishot::Subject s;
    s.half   = half;
    s.center = glm::vec3(0.0f, half.y + (ground ? ground(0.0f, 0.0f) : 0.0f), 0.0f);
    s.yaw    = 0.0f;   // nose along +Z

    Result r;
    // A shot ENDING and the same one starting again is invisible in the shot id
    // -- it stays the same number. What gives it away is the clock: `remaining`
    // counts down and can only jump back up when a new shot has begun.
    int   last    = -1;
    float lastRem = -1.0f;
    const int frames = static_cast<int>(seconds / kDt);
    for (int i = 0; i < frames; ++i) {
        // The subject first, then the camera -- the order the frame runs in.
        s.center.z += speed * kDt;
        if (ground) s.center.y = ground(s.center.x, s.center.z) + half.y;

        const camerasys::Pose p = dir.update(s, st, kDt, ground);
        ++r.frames;
        r.eyes.push_back(p.position);

        const int   shot = dir.shot();
        const float rem  = dir.remaining();
        if (shot >= 0 && shot < multishot::ShotCount) ++r.used[shot];
        if (last < 0) {
            last = shot;
        } else if (rem > lastRem + 1e-3f) {     // a new shot began this frame
            ++r.cuts;
            if (shot == last) ++r.repeats;
            last = shot;
        }
        lastRem = rem;

        if (!finite3(p.position) || !finite3(p.front) || !finite3(p.up) ||
            std::fabs(glm::length(p.front) - 1.0f) > 1e-3f ||
            std::fabs(glm::length(p.up) - 1.0f) > 1e-3f ||
            std::fabs(glm::dot(p.front, p.up)) > 1e-3f ||
            !(p.fov > 5.0f && p.fov < 150.0f)) {
            ++r.broken;
            continue;
        }

        // Is the subject in frame? Measured against the VERTICAL half angle,
        // which is the strict version -- a 16:9 frame is wider than it is tall,
        // so anything inside this is comfortably inside the picture.
        const glm::vec3 to = s.center - p.position;
        const float dist = glm::length(to);
        r.maxDist = std::max(r.maxDist, dist);
        if (dist > 1e-3f) {
            const float ang = glm::degrees(std::acos(
                glm::clamp(glm::dot(glm::normalize(to), p.front), -1.0f, 1.0f)));
            const float half_fov = p.fov * 0.5f;
            // The subject has size: a car centre may sit outside the frame's
            // middle by the angle its own body subtends and still be perfectly
            // framed. Allow that, and nothing more.
            const float body = glm::degrees(std::atan(
                glm::length(glm::vec2(half.x, half.z)) / std::max(dist, 0.5f)));
            if (ang > half_fov + body) {
                ++r.offFrame;
                if (ang - half_fov - body > r.worstOff) {
                    r.worstOff  = ang - half_fov - body;
                    r.worstShot = shot;
                }
            }
        }
        // Framing sanity: a shot 400 metres off a car is not a shot.
        const float span = std::max(glm::length(half), 0.5f);
        if (dist > span * 60.0f * std::max(st.distance, 0.05f)) ++r.tooFar;

        if (ground) {
            const float clear = ground(p.position.x, p.position.z) + st.clearance;
            if (p.position.y < clear - 0.01f) {
                ++r.underground;
                r.worstDig = std::max(r.worstDig, clear - p.position.y);
            }
        }
    }
    return r;
}

int distinctShots(const Result& r) {
    int n = 0;
    for (int i = 0; i < multishot::ShotCount; ++i) if (r.used[i] > 0) ++n;
    return n;
}

// Fraction of the run spent in shots that need the subject to be moving.
float travellingShare(const Result& r) {
    int travel = 0, total = 0;
    for (int i = 0; i < multishot::ShotCount; ++i) {
        total += r.used[i];
        if (i == multishot::FlyBy || i == multishot::Overtake ||
            i == multishot::Tracking || i == multishot::Nose ||
            i == multishot::LowAngle)
            travel += r.used[i];
    }
    return total ? static_cast<float>(travel) / static_cast<float>(total) : 0.0f;
}

int fails = 0;

void check(bool ok, const char* what, const char* detail = "") {
    if (!ok) ++fails;
    std::printf("  %-46s %s%s%s\n", what, ok ? "ok" : "FAIL",
                *detail ? "  " : "", detail);
}

void report(const char* title, const Result& r) {
    std::printf("\n%s: %d frames, %d cuts, %d shots used, %.0f%% travelling\n",
                title, r.frames, r.cuts, distinctShots(r),
                travellingShare(r) * 100.0f);
    char d[128];
    std::snprintf(d, sizeof d, "(worst %.1f deg out, in %s)", r.worstOff,
                  r.worstShot >= 0 ? multishot::shotName(r.worstShot) : "-");
    check(r.broken == 0, "every pose finite, orthonormal, sane lens");
    check(r.offFrame == 0, "subject in frame every frame", d);
    std::snprintf(d, sizeof d, "(deepest %.2f m)", r.worstDig);
    check(r.underground == 0, "eye above the ground clearance", d);
    check(r.tooFar == 0, "framing distance scaled to the subject");
    check(r.repeats == 0, "no shot repeats back to back");
}

} // namespace

int main() {
    std::printf("multishot check\n");

    // A car: 1.8 m wide, 1.4 m tall, 4.4 m long.
    const glm::vec3 car(0.9f, 0.7f, 2.2f);
    // ...and a lorry, to prove the framing is a multiple of the subject rather
    // than a number that happens to suit a car.
    const glm::vec3 lorry(1.3f, 2.0f, 7.5f);

    const multishot::GroundFn flat  = [](float, float) { return 0.0f; };
    const multishot::GroundFn slope = [](float x, float z) {
        return 0.15f * x + 0.05f * z + 2.0f * std::sin(z * 0.02f);
    };

    multishot::Settings st;          // defaults: every shot enabled, hard cuts
    st.fov = 45.0f;

    // 1. A parked car. The standing moves have to carry the whole sequence.
    Result parked = run(st, car, 0.0f, 120.0f, flat);
    report("parked car, flat ground", parked);
    check(distinctShots(parked) >= 5, "a parked subject still gets coverage");
    check(parked.used[multishot::FlyBy] == 0,
          "no fly-by of a stationary subject");
    check(parked.used[multishot::Overtake] == 0,
          "no overtake of a stationary subject");

    // 2. The same car at 40 m/s (144 km/h), over a hill.
    Result fast = run(st, car, 40.0f, 120.0f, slope);
    report("car at 144 km/h, sloped ground", fast);
    check(travellingShare(fast) > 0.35f,
          "travelling shots take over once there is travel");

    // 3. A lorry, close framing. Same settings, ten times the volume.
    multishot::Settings tight = st;
    tight.distance = 0.6f;
    Result big = run(tight, lorry, 18.0f, 90.0f, slope);
    report("lorry at 65 km/h, tight framing", big);

    // 4. Every effect on at once: handheld, dutch, cross-fades, a long lead.
    //    The fades are what could produce a pose that is neither shot -- a basis
    //    mixed to zero length is the classic way to get a NaN view matrix.
    multishot::Settings loud = st;
    loud.shake = 1.0f; loud.dutch = 25.0f; loud.blend = 0.8f;
    loud.lead = 1.0f;  loud.speed = 2.5f;  loud.height = 2.5f;
    Result effects = run(loud, car, 25.0f, 90.0f, slope);
    report("all effects on, cross-fades", effects);

    // 5. One shot at a time: each move on its own, so a fault in a rare shot
    //    cannot hide behind twelve good ones in a shuffled run.
    std::printf("\nper shot (25 m/s, sloped ground)\n");
    int shotFails = 0;
    for (int i = 0; i < multishot::ShotCount; ++i) {
        multishot::Settings one = st;
        for (int k = 0; k < multishot::ShotCount; ++k) one.use[k] = (k == i);
        const Result r = run(one, car, 25.0f, 30.0f, slope);
        const bool ok = r.broken == 0 && r.offFrame == 0 && r.underground == 0 &&
                        r.tooFar == 0;
        if (!ok) ++shotFails;
        std::printf("  %-12s %s  max dist %6.1f m  off-frame %4d (%.1f deg)"
                    "  under %4d (%.2f m)\n",
                    multishot::shotName(i), ok ? "ok  " : "FAIL", r.maxDist,
                    r.offFrame, r.worstOff, r.underground, r.worstDig);
    }
    fails += shotFails;

    // 6. A seed is a take: the same seed has to give the same run, or "re-record
    //    it after moving that light" is a promise the component cannot keep.
    std::printf("\nreproducibility\n");
    multishot::Settings seeded = st;
    seeded.seed = 7;
    const Result a = run(seeded, car, 30.0f, 40.0f, flat);
    const Result b = run(seeded, car, 30.0f, 40.0f, flat);
    seeded.seed = 8;
    const Result c = run(seeded, car, 30.0f, 40.0f, flat);
    bool same = a.eyes.size() == b.eyes.size();
    for (size_t i = 0; same && i < a.eyes.size(); ++i)
        same = glm::length(a.eyes[i] - b.eyes[i]) < 1e-4f;
    bool differs = a.eyes.size() == c.eyes.size();
    size_t apart = 0;
    for (size_t i = 0; i < c.eyes.size() && i < a.eyes.size(); ++i)
        if (glm::length(a.eyes[i] - c.eyes[i]) > 0.01f) ++apart;
    differs = differs && apart > c.eyes.size() / 10;
    check(same, "the same seed replays the same take");
    check(differs, "a different seed gives a different one");

    std::printf("\n%s\n", fails ? "FAIL: the shot list has faults above"
                                : "OK: every shot frames its subject and clears the ground");
    return fails ? 1 : 0;
}
