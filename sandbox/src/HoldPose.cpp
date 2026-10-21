#include "HoldPose.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace holdpose {

namespace {

const float kDeg = 3.14159265f / 180.0f;

glm::vec3 posOf(const glm::mat4& m) { return glm::vec3(m[3]); }

// The turn of a transform, its scale taken out.
glm::quat turnOf(const glm::mat4& m) {
    glm::mat3 r(m);
    for (int k = 0; k < 3; ++k) {
        const float len = glm::length(r[k]);
        if (len > 1e-9f) r[k] /= len;
    }
    return glm::normalize(glm::quat_cast(r));
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool readText(const std::string& file, std::string& text, std::string& err) {
    std::ifstream f(fs::path(file), std::ios::binary);
    if (!f) {
        err = "Cannot read " + fs::path(file).filename().string();
        return false;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}

std::vector<float> numbers(const std::string& list) {
    std::vector<float> out;
    std::stringstream ss(list);
    std::string part;
    while (std::getline(ss, part, ',')) {
        const auto a = part.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        out.push_back(std::strtof(part.c_str() + a, nullptr));
    }
    return out;
}

std::string fmt(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", static_cast<double>(std::abs(v) < 5e-7f ? 0.0f : v));
    std::string s = b;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s += '0';
    return s;
}

std::string list(std::initializer_list<float> v) {
    std::string s;
    for (float x : v) s += (s.empty() ? "" : ", ") + fmt(x);
    return s;
}

json vec(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }
glm::vec3 vecOf(const json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return glm::vec3(j[0].get<float>(), j[1].get<float>(), j[2].get<float>());
}

// The pattern of one weapon's entry: "name = { r = {...}, t = {...} }".
std::regex entryPattern(const std::string& name) {
    return std::regex("(\\b" + name + "\\s*=\\s*\\{\\s*r\\s*=\\s*\\{)([^}]*)(\\}\\s*,\\s*t\\s*=\\s*\\{)([^}]*)(\\}\\s*\\})");
}

bool validName(const std::string& n) {
    if (n.empty() || std::isdigit(static_cast<unsigned char>(n[0]))) return false;
    for (char c : n)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
    return true;
}

} // namespace

// --- Recipe -----------------------------------------------------------------------------

std::string recipePath(const std::string& arms) { return arms + ".holdposes"; }

std::string resolve(const std::string& arms, const std::string& rel) {
    if (rel.empty()) return {};
    fs::path p(rel);
    if (p.is_relative()) p = fs::path(arms).parent_path() / p;
    return p.lexically_normal().generic_string();
}

std::string relative(const std::string& arms, const std::string& file) {
    std::error_code ec;
    const fs::path r = fs::relative(fs::path(file), fs::path(arms).parent_path(), ec);
    return ec || r.empty() ? fs::path(file).generic_string() : r.generic_string();
}

bool loadRecipe(const std::string& arms, Recipe& r) {
    r = Recipe{};
    std::ifstream f(fs::path(recipePath(arms)));
    if (!f) return false;
    const json j = json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    r.holds = j.value("holds", std::string());
    r.fov = j.value("fov", 60.0f);
    if (j.contains("poses") && j["poses"].is_object())
        for (const auto& [name, p] : j["poses"].items()) {
            Weapon w;
            w.model = p.value("weapon", std::string());
            w.scale = vecOf(p.value("scale", json()), glm::vec3(1.0f));
            w.off = vecOf(p.value("off", json()), w.off);
            w.aim = vecOf(p.value("aim", json()), glm::vec3(0.0f));
            r.poses[name] = w;
        }
    return true;
}

bool saveRecipe(const std::string& arms, const Recipe& r) {
    json j;
    j["holds"] = r.holds;
    j["fov"] = r.fov;
    json poses = json::object();
    for (const auto& [name, w] : r.poses)
        poses[name] = {{"weapon", w.model}, {"scale", vec(w.scale)}, {"off", vec(w.off)}, {"aim", vec(w.aim)}};
    j["poses"] = poses;
    std::ofstream f(fs::path(recipePath(arms)), std::ios::binary);
    if (!f) return false;
    f << j.dump(2) << "\n";
    return static_cast<bool>(f);
}

// --- The Lua table -------------------------------------------------------------------------

bool readHoldsInfo(const std::string& file, HoldsInfo& info, std::string& err) {
    info = HoldsInfo{};
    std::string text;
    if (!readText(file, text, err)) return false;
    std::smatch m;
    if (std::regex_search(text, m, std::regex("\\bscale\\s*=\\s*([-+0-9.eE]+)")))
        info.scale = std::strtof(m[1].str().c_str(), nullptr);
    // The first centre is the arms' own (a later one belongs to the body).
    if (std::regex_search(text, m, std::regex("\\bcentre\\s*=\\s*\\{([^}]*)\\}"))) {
        const std::vector<float> c = numbers(m[1].str());
        if (c.size() == 3) {
            info.centre = glm::vec3(c[0], c[1], c[2]);
            info.haveCentre = true;
        }
    }
    return true;
}

bool readPlacement(const std::string& file, const std::string& name, Placement& p, std::string& err) {
    std::string text;
    if (!readText(file, text, err)) return false;
    if (!validName(name)) return false;
    std::smatch m;
    if (!std::regex_search(text, m, entryPattern(name))) return false;
    const std::vector<float> r = numbers(m[2].str()), t = numbers(m[4].str());
    if (r.size() != 9 || t.size() != 3) {
        err = "The entry '" + name + "' does not have 9 + 3 numbers";
        return false;
    }
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col) p.r[col][row] = r[static_cast<std::size_t>(row * 3 + col)];
    p.t = glm::vec3(t[0], t[1], t[2]);
    return true;
}

bool writePlacement(const std::string& file, const std::string& name, const Placement& p, std::string& err) {
    if (!validName(name)) {
        err = "'" + name + "' cannot be a key of the Lua table";
        return false;
    }
    std::string text;
    if (!readText(file, text, err)) return false;
    const glm::mat3& r = p.r;
    const std::string rs = list({r[0][0], r[1][0], r[2][0], r[0][1], r[1][1], r[2][1], r[0][2], r[1][2], r[2][2]});
    const std::string ts = list({p.t.x, p.t.y, p.t.z});
    std::smatch m;
    if (std::regex_search(text, m, entryPattern(name))) {
        text = m.prefix().str() + m[1].str() + rs + m[3].str() + ts + m[5].str() + m.suffix().str();
    } else {
        const std::size_t close = text.rfind('}');
        if (close == std::string::npos) {
            err = fs::path(file).filename().string() + " does not end in a table";
            return false;
        }
        const std::string entry = "    " + name + " = { r = {" + rs + "},\n" + std::string(name.size() + 7, ' ') +
                                  "t = {" + ts + "} },\n";
        std::size_t at = text.rfind('\n', close);
        at = at == std::string::npos ? close : at + 1;
        text.insert(at, entry);
    }
    const std::string tmp = file + ".writing";
    {
        std::ofstream f(fs::path(tmp), std::ios::binary);
        if (!f || !(f << text)) {
            err = "Cannot write " + fs::path(file).filename().string();
            return false;
        }
    }
    std::error_code ec;
    fs::rename(fs::path(tmp), fs::path(file), ec);
    if (ec) {
        err = "Cannot replace " + fs::path(file).filename().string() + ": " + ec.message();
        return false;
    }
    return true;
}

// --- Frames ---------------------------------------------------------------------------------

glm::mat3 euler(const glm::vec3& a) {
    const glm::mat4 rx = glm::rotate(glm::mat4(1.0f), a.x * kDeg, glm::vec3(1, 0, 0));
    const glm::mat4 ry = glm::rotate(glm::mat4(1.0f), a.y * kDeg, glm::vec3(0, 1, 0));
    const glm::mat4 rz = glm::rotate(glm::mat4(1.0f), a.z * kDeg, glm::vec3(0, 0, 1));
    return glm::mat3(rz * ry * rx);
}

glm::mat4 weaponFrame(const Weapon& w) {
    return glm::translate(glm::mat4(1.0f), glm::vec3(w.off.x, w.off.y, -w.off.z)) * glm::mat4(euler(w.aim));
}

void bounds(const fitzel::ModelData& md, glm::vec3& centre, glm::vec3& size) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const fitzel::ModelPrimitive& p : md.primitives)
        for (std::size_t i = 0; i + 2 < p.vertices.size(); i += 8) {
            const glm::vec3 v(p.vertices[i], p.vertices[i + 1], p.vertices[i + 2]);
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
    if (lo.x > hi.x) lo = hi = glm::vec3(0.0f);
    centre = 0.5f * (lo + hi);
    size = hi - lo;
}

