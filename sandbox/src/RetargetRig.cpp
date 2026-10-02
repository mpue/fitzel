// Retargeting, part 1: reading a rig out of a glTF file, sampling its motions,
// and filling the humanoid template from it (see Retarget.hpp).
#include "Retarget.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>

#include <cgltf.h>
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace retarget {

namespace {

glm::mat4 trs(const glm::vec3& t, const glm::quat& r, const glm::vec3& s) {
    return glm::translate(glm::mat4(1.0f), t) * glm::mat4_cast(r) * glm::scale(glm::mat4(1.0f), s);
}

// Frame rates a recording is likely to have been made at: a key count that
// lands within a few percent of one of these means exactly that one.
float snapFps(float f) {
    static const float kCommon[] = {12, 15, 24, 25, 30, 48, 50, 60, 90, 120};
    for (float c : kCommon)
        if (std::abs(f - c) <= 0.03f * c) return c;
    return std::clamp(std::round(f), 1.0f, 240.0f);
}

int findKey(const std::vector<float>& times, float t) {
    // The last key at or before t (0 when t is before the first).
    auto it = std::upper_bound(times.begin(), times.end(), t);
    if (it == times.begin()) return 0;
    return static_cast<int>(it - times.begin()) - 1;
}

glm::vec4 sampleChannel(const Motion::Channel& c, float t) {
    const std::size_t n = c.times.size();
    const bool cubic = c.interp == 2;
    auto value = [&](std::size_t k) { return cubic ? c.values[k * 3 + 1] : c.values[k]; };
    if (n == 0) return glm::vec4(0.0f);
    if (n == 1 || t <= c.times.front()) return value(0);
    if (t >= c.times.back()) return value(n - 1);
    const std::size_t k = static_cast<std::size_t>(findKey(c.times, t));
    const float t0 = c.times[k], t1 = c.times[k + 1];
    const float dt = std::max(t1 - t0, 1e-6f);
    const float s = std::clamp((t - t0) / dt, 0.0f, 1.0f);
    if (c.interp == 1) return value(k);
    if (cubic) {
        const glm::vec4 v0 = c.values[k * 3 + 1], b0 = c.values[k * 3 + 2];
        const glm::vec4 a1 = c.values[(k + 1) * 3], v1 = c.values[(k + 1) * 3 + 1];
        const float s2 = s * s, s3 = s2 * s;
        glm::vec4 p = (2 * s3 - 3 * s2 + 1) * v0 + (s3 - 2 * s2 + s) * dt * b0 +
                      (-2 * s3 + 3 * s2) * v1 + (s3 - s2) * dt * a1;
        if (c.path == 1) p = glm::normalize(p);
        return p;
    }
    const glm::vec4 a = value(k), b = value(k + 1);
    if (c.path == 1) {
        const glm::quat qa(a.w, a.x, a.y, a.z), qb(b.w, b.x, b.y, b.z);
        const glm::quat q = glm::slerp(qa, glm::dot(qa, qb) < 0.0f ? -qb : qb, s);
        return glm::vec4(q.x, q.y, q.z, q.w);
    }
    return a + (b - a) * s;
}

} // namespace

// --- Rig ------------------------------------------------------------------------

int Motion::frames() const {
    if (duration <= 0.0f) return 1;
    return static_cast<int>(std::lround(duration * fps)) + 1;
}

int Rig::find(const std::string& name) const {
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
        if (nodes[static_cast<std::size_t>(i)].name == name) return i;
    return -1;
}

int Rig::jointCount() const {
    int n = 0;
    for (const Node& nd : nodes) n += nd.joint ? 1 : 0;
    return n;
}

void Rig::sample(int m, float t, std::vector<glm::mat4>& world) const {
    const std::size_t N = nodes.size();
    std::vector<glm::vec3> T(N), S(N);
    std::vector<glm::quat> R(N);
    for (std::size_t i = 0; i < N; ++i) { T[i] = nodes[i].t; R[i] = nodes[i].r; S[i] = nodes[i].s; }
    if (m >= 0 && m < static_cast<int>(motions.size())) {
        for (const Motion::Channel& c : motions[static_cast<std::size_t>(m)].channels) {
            if (c.node < 0 || c.node >= static_cast<int>(N)) continue;
            const glm::vec4 v = sampleChannel(c, t);
            const std::size_t i = static_cast<std::size_t>(c.node);
            if (c.path == 0) T[i] = glm::vec3(v);
            else if (c.path == 1) R[i] = glm::normalize(glm::quat(v.w, v.x, v.y, v.z));
            else S[i] = glm::vec3(v);
        }
    }
    world.resize(N);
    for (int i : order) {
        const std::size_t u = static_cast<std::size_t>(i);
        const glm::mat4 local = trs(T[u], R[u], S[u]);
        const int p = nodes[u].parent;
        world[u] = p >= 0 ? world[static_cast<std::size_t>(p)] * local : local;
    }
}

Rig loadRig(const std::string& path, bool withMotions) {
    Rig rig;
    rig.path = path;
    const std::string file = std::filesystem::path(path).filename().string();
    cgltf_options opt{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opt, path.c_str(), &data) != cgltf_result_success) {
        rig.error = "Could not read " + file + ".";
        return rig;
    }
    if (cgltf_load_buffers(&opt, data, path.c_str()) != cgltf_result_success) {
        rig.error = "The data of " + file + " is missing (.bin next to it?).";
        cgltf_free(data);
        return rig;
    }
    const std::size_t N = data->nodes_count;
    rig.nodes.resize(N);
    for (std::size_t i = 0; i < N; ++i) {
        const cgltf_node& cn = data->nodes[i];
        Node& n = rig.nodes[i];
        n.name = cn.name ? cn.name : ("node " + std::to_string(i));
        n.parent = cn.parent ? static_cast<int>(cn.parent - data->nodes) : -1;
        if (cn.has_matrix) {
            glm::vec3 skew; glm::vec4 persp;
            glm::decompose(glm::make_mat4(cn.matrix), n.s, n.r, n.t, skew, persp);
        } else {
            if (cn.has_translation) n.t = glm::vec3(cn.translation[0], cn.translation[1], cn.translation[2]);
            if (cn.has_rotation) n.r = glm::quat(cn.rotation[3], cn.rotation[0], cn.rotation[1], cn.rotation[2]);
            if (cn.has_scale) n.s = glm::vec3(cn.scale[0], cn.scale[1], cn.scale[2]);
        }
    }
    // Parents before children.
    std::vector<char> seen(N, 0);
    std::function<void(int)> visit = [&](int i) {
        if (seen[static_cast<std::size_t>(i)]) return;
        const int p = rig.nodes[static_cast<std::size_t>(i)].parent;
        if (p >= 0) visit(p);
        if (seen[static_cast<std::size_t>(i)]) return;
        seen[static_cast<std::size_t>(i)] = 1;
        rig.order.push_back(i);
    };
    for (std::size_t i = 0; i < N; ++i) visit(static_cast<int>(i));

    std::vector<glm::mat4> rest;
    rig.sample(-1, 0.0f, rest);
    for (std::size_t i = 0; i < N; ++i) rig.nodes[i].bind = rest[i];

    if (data->skins_count > 0) {
        const cgltf_skin& sk = data->skins[0];
        for (cgltf_size j = 0; j < sk.joints_count; ++j) {
            const int ni = static_cast<int>(sk.joints[j] - data->nodes);
            rig.skinJoints.push_back(ni);
            Node& n = rig.nodes[static_cast<std::size_t>(ni)];
            n.joint = true;
            if (sk.inverse_bind_matrices) {
                float m[16];
                cgltf_accessor_read_float(sk.inverse_bind_matrices, j, m, 16);
                n.bind = glm::inverse(glm::make_mat4(m));
            }
        }
    } else {
        // An armature without a mesh (a BVH, say): every node that carries
        // nothing else is a bone.
        for (std::size_t i = 0; i < N; ++i) {
            const cgltf_node& cn = data->nodes[i];
            rig.nodes[i].joint = !cn.mesh && !cn.camera && !cn.light;
        }
    }

    if (withMotions) {
        for (cgltf_size a = 0; a < data->animations_count; ++a) {
            const cgltf_animation& an = data->animations[a];
            Motion m;
            m.name = an.name && *an.name ? an.name : ("Take " + std::to_string(a + 1));
            std::size_t maxKeys = 0;
            for (cgltf_size c = 0; c < an.channels_count; ++c) {
                const cgltf_animation_channel& ch = an.channels[c];
                if (!ch.target_node || !ch.sampler) continue;
                int path = -1;
                if (ch.target_path == cgltf_animation_path_type_translation) path = 0;
                else if (ch.target_path == cgltf_animation_path_type_rotation) path = 1;
                else if (ch.target_path == cgltf_animation_path_type_scale) path = 2;
                if (path < 0) continue;
                const cgltf_animation_sampler& s = *ch.sampler;
                Motion::Channel mc;
                mc.node = static_cast<int>(ch.target_node - data->nodes);
                mc.path = path;
                mc.interp = s.interpolation == cgltf_interpolation_type_step           ? 1
                            : s.interpolation == cgltf_interpolation_type_cubic_spline ? 2
                                                                                       : 0;
                mc.times.resize(s.input->count);
                for (cgltf_size k = 0; k < s.input->count; ++k)
                    cgltf_accessor_read_float(s.input, k, &mc.times[k], 1);
                const int comps = path == 1 ? 4 : 3;
                mc.values.resize(s.output->count, glm::vec4(0.0f));
                for (cgltf_size k = 0; k < s.output->count; ++k) {
                    float v[4] = {0, 0, 0, 0};
                    cgltf_accessor_read_float(s.output, k, v, static_cast<cgltf_size>(comps));
                    mc.values[k] = glm::vec4(v[0], v[1], v[2], v[3]);
                }
                const std::size_t need = mc.times.size() * (mc.interp == 2 ? 3u : 1u);
                if (mc.times.empty() || mc.values.size() < need) continue;
                if (!mc.times.empty()) m.duration = std::max(m.duration, mc.times.back());
                maxKeys = std::max(maxKeys, mc.times.size());
                m.channels.push_back(std::move(mc));
            }
            if (m.channels.empty()) continue;
            m.fps = (maxKeys > 1 && m.duration > 1e-4f)
                        ? snapFps(static_cast<float>(maxKeys - 1) / m.duration)
                        : 30.0f;
            rig.motions.push_back(std::move(m));
        }
    }
    cgltf_free(data);
    if (rig.jointCount() == 0) rig.error = file + " has no skeleton.";
    return rig;
}

