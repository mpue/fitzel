#include "Sequencer.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include <nlohmann/json.hpp>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/audio/Mixer.hpp>
#include <fitzel/audio/Waveform.hpp>

#include "CameraPath.hpp"
#include "Component.hpp"
#include "SceneTypes.hpp"

namespace seq {

const char* kindName(Kind k) {
    switch (k) {
        case Kind::Camera: return "Camera";
        case Kind::Audio:  return "Audio";
        default:           return "Animation";
    }
}

// --- Strip -------------------------------------------------------------------

float Strip::sourceTime(float t) const {
    return offset + (t - start) * speed;
}

float Strip::fadeAt(float t) const {
    float a = 1.0f;
    if (fadeIn  > 1e-4f) a = std::min(a, (t - start) / fadeIn);
    if (fadeOut > 1e-4f) a = std::min(a, (end() - t) / fadeOut);
    a = std::clamp(a, 0.0f, 1.0f);
    // An S rather than a ramp: a linear gain fade sounds like it stops short
    // (the ear hears level in dB), and the same curve is what the strip draws.
    return a * a * (3.0f - 2.0f * a);
}

void Track::sort() {
    std::stable_sort(strips.begin(), strips.end(),
                     [](const Strip& a, const Strip& b) { return a.start < b.start; });
}

// --- Sequence ----------------------------------------------------------------

Track& Sequence::addTrack(Kind k, const std::string& name) {
    Track t;
    t.uid  = newUid();
    t.kind = k;
    if (name.empty()) {
        int n = 1;
        for (const Track& o : tracks) if (o.kind == k) ++n;
        t.name = std::string(kindName(k)) + (n > 1 ? " " + std::to_string(n) : "");
    } else {
        t.name = name;
    }
    // Camera tracks go above the rest and audio below, whatever order they are
    // added in: the cut is what an edit is read by, and the sound sits under
    // the picture in every editor there is.
    auto rank = [](Kind kk) { return kk == Kind::Camera ? 0 : kk == Kind::Anim ? 1 : 2; };
    auto at = tracks.end();
    for (auto it = tracks.begin(); it != tracks.end(); ++it)
        if (rank(it->kind) > rank(k)) { at = it; break; }
    return *tracks.insert(at, std::move(t));
}

Track* Sequence::trackByUid(int uid) {
    for (Track& t : tracks) if (t.uid == uid) return &t;
    return nullptr;
}
const Track* Sequence::trackByUid(int uid) const {
    return const_cast<Sequence*>(this)->trackByUid(uid);
}

Strip* Sequence::stripByUid(int uid, Track** owner) {
    for (Track& t : tracks)
        for (Strip& s : t.strips)
            if (s.uid == uid) { if (owner) *owner = &t; return &s; }
    return nullptr;
}

float Sequence::contentEnd() const {
    float e = 0.0f;
    for (const Track& t : tracks)
        for (const Strip& s : t.strips) e = std::max(e, s.end());
    return e;
}

float Sequence::snap(float t) const {
    const float f = fps > 0.5f ? fps : 30.0f;
    return std::max(0.0f, std::round(t * f) / f);
}

// --- Evaluation --------------------------------------------------------------

Frame cutAt(const Sequence& s, float t) {
    Frame f;
    for (const Track& tr : s.tracks) {
        if (tr.kind != Kind::Camera || tr.mute) continue;
        for (const Strip& st : tr.strips)
            if (st.camera >= 0 && st.covers(t)) { f.camera = st.camera; f.shot = st.uid; return f; }
    }
    // Between shots, the last one holds: a gap in the cut is a held frame, not
    // a jump back to whatever camera the editor happened to have.
    float best = -1.0f;
    for (const Track& tr : s.tracks) {
        if (tr.kind != Kind::Camera || tr.mute) continue;
        for (const Strip& st : tr.strips)
            if (st.camera >= 0 && st.end() <= t && st.end() > best) {
                best = st.end(); f.camera = st.camera; f.shot = st.uid;
            }
    }
    return f;
}

float clipLength(const anim::Clip& c) {
    const float k = c.lastKeyTime();
    return std::max(0.1f, k > 1e-3f ? k : c.duration);
}

namespace {

// The clip time a strip asks for at sequence time `t`, its loop applied.
float clipTimeAt(const Strip& st, const anim::Clip& c, float t) {
    float ct = st.sourceTime(t);
    if (st.loop) {
        const float len = clipLength(c);
        ct = std::fmod(ct, len);
        if (ct < 0.0f) ct += len;
    }
    return ct;
}

} // namespace

void applyAnim(const Sequence& s, const std::vector<anim::Clip>& clips,
               std::vector<Entity>& entities, float t) {
    for (const Track& tr : s.tracks) {
        if (tr.kind == Kind::Audio || tr.mute) continue;
        const Strip* act  = nullptr;
        const Strip* held = nullptr;
        for (const Strip& st : tr.strips) {
            if (st.covers(t)) act = &st;
            else if (st.end() <= t && (!held || st.end() >= held->end())) held = &st;
        }
        const Strip* use = act ? act : (tr.hold ? held : nullptr);
        if (!use || use->clip.empty()) continue;
        const int ci = anim::findClip(clips, use->clip);
        if (ci < 0) continue;
        // A held strip is posed at its last frame -- just inside its end, so a
        // looping strip holds the pose it actually ended on.
        const float at = act ? t : use->end() - 1e-4f;
        anim::apply(clips[ci], entities, clipTimeAt(*use, clips[ci], at));
    }
}

std::vector<std::string> clipsUsed(const Sequence& s) {
    std::vector<std::string> out;
    for (const Track& tr : s.tracks) {
        if (tr.kind == Kind::Audio) continue;
        for (const Strip& st : tr.strips)
            if (!st.clip.empty() && std::find(out.begin(), out.end(), st.clip) == out.end())
                out.push_back(st.clip);
    }
    return out;
}

int cameraOfClip(const anim::Clip& c, const std::vector<Entity>& entities) {
    for (const anim::Track& t : c.tracks)
        for (const Entity& e : entities)
            if (e.id == t.entityId && e.components.get<CameraComponent>()) return e.id;
    return -1;
}

glm::vec3 cameraRotationFor(float yawDeg, float pitchDeg) {
    // A Static camera looks down its own -Z, turned by quat(radians(rotation)).
    // Pitching -Z up by `pitch` about X and then turning it about Y by
    // -(90 + yaw) lands it on the fly camera's (cos yaw cos pitch, sin pitch,
    // sin yaw cos pitch). seqcheck holds the two against each other.
    return glm::vec3(pitchDeg, -90.0f - yawDeg, 0.0f);
}

anim::Clip cameraClipFromPath(const std::vector<CamKey>& keys, int cameraId,
                              const std::string& name) {
    anim::Clip c;
    c.name = name;
    c.fps  = 30.0f;
    if (keys.empty()) return c;
    struct Chan { const char* comp; const char* key; int index; };
    static const Chan kChans[] = {
        {"", "center", 0},   {"", "center", 1},   {"", "center", 2},
        {"", "rotation", 0}, {"", "rotation", 1}, {"", "rotation", 2},
        {"camera", "fov", 0},
    };
    for (const Chan& ch : kChans) {
        anim::Track t;
        t.entityId = cameraId;
        t.comp     = ch.comp;
        t.key      = ch.key;
        t.index    = ch.index;
        // The recorder samples a few times a second; Smooth is what makes that
        // read as a move rather than a string of straight segments.
        t.interp   = anim::Interp::Smooth;
        c.tracks.push_back(std::move(t));
    }
    const float t0 = keys.front().t;
    for (const CamKey& k : keys) {
        const float     t = k.t - t0;
        const glm::vec3 r = cameraRotationFor(k.yaw, k.pitch);
        const float v[7] = {k.pos.x, k.pos.y, k.pos.z, r.x, r.y, r.z, k.fov};
        for (int i = 0; i < 7; ++i) c.tracks[i].keys.push_back({t, v[i]});
    }
    c.duration = std::max(1.0f, keys.back().t - t0);
    return c;
}

int split(Sequence& s, int stripUid, float t) {
    Track* tr = nullptr;
    Strip* st = s.stripByUid(stripUid, &tr);
    if (!st || t <= st->start + 1e-4f || t >= st->end() - 1e-4f) return -1;
    Strip right   = *st;
    right.uid     = s.newUid();
    right.offset  = st->sourceTime(t);
    right.start   = t;
    right.length  = st->end() - t;
    right.fadeIn  = 0.0f;
    st->length    = t - st->start;
    st->fadeOut   = 0.0f;
    st->fadeIn    = std::min(st->fadeIn, st->length);
    right.fadeOut = std::min(right.fadeOut, right.length);
    tr->strips.push_back(right);
    tr->sort();
    return right.uid;
}

// --- Persistence -------------------------------------------------------------

namespace {

const char* kindKey(Kind k) {
    return k == Kind::Camera ? "camera" : k == Kind::Audio ? "audio" : "anim";
}

template <class T>
T get(const nlohmann::json& j, const char* k, T def) {
    const auto it = j.find(k);
    if (it == j.end()) return def;
    if constexpr (std::is_same_v<T, bool>) return it->is_boolean() ? it->get<bool>() : def;
    else if constexpr (std::is_same_v<T, std::string>) return it->is_string() ? it->get<std::string>() : def;
    else return it->is_number() ? it->get<T>() : def;
}

} // namespace

void save(nlohmann::json& j, const std::vector<Sequence>& seqs) {
    nlohmann::json arr = nlohmann::json::array();
    for (const Sequence& s : seqs) {
        nlohmann::json js;
        js["name"]          = s.name;
        js["duration"]      = s.duration;
        js["fps"]           = s.fps;
        js["loop"]          = s.loop;
        js["playOnStart"]   = s.playOnStart;
        js["releaseCamera"] = s.releaseCamera;
        js["nextUid"]       = s.nextUid;
        nlohmann::json tracks = nlohmann::json::array();
        for (const Track& t : s.tracks) {
            nlohmann::json jt;
            jt["uid"]  = t.uid;
            jt["kind"] = kindKey(t.kind);
            jt["name"] = t.name;
            jt["mute"] = t.mute;
            jt["solo"] = t.solo;
            jt["lock"] = t.lock;
            jt["hold"] = t.hold;
            if (t.kind == Kind::Audio) {
                jt["gain"]    = t.gain;
                jt["channel"] = t.channel;
            }
            nlohmann::json strips = nlohmann::json::array();
            for (const Strip& st : t.strips) {
                nlohmann::json js2;
                js2["uid"]    = st.uid;
                js2["start"]  = st.start;
                js2["length"] = st.length;
                js2["offset"] = st.offset;
                if (t.kind == Kind::Audio) {
                    js2["sound"]   = st.sound;
                    js2["gain"]    = st.gain;
                    js2["fadeIn"]  = st.fadeIn;
                    js2["fadeOut"] = st.fadeOut;
                } else {
                    js2["clip"]  = st.clip;
                    js2["speed"] = st.speed;
                    js2["loop"]  = st.loop;
                    if (t.kind == Kind::Camera) js2["camera"] = st.camera;
                }
                strips.push_back(std::move(js2));
            }
            jt["strips"] = std::move(strips);
            tracks.push_back(std::move(jt));
        }
        js["tracks"] = std::move(tracks);
        arr.push_back(std::move(js));
    }
    j["sequences"] = std::move(arr);
}

void load(const nlohmann::json& j, std::vector<Sequence>& seqs) {
    seqs.clear();
    const auto it = j.find("sequences");
    if (it == j.end() || !it->is_array()) return;
    for (const auto& js : *it) {
        if (!js.is_object()) continue;
        Sequence s;
        s.name          = get(js, "name", std::string("Sequence"));
        s.duration      = std::max(0.5f, get(js, "duration", 30.0f));
        s.fps           = std::clamp(get(js, "fps", 30.0f), 1.0f, 240.0f);
        s.loop          = get(js, "loop", false);
        s.playOnStart   = get(js, "playOnStart", false);
        s.releaseCamera = get(js, "releaseCamera", true);
        s.nextUid       = std::max(1, get(js, "nextUid", 1));
        int maxUid = 0;
        if (const auto tt = js.find("tracks"); tt != js.end() && tt->is_array())
            for (const auto& jt : *tt) {
                if (!jt.is_object()) continue;
                Track t;
                t.uid  = get(jt, "uid", 0);
                const std::string k = get(jt, "kind", std::string("anim"));
                t.kind = k == "camera" ? Kind::Camera : k == "audio" ? Kind::Audio : Kind::Anim;
                t.name    = get(jt, "name", std::string(kindName(t.kind)));
                t.mute    = get(jt, "mute", false);
                t.solo    = get(jt, "solo", false);
                t.lock    = get(jt, "lock", false);
                t.hold    = get(jt, "hold", true);
                t.gain    = std::clamp(get(jt, "gain", 1.0f), 0.0f, 2.0f);
                t.channel = get(jt, "channel", std::string("Music"));
                maxUid = std::max(maxUid, t.uid);
                if (const auto ss = jt.find("strips"); ss != jt.end() && ss->is_array())
                    for (const auto& js2 : *ss) {
                        if (!js2.is_object()) continue;
                        Strip st;
                        st.uid     = get(js2, "uid", 0);
                        st.start   = std::max(0.0f, get(js2, "start", 0.0f));
                        st.length  = std::max(1e-3f, get(js2, "length", 1.0f));
                        st.offset  = get(js2, "offset", 0.0f);
                        st.clip    = get(js2, "clip", std::string());
                        st.speed   = std::clamp(get(js2, "speed", 1.0f), 0.01f, 100.0f);
                        st.loop    = get(js2, "loop", false);
                        st.camera  = get(js2, "camera", -1);
                        st.sound   = get(js2, "sound", std::string());
                        st.gain    = std::clamp(get(js2, "gain", 1.0f), 0.0f, 2.0f);
                        st.fadeIn  = std::max(0.0f, get(js2, "fadeIn", 0.0f));
                        st.fadeOut = std::max(0.0f, get(js2, "fadeOut", 0.0f));
                        maxUid = std::max(maxUid, st.uid);
                        t.strips.push_back(std::move(st));
                    }
                t.sort();
                s.tracks.push_back(std::move(t));
            }
        // Never hand out a uid already in the file, whatever nextUid says: a
        // hand-edited scene must not get two strips sharing a voice.
        s.nextUid = std::max(s.nextUid, maxUid + 1);
        seqs.push_back(std::move(s));
    }
}

int find(const std::vector<Sequence>& seqs, const std::string& name) {
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i)
        if (seqs[i].name == name) return i;
    return -1;
}