glm::mat4 weaponMatrix(const Weapon& w, const glm::vec3& c) {
    return weaponFrame(w) * glm::scale(glm::mat4(1.0f), w.scale) * glm::translate(glm::mat4(1.0f), -c);
}

glm::mat4 armsMatrix(const glm::mat4& weapon, const Placement& p, const HoldsInfo& info, const glm::vec3& c) {
    const float s = info.scale;
    const glm::vec3 hc = info.haveCentre ? info.centre : c;
    return weapon * glm::translate(glm::mat4(1.0f), p.t) * glm::mat4(p.r) *
           glm::translate(glm::mat4(1.0f), hc * s) * glm::scale(glm::mat4(1.0f), glm::vec3(s)) *
           glm::translate(glm::mat4(1.0f), -c);
}

// --- The poser ------------------------------------------------------------------------------

const char* fingerName(int f) {
    static const char* n[] = {"Thumb", "Index", "Middle", "Ring", "Little"};
    return f >= 0 && f < FingerCount ? n[f] : "?";
}

bool Poser::init(const fitzel::ModelData& arms) {
    m_skel = &arms.skeleton;
    m_ok = false;
    m_problem.clear();
    for (auto& side : m_fingers)
        for (auto& f : side) f = {-1, -1, -1};
    if (arms.skeleton.empty()) {
        m_problem = "The model has no skeleton";
        return false;
    }
    const auto& skel = arms.skeleton;
    auto under = [&](int j, int root) {
        for (int k = 0; j >= 0 && k < 512; ++k) {
            if (j == root) return true;
            j = skel[static_cast<std::size_t>(j)].parent;
        }
        return false;
    };
    auto depth = [&](int j) {
        int d = 0;
        while (j >= 0 && d < 512) { j = skel[static_cast<std::size_t>(j)].parent; ++d; }
        return d;
    };
    static const std::vector<std::vector<const char*>> words = {
        {"thumb"}, {"index"}, {"middle"}, {"ring"}, {"little", "pinky"}};
    auto fingerJoint = [&](int j) {
        const std::string n = lower(skel[static_cast<std::size_t>(j)].name);
        for (const auto& f : words)
            for (const char* w : f)
                if (n.find(w) != std::string::npos) return true;
        return false;
    };
    // The arms: ik::findRig finds them by name, but a name with "hand" in it is
    // not always a hand (a helmet's "EyeHand"). A wrist is what the fingers hang
    // from: of the joints the rig's search -- or any "hand"/"wrist" -- offers, the
    // one with the most fingers under it that are not under another such joint.
    m_rig = ik::findRig(skel);
    for (int side = 0; side < 2; ++side) {
        int best = -1, bestScore = 0, bestDepth = 1 << 30;
        for (int j = 0; j < static_cast<int>(skel.size()); ++j) {
            const std::string n = lower(skel[static_cast<std::size_t>(j)].name);
            if ((n.find("hand") == std::string::npos && n.find("wrist") == std::string::npos) || fingerJoint(j)) continue;
            const std::string tag = side ? "right" : "left";
            const std::string sideTok = side ? "r" : "l";
            // The side: "Right", "_R_", "R_", ".R", "_r" ...
            bool mine = n.find(tag) != std::string::npos;
            for (std::size_t k = 0; !mine && k < n.size(); ++k) {
                if (n[k] != sideTok[0]) continue;
                const bool startOk = k == 0 || !std::isalpha(static_cast<unsigned char>(n[k - 1]));
                const bool endOk = k + 1 >= n.size() || !std::isalpha(static_cast<unsigned char>(n[k + 1]));
                mine = startOk && endOk;
            }
            if (!mine) continue;
            int score = 0;
            for (int f = 0; f < static_cast<int>(skel.size()); ++f)
                if (f != j && fingerJoint(f) && under(f, j)) ++score;
            const int d = depth(j);
            if (score > bestScore || (score == bestScore && score > 0 && d < bestDepth)) {
                best = j;
                bestScore = score;
                bestDepth = d;
            }
        }
        if (best < 0) continue;
        ik::Chain c;
        c.end = best;
        c.mid = skel[static_cast<std::size_t>(c.end)].parent;
        c.root = c.mid >= 0 ? skel[static_cast<std::size_t>(c.mid)].parent : -1;
        // Twist bones in between are stepped over, as ik::findRig does.
        auto twist = [&](int j) {
            const std::string n = lower(skel[static_cast<std::size_t>(j)].name);
            return n.find("twist") != std::string::npos || n.find("roll") != std::string::npos ||
                   n.find("share") != std::string::npos;
        };
        while (c.mid >= 0 && twist(c.mid)) c.mid = skel[static_cast<std::size_t>(c.mid)].parent;
        c.root = c.mid >= 0 ? skel[static_cast<std::size_t>(c.mid)].parent : -1;
        while (c.root >= 0 && twist(c.root)) c.root = skel[static_cast<std::size_t>(c.root)].parent;
        if (c.ok()) m_rig.arm[static_cast<std::size_t>(side)] = c;
    }
    if (!m_rig.arm[0].ok() && !m_rig.arm[1].ok()) {
        m_problem = "No arm found on the skeleton (a hand or wrist with two bones above it)";
        return false;
    }
    for (int side = 0; side < 2; ++side) {
        const int w = m_rig.arm[static_cast<std::size_t>(side)].end;
        if (w < 0) continue;
        for (int f = 0; f < FingerCount; ++f) {
            std::vector<std::pair<int, int>> found;   // depth, joint
            for (int j = 0; j < static_cast<int>(skel.size()); ++j) {
                if (j == w || !under(j, w)) continue;
                const std::string n = lower(skel[static_cast<std::size_t>(j)].name);
                bool hit = false;
                for (const char* word : words[static_cast<std::size_t>(f)]) hit = hit || n.find(word) != std::string::npos;
                // "ring" is in no other finger's name; "middle" neither -- but a
                // metacarpal ("carpal") is not a joint that curls.
                if (hit && n.find("carpal") == std::string::npos) found.push_back({depth(j), j});
            }
            std::sort(found.begin(), found.end());
            for (std::size_t k = 0; k < found.size() && k < 3; ++k)
                m_fingers[static_cast<std::size_t>(side)][static_cast<std::size_t>(f)][k] = found[k].second;
        }
    }
    m_ok = true;
    return true;
}

