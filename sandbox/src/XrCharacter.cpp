#include "XrCharacter.hpp"

#include <algorithm>
#include <cstring>
#include <deque>

#include "XrArchive.hpp"
#include "XrChunks.hpp"

namespace xray {

namespace {

using namespace detail;

enum : std::uint32_t {
    OGF_TEXTURE = 2, OGF_VERTICES = 3, OGF_INDICES = 4, OGF_SWIDATA = 6, OGF_CHILDREN = 9,
    OGF_S_BONE_NAMES = 13, OGF_S_MOTIONS = 14, OGF_S_SMPARAMS = 15, OGF_S_IKDATA = 16,
    OGF_S_MOTION_REFS = 19, OGF_S_MOTION_REFS2 = 24
};
constexpr std::uint32_t kBoned1 = 0x12071980u;

// CMotionDef flags.
constexpr std::uint32_t kStopAtEnd = 1u << 1;
// CMotion flags.
constexpr std::uint8_t kTKeyPresent = 1, kRKeyAbsent = 2, kTKey16 = 4;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The skin of a skeleton model: its children, each a part with a bone-weighted
// vertex format (one to four bones per vertex).
void collectSkin(const std::uint8_t* p, std::size_t n, std::vector<SkinPart>& out, int depth) {
    if (depth > 8) return;
    std::deque<std::vector<std::uint8_t>> store;
    const std::vector<Chunk> cs = chunksOf(p, n, store);
    if (const Chunk* kids = find(cs, OGF_CHILDREN))
        for (const Chunk& k : chunksOf(kids->data, kids->size, store))
            collectSkin(k.data, k.size, out, depth + 1);
    const Chunk* vc = find(cs, OGF_VERTICES);
    const Chunk* ic = find(cs, OGF_INDICES);
    if (!vc || !ic) return;
    SkinPart part;
    if (const Chunk* tc = find(cs, OGF_TEXTURE)) {
        Reader r(*tc);
        part.texture = r.str();
        part.shader = r.str();
    }
    Reader v(*vc);
    const std::uint32_t format = v.get<std::uint32_t>();
    const std::uint32_t count = v.get<std::uint32_t>();
    // Bones per vertex: Shadow of Chernobyl writes 1 or 2 times kBoned1, Clear
    // Sky and later (and the Blender exporter) simply 1 to 4.
    int bones = format >= 1 && format <= 4 ? static_cast<int>(format) : 0;
    for (int b = 1; b <= 4; ++b)
        if (format == static_cast<std::uint32_t>(b) * kBoned1) bones = b;
    if (!bones) return;   // not a skin (a rigid FVF part of a hierarchy)
    // vertBoned1W: P N T B uv u32 bone. The others: u16 bones, P N T B, the
    // weights of all but the last bone, uv.
    const std::size_t stride = bones == 1 ? 60 : 2 * bones + 48 + 4 * (bones - 1) + 8;
    const std::size_t at = bones == 1 ? 0 : 2 * bones;
    const std::size_t wAt = at + 48;
    const std::size_t uvAt = bones == 1 ? 48 : wAt + 4 * (bones - 1);
    const std::uint8_t* data = v.skip(static_cast<std::size_t>(count) * stride);
    if (!data) return;
    part.verts.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* q = data + static_cast<std::size_t>(i) * stride;
        SkinVertex& sv = part.verts[i];
        float f[6], t[2];
        std::memcpy(f, q + at, 24);
        std::memcpy(t, q + uvAt, 8);
        sv.pos = {f[0], f[1], f[2]};
        sv.normal = {f[3], f[4], f[5]};
        sv.uv = {t[0], t[1]};
        if (bones == 1) {
            std::uint32_t b;
            std::memcpy(&b, q + 56, 4);
            sv.joints[0] = static_cast<std::uint16_t>(b);
            continue;
        }
        float w[3] = {0.0f, 0.0f, 0.0f}, rest = 1.0f;
        std::memcpy(w, q + wAt, 4 * (bones - 1));
        for (int k = 0; k < bones; ++k) {
            std::uint16_t b;
            std::memcpy(&b, q + 2 * k, 2);
            sv.joints[k] = b;
            // vertBoned2W lerps from the first bone to the second by w.
            const float wk = bones == 2 ? (k == 0 ? 1.0f - w[0] : w[0])
                           : k < bones - 1 ? w[k] : rest;
            if (bones > 2 && k < bones - 1) rest -= w[k];
            sv.weights[k] = std::max(0.0f, wk);
        }
    }
    Reader ir(*ic);
    const std::uint32_t nidx = ir.get<std::uint32_t>();
    const std::uint8_t* idx = ir.skip(static_cast<std::size_t>(nidx) * 2);
    if (!idx) return;
    std::uint32_t first = 0, used = nidx;
    if (const Chunk* sd = find(cs, OGF_SWIDATA)) {
        Reader r(*sd);
        if (const auto w = firstWindow(r)) {
            first = w->offset;
            used = std::min<std::uint32_t>(3u * w->tris, nidx - std::min(nidx, first));
        }
    }
    for (std::uint32_t k = 0; k + 2 < used; k += 3) {
        std::uint16_t tri[3];
        std::memcpy(tri, idx + 2 * (first + k), 6);
        if (tri[0] >= count || tri[1] >= count || tri[2] >= count) return;
        if (tri[0] == tri[1] || tri[1] == tri[2] || tri[0] == tri[2]) continue;
        part.indices.insert(part.indices.end(), {tri[0], tri[1], tri[2]});
    }
    if (!part.indices.empty()) out.push_back(std::move(part));
}

// "a,b" or "a, b" -> {a, b}
std::vector<std::string> splitRefs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s + ",") {
        if (c == ',') {
            while (!cur.empty() && cur.back() == ' ') cur.pop_back();
            std::size_t b = cur.find_first_not_of(' ');
            if (b != std::string::npos) out.push_back(lower(cur.substr(b)));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    return out;
}

} // namespace