// --- Runner ------------------------------------------------------------------

// A strip's voice. Its own copy of where the strip sits, so the clock can be
// read back off it (lockToAudio) without finding the strip again.
struct Runner::Voice {
    fitzel::Sound sound;
    float         length    = 0.0f;   // the file's, in seconds (0 = unknown)
    bool          started   = false;
    float         start     = 0.0f;   // the strip's start on the sequence
    float         offset    = 0.0f;   // ...and its In
    float         playedFor = 0.0f;   // seconds since it started sounding
};

namespace {
// Long enough to take the click off a cut in the middle of a waveform, short
// enough that nobody hears it as a fade.
constexpr float kDeclickMs = 6.0f;
// Over how long a level change glides in the audio thread: a frame or two, so
// the per-frame fade steps join up instead of buzzing.
constexpr float kGlideMs   = 40.0f;
}

Runner::Runner()  = default;
Runner::~Runner() = default;

void Runner::bindAudio(fitzel::Audio* audio,
                       std::function<std::string(const std::string&)> resolve,
                       std::function<int(const std::string&)> route) {
    silence();
    m_audio   = audio;
    m_resolve = std::move(resolve);
    m_route   = std::move(route);
}

void Runner::retireVoice(std::unique_ptr<Voice> v) {
    if (!v) return;
    if (v->sound.isValid() && v->started) {
        v->sound.stopFaded(kDeclickMs);
        m_dying.emplace_back(std::move(v), 0.1f);
    }
}

