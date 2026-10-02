// Retargeting, part 3: writing a character back as .glb -- its original bytes,
// the retargeted clips appended, and the repairs a Daz-style export needs
// (see Retarget.hpp). Only ever appends to the binary chunk: meshes, images and
// every clip that is not being replaced pass through untouched.
#include "Retarget.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <unordered_map>

#include <cgltf.h>
#include <nlohmann/json.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace fs = std::filesystem;
using nlohmann::json;

namespace retarget {

namespace {

constexpr std::uint32_t kMagic = 0x46546C67u;   // "glTF"
constexpr std::uint32_t kJson  = 0x4E4F534Au;   // "JSON"
constexpr std::uint32_t kBin   = 0x004E4942u;   // "BIN\0"

struct Glb {
    std::vector<std::uint8_t> raw;   // the file as read
    json                      j;
    std::vector<std::uint8_t> bin;
};

std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t at) {
    std::uint32_t v = 0;
    std::memcpy(&v, b.data() + at, 4);
    return v;
}

bool readGlb(const std::string& path, Glb& g, std::string& err) {
    const std::string file = fs::path(path).filename().string();
    std::ifstream f(fs::path(path), std::ios::binary);
    if (!f) { err = "Could not open " + file + "."; return false; }
    g.raw.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (g.raw.size() < 20 || u32(g.raw, 0) != kMagic) {
        err = file + " is not a binary glTF (.glb) -- only .glb characters can be written.";
        return false;
    }
    std::size_t at = 12;
    bool haveJson = false;
    while (at + 8 <= g.raw.size()) {
        const std::uint32_t len = u32(g.raw, at), type = u32(g.raw, at + 4);
        if (at + 8 + len > g.raw.size()) break;
        const auto* p = g.raw.data() + at + 8;
        if (type == kJson) {
            g.j = json::parse(std::string(reinterpret_cast<const char*>(p), len), nullptr, false);
            haveJson = !g.j.is_discarded();
        } else if (type == kBin) {
            g.bin.assign(p, p + len);
        }
        at += 8 + len;
    }
    if (!haveJson) { err = file + ": its glTF header could not be read."; return false; }
    return true;
}

bool writeGlb(const std::string& path, const json& j, std::vector<std::uint8_t> bin, std::string& err) {
    std::string text = j.dump();
    while (text.size() % 4) text.push_back(' ');
    while (bin.size() % 4) bin.push_back(0);
    const std::uint32_t total = static_cast<std::uint32_t>(12 + 8 + text.size() + (bin.empty() ? 0 : 8 + bin.size()));
    std::ofstream f(fs::path(path), std::ios::binary | std::ios::trunc);
    if (!f) { err = "Could not write " + fs::path(path).filename().string() + "."; return false; }
    auto put = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    put(kMagic); put(2u); put(total);
    put(static_cast<std::uint32_t>(text.size())); put(kJson);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!bin.empty()) {
        put(static_cast<std::uint32_t>(bin.size())); put(kBin);
        f.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
    }
    f.close();
    if (!f) { err = "Writing " + fs::path(path).filename().string() + " failed (disk full?)."; return false; }
    return true;
}

int addView(Glb& g, const void* data, std::size_t bytes) {
    while (g.bin.size() % 4) g.bin.push_back(0);
    const std::size_t off = g.bin.size();
    const auto* p = static_cast<const std::uint8_t*>(data);
    g.bin.insert(g.bin.end(), p, p + bytes);
    g.j["bufferViews"].push_back({{"buffer", 0}, {"byteOffset", off}, {"byteLength", bytes}});
    return static_cast<int>(g.j["bufferViews"].size()) - 1;
}

// A float accessor of `count` elements with `comps` components each.
int addFloats(Glb& g, const std::vector<float>& v, int comps, const char* type, bool minmax) {
    const std::size_t count = v.size() / static_cast<std::size_t>(comps);
    json a = {{"bufferView", addView(g, v.data(), v.size() * sizeof(float))},
              {"componentType", 5126}, {"count", count}, {"type", type}};
    if (minmax && count > 0) {
        json lo = json::array(), hi = json::array();
        for (int c = 0; c < comps; ++c) {
            float mn = v[static_cast<std::size_t>(c)], mx = mn;
            for (std::size_t i = 0; i < count; ++i) {
                const float x = v[i * static_cast<std::size_t>(comps) + static_cast<std::size_t>(c)];
                mn = std::min(mn, x);
                mx = std::max(mx, x);
            }
            lo.push_back(mn);
            hi.push_back(mx);
        }
        a["min"] = lo;
        a["max"] = hi;
    }
    g.j["accessors"].push_back(std::move(a));
    return static_cast<int>(g.j["accessors"].size()) - 1;
}