// --- motions ----------------------------------------------------------------

bool MotionLibrary::load(std::vector<std::uint8_t> bytes, std::string& error) {
    bytes_ = std::move(bytes);
    trackBone_.clear();
    part_.clear();
    motions_.clear();
    std::deque<std::vector<std::uint8_t>> store;
    const std::vector<Chunk> cs = chunksOf(bytes_.data(), bytes_.size(), store);
    const Chunk* mp = find(cs, OGF_S_SMPARAMS);
    const Chunk* ms = find(cs, OGF_S_MOTIONS);
    if (!mp || !ms) { error = "no motions"; return false; }
    if (!store.empty()) { error = "packed motion chunks"; return false; }   // never seen

    // The definitions: partitions (which track is which bone), then per motion
    // its flags and speed.
    Reader r(*mp);
    const std::uint16_t version = r.get<std::uint16_t>();
    if (version < 3 || version > 4) { error = "motion parameters version " + std::to_string(version); return false; }
    const std::uint16_t parts = r.get<std::uint16_t>();
    for (std::uint16_t pi = 0; pi < parts && r.ok(); ++pi) {
        const std::string part = lower(r.str());
        const std::uint16_t nb = r.get<std::uint16_t>();
        for (std::uint16_t b = 0; b < nb && r.ok(); ++b) {
            const std::string bone = lower(r.str());
            const std::uint32_t track = r.get<std::uint32_t>();
            if (track >= 1024) { error = "bad track index"; return false; }
            if (trackBone_.size() <= track) trackBone_.resize(track + 1);
            trackBone_[track] = bone;
            part_[bone] = part;
        }
    }
    struct Def { std::uint32_t flags; float speed; };
    std::map<std::string, Def> defs;
    const std::uint16_t nm = r.get<std::uint16_t>();
    for (std::uint16_t m = 0; m < nm && r.ok(); ++m) {
        const std::string name = lower(r.str());
        const std::uint32_t flags = r.get<std::uint32_t>();
        r.get<std::uint16_t>();   // bone or part
        r.get<std::uint16_t>();   // motion
        const float speed = r.get<float>();
        r.skip(12);               // power, accrue, falloff
        if (version >= 4) {
            const std::uint32_t marks = r.get<std::uint32_t>();
            for (std::uint32_t k = 0; k < marks && r.ok(); ++k) {
                while (r.ok() && r.get<std::uint8_t>() != '\n') {}   // a line, not a zstring
                const std::uint32_t n = r.get<std::uint32_t>();
                r.skip(static_cast<std::size_t>(n) * 8);
            }
        }
        if (r.ok()) defs[name] = {flags, speed};
    }
    if (!r.ok()) { error = "motion parameters cut short"; return false; }

    // The motions: chunk 0 counts them, chunk i+1 is motion i.
    const std::vector<Chunk> mc = chunksOf(ms->data, ms->size, store);
    for (const Chunk& c : mc) {
        if (c.id == 0) continue;
        Reader q(c);
        const std::string name = lower(q.str());
        if (!q.ok() || name.empty()) continue;
        Entry e;
        e.at = static_cast<std::size_t>(c.data - bytes_.data());
        e.size = c.size;
        if (auto it = defs.find(name); it != defs.end()) {
            e.speed = it->second.speed > 0.01f ? it->second.speed : 1.0f;
            e.loops = !(it->second.flags & kStopAtEnd);
        }
        motions_.emplace(name, e);
    }
    if (motions_.empty()) { error = "no motions"; return false; }
    return true;
}

