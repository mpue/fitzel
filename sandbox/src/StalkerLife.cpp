#include "StalkerLife.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "StalkerShared.hpp"
#include "XrArchive.hpp"
#include "XrLevel.hpp"

// Only the declarations: the engine's miniaudio unit compiles stb_vorbis.
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
#undef STB_VORBIS_HEADER_ONLY

namespace stalker {

using namespace detail;

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool has(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }
bool starts(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
std::string slashes(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return lower(s);
}
std::string flat(const std::string& s) {
    std::string o;
    for (char c : s) o.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_');
    return o;
}
glm::vec3 mirror(const glm::vec3& v) { return {v.x, v.y, -v.z}; }
glm::mat3 mirror(const glm::mat3& m) {
    const glm::mat3 M(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f);
    return M * m * M;
}
json v3(const glm::vec3& v) { return json::array({v.x, v.y, v.z}); }

// --- Lua text -----------------------------------------------------------------

std::string luaStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(static_cast<char>(c)); }
        else if (c == '\n') o += "\\n";
        else if (c < 32 || c >= 127) { char b[8]; std::snprintf(b, sizeof(b), "\\%03d", c); o += b; }
        else o.push_back(static_cast<char>(c));
    }
    return o + "\"";
}
std::string num(float f, int decimals = 2) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.*f", decimals, static_cast<double>(f));
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}
std::string luaList(const std::vector<std::string>& v) {
    std::string o = "{";
    for (std::size_t i = 0; i < v.size(); ++i) o += (i ? "," : "") + luaStr(v[i]);
    return o + "}";
}

// --- sounds ---------------------------------------------------------------------

// The game's sounds, copied into the project as they are asked for. A game
// name ("anomaly\electra_idle1") is a file under sounds/, or under one of the
// localisations for the voices; it lands in sounds/stalker/ flattened
// ("xr_anomaly_electra_idle1.ogg"), because fitzel finds a sound by its file
// name alone.
class SoundBank {
public:
    SoundBank(const xray::FileSystem& fs, const fs::path& project, Session& s, LifeReport& life)
        : fs_(fs), dir_(project / "sounds" / "stalker"), s_(s), life_(life) {
        if (s_.soundDirs.empty())
            for (const std::string& p : fs_.paths()) {
                if (!starts(p, "sounds/") || p.size() < 5 || p.compare(p.size() - 4, 4, ".ogg") != 0) continue;
                const std::size_t slash = p.rfind('/');
                s_.soundDirs[p.substr(0, slash + 1)].push_back(p.substr(slash + 1));
            }
    }

    std::string file(const std::string& gameName) {
        std::string n = slashes(gameName);
        while (!n.empty() && (n.back() == ' ' || n.back() == '\t')) n.pop_back();
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ogg") == 0) n.resize(n.size() - 4);
        if (n.empty()) return {};
        if (auto it = s_.sounds.find(n); it != s_.sounds.end()) return it->second;
        std::string found;
        for (const char* root : kRoots)
            if (fs_.exists(root + n + ".ogg")) { found = root + n + ".ogg"; break; }
        std::string out;
        if (!found.empty()) {
            out = "xr_" + flat(n) + ".ogg";
            const fs::path dst = dir_ / out;
            std::error_code ec;
            std::vector<std::uint8_t> bytes;
            if (fs_.read(found, bytes)) {
                if (!fs::exists(dst, ec) || fs::file_size(dst, ec) != bytes.size()) {
                    fs::create_directories(dir_, ec);
                    std::ofstream f(dst, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    if (!f) out.clear();
                }
                if (!out.empty()) ++life_.soundFiles;
            } else {
                out.clear();
            }
        }
        s_.sounds[n] = out;
        return out;
    }

    // A prefix the game picks a random file of ("monsters\dog\bdog_idle_" ->
    // bdog_idle_0 .. _4): all of them, up to `limit`.
    std::vector<std::string> prefix(const std::string& gamePrefix, std::size_t limit = 8) {
        std::string n = slashes(gamePrefix);
        while (!n.empty() && n.back() == ' ') n.pop_back();
        if (n.empty()) return {};
        const std::size_t slash = n.rfind('/');
        const std::string dir = slash == std::string::npos ? std::string() : n.substr(0, slash + 1);
        const std::string stem = n.substr(slash == std::string::npos ? 0 : slash + 1);
        std::vector<std::string> out;
        for (const char* root : kRoots) {
            const auto it = s_.soundDirs.find(root + dir);
            if (it == s_.soundDirs.end()) continue;
            std::vector<std::string> names;
            for (const std::string& f : it->second)
                if (starts(f, stem.c_str())) names.push_back(f.substr(0, f.size() - 4));
            std::sort(names.begin(), names.end());
            for (const std::string& nm : names) {
                if (out.size() >= limit) break;
                const std::string file = this->file(dir + nm);
                if (!file.empty()) out.push_back(file);
            }
            if (!out.empty()) break;
        }
        return out;
    }

    // How long a copied sound plays, in seconds (0 when it cannot be read).
    float seconds(const std::string& file) const {
        if (file.empty()) return 0.0f;
        std::ifstream in(dir_ / file, std::ios::binary);
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), {});
        int err = 0;
        stb_vorbis* v = stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()), &err, nullptr);
        if (!v) return 0.0f;
        const float len = stb_vorbis_stream_length_in_seconds(v);
        stb_vorbis_close(v);
        return len;
    }

private:
    static constexpr const char* kRoots[] = {"sounds/", "sounds/localization/ger/",
                                            "sounds/localization/base/", "sounds/localization/eng/",
                                            "sounds/localization/rus/"};
    const xray::FileSystem& fs_;
    fs::path dir_;
    Session& s_;
    LifeReport& life_;
};

// --- figures ----------------------------------------------------------------------

// A clip of a figure: the first motion of `body` the libraries have; for
// people the legs come from it and the rest of the body from `upper` (X-Ray
// plays a walk as legs, torso and head motions at once).
struct ClipDef {
    const char* name;
    std::vector<const char*> body;
    std::vector<const char*> upper;
    bool loop;
};

const std::vector<ClipDef>& humanClips() {
    static const std::vector<ClipDef> c = {
        {"idle", {"idle_0_idle_1", "norm_idle_0", "idle"}, {}, true},
        {"walk", {"norm_walk_fwd_0", "norm_walk_fwd_1"}, {"norm_torso_0_walk_0", "norm_torso_0_walk_1"}, true},
        {"run", {"norm_run_fwd_0", "norm_run_fwd_1"}, {"norm_torso_0_run_0", "norm_torso_0_run_1"}, true},
        {"sit", {"sit_2_idle_1", "sit_2_idle_0"}, {}, true},
        {"kneel", {"sit_1_idle_1"}, {}, true},
        {"guitar", {"guitar_0"}, {}, true},
        {"harmonica", {"harmonica_0"}, {}, true},
        {"sleep", {"sleep_idle_0"}, {}, true},
        {"hello", {"hello_0_idle_0"}, {}, false},
        {"death", {"norm_death_0", "death_init"}, {}, false},
    };
    return c;
}
const std::vector<ClipDef>& traderClips() {
    static const std::vector<ClipDef> c = {
        {"idle", {"idle", "idle_0", "idle_looped"}, {}, true},
        {"hello", {"talk_0", "talk_1"}, {}, false},
        {"death", {"stand_die_0", "death_init"}, {}, false},
    };
    return c;
}
const std::vector<ClipDef>& mutantClips() {
    static const std::vector<ClipDef> c = {
        {"idle", {"stand_idle_0", "idle", "norm_idle_0", "norm_idle", "new_idle_0", "_stand_idle_0", "sit_idle_0"}, {}, true},
        {"walk", {"stand_walk_fwd_0", "new_walk_0", "_stand_walk_fwd_1", "norm_fly_fwd", "stand_walk_fwd!"}, {}, true},
        {"run", {"stand_run_0", "stand_run_fwd_0", "run_fwd_0", "new_run_fwd_0", "_stand_run_0", "norm_fly_fwd",
                 "stand_walk_fwd_0"}, {}, true},
        {"attack", {"stand_attack_0", "attack_run_0", "control_attack_0", "stand_run_attack_0", "attack 1a"}, {}, true},
        {"eat", {"stand_eat_0", "sit_eat_0", "lie_eat_0", "eat_idle_0n"}, {}, true},
        {"rest", {"lie_idle_0", "sit_idle_0", "sleep_idle_0"}, {}, true},
        {"threaten", {"stand_threaten_0"}, {}, true},
        {"death", {"stand_die_0", "norm_death_0", "death_init"}, {}, false},
    };
    return c;
}