int addU16x4(Glb& g, const std::vector<std::uint16_t>& v) {
    json a = {{"bufferView", addView(g, v.data(), v.size() * sizeof(std::uint16_t))},
              {"componentType", 5123}, {"count", v.size() / 4}, {"type", "VEC4"}};
    g.j["accessors"].push_back(std::move(a));
    return static_cast<int>(g.j["accessors"].size()) - 1;
}

std::string nodeName(const json& j, int n) {
    const json& nodes = j["nodes"];
    if (n < 0 || n >= static_cast<int>(nodes.size())) return {};
    return nodes[static_cast<std::size_t>(n)].value("name", std::string());
}

// Point every extra skin's joints at the first skin's joints of the same name.
int mergeSkins(Glb& g) {
    json& skins = g.j["skins"];
    if (!skins.is_array() || skins.size() < 2) return 0;
    std::unordered_map<std::string, int> body;
    for (const json& jn : skins[0]["joints"]) body.emplace(nodeName(g.j, jn.get<int>()), jn.get<int>());
    int moved = 0;
    for (std::size_t k = 1; k < skins.size(); ++k) {
        for (json& jn : skins[k]["joints"]) {
            const auto it = body.find(nodeName(g.j, jn.get<int>()));
            if (it != body.end() && it->second != jn.get<int>()) { jn = it->second; ++moved; }
        }
        skins[k].erase("skeleton");
    }
    return moved;
}