void Runner::silence() {
    for (auto& [uid, v] : m_voices) retireVoice(std::move(v));
    m_voices.clear();
}

void Runner::lockToAudio(float dt) {
    // The longest-sounding voice is the reference: a voice only just started
    // reports its seek target, not yet where the card has got to.
    Voice* ref = nullptr;
    for (auto& [uid, v] : m_voices)
        if (v && v->started && v->playedFor > 0.25f && v->sound.isPlaying() &&
            (!ref || v->playedFor > ref->playedFor))
            ref = v.get();
    if (!ref) return;
    const float heard = ref->start + (ref->sound.cursorSeconds() - ref->offset);
    const float err   = heard - time;
    m_skew = err;
    // Eased, not snapped: the cursor moves in mixing-block steps, and a clock
    // that jumped to it would make the animation shiver. Past half a second it
    // is not drift but a jump, and the voices are reseated instead.
    if (std::fabs(err) < 0.5f) time += err * std::min(1.0f, dt * 4.0f);
}

void Runner::reset() {
    silence();
    m_captured.clear();
    sequence      = -1;
    time          = 0.0f;
    playing       = false;
    preview       = false;
    frame         = Frame{};
    m_camBefore   = -1;
    m_camSet      = -2;
    m_camReleased = false;
    m_jumped      = false;
}

