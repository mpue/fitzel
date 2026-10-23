#include "GameRecorder.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>

#include <glm/gtc/quaternion.hpp>

#include "Component.hpp"
#include "SandboxMath.hpp"   // sceneEuler

namespace gamerec {
namespace {

// --- Binary IO ------------------------------------------------------------------
// Byte by byte, little-endian, so the layout cannot drift with padding or the
// host's byte order.
constexpr char          kMagic[4] = {'F', 'R', 'E', 'C'};
constexpr std::uint32_t kVersion  = 1;

struct Out {
    std::vector<unsigned char> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u16(std::uint16_t v) { u8(v & 0xFF); u8(v >> 8); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8((v >> (8 * i)) & 0xFF); }
    void i32(std::int32_t v) { u32(static_cast<std::uint32_t>(v)); }
    void f32(float f) { std::uint32_t v; std::memcpy(&v, &f, 4); u32(v); }
    void f64(double d) {
        std::uint64_t v; std::memcpy(&v, &d, 8);
        for (int i = 0; i < 8; ++i) u8((v >> (8 * i)) & 0xFF);
    }
    void v3(const glm::vec3& v) { f32(v.x); f32(v.y); f32(v.z); }
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
};

struct In {
    const std::vector<unsigned char>& b;
    std::size_t p = 0;
    bool ok = true;
    bool need(std::size_t n) {
        if (!ok || b.size() - p < n) ok = false;
        return ok;
    }
    std::uint8_t u8() { return need(1) ? b[p++] : 0; }
    std::uint16_t u16() { std::uint16_t lo = u8(); return lo | (std::uint16_t(u8()) << 8); }
    std::uint32_t u32() {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= std::uint32_t(u8()) << (8 * i);
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    float f32() { const std::uint32_t v = u32(); float f; std::memcpy(&f, &v, 4); return f; }
    double f64() {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= std::uint64_t(u8()) << (8 * i);
        double d; std::memcpy(&d, &v, 8); return d;
    }
    glm::vec3 v3() { const float x = f32(), y = f32(); return {x, y, f32()}; }
    std::string str() {
        const std::uint32_t n = u32();
        if (!need(n)) return {};
        std::string s(reinterpret_cast<const char*>(b.data() + p), n);
        p += n;
        return s;
    }
};

void writePose(Out& o, const Pose& p) {
    o.v3(p.pos);
    o.v3(p.rot);
    o.u8(p.active ? 1 : 0);
    o.u8(static_cast<std::uint8_t>(p.skin));
    if (p.skin != 0) {
        o.u16(static_cast<std::uint16_t>(p.clip));
        o.f32(p.time);
        o.u16(static_cast<std::uint16_t>(p.fromClip));
        o.f32(p.fromTime);
        o.f32(p.fromShare);
    }
}

Pose readPose(In& in) {
    Pose p;
    p.pos    = in.v3();
    p.rot    = in.v3();
    p.active = in.u8() != 0;
    p.skin   = static_cast<std::int8_t>(in.u8());
    if (p.skin != 0) {
        p.clip      = static_cast<std::int16_t>(in.u16());
        p.time      = in.f32();
        p.fromClip  = static_cast<std::int16_t>(in.u16());
        p.fromTime  = in.f32();
        p.fromShare = in.f32();
    }
    return p;
}

void apply(State& s, const Frame& f) {
    for (const Change& c : f.changes) {
        if (c.gone) s.erase(c.id);
        else        s[c.id] = c.pose;
    }
}

// The scene's rotation order, Rz * Ry * Rx (see SandboxMath's sceneEuler).
glm::quat sceneQuat(const glm::vec3& deg) {
    const glm::vec3 r = glm::radians(deg);
    return glm::angleAxis(r.z, glm::vec3(0, 0, 1)) * glm::angleAxis(r.y, glm::vec3(0, 1, 0)) *
           glm::angleAxis(r.x, glm::vec3(1, 0, 0));
}

// Further than this between two frames is a cut or a respawn, not motion: it is
// shown as the jump it was rather than smeared across the frame between.
constexpr float kTeleport = 25.0f;

} // namespace

std::size_t Take::bytes() const {
    std::size_t n = sizeof(Take);
    for (const Frame& f : frames) n += sizeof(Frame) + f.changes.capacity() * sizeof(Change);
    for (const Spawn& s : spawns) n += sizeof(Spawn) + s.json.size();
    return n;
}

bool save(const Take& take, const std::string& path, std::string& err) {
    Out o;
    for (char c : kMagic) o.u8(static_cast<std::uint8_t>(c));
    o.u32(kVersion);
    o.str(take.scene);
    o.u32(static_cast<std::uint32_t>(take.spawns.size()));
    for (const Spawn& s : take.spawns) { o.i32(s.id); o.str(s.json); }
    o.u32(static_cast<std::uint32_t>(take.frames.size()));
    for (const Frame& f : take.frames) {
        o.f64(f.t);
        o.v3(f.cam.pos); o.v3(f.cam.front); o.v3(f.cam.up); o.f32(f.cam.fov);
        o.f32(f.timeOfDay);
        o.f32(f.storm);
        o.u32(static_cast<std::uint32_t>(f.changes.size()));
        for (const Change& c : f.changes) {
            o.i32(c.id);
            o.u8(c.gone ? 1 : 0);
            if (!c.gone) writePose(o, c.pose);
        }
    }
    FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) { err = "can't write " + path; return false; }
    const bool ok = std::fwrite(o.b.data(), 1, o.b.size(), fp) == o.b.size();
    if (std::fclose(fp) != 0 || !ok) { err = "write failed: " + path; return false; }
    return true;
}

bool load(Take& take, const std::string& path, std::string& err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { err = "can't open " + path; return false; }
    std::vector<unsigned char> buf;
    unsigned char chunk[1 << 16];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof chunk, fp)) > 0;)
        buf.insert(buf.end(), chunk, chunk + n);
    std::fclose(fp);

    In in{buf};
    for (char c : kMagic)
        if (in.u8() != static_cast<std::uint8_t>(c)) { err = "not a recording"; return false; }
    const std::uint32_t version = in.u32();
    if (version != kVersion) {
        err = "recording version " + std::to_string(version) + " (this build reads " +
              std::to_string(kVersion) + ")";
        return false;
    }
    Take t;
    t.scene = in.str();
    // Counts are checked against what is left of the file before anything is
    // reserved: a corrupt count must not ask for gigabytes.
    const std::uint32_t nSpawns = in.u32();
    if (!in.ok || nSpawns > buf.size()) { err = "truncated recording"; return false; }
    t.spawns.reserve(nSpawns);
    for (std::uint32_t i = 0; i < nSpawns && in.ok; ++i) {
        Spawn s;
        s.id   = in.i32();
        s.json = in.str();
        t.spawns.push_back(std::move(s));
    }
    const std::uint32_t nFrames = in.u32();
    if (!in.ok || nFrames > buf.size()) { err = "truncated recording"; return false; }
    t.frames.reserve(nFrames);
    for (std::uint32_t i = 0; i < nFrames && in.ok; ++i) {
        Frame f;
        f.t         = in.f64();
        f.cam.pos   = in.v3();
        f.cam.front = in.v3();
        f.cam.up    = in.v3();
        f.cam.fov   = in.f32();
        f.timeOfDay = in.f32();
        f.storm     = in.f32();
        const std::uint32_t nc = in.u32();
        if (!in.ok || nc > buf.size()) { in.ok = false; break; }
        f.changes.reserve(nc);
        for (std::uint32_t k = 0; k < nc && in.ok; ++k) {
            Change c;
            c.id   = in.i32();
            c.gone = in.u8() != 0;
            if (!c.gone) c.pose = readPose(in);
            f.changes.push_back(c);
        }
        t.frames.push_back(std::move(f));
    }
    if (!in.ok) { err = "truncated recording"; return false; }
    take = std::move(t);
    return true;
}