// Bind every mesh without a skin whole to the mapped bone most of its
// vertices are nearest to (hair -> head). Its node transform is baked into the
// vertices, because a skinned mesh's node transform does not count.
int bindLoose(Glb& g, cgltf_data* data, const BoneMap& tgtMap, std::vector<std::string>* notes) {
    if (!data || data->skins_count == 0) return 0;
    const cgltf_skin& sk = data->skins[0];
    struct Bone { int slot; glm::vec3 p; };
    std::vector<Bone> bones;
    auto skinSlot = [&](int node) {
        for (cgltf_size k = 0; k < sk.joints_count; ++k)
            if (sk.joints[k] - data->nodes == node) return static_cast<int>(k);
        return -1;
    };
    auto jointPos = [&](int k) {
        if (!sk.inverse_bind_matrices) {
            float w[16];
            cgltf_node_transform_world(sk.joints[k], w);
            return glm::vec3(w[12], w[13], w[14]);
        }
        float m[16];
        cgltf_accessor_read_float(sk.inverse_bind_matrices, static_cast<cgltf_size>(k), m, 16);
        return glm::vec3(glm::inverse(glm::make_mat4(m))[3]);
    };
    for (int s = 0; s < SlotCount; ++s) {
        const int k = skinSlot(tgtMap[static_cast<std::size_t>(s)]);
        if (k >= 0) bones.push_back({k, jointPos(k)});
    }
    if (bones.empty())
        for (cgltf_size k = 0; k < sk.joints_count; ++k) bones.push_back({static_cast<int>(k), jointPos(static_cast<int>(k))});
    if (bones.empty()) return 0;

    int bound = 0;
    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& cn = data->nodes[ni];
        if (!cn.mesh || cn.skin) continue;
        float wm[16];
        cgltf_node_transform_world(&cn, wm);
        const glm::mat4 M = glm::make_mat4(wm);
        const glm::mat3 Nm = glm::transpose(glm::inverse(glm::mat3(M)));
        struct Prim { std::vector<float> P, N; };
        std::vector<Prim> prims(cn.mesh->primitives_count);
        std::vector<int> votes(bones.size(), 0);
        for (cgltf_size pi = 0; pi < cn.mesh->primitives_count; ++pi) {
            const cgltf_primitive& pr = cn.mesh->primitives[pi];
            for (cgltf_size ai = 0; ai < pr.attributes_count; ++ai) {
                const cgltf_attribute& at = pr.attributes[ai];
                if (at.type != cgltf_attribute_type_position && at.type != cgltf_attribute_type_normal) continue;
                std::vector<float>& out = at.type == cgltf_attribute_type_position ? prims[pi].P : prims[pi].N;
                out.resize(at.data->count * 3);
                for (cgltf_size v = 0; v < at.data->count; ++v) {
                    float x[3] = {0, 0, 0};
                    cgltf_accessor_read_float(at.data, v, x, 3);
                    glm::vec3 q(x[0], x[1], x[2]);
                    if (at.type == cgltf_attribute_type_position) {
                        q = glm::vec3(M * glm::vec4(q, 1.0f));
                        std::size_t nearest = 0;
                        float d = 1e30f;
                        for (std::size_t b = 0; b < bones.size(); ++b) {
                            const glm::vec3 e = q - bones[b].p;
                            const float dd = glm::dot(e, e);
                            if (dd < d) { d = dd; nearest = b; }
                        }
                        ++votes[nearest];
                    } else {
                        q = Nm * q;
                        const float l = glm::length(q);
                        if (l > 1e-12f) q /= l;
                    }
                    out[v * 3] = q.x; out[v * 3 + 1] = q.y; out[v * 3 + 2] = q.z;
                }
            }
        }
        const std::size_t win = static_cast<std::size_t>(std::max_element(votes.begin(), votes.end()) - votes.begin());
        const std::uint16_t slot = static_cast<std::uint16_t>(bones[win].slot);
        json& jmesh = g.j["meshes"][static_cast<std::size_t>(cn.mesh - data->meshes)];
        for (cgltf_size pi = 0; pi < cn.mesh->primitives_count; ++pi) {
            json& attrs = jmesh["primitives"][pi]["attributes"];
            const Prim& p = prims[pi];
            if (p.P.empty()) continue;
            const std::size_t n = p.P.size() / 3;
            attrs["POSITION"] = addFloats(g, p.P, 3, "VEC3", true);
            if (!p.N.empty()) attrs["NORMAL"] = addFloats(g, p.N, 3, "VEC3", false);
            attrs.erase("TANGENT");
            std::vector<std::uint16_t> jt(n * 4, 0);
            std::vector<float> wt(n * 4, 0.0f);
            for (std::size_t v = 0; v < n; ++v) { jt[v * 4] = slot; wt[v * 4] = 1.0f; }
            attrs["JOINTS_0"] = addU16x4(g, jt);
            attrs["WEIGHTS_0"] = addFloats(g, wt, 4, "VEC4", false);
        }
        g.j["nodes"][ni]["skin"] = 0;
        ++bound;
        if (notes)
            notes->push_back(std::string(cn.name ? cn.name : "a mesh") + " bound to " +
                             (sk.joints[bones[win].slot]->name ? sk.joints[bones[win].slot]->name : "a bone"));
    }
    return bound;
}

void addClip(Glb& g, const Clip& c) {
    const int frames = c.frames();
    if (frames == 0) return;
    std::vector<float> times(static_cast<std::size_t>(frames));
    for (int k = 0; k < frames; ++k) times[static_cast<std::size_t>(k)] = static_cast<float>(k) / c.fps;
    const int input = addFloats(g, times, 1, "SCALAR", true);
    json channels = json::array(), samplers = json::array();
    auto channel = [&](int node, const char* path, int output) {
        samplers.push_back({{"input", input}, {"output", output}, {"interpolation", "LINEAR"}});
        channels.push_back({{"sampler", samplers.size() - 1}, {"target", {{"node", node}, {"path", path}}}});
    };
    for (std::size_t n = 0; n < c.nodes.size(); ++n) {
        std::vector<float> v;
        v.reserve(static_cast<std::size_t>(frames) * 4);
        for (const glm::quat& q : c.rot[n]) { v.push_back(q.x); v.push_back(q.y); v.push_back(q.z); v.push_back(q.w); }
        channel(c.nodes[n], "rotation", addFloats(g, v, 4, "VEC4", false));
    }
    if (c.hipNode >= 0 && !c.hipT.empty()) {
        std::vector<float> v;
        for (const glm::vec3& t : c.hipT) { v.push_back(t.x); v.push_back(t.y); v.push_back(t.z); }
        channel(c.hipNode, "translation", addFloats(g, v, 3, "VEC3", false));
    }
    g.j["animations"].push_back({{"name", c.name}, {"channels", channels}, {"samplers", samplers}});
}

} // namespace

