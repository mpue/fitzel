#include "WorldAudio.hpp"

#include <algorithm>
#include <cmath>

#include "CityGen.hpp"
#include "Component.hpp"
#include "SceneTypes.hpp"

namespace {

// Distance attenuation for a rival's engine and for a swoosh. Both are things
// heard across a track rather than across a room, hence the generous minimum:
// inside it the source is at full level, which keeps a rival held alongside from
// swinging in volume with every twitch of the racing line.
constexpr float kRivalMin = 12.0f, kRivalMax = 160.0f, kRivalRolloff = 1.4f;
constexpr float kPassMin  = 8.0f,  kPassMax  = 90.0f,  kPassRolloff  = 1.6f;

float lengthSafe(const glm::vec3& v) {
    const float l2 = glm::dot(v, v);
    return (l2 > 1e-12f) ? std::sqrt(l2) : 0.0f;
}

} // namespace

void WorldAudio::load(fitzel::Audio& audio, const std::string& soundDir) {
    if (m_loaded) return;
    m_audio = &audio;

    // One Sound per voice, all from the same file. This is what a pool IS: the
    // sample is decoded once per voice here and never again, so firing one later
    // is a seek and a start rather than a load.
    for (RivalVoice& v : m_rivals) {
        v.sound = fitzel::Sound::fromFile(audio, soundDir + "/jet_thrust.wav", true);
        if (!v.sound.isValid()) continue;
        v.sound.setSpatial(true);
        v.sound.setAttenuation(kRivalMin, kRivalMax, kRivalRolloff);
        v.sound.setVolume(0.0f);   // silent until it has somebody to follow
    }
    // The first voice decides. Without it the sample is missing, and loading the
    // other five would print the same failure five more times to say so.
    const std::string swoosh = soundDir + "/swoosh.wav";
    m_pass[0] = fitzel::Sound::fromFile(audio, swoosh, false);
    for (int i = 1; i < kPassVoices && m_pass[0].isValid(); ++i)
        m_pass[i] = fitzel::Sound::fromFile(audio, swoosh, false);
    for (fitzel::Sound& s : m_pass) {
        if (!s.isValid()) continue;
        s.setSpatial(true);
        s.setAttenuation(kPassMin, kPassMax, kPassRolloff);
    }
    m_loaded = true;
}

void WorldAudio::reset() {
    for (RivalVoice& v : m_rivals) {
        if (v.sound.isValid()) { v.sound.setVolume(0.0f); v.sound.stop(); }
        v.entity  = -1;
        v.hasLast = false;
    }
    for (fitzel::Sound& s : m_pass)
        if (s.isValid()) s.stop();
    m_tracks.clear();
}

void WorldAudio::firePass(const glm::vec3& pos, const glm::vec3& vel,
                          float strength) {
    strength = std::clamp(strength, 0.0f, 1.0f);
    for (fitzel::Sound& s : m_pass) {
        if (!s.isValid() || s.isPlaying()) continue;
        s.setPosition(pos.x, pos.y, pos.z);
        // The velocity is the RELATIVE motion of the pass, which is what the
        // spatializer needs to bend the pitch. A swoosh with no velocity is a
        // sample; with one it is a thing going past.
        s.setVelocity(vel.x, vel.y, vel.z);
        s.setDopplerFactor(doppler);
        s.setVolume(passGain * (0.35f + 0.65f * strength));
        // Faster passes are brighter as well as louder. The range is narrow on
        // purpose: past about a fifth either way a pitched sample stops sounding
        // like the same object moving and starts sounding like a different one.
        s.setPitch(0.9f + 0.35f * strength);
        s.play();
        return;
    }
    // Pool dry: this pass is dropped. That is the right failure -- the voices
    // still sounding are the closer, louder ones that were fired first.
}