std::vector<glm::mat4> Poser::basePose(const fitzel::ModelData& arms, int clip) const {
    std::vector<glm::mat4> pal;
    if (clip >= 0 && clip < static_cast<int>(arms.animations.size())) pal = fitzel::sampleSkeleton(arms, clip, 0.0f);
    if (pal.empty()) {
        // The rest pose: each joint's rest under its parent's.
        const auto& skel = arms.skeleton;
        std::vector<glm::mat4> G(skel.size());
        for (std::size_t j = 0; j < skel.size(); ++j) {
            const fitzel::SkeletonJoint& s = skel[j];
            const glm::mat4 L = glm::translate(glm::mat4(1.0f), s.restT) * glm::mat4_cast(s.restR) *
                                glm::scale(glm::mat4(1.0f), s.restS);
            G[j] = (s.parent >= 0 && s.parent < static_cast<int>(j) ? G[static_cast<std::size_t>(s.parent)] : s.baseParent) * L;
        }
        return G;
    }
    return ik::joints(pal, arms.skeleton);
}

Hand Poser::handOf(const std::vector<glm::mat4>& base, int side, const glm::mat4& arms, const glm::mat4& weapon) const {
    Hand h;
    const int w = wrist(side);
    if (w < 0 || w >= static_cast<int>(base.size())) {
        h.on = false;
        return h;
    }
    const glm::mat4 inWeapon = glm::inverse(weapon) * arms * base[static_cast<std::size_t>(w)];
    h.pos = posOf(inWeapon);
    h.rot = turnOf(inWeapon);
    return h;
}

