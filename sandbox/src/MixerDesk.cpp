#include "MixerPanel.hpp"

#include <nlohmann/json.hpp>

// The desk's runtime half: the default layout, saving and loading it with the
// scene, and making fitzel::Mixer follow it. Compiled into the player as well --
// a shipped game mixes with the desk its scene was saved with.
namespace mixerui {

using fitzel::mixfx::Type;

namespace {

Insert makeInsert(Type t) {
    Insert in;
    in.type = t;
    for (const auto& p : fitzel::mixfx::params(t)) in.values.push_back(p.def);
    return in;
}

const char* kindKey(Kind k) {
    return k == Kind::Aux ? "aux" : k == Kind::Master ? "master" : "channel";
}

nlohmann::json stripJson(const Strip& s) {
    nlohmann::json j;
    j["uid"]   = s.uid;
    j["kind"]  = kindKey(s.kind);
    j["name"]  = s.name;
    j["level"] = s.level;
    j["pan"]   = s.pan;
    j["mute"]  = s.mute;
    nlohmann::json ins = nlohmann::json::array();
    for (const Insert& in : s.inserts) {
        nlohmann::json e;
        e["type"]   = fitzel::mixfx::key(in.type);
        e["bypass"] = in.bypass;
        // By parameter NAME: a parameter added to an effect later must not shift
        // every saved value one place along.
        nlohmann::json p = nlohmann::json::object();
        const auto& defs = fitzel::mixfx::params(in.type);
        for (std::size_t i = 0; i < defs.size() && i < in.values.size(); ++i)
            p[defs[i].name] = in.values[i];
        e["params"] = p;
        ins.push_back(e);
    }
    j["inserts"] = ins;
    nlohmann::json sends = nlohmann::json::array();
    for (const auto& [aux, sd] : s.sends)
        if (sd.level > 0.0f)
            sends.push_back({{"aux", aux}, {"level", sd.level}, {"pre", sd.pre}});
    j["sends"] = sends;
    return j;
}

template <class T>
T get(const nlohmann::json& j, const char* k, T def) {
    const auto it = j.find(k);
    if (it == j.end()) return def;
    if constexpr (std::is_same_v<T, bool>) return it->is_boolean() ? it->get<bool>() : def;
    else if constexpr (std::is_same_v<T, std::string>) return it->is_string() ? it->get<std::string>() : def;
    else return it->is_number() ? it->get<T>() : def;
}

Strip stripFrom(const nlohmann::json& j) {
    Strip s;
    s.uid   = get(j, "uid", 0);
    const std::string k = get(j, "kind", std::string("channel"));
    s.kind  = k == "aux" ? Kind::Aux : k == "master" ? Kind::Master : Kind::Channel;
    s.name  = get(j, "name", std::string("Channel"));
    s.level = std::max(0.0f, get(j, "level", 1.0f));
    s.pan   = std::clamp(get(j, "pan", 0.0f), -1.0f, 1.0f);
    s.mute  = get(j, "mute", false);
    if (const auto it = j.find("inserts"); it != j.end() && it->is_array())
        for (const auto& e : *it) {
            Type t;
            if (!e.is_object() || !fitzel::mixfx::fromKey(get(e, "type", std::string()), t)) continue;
            Insert in = makeInsert(t);
            in.bypass = get(e, "bypass", false);
            if (const auto p = e.find("params"); p != e.end() && p->is_object()) {
                const auto& defs = fitzel::mixfx::params(t);
                for (std::size_t i = 0; i < defs.size(); ++i)
                    in.values[i] = std::clamp(get(*p, defs[i].name, defs[i].def), defs[i].min, defs[i].max);
            }
            s.inserts.push_back(std::move(in));
        }
    if (const auto it = j.find("sends"); it != j.end() && it->is_array())
        for (const auto& e : *it) {
            if (!e.is_object()) continue;
            Send sd;
            sd.level = std::max(0.0f, get(e, "level", 0.0f));
            sd.pre   = get(e, "pre", false);
            s.sends[get(e, "aux", 0)] = sd;
        }
    return s;
}

} // namespace

Desk::Desk() { reset(); }

void Desk::reset() {
    // The engine strips of the old layout go on the next sync.
    for (const Strip& s : strips) if (s.engine >= 0) retired.push_back(s.engine);
    strips.clear();
    const int keepMasterEngine = master.engine;
    master = Strip{};
    master.kind   = Kind::Master;
    master.name   = "Master";
    master.uid    = 0;
    master.level  = 0.8f;
    master.engine = keepMasterEngine;
    nextUid = 1;
    add(Kind::Channel, "Ambient");
    add(Kind::Channel, "SFX");
    add(Kind::Channel, "Music");
    Strip& rv = add(Kind::Aux, "Reverb");
    Insert in = makeInsert(Type::Reverb);
    in.values = {0.7f, 0.45f, 1.0f};     // a bus reverb is all wet
    rv.inserts.push_back(in);
    ++revision;
}

void Desk::loadLegacy(float ambient, bool ambientMute, float sfx, bool sfxMute,
                      float masterLevel, bool masterMute) {
    reset();
    for (Strip& s : strips) {
        if (s.name == "Ambient") { s.level = ambient; s.mute = ambientMute; }
        if (s.name == "SFX")     { s.level = sfx;     s.mute = sfxMute; }
    }
    master.level = masterLevel;
    master.mute  = masterMute;
}

Strip* Desk::find(int uid) {
    if (uid == master.uid) return &master;
    for (Strip& s : strips) if (s.uid == uid) return &s;
    return nullptr;
}
const Strip* Desk::find(int uid) const { return const_cast<Desk*>(this)->find(uid); }

std::string Desk::uniqueName(const std::string& base) const {
    auto taken = [&](const std::string& n) {
        for (const Strip& s : strips) if (s.name == n) return true;
        return n == master.name;
    };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        const std::string n = base + " " + std::to_string(i);
        if (!taken(n)) return n;
    }
}