// --- Recorder -------------------------------------------------------------------

void Recorder::begin(const std::string& scene) {
    m_take = Take{};
    m_take.scene = scene;
    m_last.clear();
    m_t  = 0.0;
    m_on = true;
}

void Recorder::capture(float dt, const CamState& cam, float timeOfDay, float storm,
                       const std::vector<Entity>& entities,
                       const std::function<std::string(const Entity&)>& spawnJson) {
    if (!m_on) return;
    const bool first = m_take.frames.empty();
    if (!first) m_t += std::max(dt, 0.0f);

    Frame f;
    f.t = m_t;
    f.cam = cam;
    f.timeOfDay = timeOfDay;
    f.storm = storm;

    std::unordered_set<int> seen;
    seen.reserve(entities.size());
    for (const Entity& e : entities) {
        seen.insert(e.id);
        const Pose p = poseOf(e);
        auto it = m_last.find(e.id);
        if (it == m_last.end()) {
            // New after frame 0: a script made it. Its JSON is what lets the
            // replay put it back (once per id, even if it comes and goes).
            if (!first && spawnJson) {
                bool known = false;
                for (const Spawn& s : m_take.spawns) known |= s.id == e.id;
                if (!known) m_take.spawns.push_back({e.id, spawnJson(e)});
            }
            m_last.emplace(e.id, p);
            f.changes.push_back({e.id, p, false});
        } else if (!(it->second == p)) {
            it->second = p;
            f.changes.push_back({e.id, p, false});
        }
    }
    for (auto it = m_last.begin(); it != m_last.end();) {
        if (!seen.count(it->first)) {
            f.changes.push_back({it->first, Pose{}, true});
            it = m_last.erase(it);
        } else {
            ++it;
        }
    }
    m_take.frames.push_back(std::move(f));
}

