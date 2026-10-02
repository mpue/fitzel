// Retargeting, part 2: lining two skeletons up, turning source frames into
// target poses and clips, and the recipe that says which clips a character
// gets (see Retarget.hpp).
#include "Retarget.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace fs = std::filesystem;

namespace retarget {

namespace {

glm::vec3 pos(const glm::mat4& m) { return glm::vec3(m[3]); }

// The rotation part of a matrix, scale taken out of each axis.
glm::mat3 rot(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int c = 0; c < 3; ++c) {
        const float l = glm::length(r[c]);
        if (l > 1e-12f) r[c] /= l;
    }
    return r;
}

glm::mat4 trs(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
}

// The shortest rotation taking direction a onto direction b.
glm::mat3 between(glm::vec3 a, glm::vec3 b) {
    const float la = glm::length(a), lb = glm::length(b);
    if (la < 1e-9f || lb < 1e-9f) return glm::mat3(1.0f);
    a /= la; b /= lb;
    const glm::vec3 v = glm::cross(a, b);
    const float c = glm::dot(a, b);
    if (c > 1.0f - 1e-7f) return glm::mat3(1.0f);
    if (c < -1.0f + 1e-7f) {
        glm::vec3 ax = std::abs(a.x) < 0.9f ? glm::cross(a, glm::vec3(1, 0, 0)) : glm::cross(a, glm::vec3(0, 1, 0));
        ax = glm::normalize(ax);
        return 2.0f * glm::outerProduct(ax, ax) - glm::mat3(1.0f);
    }
    // Rodrigues: I + K + K^2 / (1 + c), K the cross-product matrix of v.
    const glm::mat3 K(0.0f, v.z, -v.y,   -v.z, 0.0f, v.x,   v.y, -v.x, 0.0f);
    return glm::mat3(1.0f) + K + K * K * (1.0f / (1.0f + c));
}

// Which way a skeleton faces at rest: its left side crossed with up.
glm::vec3 facingOf(const Rig& r, const BoneMap& m) {
    const int pairs[][2] = {{LUpperLeg, RUpperLeg}, {LUpperArm, RUpperArm}, {LShoulder, RShoulder}};
    for (const auto& pr : pairs) {
        const int a = m[static_cast<std::size_t>(pr[0])], b = m[static_cast<std::size_t>(pr[1])];
        if (a < 0 || b < 0) continue;
        const glm::vec3 lr = pos(r.nodes[static_cast<std::size_t>(a)].bind) - pos(r.nodes[static_cast<std::size_t>(b)].bind);
        glm::vec3 f = glm::cross(lr, glm::vec3(0.0f, 1.0f, 0.0f));
        f.y = 0.0f;
        if (glm::length(f) > 1e-6f) return glm::normalize(f);
    }
    return glm::vec3(0.0f, 0.0f, 1.0f);
}

// The floor under a skeleton at rest: its lowest foot or toe, else its lowest bone.
float groundOf(const Rig& r, const BoneMap& m, const glm::mat4& xf) {
    float g = 1e30f;
    for (int s : {LFoot, RFoot, LToes, RToes}) {
        const int n = m[static_cast<std::size_t>(s)];
        if (n >= 0) g = std::min(g, (xf * r.nodes[static_cast<std::size_t>(n)].bind)[3].y);
    }
    if (g < 1e29f) return g;
    for (const Node& nd : r.nodes)
        if (nd.joint) g = std::min(g, (xf * nd.bind)[3].y);
    return g < 1e29f ? g : 0.0f;
}

// The slots a bone's direction is measured against (the first mapped on both
// sides wins), and what it does when there is none: keep its own rest (false)
// or turn like the bone above it (true).
struct DirRule { std::vector<int> kids; bool likeParent = false; };

DirRule dirRule(int s) {
    switch (s) {
        case Hips:       return {{Spine, Chest, UpperChest, Neck, Head}, false};
        case Spine:      return {{Chest, UpperChest, Neck, Head}, false};
        case Chest:      return {{UpperChest, Neck, Head}, false};
        case UpperChest: return {{Neck, UpperNeck, Head}, false};
        case Neck:       return {{UpperNeck, Head}, false};
        case UpperNeck:  return {{Head}, false};
        case LShoulder:  return {{LUpperArm}, false};
        case RShoulder:  return {{RUpperArm}, false};
        case LUpperArm:  return {{LLowerArm, LHand}, false};
        case RUpperArm:  return {{RLowerArm, RHand}, false};
        case LLowerArm:  return {{LHand}, false};
        case RLowerArm:  return {{RHand}, false};
        case LHand:      return {{LMiddle1, LIndex1, LRing1}, true};
        case RHand:      return {{RMiddle1, RIndex1, RRing1}, true};
        case LUpperLeg:  return {{LLowerLeg, LFoot}, false};
        case RUpperLeg:  return {{RLowerLeg, RFoot}, false};
        case LLowerLeg:  return {{LFoot}, false};
        case RLowerLeg:  return {{RFoot}, false};
        case LFoot:      return {{LToes}, false};
        case RFoot:      return {{RToes}, false};
        default: break;
    }
    if (s >= LThumb1 && s < SlotCount) {
        const int k = (s - LThumb1) % 3;   // joint 1, 2 or 3 of a finger
        if (k == 0) return {{s + 1, s + 2}, false};
        if (k == 1) return {{s + 1}, false};
        return {{}, true};
    }
    return {{}, false};   // pelvis, head, toes
}

} // namespace