bool Poser::palm(const std::vector<glm::mat4>& G, int side, glm::vec3& centre, glm::vec3& facing) const {
    const auto& F = m_fingers[static_cast<std::size_t>(side & 1)];
    const int w = wrist(side);
    if (w < 0 || w >= static_cast<int>(G.size())) return false;
    const glm::vec3 W = posOf(G[static_cast<std::size_t>(w)]);
    const int mid = F[Middle][0] >= 0 ? F[Middle][0] : F[Index][0];
    if (mid < 0) {
        centre = W;
        facing = glm::vec3(0.0f);
        return true;
    }
    const glm::vec3 M = posOf(G[static_cast<std::size_t>(mid)]);
    centre = 0.5f * (W + M);
    facing = glm::vec3(0.0f);
    if (F[Index][0] >= 0 && F[Little][0] >= 0) {
        const glm::vec3 f = glm::normalize(M - W);
        glm::vec3 a = posOf(G[static_cast<std::size_t>(F[Little][0])]) - posOf(G[static_cast<std::size_t>(F[Index][0])]);
        a -= f * glm::dot(a, f);
        if (glm::length(a) > 1e-6f) {
            a = glm::normalize(a);
            facing = (side & 1) ? glm::cross(f, a) : glm::cross(a, f);
        }
    }
    return true;
}