// --- The humanoid template ---------------------------------------------------------

namespace {

const SlotInfo kSlots[SlotCount] = {
    {"hips", "Hips", GBody, -1},
    {"pelvis", "Pelvis", GBody, Hips},
    {"spine", "Spine", GBody, Hips},
    {"chest", "Chest", GBody, Spine},
    {"upperChest", "Upper chest", GBody, Chest},
    {"neck", "Neck", GBody, UpperChest},
    {"upperNeck", "Upper neck", GBody, Neck},
    {"head", "Head", GBody, UpperNeck},
    {"leftShoulder", "Shoulder", GLeftArm, UpperChest},
    {"leftUpperArm", "Upper arm", GLeftArm, LShoulder},
    {"leftLowerArm", "Forearm", GLeftArm, LUpperArm},
    {"leftHand", "Hand", GLeftArm, LLowerArm},
    {"rightShoulder", "Shoulder", GRightArm, UpperChest},
    {"rightUpperArm", "Upper arm", GRightArm, RShoulder},
    {"rightLowerArm", "Forearm", GRightArm, RUpperArm},
    {"rightHand", "Hand", GRightArm, RLowerArm},
    {"leftUpperLeg", "Thigh", GLeftLeg, Pelvis},
    {"leftLowerLeg", "Shin", GLeftLeg, LUpperLeg},
    {"leftFoot", "Foot", GLeftLeg, LLowerLeg},
    {"leftToes", "Toes", GLeftLeg, LFoot},
    {"rightUpperLeg", "Thigh", GRightLeg, Pelvis},
    {"rightLowerLeg", "Shin", GRightLeg, RUpperLeg},
    {"rightFoot", "Foot", GRightLeg, RLowerLeg},
    {"rightToes", "Toes", GRightLeg, RFoot},
    {"leftThumb1", "Thumb 1", GLeftHand, LHand},
    {"leftThumb2", "Thumb 2", GLeftHand, LThumb1},
    {"leftThumb3", "Thumb 3", GLeftHand, LThumb2},
    {"leftIndex1", "Index 1", GLeftHand, LHand},
    {"leftIndex2", "Index 2", GLeftHand, LIndex1},
    {"leftIndex3", "Index 3", GLeftHand, LIndex2},
    {"leftMiddle1", "Middle 1", GLeftHand, LHand},
    {"leftMiddle2", "Middle 2", GLeftHand, LMiddle1},
    {"leftMiddle3", "Middle 3", GLeftHand, LMiddle2},
    {"leftRing1", "Ring 1", GLeftHand, LHand},
    {"leftRing2", "Ring 2", GLeftHand, LRing1},
    {"leftRing3", "Ring 3", GLeftHand, LRing2},
    {"leftLittle1", "Little 1", GLeftHand, LHand},
    {"leftLittle2", "Little 2", GLeftHand, LLittle1},
    {"leftLittle3", "Little 3", GLeftHand, LLittle2},
    {"rightThumb1", "Thumb 1", GRightHand, RHand},
    {"rightThumb2", "Thumb 2", GRightHand, RThumb1},
    {"rightThumb3", "Thumb 3", GRightHand, RThumb2},
    {"rightIndex1", "Index 1", GRightHand, RHand},
    {"rightIndex2", "Index 2", GRightHand, RIndex1},
    {"rightIndex3", "Index 3", GRightHand, RIndex2},
    {"rightMiddle1", "Middle 1", GRightHand, RHand},
    {"rightMiddle2", "Middle 2", GRightHand, RMiddle1},
    {"rightMiddle3", "Middle 3", GRightHand, RMiddle2},
    {"rightRing1", "Ring 1", GRightHand, RHand},
    {"rightRing2", "Ring 2", GRightHand, RRing1},
    {"rightRing3", "Ring 3", GRightHand, RRing2},
    {"rightLittle1", "Little 1", GRightHand, RHand},
    {"rightLittle2", "Little 2", GRightHand, RLittle1},
    {"rightLittle3", "Little 3", GRightHand, RLittle2},
};

} // namespace

