#include "MultiShot.hpp"

#include <cmath>

namespace multishot {

namespace {

constexpr glm::vec3 kUp{0.0f, 1.0f, 0.0f};

// What each shot is, how long it wants to run, how much motion it needs, and how
// much of the authored dutch tilt suits it.
//
// `length` is a multiplier on the authored shot length rather than a time: a
// turntable orbit that is over in two seconds is a twitch, and a fly-by held for
// six is four seconds of empty road. Keeping the RATIO here means the author sets
// one number and the cut still breathes.
//
// `travel` is the honest statement of what the shot needs from its subject: 0
// works on a parked car, 1 is meaningless without speed. It is the whole of the
// shot selection's "intelligence" -- see weightOf().
struct Info {
    const char* name;
    const char* key;
    const char* hint;
    float       length;
    float       travel;
    float       dutch;   // how much of the authored tilt this shot takes
};

const Info kShots[ShotCount] = {
    {"Hero",       "hero",       "Three-quarter front, held, drifting slowly in",      1.15f, 0.0f, 0.30f},
    {"Orbit",      "orbit",      "Circles the subject -- the turntable",               1.35f, 0.0f, 0.40f},
    {"Crane down", "crane",      "Descends out of the sky onto the subject",           1.10f, 0.1f, 0.50f},
    {"Slider",     "slider",     "Travels sideways with the aim locked (parallax)",    1.00f, 0.1f, 0.30f},
    {"Push in",    "push_in",    "Dollies in on one angle",                            1.00f, 0.1f, 0.20f},
    {"Pull out",   "pull_out",   "Dollies out and up -- the reveal",                   0.95f, 0.1f, 0.30f},
    {"Dolly zoom", "dolly_zoom", "Pulls back as the lens narrows (Vertigo)",           1.10f, 0.0f, 0.20f},
    {"Top down",   "top_down",   "Bird's eye, drifting round",                         1.10f, 0.2f, 0.60f},
    {"Low angle",  "low_angle",  "Wheel height, close, looking up as it passes",       0.85f, 0.5f, 1.00f},
    {"Tracking",   "tracking",   "Car-to-car: rides alongside on a long lens",         1.25f, 0.7f, 0.50f},
    {"Nose",       "nose",       "Ahead of the subject, looking back at it",           1.00f, 0.6f, 0.40f},
    {"Fly by",     "fly_by",     "Camera planted; the subject rushes past it",         0.55f, 1.0f, 0.80f},
    {"Overtake",   "overtake",   "Comes past from behind and lets it fall away",       0.75f, 0.9f, 0.90f},
};

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float mixf(float a, float b, float t)     { return a + (b - a) * t; }

// Smooth start AND smooth stop. Every move in here is eased: a camera that
// starts at full speed on frame one is the single clearest tell that a shot was
// generated rather than operated.
float ease(float u)    { return u * u * (3.0f - 2.0f * u); }
float easeOut(float u) { const float k = 1.0f - u; return 1.0f - k * k * k; }

// Handheld. Three incommensurate sines rather than a random number per frame,
// because what a shoulder mount actually does is drift and settle -- white noise
// per frame reads as a broken camera, not as a human holding one.
float wobble(float t, float phase) {
    return std::sin(t * 1.7f + phase) * 0.60f +
           std::sin(t * 3.9f + phase * 2.3f) * 0.30f +
           std::sin(t * 7.3f + phase * 4.7f) * 0.10f;
}

// The subject's frame and its dimensions, worked out once per frame. EVERY
// number a shot uses comes from here, which is what makes one set of settings
// frame a go-kart and a lorry alike.
struct Ctx {
    glm::vec3 c{0.0f};          // subject centre
    glm::vec3 fwd{0.0f, 0.0f, 1.0f};
    glm::vec3 rt{1.0f, 0.0f, 0.0f};
    glm::vec3 lead{0.0f};       // aim offset for a moving subject
    float r     = 1.0f;         // plan radius (m) -- half the diagonal footprint
    float h     = 1.0f;         // height (m)
    float d     = 6.0f;         // framing distance (m), authored multiplier applied
    float rise  = 1.0f;         // Settings::height
    float pace  = 1.0f;         // Settings::speed
    float fov   = 45.0f;
    float side  = 1.0f;         // the flank this shot works from
    float ang   = 0.0f;         // its azimuth around the subject (0 = off the nose)
    float clock = 0.0f;         // free-running seconds, for the handheld
};

// A point on the subject's compass: 0 is straight off the nose, +90 degrees is
// off the `rt` flank.
glm::vec3 dirAt(const Ctx& cx, float a) {
    return cx.fwd * std::cos(a) + cx.rt * std::sin(a);
}

// What a shot hands back: two points and a lens. Deliberately not a Pose -- a
// shot says where it stands and what it is pointed at, and the basis (including
// which way is up on a dutch angle) is worked out once, in one place, below.
struct View {
    glm::vec3 eye{0.0f};
    glm::vec3 aim{0.0f};
    float     fov   = 45.0f;
    float     dutch = 0.0f;   // degrees of horizon tilt
};

// The moves themselves. `u` runs 0..1 across the shot.
View frameShot(int shot, float u, const Ctx& cx, const glm::vec3& anchor) {
    const float e = ease(u);
    View v;
    v.fov = cx.fov;
    v.aim = cx.c + kUp * (cx.h * 0.15f) + cx.lead;

    switch (shot) {
    case Hero: {
        // The still that sells the object: three quarters front, a touch above
        // the waistline, creeping almost imperceptibly closer. The creep is what
        // stops it looking like a screenshot.
        const float a    = cx.ang + cx.side * glm::radians(7.0f) * e * cx.pace;
        const float dist = cx.d * mixf(1.30f, 1.18f, e * cx.pace);
        v.eye = cx.c + dirAt(cx, a) * dist + kUp * (cx.h * 0.55f + 0.35f) * cx.rise;
        v.aim = cx.c + kUp * cx.h * 0.18f;
        v.fov = cx.fov * 0.90f;                       // slightly long: flattering
        break;
    }
    case Orbit: {
        // The turntable. A third of a circle, not a full one: a whole lap in one
        // shot is a car configurator, and a cut away at 120 degrees is an advert.
        const float a = cx.ang + cx.side * glm::radians(120.0f) * e * cx.pace;
        v.eye = cx.c + dirAt(cx, a) * (cx.d * 1.30f) +
                kUp * (cx.h * 0.70f + 0.50f) * cx.rise;
        break;
    }
    case Crane: {
        // Jib down: high and wide out of the sky, ending near shoulder height.
        // The azimuth drifts a little on the way so it is a descent, not a lift
        // shaft.
        const float a    = cx.ang + cx.side * glm::radians(28.0f) * e * cx.pace;
        const float dist = mixf(cx.d * 0.85f, cx.d * 1.30f, e);
        const float lift = mixf(cx.h * 4.5f + 6.0f, cx.h * 0.60f + 0.45f, e);
        v.eye = cx.c + dirAt(cx, a) * dist + kUp * lift * cx.rise;
        break;
    }
    case Slider: {
        // A dolly track laid alongside, aim locked on the subject: the
        // foreground streaks, the subject stays nailed to the middle of frame.
        // The oldest trick in the product-film book and still the best one.
        const float along = mixf(-1.15f, 1.15f, e) * cx.d * 0.90f * cx.pace;
        v.eye = cx.c + cx.rt * cx.side * (cx.d * 0.95f) + cx.fwd * along +
                kUp * (cx.h * 0.60f + 0.30f) * cx.rise;
        break;
    }
    case PushIn: {
        const float dist = mixf(cx.d * 2.30f, cx.d * 0.95f, e);
        const float lift = mixf(cx.h * 1.10f + 1.0f, cx.h * 0.50f + 0.35f, e);
        v.eye = cx.c + dirAt(cx, cx.ang) * dist + kUp * lift * cx.rise;
        v.fov = cx.fov * 0.95f;
        break;
    }
    case PullOut: {
        // The reveal: away and up, so the subject shrinks into the place it is
        // standing in. Eased OUT rather than in and out -- it should leave
        // quickly and settle, which is how a reveal lands.
        const float k    = easeOut(u);
        const float dist = mixf(cx.d * 0.85f, cx.d * 3.20f, k);
        const float lift = mixf(cx.h * 0.45f + 0.35f, cx.h * 2.40f + 3.0f, k);
        v.eye = cx.c + dirAt(cx, cx.ang) * dist + kUp * lift * cx.rise;
        break;
    }
    case DollyZoom: {
        // Vertigo. The eye pulls back while the lens narrows by exactly the
        // amount that keeps the subject the same size on screen -- so the
        // subject does not move and the WORLD BEHIND IT swells forward. That
        // equality is the whole effect, which is why the fov is computed from
        // the distance rather than authored beside it:
        //     tan(fov/2) * distance = constant
        const float d0   = cx.d * 0.95f;
        const float dist = mixf(d0, cx.d * 2.80f, e);
        const float t0   = std::tan(glm::radians(cx.fov) * 0.5f);
        v.fov = glm::degrees(2.0f * std::atan(t0 * d0 / std::max(dist, 0.01f)));
        v.eye = cx.c + dirAt(cx, cx.ang) * dist +
                kUp * (cx.h * 0.55f + 0.35f) * cx.rise;
        break;
    }
    case TopDown: {
        // Bird's eye, but never straight down: a fraction of the distance is
        // kept in plan so the frame still has a horizon direction to be level
        // against, and so the subject reads as a shape rather than as a roof.
        const float a = cx.ang + cx.side * glm::radians(40.0f) * e * cx.pace;
        v.eye = cx.c + dirAt(cx, a) * (cx.d * 0.45f) +
                kUp * (cx.d * 1.50f + cx.h * 2.0f) * cx.rise;
        v.aim = cx.c;
        break;
    }
    case LowAngle: {
        // Wheel height, an arm's length off the flank, looking up along the
        // body as the subject slides past. Sits BELOW the subject's centre on
        // purpose -- that is where it has to be to make a car look heavy -- and
        // relies on the ground clearance to keep it out of the tarmac.
        const float along = mixf(1.5f, -1.5f, e) * cx.r * 1.10f * cx.pace;
        v.eye = cx.c + cx.rt * cx.side * (cx.r * 1.20f + 0.55f) + cx.fwd * along -
                kUp * (cx.h * 0.30f);
        v.aim = cx.c + kUp * (cx.h * 0.40f) + cx.lead;
        v.fov = cx.fov * 1.25f;                       // wide: exaggerates the pass
        break;
    }
    case Tracking: {
        // Car to car. A long lens from a fixed flank offset, easing gently fore
        // and aft, with a small vertical bob -- the shot a camera car actually
        // gives, right down to not being perfectly still.
        const float along = mixf(0.45f, -0.45f, e) * cx.d * cx.pace;
        const float bob   = std::sin(cx.clock * 1.9f) * 0.03f * cx.h;
        v.eye = cx.c + cx.rt * cx.side * (cx.d * 0.85f) + cx.fwd * along +
                kUp * ((cx.h * 0.45f + 0.30f) * cx.rise + bob);
        v.fov = cx.fov * 0.85f;
        break;
    }
    case Nose: {
        // Out in front, running backwards, looking into the face of the thing.
        // Slightly off the centreline so it is a shot rather than an elevation
        // drawing, and closing in over its length.
        const float dist = mixf(1.35f, 0.95f, e) * cx.d;
        v.eye = cx.c + cx.fwd * dist + cx.rt * cx.side * (cx.r * 0.55f) +
                kUp * (cx.h * 0.25f + 0.25f) * cx.rise;
        break;
    }
    case FlyBy: {
        // The only shot whose eye is NOT in the subject's frame: the tripod is
        // planted in the world ahead of where the subject is going (see
        // Director::start) and simply left there. The subject drives into
        // frame, past, and away, and the camera pans to keep it -- which is the
        // whole shot, and which cannot be expressed as an offset from a moving
        // object.
        const float drift = std::sin(cx.clock * 0.6f) * 0.04f * cx.d;
        v.eye = anchor + cx.rt * drift;
        v.fov = cx.fov * 1.05f;
        break;
    }
    case Overtake: {
        // Comes up from behind, passes, and ends in front and higher -- so the
        // subject fills the frame in the middle of the shot and falls away at
        // the end of it. Reads as speed even when the subject is doing 30.
        const float along = mixf(-1.8f, 1.5f, e) * cx.d * cx.pace;
        const float lift  = mixf(cx.h * 0.35f + 0.25f, cx.h * 1.10f + 0.9f, e);
        v.eye = cx.c + cx.fwd * along + cx.rt * cx.side * (cx.r * 1.60f) +
                kUp * lift * cx.rise;
        break;
    }
    default:
        v.eye = cx.c + dirAt(cx, cx.ang) * cx.d + kUp * cx.h * cx.rise;
        break;
    }
    return v;
}

// Two points and a lens -> the pose the renderer wants. One place, so a dutch
// angle can never disagree with the direction it is tilting around.
camerasys::Pose compose(const View& v) {
    camerasys::Pose p;
    p.position = v.eye;
    p.fov      = clampf(v.fov, 10.0f, 140.0f);

    const glm::vec3 d = v.aim - v.eye;
    p.front = (glm::length(d) > 1e-4f) ? glm::normalize(d) : glm::vec3(0.0f, 0.0f, -1.0f);

    // Level to the world horizon first. Straight down (the bird's eye at its
    // most extreme) leaves no horizon to be level against, so fall back to an
    // arbitrary but STABLE axis rather than normalizing a zero vector.
    glm::vec3 rt = glm::cross(p.front, kUp);
    if (glm::length(rt) < 1e-3f) rt = glm::cross(p.front, glm::vec3(0.0f, 0.0f, 1.0f));
    if (glm::length(rt) < 1e-3f) rt = glm::vec3(1.0f, 0.0f, 0.0f);
    rt = glm::normalize(rt);
    glm::vec3 up = glm::normalize(glm::cross(rt, p.front));

    if (std::fabs(v.dutch) > 1e-3f) {
        const float a = glm::radians(v.dutch);
        up = glm::normalize(up * std::cos(a) + rt * std::sin(a));
    }
    p.up = up;
    return p;
}

// How well a shot suits the speed the subject is ACTUALLY doing. This is the
// selection's whole judgement, and it is deliberately a curve rather than a
// threshold: around walking pace both kinds of shot are plausible and the
// rotation should offer both.
float weightOf(int shot, float speed) {
    const Info& in = kShots[shot];
    const float sN = clampf(speed / 14.0f, 0.0f, 1.0f);   // ~50 km/h = fully "moving"
    // A fly-by of a stationary object is a camera watching nothing happen. Below
    // a crawl the travelling shots are not merely unlikely, they are wrong.
    if (in.travel >= 0.8f && speed < 1.5f) return 0.0f;
    return (1.0f - in.travel) * (1.15f - 0.55f * sN) +
           in.travel * (0.10f + 1.70f * sN);
}

} // namespace

const char* shotName(int s) { return (s >= 0 && s < ShotCount) ? kShots[s].name : "?"; }
const char* shotKey(int s)  { return (s >= 0 && s < ShotCount) ? kShots[s].key  : "?"; }
const char* shotHint(int s) { return (s >= 0 && s < ShotCount) ? kShots[s].hint : ""; }

unsigned Director::rand32() {
    // xorshift32: a sequence that is the same on every machine and every run, so
    // a seed really is a take you can shoot again.
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return m_rng;
}

float Director::rand01() {
    return static_cast<float>(rand32() & 0xffffffu) / static_cast<float>(0x1000000u);
}

int Director::choose(const Settings& st) {
    int enabled = 0, first = -1;
    for (int i = 0; i < ShotCount; ++i)
        if (st.use[i]) { ++enabled; if (first < 0) first = i; }
    if (enabled == 0) return Hero;      // an empty list still has to shoot something
    if (enabled == 1) return first;

    // A storyboard: the enabled shots in list order, over and over. For a spot
    // that has to come out the same every time, which is what an actual edit is.
    if (st.sequential) {
        for (int step = 1; step <= ShotCount; ++step) {
            const int i = (m_shot + step) % ShotCount;
            if (st.use[i]) return i;
        }
        return first;
    }

    // Coverage: weighted by fit, never the shot just played. Excluding the last
    // one matters more than it sounds -- two orbits in a row do not read as two
    // shots, they read as one shot with a glitch in the middle.
    float total = 0.0f;
    float w[ShotCount];
    for (int i = 0; i < ShotCount; ++i) {
        w[i] = (st.use[i] && i != m_shot) ? weightOf(i, m_speed) : 0.0f;
        total += w[i];
    }
    if (total <= 1e-4f) {
        // Everything the author enabled is unsuitable at this speed (a list of
        // travelling shots on a parked car). Rather than freeze on one frame,
        // fall back to the enabled list ignoring fit: a mediocre shot beats a
        // camera that stopped.
        for (int step = 1; step <= ShotCount; ++step) {
            const int i = (m_shot + step) % ShotCount;
            if (st.use[i]) return i;
        }
        return first;
    }
    float r = rand01() * total;
    for (int i = 0; i < ShotCount; ++i) {
        r -= w[i];
        if (r <= 0.0f && w[i] > 0.0f) return i;
    }
    return first;
}

void Director::start(int shot, const Subject& s, const Settings& st) {
    m_shot = glm::clamp(shot, 0, ShotCount - 1);
    m_t    = 0.0f;

    const Info& in = kShots[m_shot];
    const float var = clampf(st.variance, 0.0f, 0.9f);
    m_len = std::max(0.3f, st.duration * in.length * (1.0f + (rand01() * 2.0f - 1.0f) * var));

    // Alternate flanks. Two shots running down the same side of the car look
    // like one camera that hopped; crossing the line every cut is what makes a
    // handful of moves read as a crew with several cameras.
    m_side = -m_side;

    // Where round the subject this shot sets up. Each move wants a different
    // slice of the compass: a hero angle is only a hero angle at three quarters,
    // an orbit can start anywhere, a push-in from dead astern is a rear-view
    // mirror rather than a shot.
    const float rnd = rand01();
    switch (m_shot) {
    case Hero:      m_ang = m_side * glm::radians(mixf(32.0f, 52.0f, rnd)); break;
    case Crane:     m_ang = m_side * glm::radians(mixf(20.0f, 75.0f, rnd)); break;
    case PushIn:
    case PullOut:
    case DollyZoom: m_ang = m_side * glm::radians(mixf(25.0f, 145.0f, rnd)); break;
    case Orbit:
    case TopDown:   m_ang = glm::radians(mixf(-180.0f, 180.0f, rnd));       break;
    default:        m_ang = 0.0f;                                           break;   // frame-relative moves
    }

    // The fly-by plants its tripod HERE, once, ahead of where the subject is
    // headed -- far enough that the pass happens around the middle of the shot.
    // A minimum speed is assumed for that distance so a subject that slows down
    // after the cut still arrives instead of crawling toward a camera that is
    // half a kilometre away.
    if (m_shot == FlyBy) {
        const glm::vec3 fwd(std::sin(s.yaw), 0.0f, std::cos(s.yaw));
        const glm::vec3 rt = glm::normalize(glm::cross(kUp, fwd));
        const glm::vec3 dir = (m_speed > 0.5f) ? glm::normalize(m_vel) : fwd;
        const float r = std::max(glm::length(glm::vec2(s.half.x, s.half.z)), 0.4f);
        const float h = std::max(s.half.y * 2.0f, 0.5f);
        const float d = (r * 2.6f + 1.5f) * std::max(st.distance, 0.05f);
        m_anchor = s.center + dir * (std::max(m_speed, 5.0f) * m_len * 0.55f) +
                   rt * m_side * (d * 0.60f) +
                   kUp * ((h * 0.35f + 0.45f) * std::max(st.height, 0.05f));
    }
}

camerasys::Pose Director::update(const Subject& s, const Settings& st, float dt,
                                 const GroundFn& ground) {
    // A hitch (a scene load, a shader compile) must not be allowed to skip a
    // whole shot, and must not be allowed to become a measured velocity of a
    // hundred metres a second either.
    dt = clampf(dt, 0.0f, 0.25f);
    m_clock += dt;

    // Re-seeding on a changed seed rather than only in reset(): the author drags
    // the seed slider to reroll a sequence, and that has to actually reroll it.
    if (st.seed != m_seed) {
        m_seed = st.seed;
        m_rng  = static_cast<unsigned>(st.seed) * 2654435761u + 0x9e3779b9u;
        if (m_rng == 0) m_rng = 0x9e3779b9u;
    }

    // Measure the subject. A teleport is not speed: a respawn or a scene load
    // moves an object a hundred metres between two frames, and a shot list that
    // believed that number would spend the next few seconds shooting fly-bys of
    // a car standing still.
    if (m_seen && dt > 1e-5f) {
        const glm::vec3 v = (s.center - m_lastCenter) / dt;
        if (glm::length(v) < 400.0f) {
            const float k = 1.0f - std::exp(-3.0f * dt);   // ~0.3 s of memory
            m_vel += (v - m_vel) * k;
        }
    }
    m_lastCenter = s.center;
    m_seen = true;
    m_speed = glm::length(m_vel);

    // Advance the edit.
    const bool first = (m_shot < 0);
    if (first) {
        if (m_forced >= 0) { start(m_forced, s, st); m_forced = -1; }
        else               start(choose(st), s, st);
    } else {
        m_t += dt;
        if (m_t >= m_len || m_forced >= 0) {
            // A cross-fade holds the OUTGOING frame, so it has to be taken
            // before the new shot overwrites the state it was made from.
            if (st.blend > 0.01f) {
                m_fromEye   = m_lastEye;
                m_fromAim   = m_lastAim;
                m_fromFov   = m_lastFov;
                m_fromDutch = m_lastDutch;
                m_blendLen  = st.blend;
                m_blend     = st.blend;
            }
            const int next = (m_forced >= 0) ? m_forced : choose(st);
            m_forced = -1;
            start(next, s, st);
        }
    }

    // --- The shot itself -----------------------------------------------------
    Ctx cx;
    cx.c    = s.center;
    cx.fwd  = glm::vec3(std::sin(s.yaw), 0.0f, std::cos(s.yaw));
    cx.rt   = glm::normalize(glm::cross(kUp, cx.fwd));
    cx.r    = std::max(glm::length(glm::vec2(s.half.x, s.half.z)), 0.4f);
    cx.h    = std::max(s.half.y * 2.0f, 0.5f);
    cx.d    = (cx.r * 2.6f + 1.5f) * std::max(st.distance, 0.05f);
    cx.rise = std::max(st.height, 0.05f);
    cx.pace = clampf(st.speed, 0.05f, 4.0f);
    cx.fov  = st.fov > 1.0f ? st.fov : 45.0f;
    cx.side = m_side;
    cx.ang  = m_ang;
    cx.clock = m_clock;
    // Aiming ahead of a moving subject leaves it sitting in the trailing third
    // of frame, which is where an operator puts a car that is going somewhere.
    //
    // CAPPED AGAINST THE FRAMING DISTANCE, not left as the authored seconds.
    // A second of lead is a metre at walking pace and twenty-five on a motorway,
    // and the second one does not put the car in the trailing third of frame --
    // it puts it out of the picture entirely and points the camera at empty road.
    // (Measured: a full second of lead at 90 km/h aimed 130 degrees off the
    // subject.) A third of the framing distance is as far off-centre as a subject
    // can go and still be a subject.
    cx.lead = m_vel * clampf(st.lead, 0.0f, 1.5f);
    const float maxLead = cx.d * 0.35f;
    if (glm::length(cx.lead) > maxLead)
        cx.lead = glm::normalize(cx.lead) * maxLead;

    const float u = clampf(m_t / std::max(m_len, 0.01f), 0.0f, 1.0f);
    View v = frameShot(m_shot, u, cx, m_anchor);

    // Dutch: eased in and back out across the shot, never cut into. A tilt that
    // appears on frame one is a mistake in the rig; one that arrives is a
    // decision.
    if (std::fabs(st.dutch) > 0.01f)
        v.dutch = st.dutch * kShots[m_shot].dutch * m_side *
                  std::sin(u * 3.14159265f);

    // Handheld, scaled to the subject: a centimetre of wobble is invisible on a
    // lorry and seasick on a wing mirror.
    if (st.shake > 0.001f) {
        const float amp = st.shake * (0.02f * cx.d + 0.05f);
        v.eye += glm::vec3(wobble(m_clock, 0.0f), wobble(m_clock, 2.1f) * 0.6f,
                           wobble(m_clock, 4.2f)) * amp;
        // The aim wanders less than the eye -- an operator's hands shake, but
        // they are still trying to keep the subject in the middle.
        v.aim += glm::vec3(wobble(m_clock * 0.8f, 1.3f), wobble(m_clock * 0.8f, 3.4f),
                           wobble(m_clock * 0.8f, 5.5f)) * amp * 0.25f;
    }

    // --- Cross-fade ----------------------------------------------------------
    // Before the pose is built, not after: what is being crossed is where the
    // camera STANDS and what it is POINTED AT, and both shots point at the same
    // object (see the note on m_lastEye). Coming across as one continuous move
    // that keeps the subject framed is the whole reason to prefer this over the
    // cut it replaces.
    if (m_blend > 0.0f && m_blendLen > 0.0f) {
        m_blend = std::max(0.0f, m_blend - dt);
        const float w = ease(clampf(1.0f - m_blend / m_blendLen, 0.0f, 1.0f));
        v.eye   = glm::mix(m_fromEye, v.eye, w);
        v.aim   = glm::mix(m_fromAim, v.aim, w);
        v.fov   = mixf(m_fromFov, v.fov, w);
        v.dutch = mixf(m_fromDutch, v.dutch, w);
    }

    // KEEP THE SUBJECT IN THE PICTURE, whatever the aim offsets add up to.
    // A shot's aim is deliberately not the subject's centre -- it leads a moving
    // car, or sits at the roofline rather than the middle -- and both are
    // framing. What decides whether they are still framing is the LENS, which the
    // shot has only just chosen: the offset that leaves a car in the trailing
    // third at 45 degrees throws it clean out of frame at 15, which is where the
    // dolly zoom ends up by design. So the check belongs here: after the lens is
    // known AND after a cross-fade has had its say, because a fade moves the eye
    // between two shots without moving the aim either of them was composed
    // against. One rule for every shot, rather than a fudge per shot.
    {
        const glm::vec3 toC = cx.c - v.eye;
        const glm::vec3 toA = v.aim - v.eye;
        const float lc = glm::length(toC), la = glm::length(toA);
        if (lc > 1e-3f && la > 1e-3f) {
            const glm::vec3 dc = toC / lc, da = toA / la;
            const float ang = std::acos(clampf(glm::dot(dc, da), -1.0f, 1.0f));
            // Just over half the half-angle: far enough off-centre to read as a
            // composed frame, near enough that the subject cannot fall out of one.
            const float lim = glm::radians(v.fov * 0.5f) * 0.55f;
            if (ang > lim && ang > 1e-4f) {
                const glm::vec3 d = glm::mix(dc, da, lim / ang);
                if (glm::length(d) > 1e-4f) v.aim = v.eye + glm::normalize(d) * la;
            }
        }
    }

    camerasys::Pose p = compose(v);

    // Keep the eye out of the ground. Done on the composed pose rather than
    // inside each shot so no move can forget it -- and the low shots, which are
    // the ones worth having, are exactly the ones that would.
    const float floorY = s.center.y - s.half.y - 0.2f;   // never under the subject
    float minY = floorY + std::max(st.clearance, 0.0f);
    if (ground) minY = std::max(minY, ground(p.position.x, p.position.z) +
                                      std::max(st.clearance, 0.0f));
    if (p.position.y < minY) {
        // Lift the eye and re-aim, so a shot pushed up off the tarmac still
        // looks at the subject instead of over its roof.
        v.eye.y = minY;
        p = compose(v);
    }

    // What the viewer actually saw, which is what the next fade starts from.
    m_lastEye   = v.eye;
    m_lastAim   = v.aim;
    m_lastFov   = v.fov;
    m_lastDutch = v.dutch;
    return p;
}

void Director::reset() {
    m_shot = m_forced = -1;
    m_t = 0.0f;
    m_len = 1.0f;
    m_clock = 0.0f;
    m_side = 1.0f;
    m_seen = false;
    m_vel = glm::vec3(0.0f);
    m_speed = 0.0f;
    m_blend = m_blendLen = 0.0f;
    m_lastEye = m_fromEye = glm::vec3(0.0f);
    m_lastAim = m_fromAim = glm::vec3(0.0f, 0.0f, -1.0f);
    m_rng = static_cast<unsigned>(m_seed) * 2654435761u + 0x9e3779b9u;
    if (m_rng == 0) m_rng = 0x9e3779b9u;
}

void Director::cut() { m_t = m_len; }

void Director::play(int shot) {
    if (shot < 0 || shot >= ShotCount) return;
    m_forced = shot;
}

float Director::remaining() const { return std::max(0.0f, m_len - m_t); }

} // namespace multishot