std::vector<std::string> MotionLibrary::names() const {
    std::vector<std::string> out;
    for (const auto& [n, e] : motions_) out.push_back(n);
    return out;
}

std::string MotionLibrary::partOf(const std::string& bone) const {
    const auto it = part_.find(lower(bone));
    return it == part_.end() ? std::string() : it->second;
}

bool MotionLibrary::decode(const std::string& name, const std::vector<Bone>& skeleton, Motion& out,
                           std::string& error) const {
    const auto it = motions_.find(lower(name));
    if (it == motions_.end()) { error = "no motion " + name; return false; }
    Reader r(bytes_.data() + it->second.at, it->second.size);
    out = Motion{};
    out.name = r.str();
    const std::uint32_t frames = r.get<std::uint32_t>();
    if (!r.ok() || frames == 0 || frames > 100000) { error = name + ": bad length"; return false; }
    out.frames = static_cast<int>(frames);
    out.speed = it->second.speed;
    out.loops = it->second.loops;
    out.tracks.assign(skeleton.size(), BoneTrack{});
    std::map<std::string, int> boneIndex;
    for (std::size_t b = 0; b < skeleton.size(); ++b) boneIndex[lower(skeleton[b].name)] = static_cast<int>(b);
    constexpr float q = 1.0f / 32767.0f;
    for (const std::string& boneName : trackBone_) {
        BoneTrack t;
        const std::uint8_t flags = r.get<std::uint8_t>();
        const std::uint32_t nr = (flags & kRKeyAbsent) ? 1 : frames;
        if (!(flags & kRKeyAbsent)) r.get<std::uint32_t>();   // crc
        const std::uint8_t* rk = r.skip(static_cast<std::size_t>(nr) * 8);
        if (!rk) break;
        t.rotation.resize(nr);
        for (std::uint32_t k = 0; k < nr; ++k) {
            std::int16_t v[4];
            std::memcpy(v, rk + 8 * k, 8);
            t.rotation[k] = glm::quat(v[3] * q, v[0] * q, v[1] * q, v[2] * q);
        }
        if (flags & kTKeyPresent) {
            r.get<std::uint32_t>();   // crc
            const bool hq = (flags & kTKey16) != 0;
            const std::uint8_t* tk = r.skip(static_cast<std::size_t>(frames) * (hq ? 6 : 3));
            const glm::vec3 size = r.vec3(), init = r.vec3();
            if (!tk || !r.ok()) break;
            t.offset.resize(frames);
            for (std::uint32_t k = 0; k < frames; ++k) {
                glm::vec3 key;
                if (hq) {
                    std::int16_t v[3];
                    std::memcpy(v, tk + 6 * k, 6);
                    key = {v[0], v[1], v[2]};
                } else {
                    const auto* s = reinterpret_cast<const std::int8_t*>(tk + 3 * k);
                    key = {s[0], s[1], s[2]};
                }
                t.offset[k] = key * size + init;
            }
        } else {
            t.offset.push_back(r.vec3());
        }
        if (!r.ok()) break;
        if (const auto b = boneIndex.find(boneName); b != boneIndex.end())
            out.tracks[static_cast<std::size_t>(b->second)] = std::move(t);
    }
    if (!r.ok()) { error = name + ": keys cut short"; return false; }
    return true;
}