const SlotInfo& slotInfo(int slot) { return kSlots[std::clamp(slot, 0, SlotCount - 1)]; }

const char* groupName(int group) {
    static const char* kNames[GroupCount] = {"Body", "Left arm", "Right arm", "Left leg",
                                             "Right leg", "Left hand", "Right hand"};
    return kNames[std::clamp(group, 0, GroupCount - 1)];
}

int slotByKey(const std::string& key) {
    for (int s = 0; s < SlotCount; ++s)
        if (key == kSlots[s].key) return s;
    return -1;
}

BoneMap emptyMap() {
    BoneMap m;
    m.fill(-1);
    return m;
}

int mappedCount(const BoneMap& m) {
    int n = 0;
    for (int v : m) n += v >= 0 ? 1 : 0;
    return n;
}

std::map<std::string, std::string> mapToNames(const Rig& rig, const BoneMap& m) {
    std::map<std::string, std::string> out;
    for (int s = 0; s < SlotCount; ++s) {
        const int n = m[static_cast<std::size_t>(s)];
        if (n >= 0 && n < static_cast<int>(rig.nodes.size()))
            out[kSlots[s].key] = rig.nodes[static_cast<std::size_t>(n)].name;
    }
    return out;
}

void applyNames(const Rig& rig, const std::map<std::string, std::string>& names, BoneMap& m) {
    for (const auto& [key, name] : names) {
        const int s = slotByKey(key);
        if (s < 0) continue;
        // "" is a choice too: this slot stays empty. A bone this rig does not
        // have leaves the slot as it was.
        const int n = name.empty() ? -1 : rig.find(name);
        if (!name.empty() && n < 0) continue;
        m[static_cast<std::size_t>(s)] = n;
    }
}
// --- Filling the template ------------------------------------------------------------

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