ModelInfo inspect(const std::string& path) {
    ModelInfo out;
    // Only the header: a character's .glb is mostly images, and the panel asks
    // this of every model in the project.
    json j;
    {
        std::ifstream f(fs::path(path), std::ios::binary);
        if (!f) return out;
        std::uint32_t head[5] = {0, 0, 0, 0, 0};
        f.read(reinterpret_cast<char*>(head), sizeof head);
        if (f && head[0] == kMagic && head[4] == kJson) {
            std::string text(head[3], '\0');
            f.read(text.data(), static_cast<std::streamsize>(head[3]));
            if (!f) return out;
            j = json::parse(text, nullptr, false);
            out.binary = true;
        } else {
            f.clear();
            f.seekg(0);
            j = json::parse(f, nullptr, false);
        }
    }
    if (j.is_discarded() || !j.is_object() || !j.contains("asset")) return out;
    out.ok = true;
    if (j.contains("animations") && j["animations"].is_array())
        for (const json& a : j["animations"]) out.clips.push_back(a.value("name", std::string()));
    const json& skins = j.contains("skins") ? j["skins"] : json::array();
    if (!skins.is_array() || skins.empty()) return out;
    out.skinned = true;
    Glb g;
    g.j = std::move(j);
    // A skin counts only while it still has joints of its own.
    std::set<int> body;
    for (const json& jn : skins[0]["joints"]) body.insert(jn.get<int>());
    for (std::size_t k = 1; k < skins.size(); ++k)
        for (const json& jn : skins[k]["joints"])
            if (!body.count(jn.get<int>())) { ++out.extraSkins; break; }
    if (g.j.contains("nodes"))
        for (const json& n : g.j["nodes"])
            if (n.contains("mesh") && !n.contains("skin")) {
                std::string name = n.value("name", std::string());
                if (name.empty()) name = "mesh " + std::to_string(n["mesh"].get<int>());
                out.looseMeshes.push_back(name);
            }
    return out;
}

bool writeModel(const std::string& base, const std::string& out, const std::vector<Clip>& clips,
                const Repair& repair, const BoneMap& tgtMap, std::string& err,
                std::vector<std::string>* notes) {
    Glb g;
    if (!readGlb(base, g, err)) return false;
    if (g.j.contains("buffers") && g.j["buffers"].is_array() && !g.j["buffers"].empty() &&
        g.j["buffers"][0].contains("uri")) {
        err = "The model keeps its data in a separate file; export it as one .glb first.";
        return false;
    }
    if (!g.j.contains("buffers") || !g.j["buffers"].is_array() || g.j["buffers"].empty())
        g.j["buffers"] = json::array({json{{"byteLength", 0}}});
    for (const char* k : {"bufferViews", "accessors", "animations"})
        if (!g.j.contains(k) || !g.j[k].is_array()) g.j[k] = json::array();

    if (repair.mergeSkins) {
        const int moved = mergeSkins(g);
        if (moved && notes) notes->push_back(std::to_string(moved) + " joints of extra skins put onto the body's");
    }
    if (repair.bindLoose) {
        cgltf_options opt{};
        cgltf_data* data = nullptr;
        if (cgltf_parse(&opt, g.raw.data(), g.raw.size(), &data) == cgltf_result_success &&
            cgltf_load_buffers(&opt, data, base.c_str()) == cgltf_result_success)
            bindLoose(g, data, tgtMap, notes);
        if (data) cgltf_free(data);
    }

    // Replace the clips of the same names, keep every other one.
    std::set<std::string> names;
    for (const Clip& c : clips) names.insert(c.name);
    json kept = json::array();
    for (json& a : g.j["animations"])
        if (!names.count(a.value("name", std::string()))) kept.push_back(std::move(a));
    g.j["animations"] = std::move(kept);
    for (const Clip& c : clips) addClip(g, c);
    if (g.j["animations"].empty()) g.j.erase("animations");

    g.j["buffers"][0]["byteLength"] = g.bin.size();
    const std::string tmp = out + ".writing";
    if (!writeGlb(tmp, g.j, std::move(g.bin), err)) return false;
    std::error_code ec;
    fs::rename(fs::path(tmp), fs::path(out), ec);
    if (ec) {
        err = "Could not replace " + fs::path(out).filename().string() + ": " + ec.message() +
              " (open in another program?)";
        return false;
    }
    return true;
}

} // namespace retarget