void Runner::capture(const Sequence& s, const std::vector<anim::Clip>& clips,
                     std::vector<Entity>& entities) {
    auto same = [](const anim::Track& a, const anim::Track& b) {
        return a.entityId == b.entityId && a.index == b.index && a.comp == b.comp && a.key == b.key;
    };
    for (const std::string& name : clipsUsed(s)) {
        const int ci = anim::findClip(clips, name);
        if (ci < 0) continue;
        for (const anim::Track& t : clips[ci].tracks) {
            bool have = false;
            for (const Captured& c : m_captured) if (same(c.addr, t)) { have = true; break; }
            if (have) continue;
            const anim::Bound b = anim::bind(t, entities);
            if (!b) continue;
            Captured c;
            c.addr.entityId = t.entityId;
            c.addr.comp     = t.comp;
            c.addr.key      = t.key;
            c.addr.index    = t.index;
            c.value         = anim::readValue(*b.prop, b.owner, t.index);
            m_captured.push_back(std::move(c));
        }
    }
}

void Runner::restore(std::vector<Entity>& entities) {
    for (const Captured& c : m_captured) {
        const anim::Bound b = anim::bind(c.addr, entities);
        if (b) anim::writeValue(*b.prop, b.owner, c.addr.index, c.value);
    }
}