// A bone name taken apart: which side it is on and what is left of it, in
// lower case without separators -- "CC_Base_L_Upperarm", "mixamorig:LeftArm",
// "lShldrBend", "upperarm_l" and "DEF-upper_arm.L" all become (left, ...).
struct BoneName {
    int         side = 0;   // 0 middle, 1 left, 2 right
    std::string base;
};

BoneName parseName(const std::string& raw) {
    std::string s = raw;
    for (char sep : {':', '|'}) {
        const std::size_t at = s.find_last_of(sep);
        if (at != std::string::npos) s = s.substr(at + 1);
    }
    static const char* kPrefixes[] = {"cc_base_", "bip001_", "bip01_", "bip001 ", "bip01 ", "bip001",
                                      "bip01", "def-", "org-", "mch-", "j_bip_", "mixamorig"};
    for (bool again = true; again;) {
        again = false;
        const std::string low = lower(s);
        for (const char* p : kPrefixes)
            if (low.rfind(p, 0) == 0 && low.size() > std::strlen(p)) {
                s = s.substr(std::strlen(p));
                again = true;
                break;
            }
    }
    // Words: split at separators, at lower->Upper, at the end of a run of
    // capitals ("LHip" -> L, Hip) and between letters and digits.
    std::vector<std::string> words;
    std::string cur;
    auto flush = [&] { if (!cur.empty()) words.push_back(lower(cur)); cur.clear(); };
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (!std::isalnum(c)) { flush(); continue; }
        if (!cur.empty()) {
            const unsigned char p = static_cast<unsigned char>(cur.back());
            const bool nextLower = i + 1 < s.size() && std::islower(static_cast<unsigned char>(s[i + 1]));
            if ((std::islower(p) && std::isupper(c)) ||
                (std::isupper(p) && std::isupper(c) && nextLower) ||
                (std::isdigit(p) != 0) != (std::isdigit(c) != 0))
                flush();
        }
        cur.push_back(static_cast<char>(c));
    }
    flush();
    BoneName out;
    std::vector<std::string> keep;
    for (std::size_t i = 0; i < words.size(); ++i) {
        std::string w = words[i];
        if (w == "l" || w == "left") { out.side = 1; continue; }
        if (w == "r" || w == "right") { out.side = 2; continue; }
        if (w == "c" && words.size() > 1) continue;            // VRM's centre marker
        if (w.size() > 4 && w.rfind("left", 0) == 0) { out.side = 1; w = w.substr(4); }
        else if (w.size() > 5 && w.rfind("right", 0) == 0) { out.side = 2; w = w.substr(5); }
        keep.push_back(w);
    }
    for (const std::string& w : keep) out.base += w;
    return out;
}