Strip& Desk::add(Kind kind, const std::string& name) {
    Strip s;
    s.uid  = nextUid++;
    s.kind = kind;
    s.name = uniqueName(name);
    // Aux buses after the channels, the way a desk is laid out.
    auto at = strips.end();
    if (kind == Kind::Channel)
        at = std::find_if(strips.begin(), strips.end(),
                          [](const Strip& x) { return x.kind == Kind::Aux; });
    ++revision;
    return *strips.insert(at, std::move(s));
}

void Desk::remove(int uid) {
    for (auto it = strips.begin(); it != strips.end(); ++it)
        if (it->uid == uid) {
            if (it->engine >= 0) retired.push_back(it->engine);
            strips.erase(it);
            for (Strip& s : strips) s.sends.erase(uid);
            ++revision;
            return;
        }
}

void Desk::rename(int uid, const std::string& name) {
    Strip* s = find(uid);
    if (!s || s->kind == Kind::Master || name.empty() || name == s->name) return;
    s->name = uniqueName(name);
    ++revision;
}

bool Desk::anySolo() const {
    for (const Strip& s : strips) if (s.kind == Kind::Channel && s.solo) return true;
    return false;
}

float Desk::gainOf(const Strip& s) const {
    if (s.mute) return 0.0f;
    // Solo silences the other CHANNELS. Aux buses are solo-safe, as on a desk:
    // soloing a voice should still let you hear its reverb.
    if (s.kind == Kind::Channel && anySolo() && !s.solo) return 0.0f;
    return s.level;
}

std::vector<std::string> Desk::channelNames() const {
    std::vector<std::string> out;
    for (const Strip& s : strips) if (s.kind == Kind::Channel) out.push_back(s.name);
    return out;
}

int Desk::route(const std::string& name, const std::string& fallback) const {
    for (const Strip& s : strips) if (s.kind == Kind::Channel && s.name == name) return s.engine;
    for (const Strip& s : strips) if (s.kind == Kind::Channel && s.name == fallback) return s.engine;
    return master.engine;
}