glm::vec3 facing(const Rig& rig, const BoneMap& m) { return facingOf(rig, m); }

// --- Transfer ------------------------------------------------------------------------

Transfer::Transfer(const Rig& src, const BoneMap& sm, const Rig& tgt, const BoneMap& tm) : m_tgt(&tgt) {
    if (!src.ok()) { m_problem = src.error.empty() ? "The motion has no skeleton." : src.error; return; }
    if (!tgt.ok()) { m_problem = tgt.error.empty() ? "The character has no skeleton." : tgt.error; return; }
    auto both = [&](int s) { return sm[static_cast<std::size_t>(s)] >= 0 && tm[static_cast<std::size_t>(s)] >= 0; };
    if (!both(Hips)) { m_problem = "The hips are not mapped on both sides."; return; }

    // Turn the source to face where the target faces.
    const glm::vec3 fs = facingOf(src, sm), ft = facingOf(tgt, tm);
    const float ang = std::atan2(glm::cross(fs, ft).y, glm::dot(fs, ft));
    m_yaw = glm::rotate(glm::mat4(1.0f), ang, glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat3 yaw3(m_yaw);
    auto srcBind = [&](int n) { return m_yaw * src.nodes[static_cast<std::size_t>(n)].bind; };
    auto tgtBind = [&](int n) { return tgt.nodes[static_cast<std::size_t>(n)].bind; };

    m_srcHip = sm[Hips];
    m_tgtHip = tm[Hips];
    m_srcHipBind = pos(srcBind(m_srcHip));
    m_tgtHipBind = pos(tgtBind(m_tgtHip));
    const float hs = m_srcHipBind.y - groundOf(src, sm, m_yaw);
    const float ht = m_tgtHipBind.y - groundOf(tgt, tm, glm::mat4(1.0f));
    m_scale = hs > 1e-4f && ht > 1e-4f ? ht / hs : 1.0f;

    std::array<glm::mat3, SlotCount> O;
    std::array<bool, SlotCount> haveO{};
    for (int s = 0; s < SlotCount; ++s) {
        if (!both(s)) continue;
        const int sn = sm[static_cast<std::size_t>(s)], tn = tm[static_cast<std::size_t>(s)];
        const DirRule rule = dirRule(s);
        int kid = -1;
        for (int c : rule.kids)
            if (both(c)) { kid = c; break; }
        glm::mat3 o(1.0f);
        if (kid >= 0) {
            const glm::vec3 dt = pos(tgtBind(tm[static_cast<std::size_t>(kid)])) - pos(tgtBind(tn));
            const glm::vec3 ds = pos(srcBind(sm[static_cast<std::size_t>(kid)])) - pos(srcBind(sn));
            o = between(dt, ds);
        } else if (rule.likeParent) {
            const int p = slotInfo(s).parent;
            if (p >= 0 && haveO[static_cast<std::size_t>(p)]) o = O[static_cast<std::size_t>(p)];
        }
        O[static_cast<std::size_t>(s)] = o;
        haveO[static_cast<std::size_t>(s)] = true;
        Step st;
        st.src = sn;
        st.tgt = tn;
        st.srcBindInv = glm::transpose(rot(srcBind(sn)));
        st.base = o * rot(tgtBind(tn));
        m_plan.push_back(st);
    }
    (void)yaw3;
    m_stepOf.assign(tgt.nodes.size(), -1);
    for (std::size_t k = 0; k < m_plan.size(); ++k) m_stepOf[static_cast<std::size_t>(m_plan[k].tgt)] = static_cast<int>(k);

    // Bones outside the template that the source has too -- same name, same
    // parent name (a twist bone, a toe, a jaw) -- take over the source bone's
    // turn away from its rest, local to its parent. Between two characters of
    // one rig family that is the rest of the skeleton; between two different
    // rigs it is nothing at all. Only bones the source actually moves.
    std::unordered_map<std::string, int> byName;
    for (std::size_t i = 0; i < src.nodes.size(); ++i)
        if (src.nodes[i].joint) byName.emplace(src.nodes[i].name, static_cast<int>(i));
    std::vector<char> moving(src.nodes.size(), 0);
    for (const Motion& m : src.motions)
        for (const Motion::Channel& c : m.channels)
            if (c.path == 1 && c.node >= 0 && c.node < static_cast<int>(moving.size()))
                moving[static_cast<std::size_t>(c.node)] = 1;
    m_copyOf.assign(tgt.nodes.size(), Copy{});
    for (std::size_t i = 0; i < tgt.nodes.size(); ++i) {
        const Node& tn = tgt.nodes[i];
        if (m_stepOf[i] >= 0 || !tn.joint) continue;
        const auto it = byName.find(tn.name);
        if (it == byName.end() || !moving[static_cast<std::size_t>(it->second)]) continue;
        const Node& sn = src.nodes[static_cast<std::size_t>(it->second)];
        if ((tn.parent < 0) != (sn.parent < 0)) continue;
        if (tn.parent >= 0 &&
            tgt.nodes[static_cast<std::size_t>(tn.parent)].name != src.nodes[static_cast<std::size_t>(sn.parent)].name)
            continue;
        m_copyOf[i] = Copy{it->second, sn.parent, glm::inverse(sn.r)};
        ++m_copied;
    }
    for (int i : tgt.order)
        if (m_stepOf[static_cast<std::size_t>(i)] >= 0 || m_copyOf[static_cast<std::size_t>(i)].src >= 0)
            m_animated.push_back(i);
}

std::vector<int> Transfer::animatedNodes() const { return m_animated; }

void Transfer::run(const std::vector<glm::mat4>& sw, const glm::vec3& shift, std::vector<glm::mat4>& W,
                   std::vector<glm::quat>* localRot, glm::vec3* hipT) const {
    if (!valid() || !m_tgt) return;
    const Rig& tg = *m_tgt;
    W.resize(tg.nodes.size());
    if (localRot) localRot->resize(m_animated.size());
    const glm::vec3 srcHip = glm::vec3(m_yaw * glm::vec4(pos(sw[static_cast<std::size_t>(m_srcHip)]), 1.0f));
    const glm::vec3 hipPos = m_tgtHipBind + (srcHip - m_srcHipBind - shift) * m_scale;
    const glm::mat3 yaw3(m_yaw);
    std::size_t out = 0;
    for (int i : tg.order) {
        const std::size_t u = static_cast<std::size_t>(i);
        const Node& n = tg.nodes[u];
        const glm::mat4 P = n.parent >= 0 ? W[static_cast<std::size_t>(n.parent)] : glm::mat4(1.0f);
        const int st = m_stepOf[u];
        if (st < 0) {
            const Copy& c = m_copyOf[u];
            if (c.src < 0) { W[u] = P * trs(n.t, n.r, n.s); continue; }
            const glm::mat3 own = rot(sw[static_cast<std::size_t>(c.src)]);
            const glm::mat3 local = c.srcParent >= 0 ? glm::transpose(rot(sw[static_cast<std::size_t>(c.srcParent)])) * own : own;
            const glm::quat q = glm::normalize(n.r * (c.srcRestInv * glm::quat_cast(local)));
            W[u] = P * trs(n.t, q, n.s);
            if (localRot) (*localRot)[out] = q;
            ++out;
            continue;
        }
        const Step& s = m_plan[static_cast<std::size_t>(st)];
        const glm::mat3 want = yaw3 * rot(sw[static_cast<std::size_t>(s.src)]) * s.srcBindInv * s.base;
        const glm::quat q = glm::normalize(glm::quat_cast(glm::transpose(rot(P)) * want));
        glm::vec3 T = n.t;
        if (i == m_tgtHip) {
            T = glm::vec3(glm::inverse(P) * glm::vec4(hipPos, 1.0f));
            if (hipT) *hipT = T;
        }
        W[u] = P * trs(T, q, n.s);
        if (localRot) (*localRot)[out] = q;
        ++out;
    }
}

void Transfer::pose(const std::vector<glm::mat4>& sw, const glm::vec3& shift, std::vector<glm::mat4>& W) const {
    run(sw, shift, W, nullptr, nullptr);
}

void Transfer::poseLocal(const std::vector<glm::mat4>& sw, const glm::vec3& shift, std::vector<glm::mat4>& W,
                         std::vector<glm::quat>& localRot, glm::vec3& hipT) const {
    run(sw, shift, W, &localRot, &hipT);
}

void clipRange(const Rig& src, int motion, const Options& o, float& a, float& b) {
    const float D = motion >= 0 && motion < static_cast<int>(src.motions.size())
                        ? src.motions[static_cast<std::size_t>(motion)].duration
                        : 0.0f;
    a = std::clamp(o.start, 0.0f, D);
    b = o.end > o.start ? std::clamp(o.end, a, D) : D;
}

glm::vec3 Transfer::drift(const Rig& src, int motion, const Options& o, float t) const {
    if (!o.inPlace || !valid()) return glm::vec3(0.0f);
    float a = 0.0f, b = 0.0f;
    clipRange(src, motion, o, a, b);
    std::vector<glm::mat4> w;
    src.sample(motion, a, w);
    const glm::vec3 p0 = glm::vec3(m_yaw * glm::vec4(pos(w[static_cast<std::size_t>(m_srcHip)]), 1.0f));
    src.sample(motion, b, w);
    const glm::vec3 p1 = glm::vec3(m_yaw * glm::vec4(pos(w[static_cast<std::size_t>(m_srcHip)]), 1.0f));
    glm::vec3 d0 = p0 - m_srcHipBind, v = p1 - p0;
    d0.y = 0.0f;
    v.y = 0.0f;
    const float f = b > a ? std::clamp((t - a) / (b - a), 0.0f, 1.0f) : 0.0f;
    return d0 + v * f;
}

Clip bake(const Rig& src, int motion, const Transfer& tx, const Rig& tgt, const Options& o,
          const std::string& name) {
    (void)tgt;
    Clip c;
    c.name = name;
    if (!tx.valid() || motion < 0 || motion >= static_cast<int>(src.motions.size())) return c;
    const Motion& m = src.motions[static_cast<std::size_t>(motion)];
    c.fps = m.fps;
    float a = 0.0f, b = 0.0f;
    clipRange(src, motion, o, a, b);
    const int frames = std::max(1, static_cast<int>(std::lround((b - a) * m.fps)) + 1);
    c.nodes = tx.animatedNodes();
    c.rot.assign(c.nodes.size(), {});
    c.hipNode = tx.hipNode();
    std::vector<glm::mat4> sw, W;
    std::vector<glm::quat> q;
    glm::vec3 hipT(0.0f);
    for (int k = 0; k < frames; ++k) {
        const float t = std::min(a + static_cast<float>(k) / m.fps, b);
        src.sample(motion, t, sw);
        tx.poseLocal(sw, tx.drift(src, motion, o, t), W, q, hipT);
        for (std::size_t n = 0; n < q.size(); ++n) {
            glm::quat v = q[n];
            if (!c.rot[n].empty() && glm::dot(c.rot[n].back(), v) < 0.0f) v = -v;   // no flips between keys
            c.rot[n].push_back(v);
        }
        c.hipT.push_back(hipT);
    }
    return c;
}

int motionIndex(const Rig& rig, const std::string& take) {
    if (rig.motions.empty()) return -1;
    for (std::size_t i = 0; i < rig.motions.size(); ++i)
        if (rig.motions[i].name == take) return static_cast<int>(i);
    // No choice made: the one that moves the skeleton most -- a file exported
    // from Blender also carries an action per mesh (shape keys, a prop).
    int best = 0;
    std::size_t bestN = 0;
    float bestLen = -1.0f;
    for (std::size_t i = 0; i < rig.motions.size(); ++i) {
        std::size_t n = 0;
        for (const Motion::Channel& c : rig.motions[i].channels)
            n += c.node >= 0 && c.node < static_cast<int>(rig.nodes.size()) &&
                         rig.nodes[static_cast<std::size_t>(c.node)].joint ? 1 : 0;
        const float len = rig.motions[i].duration;
        if (n > bestN || (n == bestN && len > bestLen)) { best = static_cast<int>(i); bestN = n; bestLen = len; }
    }
    return best;
}
// --- The recipe ------------------------------------------------------------------------

std::string recipePath(const std::string& model) { return model + ".retarget"; }
std::string originalPath(const std::string& model) { return model + ".orig"; }

bool loadRecipe(const std::string& model, Recipe& r) {
    r = Recipe{};
    std::ifstream f(fs::path(recipePath(model)));
    if (!f) return false;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    auto names = [](const nlohmann::json& o) {
        std::map<std::string, std::string> m;
        if (o.is_object())
            for (auto it = o.begin(); it != o.end(); ++it)
                if (it.value().is_string()) m[it.key()] = it.value().get<std::string>();
        return m;
    };
    if (j.contains("repair") && j["repair"].is_object()) {
        r.repair.mergeSkins = j["repair"].value("mergeSkins", true);
        r.repair.bindLoose  = j["repair"].value("bindLoose", true);
    }
    if (j.contains("targetBones")) r.tgtMap = names(j["targetBones"]);
    r.bakedHash = j.value("bakedHash", std::string());
    if (j.contains("clips") && j["clips"].is_array())
        for (const nlohmann::json& c : j["clips"]) {
            if (!c.is_object()) continue;
            ClipEntry e;
            e.name    = c.value("name", std::string());
            e.source  = c.value("source", std::string());
            e.take    = c.value("take", std::string());
            e.start   = c.value("start", 0.0f);
            e.end     = c.value("end", 0.0f);
            e.inPlace = c.value("inPlace", false);
            if (c.contains("sourceBones")) e.srcMap = names(c["sourceBones"]);
            if (!e.name.empty() && !e.source.empty()) r.clips.push_back(std::move(e));
        }
    return true;
}

bool saveRecipe(const std::string& model, const Recipe& r) {
    nlohmann::json j;
    j["version"] = 1;
    j["repair"] = {{"mergeSkins", r.repair.mergeSkins}, {"bindLoose", r.repair.bindLoose}};
    j["targetBones"] = r.tgtMap;
    j["bakedHash"] = r.bakedHash;
    nlohmann::json clips = nlohmann::json::array();
    for (const ClipEntry& e : r.clips) {
        nlohmann::json c = {{"name", e.name}, {"source", e.source}, {"take", e.take},
                            {"start", e.start}, {"end", e.end}, {"inPlace", e.inPlace}};
        c["sourceBones"] = e.srcMap;
        clips.push_back(std::move(c));
    }
    j["clips"] = std::move(clips);
    const std::string path = recipePath(model);
    std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}

std::string sourceFile(const std::string& model, const std::string& source) {
    const fs::path p = fs::path(source);
    if (p.is_absolute()) return p.lexically_normal().generic_string();
    return (fs::path(model).parent_path() / p).lexically_normal().generic_string();
}

std::string sourceRef(const std::string& model, const std::string& projectDir, const std::string& file) {
    const fs::path f = fs::path(file).lexically_normal();
    if (!projectDir.empty()) {
        const std::string proj = fs::path(projectDir).lexically_normal().generic_string();
        const std::string abs = f.generic_string();
        auto low = [](std::string s) {
            for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        if (low(abs).rfind(low(proj), 0) == 0) {
            const fs::path rel = f.lexically_relative(fs::path(model).parent_path().lexically_normal());
            if (!rel.empty()) return rel.generic_string();
        }
    }
    return f.generic_string();
}

std::string fileHash(const std::string& path) {
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) return {};
    std::uint64_t h = 14695981039346656037ull;
    std::vector<char> buf(1 << 20);
    while (f) {
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize n = f.gcount();
        for (std::streamsize i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(buf[static_cast<std::size_t>(i)]);
            h *= 1099511628211ull;
        }
    }
    char out[24];
    std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(h));
    return out;
}