bool isTwist(const std::string& base) {
    return has(base, "twist") || has(base, "roll") || has(base, "share");
}
bool isEnd(const std::string& base) {
    return has(base, "end") || has(base, "nub") || has(base, "tip") || has(base, "null") ||
           has(base, "top");
}

struct Tree {
    const Rig&              rig;
    std::vector<BoneName>   names;
    std::vector<int>        depth;
    std::vector<std::vector<int>> kids;

    explicit Tree(const Rig& r) : rig(r) {
        const std::size_t N = r.nodes.size();
        names.resize(N);
        depth.assign(N, 0);
        kids.resize(N);
        for (std::size_t i = 0; i < N; ++i) names[i] = parseName(r.nodes[i].name);
        for (int i : r.order) {
            const int p = r.nodes[static_cast<std::size_t>(i)].parent;
            if (p >= 0) {
                depth[static_cast<std::size_t>(i)] = depth[static_cast<std::size_t>(p)] + 1;
                kids[static_cast<std::size_t>(p)].push_back(i);
            }
        }
    }
    bool joint(int i) const { return rig.nodes[static_cast<std::size_t>(i)].joint; }
    int  parent(int i) const { return rig.nodes[static_cast<std::size_t>(i)].parent; }
    bool above(int a, int b) const {   // a is a strict ancestor of b
        for (int p = parent(b); p >= 0; p = parent(p))
            if (p == a) return true;
        return false;
    }
    int lca(int a, int b) const {
        if (a < 0) return b;
        if (b < 0) return a;
        while (depth[static_cast<std::size_t>(a)] > depth[static_cast<std::size_t>(b)]) a = parent(a);
        while (depth[static_cast<std::size_t>(b)] > depth[static_cast<std::size_t>(a)]) b = parent(b);
        while (a != b && a >= 0 && b >= 0) { a = parent(a); b = parent(b); }
        return a;
    }
    // The shallowest joint (first in file order on a tie) passing `ok`.
    int best(const std::function<bool(int)>& ok, int under = -1) const {
        int found = -1;
        for (int i : rig.order) {
            if (!joint(i) || !ok(i)) continue;
            if (under >= 0 && !above(under, i)) continue;
            if (found < 0 || depth[static_cast<std::size_t>(i)] < depth[static_cast<std::size_t>(found)])
                found = i;
        }
        return found;
    }
    // A bone found by its name: one of `exact`, else one containing `part`
    // (and no `not` word), on `side`.
    int byName(int side, std::initializer_list<const char*> exact, const char* part,
               std::initializer_list<const char*> notWords = {}) const {
        int i = best([&](int n) {
            const BoneName& b = names[static_cast<std::size_t>(n)];
            if (b.side != side) return false;
            for (const char* e : exact)
                if (b.base == e) return true;
            return false;
        });
        if (i >= 0 || !part) return i;
        return best([&](int n) {
            const BoneName& b = names[static_cast<std::size_t>(n)];
            if (b.side != side || !has(b.base, part) || isTwist(b.base) || isEnd(b.base)) return false;
            for (const char* w : notWords)
                if (has(b.base, w)) return false;
            return true;
        });
    }
    // The bones strictly between `top` and `bottom`, from the top down.
    std::vector<int> between(int top, int bottom) const {
        std::vector<int> path;
        if (top < 0 || bottom < 0 || !above(top, bottom)) return path;
        for (int p = parent(bottom); p >= 0 && p != top; p = parent(p)) path.push_back(p);
        std::reverse(path.begin(), path.end());
        return path;
    }
};

} // namespace