// The animation graph state of each clip: what the scripts set as `mode`.
const std::vector<std::pair<const char*, const char*>>& graphStates(const std::string& family) {
    static const std::vector<std::pair<const char*, const char*>> human = {
        {"idle", "idle"}, {"walk", "walk"}, {"run", "run"}, {"sit", "sit"}, {"kneel", "kneel"},
        {"guitar", "guitar"}, {"harmonica", "harmonica"}, {"sleep", "sleep"}, {"hello", "hello"},
        {"dead", "death"}};
    static const std::vector<std::pair<const char*, const char*>> mutant = {
        {"idle", "idle"}, {"walk", "walk"}, {"run", "run"}, {"attack", "attack"}, {"eat", "eat"},
        {"rest", "rest"}, {"threaten", "threaten"}, {"unused7", ""}, {"unused8", ""}, {"dead", "death"}};
    return family == "mutant" ? mutant : human;
}

json graphJson(const std::string& name, const std::string& family) {
    const auto& states = graphStates(family);
    json ss = json::array(), ts = json::array();
    for (std::size_t i = 0; i < states.size(); ++i) {
        const std::string st = states[i].first;
        const bool once = st == "dead" || st == "hello";
        const bool moves = st == "walk" || st == "run";
        ss.push_back({{"name", st}, {"clip", ""}, {"modelClip", states[i].second}, {"loop", !once},
                      {"speed", 1.0}, {"speedParam", moves ? "speed" : ""},
                      {"x", 200 * static_cast<int>(i % 5)}, {"y", 150 * static_cast<int>(i / 5)}});
        ts.push_back({{"from", -1}, {"to", i}, {"exit", false}, {"exitTime", 1.0},
                      {"fade", st == "dead" ? 0.15 : 0.3}, {"curve", "smooth"}, {"offset", 0.0},
                      {"conds", json::array({{{"param", "mode"}, {"op", "=="}, {"value", static_cast<float>(i)}}})}});
    }
    return {{"name", name}, {"entry", 0},
            {"params", json::array({{{"name", "mode"}, {"type", "Number"}, {"def", 0.0}},
                                    {{"name", "speed"}, {"type", "Number"}, {"def", 1.0}}})},
            {"states", ss}, {"transitions", ts}};
}

struct Clip {
    std::string name;
    bool  loop = true;
    int   frames = 0;
    float fps = xray::kMotionFps;
    std::vector<xray::BoneTrack> tracks;   // per bone; empty = rest pose
};

glm::quat keyAt(const std::vector<glm::quat>& k, float f) {
    if (k.size() == 1) return k[0];
    const float c = std::clamp(f, 0.0f, static_cast<float>(k.size() - 1));
    const std::size_t i = static_cast<std::size_t>(c);
    const std::size_t j = std::min(i + 1, k.size() - 1);
    glm::quat b = k[j];
    if (glm::dot(k[i], b) < 0.0f) b = -b;
    return glm::normalize(glm::slerp(k[i], b, c - static_cast<float>(i)));
}
glm::vec3 keyAt(const std::vector<glm::vec3>& k, float f) {
    if (k.size() == 1) return k[0];
    const float c = std::clamp(f, 0.0f, static_cast<float>(k.size() - 1));
    const std::size_t i = static_cast<std::size_t>(c);
    const std::size_t j = std::min(i + 1, k.size() - 1);
    return glm::mix(k[i], k[j], c - static_cast<float>(i));
}

// A track of `from` frames played over `to` frames instead (the torso of a
// walk whose legs take longer).
xray::BoneTrack stretch(const xray::BoneTrack& t, int from, int to) {
    if (from == to) return t;
    xray::BoneTrack o;
    auto at = [&](int k) { return to > 1 ? static_cast<float>(k) * static_cast<float>(from - 1) / static_cast<float>(to - 1) : 0.0f; };
    if (t.rotation.size() == 1) o.rotation = t.rotation;
    else for (int k = 0; k < to; ++k) o.rotation.push_back(keyAt(t.rotation, at(k)));
    if (t.offset.size() == 1) o.offset = t.offset;
    else for (int k = 0; k < to; ++k) o.offset.push_back(keyAt(t.offset, at(k)));
    return o;
}

const xray::MotionLibrary* motionLibrary(const xray::FileSystem& fs, Session& s, const std::string& ref,
                                         std::vector<std::string>& warnings) {
    auto it = s.motions.find(ref);
    if (it == s.motions.end()) {
        xray::MotionLibrary lib;
        std::string e;
        if (!xray::readMotionLibrary(fs, ref, lib, e)) warnings.push_back(e);
        it = s.motions.emplace(ref, std::move(lib)).first;
    }
    return it->second.empty() ? nullptr : &it->second;
}

bool buildClip(const ClipDef& def, const xray::Character& ch, const std::vector<const xray::MotionLibrary*>& libs,
               Clip& out, std::string& error) {
    // The first candidate the libraries have and that decodes (a motion or two
    // in the game's files are cut short).
    auto decodeFirst = [&](const std::vector<const char*>& names, xray::Motion& m)
        -> const xray::MotionLibrary* {
        for (const char* n : names)
            for (const xray::MotionLibrary* l : libs) {
                std::string e;
                if (!l->has(n)) continue;
                if (l->decode(n, ch.bones, m, e)) return l;
                error = e;
            }
        return nullptr;
    };
    xray::Motion mb, mu;
    const xray::MotionLibrary* bodyLib = decodeFirst(def.body, mb);
    if (!bodyLib) return false;
    error.clear();
    const bool split = !def.upper.empty() && decodeFirst(def.upper, mu);
    out = Clip{};
    out.name = def.name;
    out.loop = def.loop && mb.loops;
    out.frames = mb.frames;
    out.fps = xray::kMotionFps * mb.speed;
    out.tracks.resize(ch.bones.size());
    for (std::size_t b = 0; b < ch.bones.size(); ++b) {
        const bool legs = bodyLib->partOf(ch.bones[b].name) == "legs";
        if (split && !legs && !mu.tracks[b].rotation.empty())
            out.tracks[b] = stretch(mu.tracks[b], mu.frames, mb.frames);
        else
            out.tracks[b] = mb.tracks[b];
    }
    return true;
}