// --- the model --------------------------------------------------------------

bool readCharacter(const FileSystem& fs, const std::string& path, Character& out, std::string& error) {
    out = Character{};
    std::vector<std::uint8_t> bytes;
    if (!fs.read(path, bytes)) { error = (fs.exists(path) ? "cannot unpack " : "no ") + path; return false; }
    std::deque<std::vector<std::uint8_t>> store;
    const std::vector<Chunk> cs = chunksOf(bytes.data(), bytes.size(), store);

    const Chunk* names = find(cs, OGF_S_BONE_NAMES);
    const Chunk* ik = find(cs, OGF_S_IKDATA);
    if (!names || !ik) { error = path + ": not a skeleton model"; return false; }
    Reader r(*names);
    const std::uint32_t nb = r.get<std::uint32_t>();
    if (!r.ok() || nb == 0 || nb > 512) { error = path + ": bad bone count"; return false; }
    std::vector<std::string> parentName(nb);
    out.bones.resize(nb);
    for (std::uint32_t b = 0; b < nb && r.ok(); ++b) {
        out.bones[b].name = lower(r.str());
        parentName[b] = lower(r.str());
        r.skip(60);   // the bone's box
    }
    for (std::uint32_t b = 0; b < nb; ++b) {
        if (parentName[b].empty()) continue;
        for (std::uint32_t p = 0; p < nb; ++p)
            if (out.bones[p].name == parentName[b]) { out.bones[b].parent = static_cast<int>(p); break; }
    }
    // The IK data per bone: version, material, shape (112 bytes), joint (76
    // bytes, 72 before version 1), then the rest angles and offset.
    Reader k(*ik);
    for (std::uint32_t b = 0; b < nb && k.ok(); ++b) {
        const std::uint32_t version = k.get<std::uint32_t>();
        k.str();
        k.skip(112);
        k.skip(version > 0 ? 76 : 72);
        out.bones[b].restRotation = k.vec3();
        out.bones[b].restOffset = k.vec3();
        k.skip(16);   // mass, centre of mass
    }
    if (!r.ok() || !k.ok()) { error = path + ": skeleton cut short"; return false; }
    // A parent must come before its children for the poses to be built in
    // order; X-Ray writes them so, and a model that does not is refused.
    for (std::uint32_t b = 0; b < nb; ++b)
        if (out.bones[b].parent >= static_cast<int>(b)) { error = path + ": bones out of order"; return false; }

    collectSkin(bytes.data(), bytes.size(), out.parts, 0);
    if (out.parts.empty()) { error = path + ": no skin"; return false; }
    for (const SkinPart& p : out.parts)
        for (const SkinVertex& v : p.verts)
            for (int j = 0; j < 4; ++j)
                if (v.weights[j] > 0.0f && v.joints[j] >= nb) { error = path + ": a vertex names a missing bone"; return false; }

    if (const Chunk* refs = find(cs, OGF_S_MOTION_REFS2)) {
        Reader q(*refs);
        const std::uint32_t n = q.get<std::uint32_t>();
        for (std::uint32_t i = 0; i < n && q.ok(); ++i) out.motionRefs.push_back(lower(q.str()));
    } else if (const Chunk* refs1 = find(cs, OGF_S_MOTION_REFS)) {
        Reader q(*refs1);
        out.motionRefs = splitRefs(q.str());
    }
    if (find(cs, OGF_S_MOTIONS) && find(cs, OGF_S_SMPARAMS)) {
        std::string e;
        if (!out.embedded.load(bytes, e)) error = path + ": " + e;   // a warning; refs may still do
    }
    return true;
}

bool readMotionLibrary(const FileSystem& fs, const std::string& ref, MotionLibrary& out,
                       std::string& error) {
    std::string path = "meshes/" + ref + ".omf";
    for (char& c : path) if (c == '\\') c = '/';
    std::vector<std::uint8_t> bytes;
    if (!fs.read(path, bytes)) { error = "no " + path; return false; }
    if (!out.load(std::move(bytes), error)) { error = path + ": " + error; return false; }
    return true;
}

} // namespace xray