void Runner::beginPreview(int seqIndex, const std::vector<Sequence>& seqs,
                          const std::vector<anim::Clip>& clips,
                          std::vector<Entity>& entities, int activeCam) {
    if (seqIndex < 0 || seqIndex >= static_cast<int>(seqs.size())) return;
    m_captured.clear();
    sequence      = seqIndex;
    preview       = true;
    m_camBefore   = activeCam;
    m_camSet      = -2;
    m_camReleased = false;
    m_jumped      = true;
    capture(seqs[seqIndex], clips, entities);
}

void Runner::endPreview(const std::vector<anim::Clip>&, std::vector<Entity>& entities,
                        int& activeCam) {
    if (!preview) return;
    silence();
    restore(entities);
    m_captured.clear();
    if (!m_camReleased && m_camSet != -2) activeCam = m_camBefore;
    m_camSet  = -2;
    preview   = false;
    playing   = false;
    frame     = Frame{};
}

void Runner::start(int seqIndex, int activeCam) {
    silence();
    m_captured.clear();
    sequence      = seqIndex;
    time          = 0.0f;
    playing       = true;
    preview       = false;
    m_camBefore   = activeCam;
    m_camSet      = -2;
    m_camReleased = false;
    m_jumped      = true;
    frame         = Frame{};
}

void Runner::stop(const std::vector<Sequence>& seqs, int& activeCam) {
    silence();
    const bool release = sequence >= 0 && sequence < static_cast<int>(seqs.size()) &&
                         seqs[sequence].releaseCamera;
    if (release && !m_camReleased && m_camSet != -2) activeCam = m_camBefore;
    m_camSet = -2;
    playing  = false;
    sequence = preview ? sequence : -1;
    frame    = Frame{};
}

bool Runner::tick(const std::vector<Sequence>& seqs, const std::vector<anim::Clip>& clips,
                  std::vector<Entity>& entities, float dt, int& activeCam) {
    // Fades still playing out, whatever the runner is doing now.
    for (auto it = m_dying.begin(); it != m_dying.end();) {
        it->second -= std::max(dt, 0.0f);
        it = (it->second <= 0.0f) ? m_dying.erase(it) : it + 1;
    }
    if (sequence < 0 || sequence >= static_cast<int>(seqs.size())) {
        if (!m_voices.empty()) silence();
        return true;
    }
    if (!playing && !preview) return true;
    const Sequence& s = seqs[sequence];

    bool finished = false;
    if (playing) {
        time += dt;
        if (!m_jumped) lockToAudio(dt);
        const float end = std::max(0.1f, s.duration);
        if (time >= end) {
            if (s.loop) {
                time = std::fmod(time, end);
                m_jumped = true;
            } else {
                time     = end;
                playing  = false;
                finished = true;
            }
        }
    }

    if (preview) {
        // Strips added since the preview began own properties nobody captured
        // yet; take them while they are still the authored values.
        restore(entities);
        capture(s, clips, entities);
    }
    applyAnim(s, clips, entities, time);

    frame = cutAt(s, time);
    if (!m_camReleased) {
        // Somebody else moved the view since this runner last set it -- the
        // viewport's exit button, a CameraSwitcher, a script. Theirs now.
        if (m_camSet != -2 && activeCam != m_camSet) {
            m_camReleased = true;
        } else if (frame.camera >= 0) {
            activeCam = frame.camera;
            m_camSet  = activeCam;
        }
    }

    feedAudio(s, dt);
    m_jumped = false;

    if (finished && !preview) {
        stop(seqs, activeCam);
        return false;
    }
    return true;
}