Take Recorder::end() {
    m_on = false;
    m_last.clear();
    return std::move(m_take);
}

// --- Cursor ---------------------------------------------------------------------

void Cursor::reset(const Take* take) {
    m_take = take;
    m_keys.clear();
    m_state.clear();
    m_index = -1;
    if (!valid()) return;
    State s;
    for (int i = 0; i < static_cast<int>(take->frames.size()); ++i) {
        apply(s, take->frames[i]);
        if (i % kKeyEvery == 0) m_keys.push_back(s);
    }
}

void Cursor::seek(int i) {
    if (i == m_index) return;
    if (i < m_index || m_index < 0 || i - m_index > kKeyEvery) {
        const int k = i / kKeyEvery;
        m_state = m_keys[k];
        m_index = k * kKeyEvery;
    }
    while (m_index < i) apply(m_state, m_take->frames[++m_index]);
}

void Cursor::sample(double t, State& out, CamState& cam, float& timeOfDay, float& storm) {
    if (!valid()) return;
    const auto& fr = m_take->frames;
    // The last frame at or before t.
    auto it = std::upper_bound(fr.begin(), fr.end(), t,
                               [](double v, const Frame& f) { return v < f.t; });
    const int i = std::max(0, static_cast<int>(it - fr.begin()) - 1);
    seek(i);
    out = m_state;
    const Frame& a = fr[i];
    cam = a.cam;
    timeOfDay = a.timeOfDay;
    storm = a.storm;
    if (i + 1 >= static_cast<int>(fr.size())) return;

    const Frame& b = fr[i + 1];
    const double span = b.t - a.t;
    if (span <= 1e-9) return;
    const float alpha = static_cast<float>(std::clamp((t - a.t) / span, 0.0, 1.0));
    if (alpha <= 0.0f) return;
    cam = lerp(a.cam, b.cam, alpha);
    // The clock wraps at midnight: across it the hour is taken, not blended.
    timeOfDay = std::abs(b.timeOfDay - a.timeOfDay) < 1.0f
                    ? a.timeOfDay + (b.timeOfDay - a.timeOfDay) * alpha
                    : (alpha < 0.5f ? a.timeOfDay : b.timeOfDay);
    storm = a.storm + (b.storm - a.storm) * alpha;
    for (const Change& c : b.changes) {
        if (c.gone) continue;
        auto p = out.find(c.id);
        if (p != out.end()) p->second = lerp(p->second, c.pose, alpha);
    }
}