// The GLB of a figure: one mesh (a primitive per textured part) skinned to
// the skeleton's joints, and its clips. All mirrored into fitzel's axes: a
// point (x, y, z) to (x, y, -z), a rotation R to M R M, and X-Ray's key
// quaternion (the transpose of the textbook rotation) to (x, y, -z, w).
bool writeFigure(const fs::path& file, const std::string& name, const xray::Character& ch,
                 const std::vector<Clip>& clips, const std::vector<std::pair<std::string, bool>>& textures,
                 Figure& fig) {
    Glb glb;
    json& g = glb.gltf;
    g["samplers"] = json::array({{{"magFilter", 9729}, {"minFilter", 9987}, {"wrapS", 10497}, {"wrapT", 10497}}});
    g["images"] = json::array();
    g["textures"] = json::array();
    g["materials"] = json::array();
    std::map<std::string, int> matOf;
    json prims = json::array();
    fig.lo = glm::vec3(1e30f);
    fig.hi = glm::vec3(-1e30f);
    for (std::size_t p = 0; p < ch.parts.size(); ++p) {
        const xray::SkinPart& part = ch.parts[p];
        const auto& [texFile, alpha] = textures[p];
        const std::string key = texFile + (alpha ? "|a" : "");
        int mat;
        if (auto it = matOf.find(key); it != matOf.end()) {
            mat = it->second;
        } else {
            json m = {{"name", "chr_" + fs::path(texFile).stem().string() + (alpha ? "_cutout" : "")}};
            json pbr = {{"metallicFactor", 0.0}, {"roughnessFactor", 0.8}};
            if (!texFile.empty()) {
                g["images"].push_back({{"uri", "textures/" + texFile},
                                       {"mimeType", alpha ? "image/png" : "image/jpeg"}});
                g["textures"].push_back({{"sampler", 0}, {"source", g["images"].size() - 1}});
                pbr["baseColorTexture"] = {{"index", g["textures"].size() - 1}};
            }
            m["pbrMetallicRoughness"] = pbr;
            if (alpha) { m["alphaMode"] = "MASK"; m["alphaCutoff"] = 0.5; m["doubleSided"] = true; }
            g["materials"].push_back(m);
            mat = matOf[key] = static_cast<int>(g["materials"].size()) - 1;
        }
        std::vector<float> pos, nrm, uv, wts;
        std::vector<std::uint16_t> jnt;
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const xray::SkinVertex& v : part.verts) {
            const glm::vec3 P = mirror(v.pos), N = mirror(v.normal);
            pos.insert(pos.end(), {P.x, P.y, P.z});
            nrm.insert(nrm.end(), {N.x, N.y, N.z});
            uv.insert(uv.end(), {v.uv.x, v.uv.y});
            float sum = 0.0f;
            for (float w : v.weights) sum += w;
            for (int k = 0; k < 4; ++k) {
                jnt.push_back(v.weights[k] > 0.0f ? v.joints[k] : 0);
                wts.push_back(sum > 0.0f ? v.weights[k] / sum : (k == 0 ? 1.0f : 0.0f));
            }
            lo = glm::min(lo, P);
            hi = glm::max(hi, P);
        }
        std::vector<std::uint32_t> idx;
        for (std::size_t t = 0; t + 2 < part.indices.size(); t += 3)
            idx.insert(idx.end(), {part.indices[t], part.indices[t + 2], part.indices[t + 1]});
        const std::size_t nv = part.verts.size();
        const int aPos = glb.accessor(glb.view(pos.data(), pos.size() * 4, 34962), 5126, nv, "VEC3", v3(lo), v3(hi));
        const int aNrm = glb.accessor(glb.view(nrm.data(), nrm.size() * 4, 34962), 5126, nv, "VEC3");
        const int aUv = glb.accessor(glb.view(uv.data(), uv.size() * 4, 34962), 5126, nv, "VEC2");
        const int aJ = glb.accessor(glb.view(jnt.data(), jnt.size() * 2, 34962), 5123, nv, "VEC4");
        const int aW = glb.accessor(glb.view(wts.data(), wts.size() * 4, 34962), 5126, nv, "VEC4");
        const int aIdx = glb.accessor(glb.view(idx.data(), idx.size() * 4, 34963), 5125, idx.size(), "SCALAR");
        prims.push_back({{"attributes", {{"POSITION", aPos}, {"NORMAL", aNrm}, {"TEXCOORD_0", aUv},
                                         {"JOINTS_0", aJ}, {"WEIGHTS_0", aW}}},
                         {"indices", aIdx}, {"material", mat}, {"mode", 4}});
        fig.lo = glm::min(fig.lo, lo);
        fig.hi = glm::max(fig.hi, hi);
    }

    // The skeleton: node 0 is the mesh, node 1 + b bone b.
    const std::size_t nb = ch.bones.size();
    json nodes = json::array();
    nodes.push_back({{"name", name}, {"mesh", 0}, {"skin", 0}});
    std::vector<glm::mat4> global(nb);
    std::vector<float> ibm;
    json roots = json::array({0});
    for (std::size_t b = 0; b < nb; ++b) {
        const xray::Bone& bone = ch.bones[b];
        const glm::mat3 R = mirror(xray::objectRotation(-bone.restRotation));
        const glm::vec3 t = mirror(bone.restOffset);
        glm::quat q = glm::normalize(glm::quat_cast(R));
        glm::mat4 local = glm::translate(glm::mat4(1.0f), t) * glm::mat4(R);
        global[b] = bone.parent >= 0 ? global[static_cast<std::size_t>(bone.parent)] * local : local;
        const glm::mat4 inv = glm::inverse(global[b]);
        ibm.insert(ibm.end(), glm::value_ptr(inv), glm::value_ptr(inv) + 16);
        json n = {{"name", bone.name}, {"translation", v3(t)},
                  {"rotation", json::array({q.x, q.y, q.z, q.w})}};
        json kids = json::array();
        for (std::size_t c = 0; c < nb; ++c)
            if (ch.bones[c].parent == static_cast<int>(b)) kids.push_back(1 + c);
        if (!kids.empty()) n["children"] = kids;
        nodes.push_back(n);
        if (bone.parent < 0) roots.push_back(1 + b);
    }
    json joints = json::array();
    for (std::size_t b = 0; b < nb; ++b) joints.push_back(1 + b);
    const int aIbm = glb.accessor(glb.view(ibm.data(), ibm.size() * 4, 0), 5126, nb, "MAT4");
    g["skins"] = json::array({{{"joints", joints}, {"inverseBindMatrices", aIbm}, {"skeleton", roots[1]}}});
    g["meshes"] = json::array({{{"name", name}, {"primitives", prims}}});
    g["nodes"] = nodes;
    g["scenes"] = json::array({{{"nodes", roots}}});
    g["scene"] = 0;

    // The clips. A looping one gets a closing key (its first again), because
    // X-Ray wraps from the last frame to the first; fitzel loops at the end.
    json anims = json::array();
    for (const Clip& c : clips) {
        const int keys = c.frames + (c.loop && c.frames > 1 ? 1 : 0);
        std::vector<float> times(static_cast<std::size_t>(keys));
        for (int k = 0; k < keys; ++k) times[static_cast<std::size_t>(k)] = static_cast<float>(k) / c.fps;
        const float end = std::max(times.back(), 1.0f / c.fps);
        const float hold[2] = {0.0f, end};
        const int aT = glb.accessor(glb.view(times.data(), times.size() * 4, 0), 5126, times.size(), "SCALAR",
                                    json::array({0.0f}), json::array({times.back()}));
        const int aHold = glb.accessor(glb.view(hold, 8, 0), 5126, 2, "SCALAR", json::array({0.0f}), json::array({end}));
        json samplers = json::array(), channels = json::array();
        for (std::size_t b = 0; b < nb; ++b) {
            const xray::BoneTrack& t = c.tracks[b];
            if (t.rotation.empty() || t.offset.empty()) continue;
            std::vector<float> rv;
            glm::quat prev(1.0f, 0.0f, 0.0f, 0.0f);
            auto pushQ = [&](glm::quat q, bool first) {
                glm::quat m(q.w, q.x, q.y, -q.z);
                if (!first && glm::dot(prev, m) < 0.0f) m = -m;
                prev = m;
                rv.insert(rv.end(), {m.x, m.y, m.z, m.w});
            };
            const bool rConst = t.rotation.size() == 1;
            if (rConst) { pushQ(t.rotation[0], true); pushQ(t.rotation[0], false); }
            else for (int k = 0; k < keys; ++k) pushQ(t.rotation[static_cast<std::size_t>(k % c.frames)], k == 0);
            std::vector<float> tv;
            const bool tConst = t.offset.size() == 1;
            const int tn = tConst ? 2 : keys;
            for (int k = 0; k < tn; ++k) {
                const glm::vec3 o = mirror(t.offset[tConst ? 0 : static_cast<std::size_t>(k % c.frames)]);
                tv.insert(tv.end(), {o.x, o.y, o.z});
            }
            const int aR = glb.accessor(glb.view(rv.data(), rv.size() * 4, 0), 5126, rv.size() / 4, "VEC4");
            const int aTr = glb.accessor(glb.view(tv.data(), tv.size() * 4, 0), 5126, tv.size() / 3, "VEC3");
            samplers.push_back({{"input", rConst ? aHold : aT}, {"output", aR}, {"interpolation", "LINEAR"}});
            channels.push_back({{"sampler", samplers.size() - 1}, {"target", {{"node", 1 + b}, {"path", "rotation"}}}});
            samplers.push_back({{"input", tConst ? aHold : aT}, {"output", aTr}, {"interpolation", "LINEAR"}});
            channels.push_back({{"sampler", samplers.size() - 1}, {"target", {{"node", 1 + b}, {"path", "translation"}}}});
        }
        if (channels.empty()) continue;
        anims.push_back({{"name", c.name}, {"samplers", samplers}, {"channels", channels}});
        fig.clips.insert(c.name);
    }
    if (!anims.empty()) g["animations"] = anims;
    return glb.write(file);
}