void WorldAudio::update(float dt,
                        const glm::vec3& listenerPos, const glm::vec3& listenerFwd,
                        const glm::vec3& listenerUp, const glm::vec3& listenerVel,
                        const std::vector<Entity>& entities,
                        const city::District* district,
                        int skipIdA, int skipIdB,
                        float masterGain) {
    if (!m_loaded || !m_audio) return;
    dt = std::clamp(dt, 1.0f / 240.0f, 0.1f);

    m_audio->setListener(listenerPos.x, listenerPos.y, listenerPos.z,
                         listenerFwd.x, listenerFwd.y, listenerFwd.z,
                         listenerUp.x,  listenerUp.y,  listenerUp.z,
                         listenerVel.x, listenerVel.y, listenerVel.z);

    const float listenerSpeed = lengthSafe(listenerVel);
    for (auto& kv : m_tracks) kv.second.seen = false;

    // --- Rivals: the nearest few get a voice ---------------------------------
    // Nearest rather than "the ones in front": a rival being caught and one
    // catching up are both worth hearing, and which of them it is comes out of
    // the Doppler shift anyway.
    struct Cand { int id; float d2; glm::vec3 pos; };
    std::vector<Cand> cands;
    cands.reserve(16);
    for (const Entity& e : entities) {
        const auto* op = e.components.get<OpponentComponent>();
        if (!op || !op->entered || !e.activeInHierarchy) continue;
        if (e.id == skipIdA || e.id == skipIdB) continue;
        const glm::vec3 rel = e.center - listenerPos;
        const float d2 = glm::dot(rel, rel);
        if (d2 > rivalRange * rivalRange) continue;
        cands.push_back({e.id, d2, e.center});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
    if (static_cast<int>(cands.size()) > kRivalVoices)
        cands.resize(kRivalVoices);

    // Hand each voice the rival it already had where possible. A voice that
    // jumps between craft every time the order changes makes the engine note
    // teleport across the stereo field, which is far more noticeable than a
    // rival that simply goes quiet.
    bool taken[kRivalVoices] = {};
    std::vector<bool> placed(cands.size(), false);
    for (int v = 0; v < kRivalVoices; ++v) {
        if (m_rivals[v].entity < 0) continue;
        for (std::size_t c = 0; c < cands.size(); ++c)
            if (!placed[c] && cands[c].id == m_rivals[v].entity) {
                taken[v] = true; placed[c] = true;
                break;
            }
        if (!taken[v]) {           // its rival dropped out of range
            m_rivals[v].entity  = -1;
            m_rivals[v].hasLast = false;
            if (m_rivals[v].sound.isValid()) m_rivals[v].sound.setVolume(0.0f);
        }
    }
    for (std::size_t c = 0; c < cands.size(); ++c) {
        if (placed[c]) continue;
        for (int v = 0; v < kRivalVoices; ++v) {
            if (taken[v] || m_rivals[v].entity >= 0) continue;
            m_rivals[v].entity  = cands[c].id;
            m_rivals[v].hasLast = false;   // no history: a new craft, not a jump
            taken[v] = true; placed[c] = true;
            break;
        }
    }

    for (RivalVoice& v : m_rivals) {
        if (!v.sound.isValid()) continue;
        if (v.entity < 0) { if (v.sound.isPlaying()) v.sound.setVolume(0.0f); continue; }
        const Cand* cd = nullptr;
        for (const Cand& c : cands) if (c.id == v.entity) { cd = &c; break; }
        if (!cd) continue;

        // Velocity from the position delta. The AI's own speed is along the road
        // and this needs a world vector; a delta gives that without reaching into
        // how the opponent is driven at all.
        glm::vec3 vel(0.0f);
        if (v.hasLast) vel = (cd->pos - v.lastPos) / dt;
        v.lastPos = cd->pos;
        v.hasLast = true;

        v.sound.setPosition(cd->pos.x, cd->pos.y, cd->pos.z);
        v.sound.setVelocity(vel.x, vel.y, vel.z);
        v.sound.setDopplerFactor(doppler);
        // A rival's engine note rises with its speed exactly as the player's
        // does -- the Doppler on top of it is the spatializer's business, and
        // the two are different things: this is how hard it is working, that is
        // how fast it is coming at you.
        const float sp = lengthSafe(vel);
        v.sound.setPitch(0.82f + std::clamp(sp / 90.0f, 0.0f, 1.0f) * 0.5f);
        v.sound.setVolume(rivalGain * masterGain);
        if (!v.sound.isPlaying()) v.sound.play();
    }

    // --- Passes: rivals and buildings ---------------------------------------
    // A pass is not "something is close", it is the MOMENT something goes by:
    // the frame its position crosses the listener's shoulder line. Watching for
    // that crossing is what puts the swoosh on the beat instead of somewhere in
    // the neighbourhood of it -- and it is self-limiting, because a thing can
    // only cross once until it crosses back.
    const auto consider = [&](int key, const glm::vec3& pos, float radius,
                              const glm::vec3& objVel) {
        const glm::vec3 rel  = pos - listenerPos;
        const float     side = glm::dot(rel, listenerFwd);
        Track&          tr   = m_tracks[key];
        const float     prev = tr.side;
        const bool      had  = tr.seen || prev != 0.0f;
        tr.side = side;
        tr.seen = true;
        if (!had || prev <= 0.0f || side > 0.0f) return;  // not a front-to-back crossing

        // How close it actually went by: the distance across the direction of
        // travel at the moment of crossing, which is the miss distance.
        const glm::vec3 across = rel - listenerFwd * side;
        const float     miss   = lengthSafe(across);
        const float     reach  = radius + passReach;
        if (miss > reach) return;

        const glm::vec3 relVel = objVel - listenerVel;
        const float     closing = lengthSafe(relVel);
        if (closing < passMinSpeed) return;

        // Loud and bright for a fast, close pass; quiet for a distant, slow one.
        // Both halves matter: a tower missed by a metre at full speed and one
        // missed by fifteen are the same event and must not sound the same.
        const float near_ = 1.0f - std::clamp(miss / std::max(reach, 1.0f), 0.0f, 1.0f);
        const float fast  = std::clamp((closing - passMinSpeed) / 60.0f, 0.0f, 1.0f);
        firePass(pos, relVel, 0.35f * near_ + 0.65f * fast * near_ + 0.25f * fast);
    };

    if (masterGain > 0.001f) {
        for (const Entity& e : entities) {
            const auto* op = e.components.get<OpponentComponent>();
            if (!op || !op->entered || !e.activeInHierarchy) continue;
            if (e.id == skipIdA || e.id == skipIdB) continue;
            glm::vec3 vel(0.0f);
            for (const RivalVoice& v : m_rivals)   // reuse the voice's own history
                if (v.entity == e.id && v.hasLast) { vel = (e.center - v.lastPos) / dt; break; }
            consider(e.id, e.center, glm::length(e.half), vel);
        }
        // Buildings are merged geometry and not entities, but the district keeps
        // a bounding sphere per building for exactly this kind of question (see
        // CityGen.hpp) -- so a tower can be heard going past without a single
        // one of them costing an entity.
        if (district && listenerSpeed >= passMinSpeed) {
            const int n = static_cast<int>(district->buildings.size());
            for (int i = 0; i < n; ++i) {
                const auto& b = district->buildings[i];
                const glm::vec3 rel = b.center - listenerPos;
                // Cheap reject first: this runs over every building in the city
                // on every frame, and the crossing test below is not free.
                const float cut = b.radius + passReach + 4.0f;
                if (glm::dot(rel, rel) > cut * cut) { continue; }
                consider(~i, b.center, b.radius, glm::vec3(0.0f));
            }
        }
    }

    // Prune what is no longer around, so the map does not grow for the length of
    // a session -- a city is thousands of buildings and every one of them would
    // otherwise keep an entry after being driven past once.
    for (auto it = m_tracks.begin(); it != m_tracks.end(); ) {
        if (it->second.seen) ++it;
        else                 it = m_tracks.erase(it);
    }
}