void Poser::curl(std::vector<glm::mat4>& G, int side, const std::array<float, FingerCount>& deg) const {
    bool any = false;
    for (float d : deg) any = any || std::abs(d) > 1e-3f;
    if (!any) return;
    glm::vec3 c, palmDir;
    if (!palm(G, side, c, palmDir) || glm::length(palmDir) < 1e-6f) return;
    const auto& F = m_fingers[static_cast<std::size_t>(side & 1)];
    const glm::vec3 W = posOf(G[static_cast<std::size_t>(wrist(side))]);
    const glm::vec3 f = glm::normalize(posOf(G[static_cast<std::size_t>(F[Middle][0] >= 0 ? F[Middle][0] : F[Index][0])]) - W);
    // Every joint about the line of the knuckles, towards the palm; the thumb
    // about its own line across the palm (as tools/prep_fpsbody.py curls them).
    const glm::vec3 knuckles = glm::normalize(glm::cross(f, palmDir));
    static const float share[3] = {1.0f, 1.0f, 0.6f};
    for (int fi = 0; fi < FingerCount; ++fi) {
        const float d = deg[static_cast<std::size_t>(fi)];
        if (std::abs(d) < 1e-3f) continue;
        const auto& J = F[static_cast<std::size_t>(fi)];
        glm::vec3 axis = knuckles;
        if (fi == Thumb) {
            if (J[0] < 0 || J[1] < 0) continue;
            const glm::vec3 t = posOf(G[static_cast<std::size_t>(J[1])]) - posOf(G[static_cast<std::size_t>(J[0])]);
            const glm::vec3 x = glm::cross(t, palmDir);
            if (glm::length(x) < 1e-6f) continue;
            axis = glm::normalize(x);
        }
        for (int k = 0; k < 3; ++k) {
            const int j = J[static_cast<std::size_t>(k)];
            if (j < 0) break;
            ik::rotateSubtree(G, *m_skel, j, posOf(G[static_cast<std::size_t>(j)]),
                              glm::angleAxis(d * share[k] * kDeg, axis));
        }
    }
}