void Runner::feedAudio(const Sequence& s, float dt) {
    if (!m_audio || !playing) { if (!m_voices.empty()) silence(); return; }

    bool anySolo = false;
    for (const Track& tr : s.tracks)
        if (tr.kind == Kind::Audio && tr.solo) anySolo = true;

    std::set<int> live;
    for (const Track& tr : s.tracks) {
        if (tr.kind != Kind::Audio) continue;
        const bool audible = !tr.mute && (!anySolo || tr.solo);
        if (!audible) continue;
        for (const Strip& st : tr.strips) {
            if (st.sound.empty()) continue;
            // Loaded half a second ahead: opening a file is not free, and a
            // strip that starts on a cut has to sound ON the cut.
            if (time < st.start - 0.5f || time >= st.end()) continue;
            live.insert(st.uid);
            auto& slot = m_voices[st.uid];
            if (!slot) {
                slot = std::make_unique<Voice>();
                const std::string path = m_resolve ? m_resolve(st.sound) : st.sound;
                slot->sound = fitzel::Sound::fromFileSmoothed(*m_audio, path, kGlideMs);
                // The length, once per file and from a decoder of its own. NOT
                // from the voice: for an mp3 that question rewinds the decoder
                // the audio thread is reading from and scans the whole file --
                // asked every frame, that was the stutter.
                auto len = m_lengths.find(path);
                if (len == m_lengths.end())
                    len = m_lengths.emplace(path, fitzel::soundFileSeconds(path)).first;
                slot->length = len->second;
                if (slot->sound.isValid())
                    slot->sound.setOutput(m_audio->mixer(),
                                          m_route ? m_route(tr.channel) : m_audio->mixer().master());
            }
            Voice& v = *slot;
            v.start  = st.start;
            v.offset = st.offset;
            if (!v.sound.isValid()) continue;
            if (!st.covers(time)) continue;   // loaded ahead, not due yet
            const float want = st.sourceTime(time);
            const float vol  = masterGain * tr.gain * st.gain * st.fadeAt(time);
            if (v.length > 0.0f && want >= v.length) continue;   // past the end of the file: nothing to play
            v.sound.setVolume(vol);   // the first one lands at once, later ones glide
            if (!v.started) {
                v.sound.playFrom(want, kDeclickMs);
                v.started   = true;
                v.playedFor = 0.0f;
            } else {
                v.playedFor += dt;
                // Reseated only when the playhead really moved (a click on the
                // ruler, a loop) or the voice is far off: a voice yanked back
                // to meet a slow frame is the stutter this used to be. Small
                // drift is the clock's to absorb (lockToAudio).
                if (m_jumped || std::fabs(v.sound.cursorSeconds() - want) > 0.5f) {
                    v.sound.seekTo(want);
                    v.playedFor = 0.0f;
                    ++m_reseats;
                }
            }
        }
    }
    for (auto it = m_voices.begin(); it != m_voices.end();) {
        // A strip that ended, was muted or moved away: fade it, don't cut it.
        const seq::Strip* st = nullptr;
        bool sounding = live.count(it->first) > 0;
        if (sounding && it->second && it->second->started) {
            for (const Track& tr : s.tracks)
                for (const Strip& x : tr.strips) if (x.uid == it->first) st = &x;
            sounding = st && st->covers(time);
        }
        if (!sounding && it->second && it->second->started) {
            retireVoice(std::move(it->second));
            it = m_voices.erase(it);
        } else if (!live.count(it->first)) {
            it = m_voices.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace seq