namespace {
std::string cleanName(const std::string& raw) {
    std::string s;
    for (char ch : raw) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) s.push_back(static_cast<char>(std::tolower(c)));
        else if (!s.empty() && s.back() != '_') s.push_back('_');
    }
    while (!s.empty() && s.back() == '_') s.pop_back();
    return s.empty() ? std::string("motion") : s;
}
} // namespace

std::string clipNameFor(const std::string& file) { return cleanName(fs::path(file).stem().string()); }

std::string clipNameForTake(const std::string& take) {
    const std::size_t bar = take.find_last_of('|');
    return cleanName(bar == std::string::npos ? take : take.substr(bar + 1));
}

std::string baseFor(const std::string& model, const Recipe& r) {
    std::error_code ec;
    const std::string orig = originalPath(model);
    if (!fs::exists(fs::path(orig), ec)) return model;
    if (r.bakedHash.empty() || !fs::exists(fs::path(model), ec)) return orig;
    return fileHash(model) == r.bakedHash ? orig : model;
}

bool bakeAll(const std::string& model, Recipe& r,
             const std::function<const Rig*(const ClipEntry&)>& source, std::string& message) {
    std::error_code ec;
    const std::string file = fs::path(model).filename().string();
    const std::string orig = originalPath(model);
    std::string base = baseFor(model, r);
    bool newOriginal = false;
    if (base == model) {
        if (!fs::exists(fs::path(model), ec)) { message = file + " is not there."; return false; }
        fs::copy_file(fs::path(model), fs::path(orig), fs::copy_options::overwrite_existing, ec);
        if (ec) { message = "Could not keep the original of " + file + ": " + ec.message(); return false; }
        base = orig;
        newOriginal = true;
    }
    const Rig tgt = loadRig(base, false);
    if (!tgt.ok()) { message = tgt.error; return false; }
    BoneMap tm = autoMap(tgt);
    applyNames(tgt, r.tgtMap, tm);

    std::vector<Clip> clips;
    for (const ClipEntry& e : r.clips) {
        const Rig* src = source(e);
        if (!src || !src->ok()) {
            message = "\"" + e.name + "\": the motion " + fs::path(e.source).filename().string() +
                      " is not ready" + (src && !src->error.empty() ? " (" + src->error + ")" : std::string()) + ".";
            return false;
        }
        BoneMap sm = autoMap(*src);
        applyNames(*src, e.srcMap, sm);
        const Transfer tx(*src, sm, tgt, tm);
        if (!tx.valid()) { message = "\"" + e.name + "\": " + tx.problem(); return false; }
        Options o;
        o.start = e.start;
        o.end = e.end;
        o.inPlace = e.inPlace;
        clips.push_back(bake(*src, motionIndex(*src, e.take), tx, tgt, o, e.name));
        if (clips.back().frames() == 0) { message = "\"" + e.name + "\": the motion has no frames."; return false; }
    }
    std::string err;
    if (!writeModel(base, model, clips, r.repair, tm, err)) { message = err; return false; }
    r.bakedHash = fileHash(model);
    saveRecipe(model, r);
    char buf[256];
    std::snprintf(buf, sizeof buf, "Wrote %d animation%s into %s%s.", static_cast<int>(clips.size()),
                  clips.size() == 1 ? "" : "s", file.c_str(),
                  newOriginal ? " (the original is kept as .orig)" : "");
    message = buf;
    return true;
}

} // namespace retarget