// The figure of a visual, converted on first use.
const Figure& figureFor(const xray::FileSystem& fs, const Options& o, Session& s, const std::string& visual,
                        bool trader, LifeReport& life, std::vector<std::string>& warnings) {
    const std::string key = slashes(visual);
    if (auto it = s.figures.find(key); it != s.figures.end()) return it->second;
    Figure& fig = s.figures[key];
    std::string path = key;
    if (path.size() > 4 && path.compare(path.size() - 4, 4, ".ogf") == 0) path.resize(path.size() - 4);
    xray::Character ch;
    std::string e;
    if (!xray::readCharacter(fs, "meshes/" + path + ".ogf", ch, e)) {
        warnings.push_back("figure " + visual + ": " + e);
        return fig;
    }
    if (!e.empty()) warnings.push_back(e);
    std::vector<const xray::MotionLibrary*> libs;
    if (!ch.embedded.empty()) libs.push_back(&ch.embedded);
    for (const std::string& ref : ch.motionRefs)
        if (const xray::MotionLibrary* l = motionLibrary(fs, s, ref, warnings)) libs.push_back(l);
    if (libs.empty()) { warnings.push_back("figure " + visual + ": no motions"); return fig; }
    bool human = false;
    for (const xray::MotionLibrary* l : libs) human |= l->has("norm_walk_fwd_0");
    fig.family = trader ? "trader" : human ? "human" : "mutant";
    const auto& defs = trader ? traderClips() : human ? humanClips() : mutantClips();
    std::vector<Clip> clips;
    for (const ClipDef& d : defs) {
        Clip c;
        std::string ce;
        if (buildClip(d, ch, libs, c, ce)) clips.push_back(std::move(c));
        else if (!ce.empty()) warnings.push_back("figure " + visual + ", clip " + d.name + ": " + ce);
    }
    if (clips.empty()) { warnings.push_back("figure " + visual + ": none of its clips found"); return fig; }

    const fs::path dir = o.project / "models" / "characters";
    const fs::path texDir = dir / "textures";
    std::error_code ec;
    fs::create_directories(texDir, ec);
    std::vector<std::pair<std::string, bool>> textures;
    for (const xray::SkinPart& p : ch.parts) {
        const std::string sh = lower(p.shader);
        const bool alpha = has(sh, "aref") || has(sh, "transparent") || has(sh, "window");
        const std::string tex = lower(p.texture);
        std::string file = flat(slashes(tex)) + (alpha ? ".png" : ".jpg");
        const fs::path dst = texDir / file;
        if (!fs::exists(dst, ec)) {
            std::string te;
            if (!convertTexture(fs, "", tex, dst, alpha, o.maxTexture, te)) {
                warnings.push_back("figure " + visual + ": " + te);
                file.clear();
            }
        }
        textures.emplace_back(file, alpha);
    }
    std::string stem = path;
    for (const char* pre : {"actors/", "monsters/"})
        if (starts(stem, pre)) stem = stem.substr(std::strlen(pre));
    fig.file = flat(stem) + ".glb";
    const fs::path glb = dir / fig.file;
    fig.guid = modelGuid(glb);
    if (!writeFigure(glb, flat(stem), ch, clips, textures, fig)) {
        warnings.push_back("cannot write " + glb.string());
        return fig;
    }
    std::ofstream(glb.string() + ".meta") << json({{"guid", fig.guid}, {"type", "Model"}}).dump(2) << "\n";
    fig.ok = true;
    ++life.models;
    return fig;
}

// --- the level's people and places --------------------------------------------------

enum class Kind { Other, Human, Trader, Mutant, Crow, Zone, Radiation, Exit, Smart };

Kind kindOf(const std::string& cls) {
    if (cls == "AI_STL_S") return Kind::Human;
    if (cls == "AI_TRD_S") return Kind::Trader;
    if (cls == "AI_CROW") return Kind::Crow;
    if (cls == "SM_POLTR") return Kind::Other;   // an invisible ghost
    if (starts(cls, "SM_")) return Kind::Mutant;
    if (cls == "Z_RADIO" || cls == "ZS_RADIO") return Kind::Radiation;
    if (starts(cls, "Z_") || starts(cls, "ZS_")) return Kind::Zone;
    if (cls == "LVL_CHNG") return Kind::Exit;
    if (cls == "SMRTTRRN") return Kind::Smart;
    return Kind::Other;
}

// A zone or exit's shapes in the world, fitzel's axes.
struct WorldShape {
    bool box = false;
    glm::vec3 center{0.0f};
    float radius = 0.0f;
    glm::vec3 axis[3];          // unit
    float half[3] = {0, 0, 0};
};

std::vector<WorldShape> worldShapes(const xray::LevelSpawn& sp, const xray::SpawnState& st) {
    std::vector<WorldShape> out;
    const glm::mat3 R = xray::objectRotation(sp.angle);
    const glm::vec3 P = sp.position;   // fitzel's axes, lifted
    for (const xray::SpawnShape& s : st.shapes) {
        WorldShape w;
        if (!s.box) {
            w.center = P + mirror(R * s.center);
            w.radius = s.radius;
        } else {
            w.box = true;
            w.center = P + mirror(R * s.offset);
            float r2 = 0.0f;
            for (int a = 0; a < 3; ++a) {
                const glm::vec3 ax = mirror(R * s.axes[a]);
                const float len = glm::length(ax);
                w.axis[a] = len > 1e-6f ? ax / len : glm::vec3(a == 0, a == 1, a == 2);
                w.half[a] = 0.5f * len;
                r2 += w.half[a] * w.half[a];
            }
            w.radius = std::sqrt(r2);
        }
        out.push_back(w);
    }
    return out;
}

std::string luaShapes(const std::vector<WorldShape>& shapes) {
    std::string o = "{";
    for (const WorldShape& w : shapes) {
        o += "{x=" + num(w.center.x) + ",y=" + num(w.center.y) + ",z=" + num(w.center.z) + ",r=" + num(w.radius);
        if (w.box) {
            o += ",box={";
            for (int a = 0; a < 3; ++a)
                o += "{" + num(w.axis[a].x, 3) + "," + num(w.axis[a].y, 3) + "," + num(w.axis[a].z, 3) + "," +
                     num(w.half[a]) + "},";
            o += "}";
        }
        o += "},";
    }
    return o + "}";
}

std::string community(const std::string& profile, const std::string& visual, const std::string& section) {
    const std::string s = lower(profile + " " + visual + " " + section);
    if (has(s, "zombi")) return "zombied";
    if (has(s, "monolit")) return "monolith";
    if (has(s, "bandit")) return "bandit";
    if (has(s, "dolg") || has(s, "duty")) return "dolg";
    if (has(s, "freedom") || has(s, "svoboda") || has(s, "_sv_")) return "freedom";
    if (has(s, "killer") || has(s, "merc")) return "killer";
    if (has(s, "ecolog") || has(s, "scien") || has(s, "nauchn") || has(s, "ucheniy")) return "ecolog";
    if (has(s, "military") || has(s, "soldier") || has(s, "army") || has(s, "voen")) return "military";
    return "stalker";
}

// The voice folder of a community (characters_voice\human_01\<it>).
std::string voiceOf(const std::string& c) {
    if (c == "freedom") return "freedom";
    if (c == "dolg") return "dolg";
    if (c == "bandit") return "bandit";
    if (c == "military") return "military";
    if (c == "monolith") return "monolith";
    if (c == "killer") return "killer";
    if (c == "ecolog") return "ecolog";
    return "stalker";
}