void Poser::solve(const std::vector<glm::mat4>& base, const Edit& e, const glm::mat4& arms, const glm::mat4& weapon,
                  std::vector<glm::mat4>& G, float* shortBy) const {
    G = base;
    if (shortBy) shortBy[0] = shortBy[1] = 0.0f;
    if (!m_ok || !m_skel) return;
    const glm::mat4 toModel = glm::inverse(arms) * weapon;
    for (int side = 0; side < 2; ++side) {
        const ik::Chain& c = m_rig.arm[static_cast<std::size_t>(side)];
        const Hand& h = e.hands[static_cast<std::size_t>(side)];
        if (!c.ok() || c.end >= static_cast<int>(G.size())) continue;
        if (h.on) {
            const glm::mat4 target = toModel * glm::translate(glm::mat4(1.0f), h.pos) * glm::mat4_cast(h.rot);
            const glm::vec3 P = posOf(target);
            const glm::vec3 S = posOf(G[static_cast<std::size_t>(c.root)]);
            const glm::vec3 pole = glm::vec3(side ? 1.0f : -1.0f, -1.0f, 0.0f);
            ik::solveTwoBone(G, *m_skel, c, P, pole, false);
            // The elbow swung round the line from the shoulder to the wrist: the
            // hand stays where it is, the arm comes up or tucks in.
            const glm::vec3 E = posOf(G[static_cast<std::size_t>(c.end)]);
            if (std::abs(h.swivel) > 1e-3f && glm::length(E - S) > 1e-6f)
                ik::rotateSubtree(G, *m_skel, c.root, S, glm::angleAxis(h.swivel * kDeg, glm::normalize(E - S)));
            // ...and turned as its target is.
            const glm::vec3 W = posOf(G[static_cast<std::size_t>(c.end)]);
            const glm::quat now = turnOf(G[static_cast<std::size_t>(c.end)]);
            ik::rotateSubtree(G, *m_skel, c.end, W, turnOf(target) * glm::inverse(now));
            if (shortBy) {
                // In metres of the view: the model may be scaled.
                const glm::vec3 a = glm::vec3(arms * glm::vec4(W, 1.0f)), b = glm::vec3(arms * glm::vec4(P, 1.0f));
                shortBy[side] = glm::length(a - b);
            }
        }
        curl(G, side, h.curl);
    }
}

// --- Writing --------------------------------------------------------------------------------

retarget::Clip clipOf(const std::string& name, const fitzel::ModelData& arms, const retarget::Rig& rig,
                      const std::vector<glm::mat4>& G) {
    retarget::Clip c;
    c.name = name;
    c.fps = 30.0f;
    const auto& skel = arms.skeleton;
    std::set<int> seen;
    for (std::size_t j = 0; j < skel.size() && j < G.size() && j < rig.slotNodes.size(); ++j) {
        const int node = rig.slotNodes[j];
        if (node < 0 || !seen.insert(node).second) continue;
        const int p = skel[j].parent;
        const glm::mat4 parent = p >= 0 && p < static_cast<int>(G.size()) ? G[static_cast<std::size_t>(p)] : skel[j].baseParent;
        const glm::mat4 L = glm::inverse(parent) * G[j];
        const glm::quat q = turnOf(L);
        c.nodes.push_back(node);
        c.rot.push_back({q, q});
        const glm::vec3 t = posOf(L);
        if (glm::length(t - skel[j].restT) > 1e-5f) {
            c.tNodes.push_back(node);
            c.trans.push_back({t, t});
        }
    }
    return c;
}

// --- Picking -------------------------------------------------------------------------------

bool pick(const fitzel::ModelData& md, const glm::mat4& place, const glm::vec3& o, const glm::vec3& d,
          float& best, glm::vec3& point, glm::vec3& normal) {
    best = 1e30f;
    bool hit = false;
    for (const fitzel::ModelPrimitive& p : md.primitives) {
        const std::size_t n = p.vertices.size() / 8;
        for (std::size_t i = 0; i + 2 < n; i += 3) {
            glm::vec3 v[3];
            for (int k = 0; k < 3; ++k) {
                const float* f = &p.vertices[(i + static_cast<std::size_t>(k)) * 8];
                v[k] = glm::vec3(place * glm::vec4(f[0], f[1], f[2], 1.0f));
            }
            // Moller-Trumbore, both faces.
            const glm::vec3 e1 = v[1] - v[0], e2 = v[2] - v[0];
            const glm::vec3 pv = glm::cross(d, e2);
            const float det = glm::dot(e1, pv);
            if (std::abs(det) < 1e-12f) continue;
            const float inv = 1.0f / det;
            const glm::vec3 tv = o - v[0];
            const float u = glm::dot(tv, pv) * inv;
            if (u < 0.0f || u > 1.0f) continue;
            const glm::vec3 qv = glm::cross(tv, e1);
            const float w = glm::dot(d, qv) * inv;
            if (w < 0.0f || u + w > 1.0f) continue;
            const float t = glm::dot(e2, qv) * inv;
            if (t <= 1e-5f || t >= best) continue;
            best = t;
            point = o + d * t;
            normal = glm::normalize(glm::cross(e1, e2));
            if (glm::dot(normal, d) > 0.0f) normal = -normal;
            hit = true;
        }
    }
    return hit;
}

} // namespace holdpose