// --- Interpolation --------------------------------------------------------------

Pose lerp(const Pose& a, const Pose& b, float alpha) {
    const Pose& near = alpha < 0.5f ? a : b;
    if (glm::length(b.pos - a.pos) > kTeleport) return near;
    Pose p = near;   // the discrete parts: active, clips
    p.pos = glm::mix(a.pos, b.pos, alpha);
    if (a.rot != b.rot)
        p.rot = sceneEuler(glm::mat3_cast(glm::slerp(sceneQuat(a.rot), sceneQuat(b.rot), alpha)));
    // A clip's clock runs on between two frames of the same clip; a change of
    // clip or a loop back to the start is a cut.
    if (a.skin == b.skin && a.clip == b.clip && b.time >= a.time)
        p.time = a.time + (b.time - a.time) * alpha;
    if (a.skin == b.skin && a.fromClip == b.fromClip && b.fromTime >= a.fromTime) {
        p.fromTime  = a.fromTime + (b.fromTime - a.fromTime) * alpha;
        p.fromShare = a.fromShare + (b.fromShare - a.fromShare) * alpha;
    }
    return p;
}

CamState lerp(const CamState& a, const CamState& b, float alpha) {
    // A camera cut: somewhere else, or looking elsewhere.
    if (glm::length(b.pos - a.pos) > kTeleport || glm::dot(a.front, b.front) < 0.5f)
        return alpha < 0.5f ? a : b;
    CamState c;
    c.pos = glm::mix(a.pos, b.pos, alpha);
    const glm::vec3 f = glm::mix(a.front, b.front, alpha);
    const glm::vec3 u = glm::mix(a.up, b.up, alpha);
    c.front = glm::length(f) > 1e-6f ? glm::normalize(f) : a.front;
    c.up    = glm::length(u) > 1e-6f ? glm::normalize(u) : a.up;
    c.fov   = a.fov + (b.fov - a.fov) * alpha;
    return c;
}

// --- Entity glue ----------------------------------------------------------------

Pose poseOf(const Entity& e) {
    Pose p;
    p.pos    = e.localCenter;
    p.rot    = e.localRotation;
    p.active = e.active;
    if (const auto* ag = e.components.get<AnimGraphComponent>(); ag && ag->skinClip >= 0) {
        p.skin      = 2;
        p.clip      = static_cast<std::int16_t>(ag->skinClip);
        p.time      = ag->skinTime;
        p.fromClip  = static_cast<std::int16_t>(ag->skinFromClip);
        p.fromTime  = ag->skinFromTime;
        p.fromShare = ag->skinFromShare;
    } else if (const auto* ac = e.components.get<AnimationComponent>()) {
        p.skin = 1;
        p.clip = static_cast<std::int16_t>(ac->clip);
        p.time = ac->time;
    }
    return p;
}

void applyTransform(Entity& e, const Pose& p) {
    e.localCenter   = p.pos;
    e.localRotation = p.rot;
    e.active        = p.active;
}

void applySkin(Entity& e, const Pose& p) {
    if (p.skin == 2) {
        if (auto* ag = e.components.get<AnimGraphComponent>()) {
            ag->skinClip      = p.clip;
            ag->skinTime      = p.time;
            ag->skinFromClip  = p.fromClip;
            ag->skinFromTime  = p.fromTime;
            ag->skinFromShare = p.fromShare;
        }
    } else if (p.skin == 1) {
        if (auto* ac = e.components.get<AnimationComponent>()) {
            ac->clip    = p.clip;
            ac->time    = p.time;
            ac->started = true;    // no autostart rewinding it
            ac->restart = false;
            ac->playing = false;   // the take owns the clock
        }
    }
}

} // namespace gamerec