std::string levelLabel(const std::string& level) {
    std::string s = lower(level);
    if (s.size() > 4 && s[0] == 'l' && std::isdigit(static_cast<unsigned char>(s[1])) &&
        std::isdigit(static_cast<unsigned char>(s[2]))) {
        const std::size_t u = s.find('_');
        if (u != std::string::npos) s = s.substr(u + 1);
    }
    for (char& c : s) if (c == '_') c = ' ';
    if (!s.empty()) s[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
    return s;
}

// What the custom data's [logic] says a person or creature does: its scheme
// ("walker", "kamp", "sleeper", "mob_home", ...) and the paths it names. A
// `cfg` points at an ltx of the game's own with the same sections.
struct Logic {
    std::string scheme, walk, look, center, home;
};

Logic logicOf(const xray::FileSystem& fs, const std::string& custom) {
    Logic L;
    xray::Ltx ltx;
    ltx.parse(custom);
    std::string cfg;
    if (ltx.find("logic", "cfg", cfg) && !cfg.empty()) {
        std::vector<std::uint8_t> bytes;
        if (fs.read("config/" + slashes(cfg), bytes)) ltx.parse(std::string(bytes.begin(), bytes.end()));
    }
    std::string active = ltx.get("logic", "active");
    if (const std::size_t brace = active.find('{'); brace != std::string::npos) active.erase(brace);
    while (!active.empty() && std::isspace(static_cast<unsigned char>(active.back()))) active.pop_back();
    if (active.empty()) return L;
    L.scheme = lower(active.substr(0, active.find('@')));
    auto path = [&](const char* key) {
        std::string v = ltx.get(active, key);
        if (const std::size_t brace = v.find('{'); brace != std::string::npos) v.erase(brace);
        while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
        return v;
    };
    L.walk = path("path_walk");
    L.look = path("path_look");
    L.center = path("center_point");
    L.home = path("path_home");
    if (L.walk.empty()) L.walk = path("path_main");
    return L;
}

struct Person {
    const xray::LevelSpawn* sp = nullptr;
    xray::SpawnState st;
    Kind kind = Kind::Other;
    const Figure* fig = nullptr;
    std::string entity;
    std::string family;          // "human", "trader", "dog", "boar", ...
    std::string community;
    std::string job;             // walker, kamp, sleep, guard, idle, wander, fly
    std::string path, look, smart;
    glm::vec3 home{0.0f};
    float yaw = 0.0f;            // degrees about fitzel's Y
};

// A mutant's kind, for the scripts' numbers (speed, bite, toughness).
std::string mutantKind(const std::string& cls, const std::string& visual) {
    static const std::map<std::string, std::string> byClass = {
        {"SM_DOG_S", "dog"}, {"SM_BOARW", "boar"}, {"SM_FLESH", "flesh"}, {"SM_P_DOG", "pseudodog"},
        {"SM_SNORK", "snork"}, {"SM_BLOOD", "bloodsucker"}, {"SM_GIANT", "gigant"}, {"SM_CONTR", "controller"},
        {"SM_TUSHK", "tushkano"}, {"SM_ZOMBI", "zombie"}, {"SM_BURER", "burer"}, {"SM_CAT_S", "cat"},
        {"SM_CHIMS", "chimera"}, {"SM_RAT", "rat"}, {"SM_IZLOM", "fracture"}};
    if (auto it = byClass.find(cls); it != byClass.end()) return it->second;
    const std::string v = lower(visual);
    for (const char* k : {"dog", "boar", "flesh", "snork", "krovosos", "gigant", "kontrol", "tushkano", "zombi", "rat"})
        if (has(v, k)) return k;
    return "mutant";
}

// The jobs of a smart terrain's people, from the names of the paths it owns
// (the game's gulag scripts hand them out by these names).
struct Gulag {
    std::vector<std::string> camps, routes, guards, sleeps;
};

Gulag gulagOf(const std::string& smart, const xray::Level& level) {
    Gulag g;
    const std::string pre = lower(smart) + "_";
    for (const xray::PatrolPath& p : level.paths) {
        const std::string n = lower(p.name);
        if (!starts(n, pre.c_str())) continue;
        if (has(n, "look")) continue;
        if (has(n, "kamp") || has(n, "camp")) g.camps.push_back(p.name);
        else if (has(n, "sleep")) g.sleeps.push_back(p.name);
        else if (has(n, "walk") && p.points.size() > 1) g.routes.push_back(p.name);
        else if (has(n, "walk") || has(n, "guard") || has(n, "post") || has(n, "point") || has(n, "stay"))
            g.guards.push_back(p.name);
    }
    return g;
}

// The particles of an anomaly, as a fitzel Particle component: what it does
// all the time, or (`blow`) the burst of a discharge, which game.emit fires.
json anomalyParticles(const std::string& kind, float radius, bool blow) {
    json p = {{"type", "particles"}, {"playing", true}, {"loop", !blow}, {"duration", blow ? 0.2 : 3.0},
              {"worldSpace", true}, {"burst", 0.0}, {"maxCount", blow ? 300.0 : 200.0}, {"shape", 1},
              {"radius", std::max(0.3f, radius * (blow ? 0.4f : 0.6f))}, {"inherit", 0.0}, {"rotation", 180.0},
              {"spin", 30.0}, {"stretch", 0.0}, {"wets", false}};
    auto set = [&](float rate, float life, float speed, glm::vec3 grav, glm::vec3 c0, glm::vec3 c1, float a0,
                   float s0, float s1, int blend, float bright) {
        p["rate"] = blow ? 0.0f : rate;
        p["burst"] = blow ? rate : 0.0f;
        p["lifetime"] = life; p["lifeVar"] = 0.4; p["speed"] = speed; p["speedVar"] = speed * 0.5f;
        p["gravity"] = v3(grav); p["drag"] = 0.8; p["colorStart"] = v3(c0); p["colorEnd"] = v3(c1);
        p["alphaStart"] = a0; p["alphaEnd"] = 0.0; p["sizeStart"] = s0; p["sizeEnd"] = s1; p["sizeVar"] = 0.3;
        p["blend"] = blend; p["brightness"] = bright; p["speedGlowMin"] = 1.0; p["speedGlowMax"] = 1.0;
    };
    const glm::vec3 none(0.0f);
    if (kind == "electra") {
        if (blow) set(90, 0.35f, 7.0f, none, {0.75f, 0.9f, 1.0f}, {0.3f, 0.5f, 1.0f}, 1.0f, 0.18f, 0.03f, 1, 5.0f);
        else set(8, 0.2f, 2.5f, none, {0.6f, 0.8f, 1.0f}, {0.2f, 0.4f, 1.0f}, 0.9f, 0.08f, 0.02f, 1, 3.0f);
    } else if (kind == "fire") {
        set(45, 0.7f, 0.5f, {0, 2.2f, 0}, {1.0f, 0.55f, 0.18f}, {0.6f, 0.1f, 0.02f}, 0.85f, 0.42f, 0.08f, 1, 2.2f);
    } else if (kind == "burner") {
        if (blow) set(160, 1.0f, 3.0f, {0, 6.0f, 0}, {1.0f, 0.6f, 0.2f}, {0.5f, 0.1f, 0.02f}, 1.0f, 0.5f, 0.2f, 1, 2.5f);
        else set(4, 1.2f, 0.3f, {0, 0.8f, 0}, {1.0f, 0.5f, 0.15f}, {0.4f, 0.1f, 0.0f}, 0.3f, 0.12f, 0.3f, 1, 1.2f);
    } else if (kind == "fuzz" || kind == "acid") {
        if (blow) set(70, 1.5f, 2.0f, {0, -0.5f, 0}, {0.6f, 0.85f, 0.3f}, {0.3f, 0.5f, 0.1f}, 0.7f, 0.2f, 0.6f, 0, 1.2f);
        else set(5, 2.0f, 0.2f, {0, -0.1f, 0}, {0.55f, 0.75f, 0.3f}, {0.3f, 0.45f, 0.15f}, 0.45f, 0.12f, 0.25f, 0, 1.0f);
    } else if (kind == "teleport") {
        set(16, 1.0f, 0.8f, {0, 0.5f, 0}, {0.6f, 0.9f, 1.0f}, {0.3f, 0.5f, 1.0f}, 0.8f, 0.12f, 0.02f, 1, 2.0f);
    } else {   // gravi, mincer, emi: dust and leaves held in the air, thrown out by a discharge
        if (blow) set(80, 1.6f, 5.0f, {0, -2.0f, 0}, {0.5f, 0.45f, 0.36f}, {0.4f, 0.35f, 0.28f}, 0.7f, 0.3f, 1.0f, 0, 1.0f);
        else set(8, 2.5f, 0.5f, {0, 0.25f, 0}, {0.45f, 0.4f, 0.32f}, {0.35f, 0.3f, 0.25f}, 0.4f, 0.22f, 0.6f, 0, 1.0f);
    }
    return p;
}

std::string zoneKind(const std::string& section, const std::string& hitType) {
    const std::string s = lower(section);
    const std::string h = lower(hitType);
    if (h == "explosion" || has(s, "mine")) return "mines";
    if (h == "burn") return has(s, "zharka") || has(s, "torrid") ? "burner" : "fire";
    if (h == "shock" || has(s, "galantine") || has(s, "electra")) return "electra";
    if (h == "chemical_burn") return has(s, "buzz") ? "acid" : "fuzz";
    if (has(s, "teleport")) return "teleport";
    if (has(s, "mincer")) return "mincer";
    if (has(s, "emi")) return "emi";
    return "gravi";
}

} // namespace

// --- the life of a level ----------------------------------------------------------