BoneMap autoMap(const Rig& rig) {
    BoneMap m = emptyMap();
    if (rig.nodes.empty()) return m;
    const Tree tr(rig);
    auto set = [&](int slot, int node) { if (node >= 0) m[static_cast<std::size_t>(slot)] = node; };

    // Anchors, by name.
    const int head = tr.byName(0, {"head"}, "head", {"neck"});
    int arm[3], hand[3], thigh[3], foot[3];
    for (int side = 1; side <= 2; ++side) {
        hand[side]  = tr.byName(side, {"hand", "wrist"}, "hand",
                                {"thumb", "index", "middle", "mid", "ring", "pinky", "little", "finger", "carpal"});
        arm[side]   = tr.byName(side, {"upperarm", "arm", "shldrbend", "shoulderbend", "uparm", "humerus", "shldr"},
                                "upperarm");
        thigh[side] = tr.byName(side, {"thigh", "thighbend", "upleg", "upperleg", "femur"}, "thigh");
        if (thigh[side] < 0) thigh[side] = tr.byName(side, {}, "upleg");
        foot[side]  = tr.byName(side, {"foot", "ankle"}, "foot", {"toe", "ik"});
    }

    // The hips: where the legs and the spine meet.
    int hips = tr.lca(tr.lca(thigh[1], thigh[2]), head);
    if (hips >= 0 && !tr.joint(hips)) hips = -1;
    if (hips < 0) hips = tr.byName(0, {"hips", "hip", "pelvis"}, nullptr);
    set(Hips, hips);
    const int legRoot = tr.lca(thigh[1], thigh[2]);
    if (legRoot >= 0 && legRoot != hips && hips >= 0 && tr.above(hips, legRoot) && tr.joint(legRoot))
        set(Pelvis, legRoot);

    // The spine: whatever lies between the hips and the head. The upper chest
    // is the one the arms hang from, the neck what comes after it.
    if (hips >= 0 && head >= 0) {
        const std::vector<int> path = tr.between(hips, head);
        int u = -1;
        for (int i = static_cast<int>(path.size()) - 1; i >= 0 && u < 0; --i)
            for (int side = 1; side <= 2; ++side) {
                const int a = arm[side] >= 0 ? arm[side] : hand[side];
                if (a >= 0 && tr.above(path[static_cast<std::size_t>(i)], a)) { u = i; break; }
            }
        if (u < 0)   // no arms to go by: the last bone before anything called neck
            for (int i = 0; i < static_cast<int>(path.size()); ++i)
                if (!has(tr.names[static_cast<std::size_t>(path[static_cast<std::size_t>(i)])].base, "neck")) u = i;
        if (u >= 0) {
            set(UpperChest, path[static_cast<std::size_t>(u)]);
            if (u >= 1) set(Spine, path[0]);
            if (u >= 2) set(Chest, path[static_cast<std::size_t>(1 + (u - 2) / 2)]);
            const int rest = static_cast<int>(path.size()) - u - 1;
            if (rest >= 1) set(Neck, path[static_cast<std::size_t>(u + 1)]);
            if (rest >= 2) set(UpperNeck, path.back());
        } else if (!path.empty()) {
            set(Neck, path.back());
        }
        set(Head, head);
    } else {
        set(Head, head);
    }

    // Arms and legs: the bones between the named ones.
    auto firstSolid = [&](const std::vector<int>& path) {
        for (int n : path)
            if (!isTwist(tr.names[static_cast<std::size_t>(n)].base)) return n;
        return -1;
    };
    const int spineSlots[] = {Hips, Spine, Chest, UpperChest, Neck, UpperNeck};
    for (int side = 1; side <= 2; ++side) {
        const bool L = side == 1;
        if (arm[side] >= 0) {
            set(L ? LUpperArm : RUpperArm, arm[side]);
            const int p = tr.parent(arm[side]);
            bool onSpine = false;
            for (int s : spineSlots) onSpine = onSpine || m[static_cast<std::size_t>(s)] == p;
            if (p >= 0 && tr.joint(p) && !onSpine) set(L ? LShoulder : RShoulder, p);
            if (hand[side] >= 0) set(L ? LLowerArm : RLowerArm, firstSolid(tr.between(arm[side], hand[side])));
        }
        set(L ? LHand : RHand, hand[side]);
        if (thigh[side] >= 0) {
            set(L ? LUpperLeg : RUpperLeg, thigh[side]);
            if (foot[side] >= 0) set(L ? LLowerLeg : RLowerLeg, firstSolid(tr.between(thigh[side], foot[side])));
        }
        if (foot[side] >= 0) {
            set(L ? LFoot : RFoot, foot[side]);
            int toe = tr.best([&](int n) {
                const std::string& b = tr.names[static_cast<std::size_t>(n)].base;
                return (has(b, "toe") || b.rfind("ball", 0) == 0) && !isEnd(b) && !isTwist(b);
            }, foot[side]);
            if (toe < 0)
                for (int k : tr.kids[static_cast<std::size_t>(foot[side])])
                    if (tr.joint(k)) { toe = k; break; }
            set(L ? LToes : RToes, toe);
        }
        // Fingers: three bones down each chain, found by name under the hand.
        if (hand[side] >= 0) {
            struct F { int first; std::initializer_list<const char*> words; };
            const F fingers[] = {
                {L ? LThumb1 : RThumb1, {"thumb"}},
                {L ? LIndex1 : RIndex1, {"index", "pointer"}},
                {L ? LMiddle1 : RMiddle1, {"middle", "mid"}},
                {L ? LRing1 : RRing1, {"ring"}},
                {L ? LLittle1 : RLittle1, {"pinky", "little", "pinkie"}},
            };
            for (const F& f : fingers) {
                auto isF = [&](int n) {
                    const std::string& b = tr.names[static_cast<std::size_t>(n)].base;
                    if (isEnd(b) || isTwist(b) || has(b, "metacarpal")) return false;
                    for (const char* w : f.words)
                        if (has(b, w)) return true;
                    return false;
                };
                int under = hand[side];
                for (int k = 0; k < 3; ++k) {
                    const int n = tr.best(isF, under);
                    if (n < 0) break;
                    set(f.first + k, n);
                    under = n;
                }
            }
        }
    }
    return m;
}

} // namespace retarget