void Desk::sync(fitzel::Mixer& m) {
    if (!m.ok()) return;
    for (int id : retired) m.removeStrip(id);
    retired.clear();

    // Strips the engine does not have yet (a new strip, a loaded scene, a fresh
    // engine). Aux buses before channels: a new channel wires its sends to the
    // buses that already exist.
    bool changed = false;
    auto ensure = [&](Strip& s) {
        if (s.engine >= 0 && m.has(s.engine)) return;
        s.engine     = s.kind == Kind::Master ? m.master() : m.addStrip(s.kind);
        s.builtValid = false;
        for (Insert& in : s.inserts) in.sent.clear();
        changed = true;
    };
    ensure(master);
    for (Strip& s : strips) if (s.kind == Kind::Aux) ensure(s);
    for (Strip& s : strips) if (s.kind == Kind::Channel) ensure(s);
    if (changed) ++revision;

    auto push = [&](Strip& s) {
        if (s.engine < 0) return;
        m.setGain(s.engine, gainOf(s));
        m.setPan(s.engine, s.pan);
        // The chain: rebuilt when its shape changed, then only what moved.
        std::vector<Type> chain;
        for (const Insert& in : s.inserts) chain.push_back(in.type);
        if (!s.builtValid || chain != s.built) {
            m.setInserts(s.engine, chain);
            s.built = chain;
            s.builtValid = true;
            for (Insert& in : s.inserts) in.sent.clear();
        }
        for (int i = 0; i < static_cast<int>(s.inserts.size()); ++i) {
            Insert& in = s.inserts[i];
            if (in.sent.size() != in.values.size()) {
                for (int p = 0; p < static_cast<int>(in.values.size()); ++p)
                    m.setInsertParam(s.engine, i, p, in.values[p]);
                in.sent = in.values;
                m.setInsertBypass(s.engine, i, in.bypass);
                in.sentBypass = in.bypass;
                continue;
            }
            for (int p = 0; p < static_cast<int>(in.values.size()); ++p)
                if (in.sent[p] != in.values[p]) {
                    m.setInsertParam(s.engine, i, p, in.values[p]);
                    in.sent[p] = in.values[p];
                }
            if (in.sentBypass != in.bypass) {
                m.setInsertBypass(s.engine, i, in.bypass);
                in.sentBypass = in.bypass;
            }
        }
        float l = 0.0f, r = 0.0f;
        m.takePeak(s.engine, l, r);
        s.peak[0] = std::max(s.peak[0], l);
        s.peak[1] = std::max(s.peak[1], r);
    };
    push(master);
    for (Strip& s : strips) {
        push(s);
        if (s.kind != Kind::Channel || s.engine < 0) continue;
        for (const Strip& a : strips) {
            if (a.kind != Kind::Aux || a.engine < 0) continue;
            const auto it = s.sends.find(a.uid);
            const Send sd = it == s.sends.end() ? Send{} : it->second;
            m.setSend(s.engine, a.engine, sd.level, sd.pre);
        }
    }
}

nlohmann::json Desk::toJson() const {
    nlohmann::json j;
    j["version"] = 1;
    j["nextUid"] = nextUid;
    j["master"]  = stripJson(master);
    nlohmann::json arr = nlohmann::json::array();
    for (const Strip& s : strips) arr.push_back(stripJson(s));
    j["strips"] = arr;
    return j;
}

void Desk::fromJson(const nlohmann::json& j) {
    if (!j.is_object()) { reset(); return; }
    for (const Strip& s : strips) if (s.engine >= 0) retired.push_back(s.engine);
    strips.clear();
    const int keepMasterEngine = master.engine;
    if (const auto it = j.find("master"); it != j.end() && it->is_object()) master = stripFrom(*it);
    else master = Strip{};
    master.kind   = Kind::Master;
    master.name   = "Master";
    master.uid    = 0;
    master.engine = keepMasterEngine;
    master.builtValid = false;
    int maxUid = 0;
    if (const auto it = j.find("strips"); it != j.end() && it->is_array())
        for (const auto& e : *it) {
            if (!e.is_object()) continue;
            Strip s = stripFrom(e);
            if (s.kind == Kind::Master) continue;
            if (s.uid <= 0 || find(s.uid)) s.uid = 100000 + static_cast<int>(strips.size());
            maxUid = std::max(maxUid, s.uid);
            strips.push_back(std::move(s));
        }
    nextUid = std::max(get(j, "nextUid", 1), maxUid + 1);
    // Sends to buses that are not there any more are dropped, not kept dangling.
    for (Strip& s : strips)
        for (auto it = s.sends.begin(); it != s.sends.end();) {
            const Strip* a = find(it->first);
            it = (a && a->kind == Kind::Aux) ? std::next(it) : s.sends.erase(it);
        }
    ++revision;
}

} // namespace mixerui