bool addLife(const xray::FileSystem& fs, const Options& o, const xray::Level& level, float lift, bool indoor,
             Session& session, json& ents, int& nextId, json& settings, Report& report, LifeReport& life,
             const Log& log) {
    auto say = [&](const std::string& s) { if (log) log(s); };
    char buf[512];
    if (!session.configLoaded) {
        const int n = session.config.loadTree(fs, "config/");
        session.configLoaded = true;
        std::snprintf(buf, sizeof(buf), "read %d config files", n);
        say(buf);
    }
    const xray::Ltx& cfg = session.config;
    SoundBank sounds(fs, o.project, session, life);
    auto prefixSounds = [&](const std::string& section, const char* key, std::size_t limit = 6) {
        std::string v;
        if (!cfg.find(section, key, v) || v.empty()) return std::vector<std::string>{};
        return sounds.prefix(xray::Ltx::list(v)[0], limit);
    };

    std::map<std::string, const xray::PatrolPath*> pathByName;
    for (const xray::PatrolPath& p : level.paths) pathByName[lower(p.name)] = &p;
    std::set<std::string> usedPaths;

    // Who and what is where.
    std::vector<Person> people;
    std::map<std::string, glm::vec3> smartCenter;
    struct Zone { const xray::LevelSpawn* sp; xray::SpawnState st; std::string kind; std::vector<WorldShape> shapes; };
    std::vector<Zone> zones;
    struct Exit { const xray::LevelSpawn* sp; xray::SpawnState st; std::vector<WorldShape> shapes; };
    std::vector<Exit> exits;
    std::map<std::string, int> nameCount;
    for (const xray::LevelSpawn& sp : level.spawns) {
        const std::string cls = cfg.get(sp.section, "class");
        const Kind kind = kindOf(cls);
        if (kind == Kind::Other) continue;
        xray::SpawnState st;
        const xray::StateLayout layout =
            kind == Kind::Human ? xray::StateLayout::Human
          : kind == Kind::Trader ? xray::StateLayout::Trader
          : kind == Kind::Exit ? xray::StateLayout::LevelChanger
          : kind == Kind::Zone || kind == Kind::Radiation ? xray::StateLayout::Zone
          : kind == Kind::Smart ? xray::StateLayout::Restrictor
          : xray::StateLayout::Object;
        if (!xray::readState(sp, layout, st)) continue;
        if (kind == Kind::Smart) {
            smartCenter[lower(sp.name)] = sp.position;
            continue;
        }
        if (kind == Kind::Zone || kind == Kind::Radiation) {
            Zone z{&sp, st, kind == Kind::Radiation ? "radiation" : zoneKind(sp.section, cfg.get(sp.section, "hit_type")),
                   worldShapes(sp, st)};
            if (!z.shapes.empty()) zones.push_back(std::move(z));
            continue;
        }
        if (kind == Kind::Exit) {
            Exit e{&sp, st, worldShapes(sp, st)};
            if (!e.shapes.empty() && !st.destLevel.empty()) exits.push_back(std::move(e));
            continue;
        }
        if (sp.section == "helicopter") continue;
        Person p;
        p.sp = &sp;
        p.st = std::move(st);
        p.kind = kind;
        if (p.st.visual.empty()) p.st.visual = cfg.get(sp.section, "visual");
        if (p.st.visual.empty()) continue;
        p.fig = &figureFor(fs, o, session, p.st.visual, kind == Kind::Trader, life, report.warnings);
        if (!p.fig->ok) continue;
        p.family = kind == Kind::Human ? "human" : kind == Kind::Trader ? "trader"
                 : kind == Kind::Crow ? "crow" : mutantKind(cls, p.st.visual);
        if (kind == Kind::Human) p.community = community(p.st.profile, p.st.visual, sp.section);
        if (kind == Kind::Trader) p.community = "trader";
        // An entity name of its own: level.spawn's names repeat now and then.
        std::string nm = sp.name.empty() ? sp.section : sp.name;
        if (const int n = nameCount[nm]++; n > 0) nm += "_" + std::to_string(n + 1);
        p.entity = nm;
        p.home = sp.position;
        p.yaw = glm::degrees(sp.angle.y);
        people.push_back(std::move(p));
    }

    // Jobs: what the logic names, else what the smart terrain's paths offer.
    std::map<std::string, std::vector<Person*>> bySmart;
    for (Person& p : people) {
        if (p.kind == Kind::Crow) { p.job = "fly"; continue; }
        if (p.kind == Kind::Trader) { p.job = "idle"; continue; }
        xray::Ltx custom;
        custom.parse(p.st.custom);
        if (const xray::Ltx::Section* st = custom.section("smart_terrains"))
            for (const auto& [name, value] : st->items)
                if (lower(value) == "true" || value.empty()) { p.smart = name; break; }
        const Logic L = logicOf(fs, p.st.custom);
        auto known = [&](const std::string& n) { return !n.empty() && pathByName.count(lower(n)); };
        if (p.kind == Kind::Mutant) {
            p.job = "wander";
            if (known(L.walk)) { p.job = "walker"; p.path = L.walk; }
            else if (known(L.home)) p.home = pathByName[lower(L.home)]->points[0].position;
            else if (auto c = smartCenter.find(p.smart); c != smartCenter.end()) p.home = c->second;
            continue;
        }
        if ((L.scheme == "kamp") && known(L.center)) { p.job = "kamp"; p.path = L.center; }
        else if (L.scheme == "sleeper" && known(L.walk)) { p.job = "sleep"; p.path = L.walk; }
        else if (known(L.walk)) {
            p.job = pathByName[lower(L.walk)]->points.size() > 1 ? "walker" : "guard";
            p.path = L.walk;
            if (known(L.look)) p.look = L.look;
        } else if (known(L.look)) { p.job = "guard"; p.look = L.look; }
        else if (!L.scheme.empty()) p.job = "idle";
        else if (!p.smart.empty()) bySmart[p.smart].push_back(&p);
        else p.job = "idle";
    }
    for (auto& [smart, members] : bySmart) {
        const Gulag g = gulagOf(smart, level);
        // Taken in turns: a camp place, a route, a camp place, a post, a bed.
        std::size_t ci = 0, ri = 0, gi = 0, si = 0;
        std::map<std::string, int> atCamp;
        auto camp = [&](Person* p) {
            if (g.camps.empty()) return false;
            const std::string& c = g.camps[ci++ % g.camps.size()];
            if (atCamp[c] >= 7) return false;
            ++atCamp[c];
            p->job = "kamp"; p->path = c;
            return true;
        };
        const char order[] = {'c', 'r', 'c', 'g', 's', 'c', 'r'};
        std::size_t turn = 0;
        for (Person* p : members) {
            bool done = false;
            for (int tries = 0; tries < 7 && !done; ++tries) {
                switch (order[turn++ % sizeof(order)]) {
                    case 'c': done = camp(p); break;
                    case 'r': if (ri < g.routes.size()) { p->job = "walker"; p->path = g.routes[ri++]; done = true; } break;
                    case 'g': if (gi < g.guards.size()) { p->job = "guard"; p->path = g.guards[gi++]; done = true; } break;
                    case 's': if (si < g.sleeps.size()) { p->job = "sleep"; p->path = g.sleeps[si++]; done = true; } break;
                }
            }
            if (!done) {
                p->job = "wander";
                if (auto c = smartCenter.find(smart); c != smartCenter.end()) p->home = c->second;
            }
        }
    }
    for (const Person& p : people) {
        if (!p.path.empty()) usedPaths.insert(lower(p.path));
        if (!p.look.empty()) usedPaths.insert(lower(p.look));
    }

    // --- the scene's entities ------------------------------------------------------
    auto group = [&](const std::string& name) {
        const int id = nextId++;
        ents.push_back({{"active", true}, {"center", v3(glm::vec3(0.0f))}, {"components", json::array()},
                        {"half", v3(glm::vec3(0.5f))}, {"id", id}, {"name", name}, {"parent", -1},
                        {"rotation", v3(glm::vec3(0.0f))}, {"type", 7}});
        return id;
    };

    if (!people.empty()) {
        const int g = group("Life");
        for (const Person& p : people) {
            const Figure& f = *p.fig;
            const glm::vec3 c = 0.5f * (f.lo + f.hi), h = glm::max(0.5f * (f.hi - f.lo), glm::vec3(0.05f));
            const float a = glm::radians(p.yaw);
            const glm::vec3 rc(c.x * std::cos(a) + c.z * std::sin(a), c.y, -c.x * std::sin(a) + c.z * std::cos(a));
            const bool mutant = f.family == "mutant";
            ents.push_back({{"active", true}, {"center", v3(p.sp->position + rc)},
                            {"components", json::array({{{"type", "model"}, {"model", f.guid},
                                                         {"modelFile", f.file}, {"scale", 1.0}},
                                                        {{"type", "anim_graph"}, {"graph", mutant ? "xr_mutant" : "xr_human"}}})},
                            {"half", v3(h)}, {"id", nextId++}, {"name", p.entity}, {"parent", g},
                            {"rotation", v3({0.0f, p.yaw, 0.0f})}, {"type", 6}});
            ++life.figures;
        }
    }

    // The anomalies that show: particles, a fire's light.
    std::map<const xray::LevelSpawn*, std::string> zoneEntity;
    {
        int g = -1;
        for (const Zone& z : zones) {
            if (z.kind == "radiation" || z.kind == "mines") continue;
            if (g < 0) g = group("Anomalies");
            const WorldShape& w = z.shapes[0];
            const float eff = cfg.number(z.sp->section, "effective_radius", 1.0f);
            const float r = w.box ? 0.5f : w.radius * eff;
            json comps = json::array({anomalyParticles(z.kind, r, false)});
            if (z.kind == "fire")
                comps.push_back({{"type", "light"}, {"lightType", 0}, {"color", v3({1.0f, 0.55f, 0.25f})},
                                 {"intensity", 6.0}, {"range", 7.0}, {"castShadows", false}, {"bakedOnly", false},
                                 {"shadowBias", 0.003}});
            std::string nm = z.sp->name;
            if (const int n = nameCount[nm]++; n > 0) nm += "_" + std::to_string(n + 1);
            zoneEntity[z.sp] = nm;
            ents.push_back({{"active", true}, {"center", v3(w.center + glm::vec3(0.0f, z.kind == "fire" ? 0.3f : 0.6f, 0.0f))},
                            {"components", comps}, {"half", v3(glm::vec3(0.25f))}, {"id", nextId++}, {"name", nm},
                            {"parent", g}, {"rotation", v3(glm::vec3(0.0f))}, {"type", 4}});
            if (z.kind != "fire" && z.kind != "teleport")
                ents.push_back({{"active", true}, {"center", v3(w.center + glm::vec3(0.0f, 0.6f, 0.0f))},
                                {"components", json::array({anomalyParticles(z.kind, r, true)})},
                                {"half", v3(glm::vec3(0.25f))}, {"id", nextId++}, {"name", nm + "_blow"},
                                {"parent", g}, {"rotation", v3(glm::vec3(0.0f))}, {"type", 4}});
            ++life.anomalies;
        }
    }

    // The level's own sound sources: a loop as an Audio Source, one that
    // plays now and then for the script.
    std::string timedSounds;
    {
        int g = -1, n = 0;
        for (const xray::StaticSound& s : level.sounds) {
            const std::string file = sounds.file(s.name);
            if (file.empty()) { report.warnings.push_back("no sound " + s.name); continue; }
            const bool timed = s.pauseTime[1] > 0 || s.playTime[1] > 0;
            if (timed) {
                timedSounds += "{file=" + luaStr(file) + ",x=" + num(s.position.x) + ",y=" + num(s.position.y) +
                               ",z=" + num(s.position.z) + ",vol=" + num(s.volume) + ",pause={" +
                               num(s.pauseTime[0] / 1000.0f) + "," + num(s.pauseTime[1] / 1000.0f) + "}},\n";
                ++life.sounds;
                continue;
            }
            if (g < 0) g = group("Sounds");
            ents.push_back({{"active", true}, {"center", v3(s.position)},
                            {"components", json::array({{{"type", "audio_source"}, {"sound", file},
                                                         {"volume", std::clamp(s.volume, 0.05f, 1.0f)}, {"loop", true},
                                                         {"playOnStart", true}, {"spatial", true}, {"radius", 18.0},
                                                         {"channel", "Ambient"}}})},
                            {"half", v3(glm::vec3(0.2f))}, {"id", nextId++},
                            {"name", "sound_" + std::to_string(n++) + "_" + fs::path(slashes(s.name)).filename().string()},
                            {"parent", g}, {"rotation", v3(glm::vec3(0.0f))}, {"type", 4}});
            ++life.sounds;
        }
    }

    // The controller: one object whose script plays everything else.
    ents.push_back({{"active", true}, {"center", v3(people.empty() ? glm::vec3(0.0f) : people[0].home)},
                    {"components", json::array({{{"type", "script"}, {"file", "stalker/zone.lua"}}})},
                    {"half", v3(glm::vec3(0.5f))}, {"id", nextId++}, {"name", "Zone"}, {"parent", -1},
                    {"rotation", v3(glm::vec3(0.0f))}, {"type", 7}});

    json graphs = json::array({graphJson("xr_human", "human"), graphJson("xr_mutant", "mutant")});
    settings["animGraphs"] = graphs;

    // --- the level's data, for the scripts -----------------------------------------
    std::string L = "-- " + o.level + ": written by stalkerimport from the game's level data. Regenerated on\n"
                    "-- every import; the scripts in scripts/stalker/ read it.\nreturn {\n";
    L += "level=" + luaStr(o.level) + ", label=" + luaStr(levelLabel(o.level)) + ", lift=" + num(lift) +
         ", indoor=" + (indoor ? "true" : "false") + ",\n";

    // People.
    std::map<std::string, std::vector<std::string>> voiceCache;
    auto voices = [&](const std::string& voice, const char* what) {
        const std::string key = voice + "|" + what;
        if (auto it = voiceCache.find(key); it != voiceCache.end()) return it->second;
        std::vector<std::string> v = sounds.prefix(std::string("characters_voice\\human_01\\") + voice + "\\" + what, 6);
        return voiceCache[key] = v;
    };
    L += "people={\n";
    for (const Person& p : people) {
        L += "{n=" + luaStr(p.entity) + ",fam=" + luaStr(p.family) + ",job=" + luaStr(p.job);
        if (!p.path.empty()) L += ",path=" + luaStr(p.path);
        if (!p.look.empty()) L += ",look=" + luaStr(p.look);
        if (!p.community.empty()) L += ",com=" + luaStr(p.community);
        L += ",x=" + num(p.sp->position.x) + ",y=" + num(p.sp->position.y) + ",z=" + num(p.sp->position.z) +
             ",yaw=" + num(p.yaw, 1) + ",hx=" + num(p.home.x) + ",hz=" + num(p.home.z);
        const Figure& f = *p.fig;
        L += ",cy=" + num(0.5f * (f.lo.y + f.hi.y), 3) + ",h=" + num(f.hi.y - f.lo.y, 3);
        if (p.kind == Kind::Mutant || p.kind == Kind::Crow) {
            const std::string& sec = p.sp->section;
            std::string walk = cfg.get(sec, "velocity_walkfwdnormal"), run = cfg.get(sec, "velocity_runfwdnormal");
            const float ws = walk.empty() ? 1.4f : std::max(0.3f, std::strtof(walk.c_str(), nullptr));
            const float rs = run.empty() ? 5.0f : std::max(ws, std::strtof(run.c_str(), nullptr));
            L += ",walk=" + num(ws) + ",run=" + num(rs);
            L += ",snd={idle=" + luaList(prefixSounds(sec, "sound_idle")) +
                 ",attack=" + luaList(prefixSounds(sec, "sound_aggressive")) +
                 ",hit=" + luaList(prefixSounds(sec, "sound_attack_hit")) +
                 ",hurt=" + luaList(prefixSounds(sec, "sound_take_damage")) +
                 ",die=" + luaList(prefixSounds(sec, "sound_die")) + "}";
        } else if (p.kind == Kind::Human) {
            const std::string v = voiceOf(p.community);
            L += ",snd={idle=" + luaList(voices(v, "states\\idle\\idle_")) +
                 ",die=" + luaList(voices(v, "fight\\death\\death_")) +
                 ",hurt=" + luaList(voices(v, "fight\\hit\\hit_")) + "}";
        }
        L += "},\n";
    }
    L += "},\n";

    // The patrol paths the people use, and the camp centres.
    L += "paths={\n";
    for (const xray::PatrolPath& p : level.paths) {
        if (!usedPaths.count(lower(p.name))) continue;
        L += "[" + luaStr(p.name) + "]={";
        for (const xray::PatrolPoint& pt : p.points) {
            L += "{x=" + num(pt.position.x) + ",y=" + num(pt.position.y) + ",z=" + num(pt.position.z);
            // The point's flags the scripts read: a=<animation> (guard,
            // sit, ...), t=<ms to wait>.
            std::string a, t;
            std::size_t at = 0;
            while ((at = pt.name.find('|', at)) != std::string::npos) {
                ++at;
                const std::size_t end = pt.name.find('|', at);
                const std::string f = pt.name.substr(at, end == std::string::npos ? std::string::npos : end - at);
                if (starts(f, "a=")) a = f.substr(2);
                else if (starts(f, "t=")) t = f.substr(2);
            }
            if (!a.empty()) L += ",a=" + luaStr(a);
            if (!t.empty()) L += ",t=" + num(std::strtof(t.c_str(), nullptr) / 1000.0f);
            L += "},";
        }
        L += "links={";
        for (const auto& l : p.links) L += "{" + std::to_string(l.from + 1) + "," + std::to_string(l.to + 1) + "},";
        L += "}},\n";
    }
    L += "},\n";

    // Camp songs: the guitar and harmonica of each community met here.
    {
        std::set<std::string> coms;
        for (const Person& p : people)
            if (p.job == "kamp" && p.kind == Kind::Human) coms.insert(voiceOf(p.community));
        auto songs = [&](const std::vector<std::string>& files) {
            std::string o = "{";
            for (const std::string& f : files) o += "{f=" + luaStr(f) + ",len=" + num(sounds.seconds(f), 1) + "},";
            return o + "}";
        };
        L += "songs={";
        for (const std::string& c : coms)
            L += "[" + luaStr(c) + "]={guitar=" + songs(voices(c, "music\\guitar_")) +
                 ",harmonica=" + songs(voices(c, "music\\harmonica_")) + "},";
        L += "},\n";
    }

    // The anomalies, the radiation, the mines.
    L += "zones={\n";
    for (const Zone& z : zones) {
        const std::string& sec = z.sp->section;
        const float power = cfg.number(sec, "max_start_power", z.st.power > 0.0f ? z.st.power : 0.5f);
        const float eff = cfg.number(sec, "effective_radius", 1.0f);
        L += "{n=" + luaStr(z.sp->name) + ",k=" + luaStr(z.kind) + ",p=" + num(power, 3) + ",eff=" + num(eff);
        if (auto it = zoneEntity.find(z.sp); it != zoneEntity.end()) L += ",fx=" + luaStr(it->second);
        auto sound = [&](const char* key) {
            std::string v = cfg.get(sec, key);
            return v.empty() ? std::string() : sounds.file(xray::Ltx::list(v)[0]);
        };
        if (z.kind != "radiation") {
            const std::string idle = sound("idle_sound"), blow = sound("blowout_sound"), hit = sound("hit_sound");
            if (!idle.empty()) L += ",idle=" + luaStr(idle);
            if (!blow.empty()) L += ",blow=" + luaStr(blow);
            if (!hit.empty()) L += ",hit=" + luaStr(hit);
        }
        L += ",s=" + luaShapes(z.shapes) + "},\n";
    }
    L += "},\n";

    // The exits: where each leads, at the graph point of the other level
    // it names (the position the packet carries is often zero).
    std::map<std::string, std::vector<xray::LevelSpawn>> other;
    L += "exits={\n";
    for (const Exit& e : exits) {
        const std::string dest = lower(e.st.destLevel);
        auto it = other.find(dest);
        if (it == other.end()) {
            std::vector<xray::LevelSpawn> sp;
            xray::readSpawns(fs, dest, sp);
            it = other.emplace(dest, std::move(sp)).first;
        }
        glm::vec3 gp = e.st.destPosition, dir = e.st.destDirection;
        bool found = false;
        for (const xray::LevelSpawn& s : it->second)
            if (s.section == "graph_point" && lower(s.name) == lower(e.st.destPoint)) {
                gp = s.gamePosition;
                found = true;
                if (glm::length(dir) < 1e-3f) {
                    const float h = s.angle.y;
                    dir = glm::vec3(-std::sin(h), 0.0f, std::cos(h));
                }
                break;
            }
        if (it->second.empty()) { report.warnings.push_back("exit " + e.sp->name + ": no level " + dest); continue; }
        if (!found && glm::length(gp) < 1e-3f) {
            report.warnings.push_back("exit " + e.sp->name + ": no point " + e.st.destPoint + " in " + dest);
            continue;
        }
        // The direction to face there, as a camera yaw in fitzel (0 = +X,
        // 90 = +Z), from X-Ray's axes.
        const glm::vec3 d = mirror(dir);
        const float yaw = glm::length(glm::vec2(d.x, d.z)) > 1e-3f ? glm::degrees(std::atan2(d.z, d.x)) : 0.0f;
        L += "{n=" + luaStr(e.sp->name) + ",to=" + luaStr(dest) + ",label=" + luaStr(levelLabel(dest)) +
             ",gx=" + num(gp.x) + ",gy=" + num(gp.y) + ",gz=" + num(gp.z) + ",yaw=" + num(yaw, 1) +
             ",s=" + luaShapes(e.shapes) + "},\n";
        ++life.exits;
    }
    L += "},\n";

    // Sound: the level's own timed sources, the ambience, the music.
    L += "sounds={\n" + timedSounds + "},\n";
    {
        const std::string set = indoor ? (has(o.level, "labx18") ? "ambient_env_x18" : "ambient_env_indoor")
                              : has(o.level, "stancia") ? "ambient_env_stancia1" : "ambient_env_0";
        std::vector<std::string> files;
        std::set<std::string> seen;
        for (const std::string& n : xray::Ltx::list(cfg.get(set, "sounds")))
            if (seen.insert(lower(n)).second && files.size() < 48) {
                const std::string f = sounds.file(n);
                if (!f.empty()) files.push_back(f);
            }
        const auto period = xray::Ltx::list(cfg.get(set, "sound_period", "5, 10"));
        const auto dist = xray::Ltx::list(cfg.get(set, "sound_dist", "30, 60"));
        auto at = [](const std::vector<std::string>& v, std::size_t i, float d) {
            return i < v.size() ? std::strtof(v[i].c_str(), nullptr) : d;
        };
        L += "ambient={period={" + num(at(period, 0, 5)) + "," + num(at(period, 1, 10)) + "},dist={" +
             num(at(dist, 0, 30)) + "," + num(at(dist, 1, 60)) + "},sounds=" + luaList(files) + "},\n";
        L += "music={";
        if (!indoor)
            if (const xray::Ltx::Section* m = cfg.section("level_music_tracks"))
                for (const auto& [track, value] : m->items) {
                    const auto v = xray::Ltx::list(value);
                    const std::string l = sounds.file(track + "_l"), r = sounds.file(track + "_r");
                    if (l.empty()) continue;
                    L += "{l=" + luaStr(l) + (r.empty() ? "" : ",r=" + luaStr(r)) + ",len=" + num(sounds.seconds(l), 1) +
                         ",from=" + num(at(v, 0, 0)) +
                         ",to=" + num(at(v, 1, 24)) + ",vol=" + num(at(v, 2, 0.5f)) + ",pause={" +
                         num(at(v, 3, 10)) + "," + num(at(v, 4, 20)) + "}},";
                }
        L += "},\n";
        const auto geiger = sounds.prefix("detectors\\geiger_", 8);
        L += "fx={geiger=" + luaList(geiger) + ",shot=" + luaStr(sounds.file("weapons\\pm_shoot")) +
             ",empty=" + luaStr(sounds.file("weapons\\pistol_empty")) +
             ",reload=" + luaStr(sounds.file("weapons\\pm_reload")) +
             ",hurt=" + luaList(sounds.prefix("actor\\bullet_hit_", 4)) +
             ",die=" + luaList(sounds.prefix("actor\\die", 2)) +
             ",explode=" + luaStr(sounds.file("weapons\\f1_explode")) + "},\n";
    }
    L += "}\n";

    std::error_code ec;
    const fs::path scripts = o.project / "scripts" / "stalker";
    fs::create_directories(scripts / "levels", ec);
    {
        std::ofstream f(scripts / "levels" / (o.level + ".lua"));
        f << L;
        if (!f) { report.warnings.push_back("cannot write the level's data"); return false; }
    }
    if (!session.scriptsWritten) {
        for (const auto& [name, text] : luaScripts()) {
            std::ofstream f(scripts / name);
            f << text;
        }
        session.scriptsWritten = true;
    }

    report.figures = life.figures;
    report.anomalies = life.anomalies;
    report.exits = life.exits;
    report.soundSources = life.sounds;
    std::snprintf(buf, sizeof(buf),
                  "life: %d figures (%d new models), %zu anomalies and zones, %d exits, %d sound sources, %d sound files",
                  life.figures, life.models, zones.size(), life.exits, life.sounds, life.soundFiles);
    say(buf);
    return true;
}

} // namespace stalker
