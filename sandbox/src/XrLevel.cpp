#include "XrLevel.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <optional>

#include "XrArchive.hpp"
#include "XrChunks.hpp"

namespace xray {

namespace {

using namespace detail;

// X-Ray to fitzel: mirror Z.
glm::vec3 axes(const glm::vec3& v) { return {v.x, v.y, -v.z}; }

// --- level.geom: the buffers --------------------------------------------------

// The fields of a D3DVERTEXELEMENT9 that matter here.
enum DeclType : std::uint8_t {
    FLOAT1 = 0, FLOAT2 = 1, FLOAT3 = 2, FLOAT4 = 3, D3DCOLOR = 4, UBYTE4 = 5, SHORT2 = 6,
    SHORT4 = 7, UBYTE4N = 8, SHORT2N = 9, SHORT4N = 10, USHORT2N = 11, USHORT4N = 12,
    UDEC3 = 13, DEC3N = 14, FLOAT16_2 = 15, FLOAT16_4 = 16, UNUSED = 17
};
enum DeclUsage : std::uint8_t { POSITION = 0, NORMAL = 3, TEXCOORD = 5, TANGENT = 6, BINORMAL = 7, COLOR = 10 };

int declSize(std::uint8_t t) {
    static const int s[] = {4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8};
    return t < 17 ? s[t] : 0;
}

struct Element {
    int offset = -1;
    std::uint8_t type = UNUSED;
    bool has() const { return offset >= 0; }
};

struct VertexBuffer {
    Element pos, normal, tangent, binormal, color, tc0, tc1;
    std::size_t stride = 0;
    std::size_t count = 0;
    const std::uint8_t* data = nullptr;
};

struct IndexBuffer {
    std::size_t count = 0;
    const std::uint8_t* data = nullptr;   // u16 each
    std::uint32_t at(std::size_t i) const {
        std::uint16_t v;
        std::memcpy(&v, data + 2 * i, 2);
        return v;
    }
};

struct Geometry {
    std::vector<VertexBuffer> vbs;
    std::vector<IndexBuffer> ibs;
    std::vector<SlideWindow> swis;   // each SWI's first window
};

bool readGeometry(const std::vector<Chunk>& cs, Geometry& g, std::string& error) {
    const Chunk* vb = find(cs, 9);
    const Chunk* ib = find(cs, 10);
    if (!vb || !ib) { error = "no vertex/index buffers"; return false; }
    Reader r(*vb);
    const std::uint32_t nvb = r.get<std::uint32_t>();
    for (std::uint32_t i = 0; i < nvb && r.ok(); ++i) {
        VertexBuffer v;
        for (int e = 0; e < 64; ++e) {
            const std::uint16_t stream = r.get<std::uint16_t>();
            const std::uint16_t offset = r.get<std::uint16_t>();
            const std::uint8_t type = r.get<std::uint8_t>();
            r.get<std::uint8_t>();   // method
            const std::uint8_t usage = r.get<std::uint8_t>();
            const std::uint8_t index = r.get<std::uint8_t>();
            if (!r.ok() || stream == 0xff || type == UNUSED) break;
            v.stride = std::max<std::size_t>(v.stride, offset + declSize(type));
            const Element el{offset, type};
            switch (usage) {
                case POSITION: v.pos = el; break;
                case NORMAL:   v.normal = el; break;
                case TANGENT:  v.tangent = el; break;
                case BINORMAL: v.binormal = el; break;
                case COLOR:    v.color = el; break;
                case TEXCOORD: (index == 0 ? v.tc0 : v.tc1) = el; break;
                default: break;
            }
        }
        v.count = r.get<std::uint32_t>();
        if (!r.ok() || v.stride == 0) break;
        v.data = r.skip(v.count * v.stride);
        if (!v.data) break;
        g.vbs.push_back(v);
    }
    if (g.vbs.size() != nvb) { error = "vertex buffers cut short"; return false; }
    Reader q(*ib);
    const std::uint32_t nib = q.get<std::uint32_t>();
    for (std::uint32_t i = 0; i < nib && q.ok(); ++i) {
        IndexBuffer b;
        b.count = q.get<std::uint32_t>();
        b.data = q.skip(b.count * 2);
        if (!b.data) break;
        g.ibs.push_back(b);
    }
    if (g.ibs.size() != nib) { error = "index buffers cut short"; return false; }
    if (const Chunk* sw = find(cs, 11)) {
        Reader s(*sw);
        const std::uint32_t n = s.get<std::uint32_t>();
        for (std::uint32_t i = 0; i < n && s.ok(); ++i) {
            const auto w = firstWindow(s);
            g.swis.push_back(w.value_or(SlideWindow{}));
        }
    }
    return true;
}

// One vertex of a buffer, decoded in X-Ray's own axes. Trees and MU models
// carry four shorts: UV at 2048 per unit, then wind data.
LevelVertex vertexAt(const VertexBuffer& vb, std::size_t i) {
    const std::uint8_t* p = vb.data + i * vb.stride;
    LevelVertex v;
    auto f3 = [&](const Element& e) {
        float f[3];
        std::memcpy(f, p + e.offset, 12);
        return glm::vec3(f[0], f[1], f[2]);
    };
    // D3DCOLOR is B, G, R, A in memory; xrLC packs x into R, y into G, z into B.
    auto color = [&](const Element& e) {
        const std::uint8_t* c = p + e.offset;
        return glm::vec4(c[2], c[1], c[0], c[3]) / 255.0f;
    };
    if (vb.pos.has() && vb.pos.type == FLOAT3) v.pos = f3(vb.pos);
    if (vb.normal.has()) {
        if (vb.normal.type == D3DCOLOR) {
            const glm::vec4 n = color(vb.normal);
            v.normal = glm::vec3(n) * 2.0f - 1.0f;
            v.hemi = n.w;
        } else if (vb.normal.type == FLOAT3) {
            v.normal = f3(vb.normal);
        }
    }
    if (vb.tc0.has()) {
        const std::uint8_t* t = p + vb.tc0.offset;
        if (vb.tc0.type == FLOAT2) {
            float f[2];
            std::memcpy(f, t, 8);
            v.uv = {f[0], f[1]};
        } else if (vb.tc0.type == SHORT4) {
            std::int16_t s[2];
            std::memcpy(s, t, 4);
            v.uv = glm::vec2(s[0], s[1]) / 2048.0f;   // FTreeVisual_quant
        } else if (vb.tc0.type == SHORT2) {
            std::int16_t s[2];
            std::memcpy(s, t, 4);
            // 1024 per unit, the fraction below that in the tangent's and the
            // binormal's alpha (xrLC's s24_tc_base).
            glm::vec2 frac(0.0f);
            if (vb.tangent.has() && vb.tangent.type == D3DCOLOR) frac.x = p[vb.tangent.offset + 3] / 255.0f;
            if (vb.binormal.has() && vb.binormal.type == D3DCOLOR) frac.y = p[vb.binormal.offset + 3] / 255.0f;
            v.uv = (glm::vec2(s[0], s[1]) + frac) / 1024.0f;
        }
    }
    if (vb.tc1.has()) {
        const std::uint8_t* t = p + vb.tc1.offset;
        if (vb.tc1.type == SHORT2) {
            std::int16_t s[2];
            std::memcpy(s, t, 4);
            v.uvLight = glm::vec2(s[0], s[1]) / 32768.0f;
        } else if (vb.tc1.type == FLOAT2) {
            float f[2];
            std::memcpy(f, t, 8);
            v.uvLight = {f[0], f[1]};
        }
    }
    return v;
}

// --- level: the visuals --------------------------------------------------------

enum VisualType : std::uint8_t {
    MT_NORMAL = 0, MT_HIERRARHY = 1, MT_PROGRESSIVE = 2, MT_LOD = 6, MT_TREE_ST = 7, MT_TREE_PM = 11
};
enum OgfChunk : std::uint32_t {
    OGF_HEADER = 1, OGF_SWIDATA = 6, OGF_VCONTAINER = 7, OGF_ICONTAINER = 8,
    OGF_TREEDEF2 = 12, OGF_SWICONTAINER = 20, OGF_GCONTAINER = 21
};

bool readVisual(const std::uint8_t* p, std::size_t n, const Geometry& g, LevelMesh& out,
                bool& isLeaf) {
    std::deque<std::vector<std::uint8_t>> store;
    const std::vector<Chunk> cs = chunksOf(p, n, store);
    const Chunk* hdr = find(cs, OGF_HEADER);
    isLeaf = false;
    if (!hdr || hdr->size < 4) return false;
    const std::uint8_t type = hdr->data[1];
    std::uint16_t shader;
    std::memcpy(&shader, hdr->data + 2, 2);
    if (type != MT_NORMAL && type != MT_PROGRESSIVE && type != MT_TREE_ST && type != MT_TREE_PM)
        return false;
    isLeaf = true;

    std::uint32_t vbi = 0, vbo = 0, vbc = 0, ibi = 0, ibo = 0, ibc = 0;
    if (const Chunk* gc = find(cs, OGF_GCONTAINER)) {
        Reader r(*gc);
        vbi = r.get<std::uint32_t>(); vbo = r.get<std::uint32_t>(); vbc = r.get<std::uint32_t>();
        ibi = r.get<std::uint32_t>(); ibo = r.get<std::uint32_t>(); ibc = r.get<std::uint32_t>();
        if (!r.ok()) return false;
    } else {
        const Chunk* vc = find(cs, OGF_VCONTAINER);
        const Chunk* ic = find(cs, OGF_ICONTAINER);
        if (!vc || !ic) return false;
        Reader r(*vc), s(*ic);
        vbi = r.get<std::uint32_t>(); vbo = r.get<std::uint32_t>(); vbc = r.get<std::uint32_t>();
        ibi = s.get<std::uint32_t>(); ibo = s.get<std::uint32_t>(); ibc = s.get<std::uint32_t>();
        if (!r.ok() || !s.ok()) return false;
    }
    // The full-detail window of a progressive mesh.
    std::optional<SlideWindow> win;
    if (const Chunk* sd = find(cs, OGF_SWIDATA)) {
        Reader r(*sd);
        win = firstWindow(r);
    } else if (const Chunk* sc = find(cs, OGF_SWICONTAINER)) {
        Reader r(*sc);
        const std::uint32_t i = r.get<std::uint32_t>();
        if (r.ok() && i < g.swis.size()) win = g.swis[i];
    }
    if (win) {
        if (win->tris == 0) return false;
        ibo += win->offset;
        ibc = 3u * win->tris;
        if (win->verts) vbc = std::min<std::uint32_t>(vbc, win->verts);
    }
    if (vbi >= g.vbs.size() || ibi >= g.ibs.size()) return false;
    const VertexBuffer& vb = g.vbs[vbi];
    const IndexBuffer& ib = g.ibs[ibi];
    if (static_cast<std::size_t>(vbo) + vbc > vb.count) return false;
    if (static_cast<std::size_t>(ibo) + ibc > ib.count) return false;

    // A tree's instance transform: rows i, j, k, translation (D3D row vectors).
    bool tree = false;
    glm::mat3 basis(1.0f), normalBasis(1.0f);
    glm::vec3 origin(0.0f);
    if (const Chunk* td = find(cs, OGF_TREEDEF2); td && td->size >= 64) {
        float m[16];
        std::memcpy(m, td->data, 64);
        basis = glm::mat3(glm::vec3(m[0], m[1], m[2]), glm::vec3(m[4], m[5], m[6]),
                          glm::vec3(m[8], m[9], m[10]));
        origin = glm::vec3(m[12], m[13], m[14]);
        normalBasis = glm::transpose(glm::inverse(basis));
        tree = true;
    }

    out = LevelMesh{};
    out.kind = tree ? LevelMesh::Kind::Tree
                    : (win ? LevelMesh::Kind::Progressive : LevelMesh::Kind::Static);
    out.shader = shader;
    out.verts.reserve(vbc);
    for (std::uint32_t i = 0; i < vbc; ++i) {
        LevelVertex v = vertexAt(vb, vbo + i);
        if (tree) {
            v.pos = basis * v.pos + origin;
            v.normal = normalBasis * v.normal;
        }
        const float len = glm::length(v.normal);
        v.normal = len > 1e-6f ? v.normal / len : glm::vec3(0.0f, 1.0f, 0.0f);
        v.pos = axes(v.pos);
        v.normal = axes(v.normal);
        out.lo = glm::min(out.lo, v.pos);
        out.hi = glm::max(out.hi, v.pos);
        out.verts.push_back(v);
    }
    out.indices.reserve(ibc);
    for (std::uint32_t t = 0; t + 2 < ibc; t += 3) {
        const std::uint32_t a = ib.at(ibo + t), b = ib.at(ibo + t + 1), c = ib.at(ibo + t + 2);
        if (a >= vbc || b >= vbc || c >= vbc) return false;
        if (a == b || b == c || a == c) continue;
        // The mirror turned D3D's winding around; swap back to front-facing.
        out.indices.insert(out.indices.end(), {a, c, b});
    }
    return !out.indices.empty();
}

// --- OGF models --------------------------------------------------------------

enum OgfModelChunk : std::uint32_t { OGF_TEXTURE = 2, OGF_VERTICES = 3, OGF_INDICES = 4, OGF_CHILDREN = 9 };
// Vertex formats: plain FVF, and the skinned ones by bones per vertex.
constexpr std::uint32_t kFvf = 0x112;   // D3DFVF_XYZ | NORMAL | TEX1
constexpr std::uint32_t kBoned1 = 0x12071980u;

void collectModel(const std::uint8_t* p, std::size_t n, std::vector<ModelPart>& out, int depth) {
    if (depth > 8) return;
    std::deque<std::vector<std::uint8_t>> store;
    const std::vector<Chunk> cs = chunksOf(p, n, store);
    if (const Chunk* kids = find(cs, OGF_CHILDREN))
        for (const Chunk& k : chunksOf(kids->data, kids->size, store))
            collectModel(k.data, k.size, out, depth + 1);
    const Chunk* vc = find(cs, OGF_VERTICES);
    const Chunk* ic = find(cs, OGF_INDICES);
    if (!vc || !ic) return;
    ModelPart part;
    if (const Chunk* tc = find(cs, OGF_TEXTURE)) {
        Reader r(*tc);
        part.texture = r.str();
        part.shader = r.str();
    }
    Reader v(*vc);
    const std::uint32_t format = v.get<std::uint32_t>();
    const std::uint32_t count = v.get<std::uint32_t>();
    // Bytes per vertex and where P, N and UV sit in it.
    std::size_t stride = 0, at = 0, uvAt = 0;
    if (format == kFvf) { stride = 32; at = 0; uvAt = 24; }
    // Skinned: 1 or 2 times kBoned1 in Shadow of Chernobyl, 1 to 4 bones per
    // vertex as plain numbers from Clear Sky on.
    else if (format == kBoned1 || format == 1) { stride = 60; at = 0; uvAt = 48; }      // P N T B uv bone
    else if (format == 2 * kBoned1 || format == 2) { stride = 64; at = 4; uvAt = 56; }  // bones P N T B w uv
    else if (format == 3 * kBoned1 || format == 3) { stride = 70; at = 6; uvAt = 62; }
    else if (format == 4 * kBoned1 || format == 4) { stride = 76; at = 8; uvAt = 68; }
    else return;
    const std::uint8_t* data = v.skip(static_cast<std::size_t>(count) * stride);
    if (!data) return;
    part.verts.resize(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint8_t* q = data + static_cast<std::size_t>(i) * stride;
        float f[6], t[2];
        std::memcpy(f, q + at, 24);
        std::memcpy(t, q + uvAt, 8);
        LevelVertex& lv = part.verts[i];
        lv.pos = {f[0], f[1], f[2]};
        lv.normal = {f[3], f[4], f[5]};
        lv.uv = {t[0], t[1]};
    }
    Reader ir(*ic);
    const std::uint32_t nidx = ir.get<std::uint32_t>();
    const std::uint8_t* idx = ir.skip(static_cast<std::size_t>(nidx) * 2);
    if (!idx) return;
    std::uint32_t first = 0, used = nidx;
    // A progressive part: its full-detail window.
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

bool readFile(const FileSystem& fs, const std::string& path, std::vector<std::uint8_t>& out,
              std::string& error) {
    if (fs.read(path, out)) return true;
    error = (fs.exists(path) ? "cannot unpack " : "no ") + path;
    return false;
}

} // namespace

bool readModel(const FileSystem& fs, const std::string& path, std::vector<ModelPart>& out,
               std::string& error) {
    out.clear();
    std::vector<std::uint8_t> bytes;
    if (!readFile(fs, path, bytes, error)) return false;
    collectModel(bytes.data(), bytes.size(), out, 0);
    if (out.empty()) { error = path + ": no geometry this reader knows"; return false; }
    return true;
}

glm::mat3 objectRotation(const glm::vec3& angle) {
    // setXYZ(x, y, z) is setHPB(y, x, z).
    const float h = angle.y, p = angle.x, b = angle.z;
    const float sh = std::sin(h), ch = std::cos(h), sp = std::sin(p), cp = std::cos(p),
                sb = std::sin(b), cb = std::cos(b);
    const float cc = ch * cb, cs = ch * sb, sc = sh * cb, ss = sh * sb;
    const glm::vec3 i(cc - sp * ss, -cp * sb, sp * cs + sc);
    const glm::vec3 j(sp * sc + cs, cp * cb, ss - sp * cc);
    const glm::vec3 k(-cp * sh, sp, cp * ch);
    return glm::mat3(i, j, k);   // columns: p.x*i + p.y*j + p.z*k
}

// level.spawn: one chunk per object, each a spawn packet.
static void readSpawnFile(const FileSystem& fs, const std::string& dir, std::vector<LevelSpawn>& out) {
    // level.spawn: one chunk per object, each a spawn packet. Its head is the
    // same for every class: message id, section, name, game type, respawn
    // point, position, angle.
    std::vector<std::uint8_t> spawn;
    if (fs.read(dir + "level.spawn", spawn)) {
        std::deque<std::vector<std::uint8_t>> sstore;
        for (const Chunk& c : chunksOf(spawn.data(), spawn.size(), sstore)) {
            Reader r(c);
            r.get<std::uint16_t>();   // M_SPAWN
            LevelSpawn s;
            s.section = r.str();
            s.name = r.str();
            r.get<std::uint8_t>();
            r.get<std::uint8_t>();
            const glm::vec3 p = r.vec3();
            s.angle = r.vec3();
            if (!r.ok()) continue;
            s.position = axes(p);
            s.gamePosition = p;
            // The rest of the packet's head, then the object's state, whose
            // first part (cse_alife_object) is the same for every class with a
            // model; the model's name follows it (cse_visual).
            r.get<std::uint16_t>();   // respawn time
            r.get<std::uint16_t>();   // id
            r.get<std::uint16_t>();   // parent
            r.get<std::uint16_t>();   // phantom
            const std::uint16_t flags = r.get<std::uint16_t>();
            const std::uint16_t version = (flags & 0x20) ? r.get<std::uint16_t>() : 0;
            if (version > 120) r.get<std::uint16_t>();       // game type
            if (version > 69) r.get<std::uint16_t>();        // script version
            if (version > 70) r.skip(r.get<std::uint16_t>()); // client data
            if (version > 79) r.get<std::uint16_t>();        // spawn id
            // The state's size counts its own two bytes.
            const std::size_t stateSize = r.get<std::uint16_t>();
            const std::size_t stateEnd = r.tell() + stateSize - std::min<std::size_t>(stateSize, 2);
            s.version = version;
            if (r.ok() && stateEnd - r.tell() <= r.left())
                s.state.assign(c.data + r.tell(), c.data + stateEnd);
            if (r.ok() && version > 111 && r.tell() + r.left() >= stateEnd) {
                r.get<std::uint16_t>();   // game vertex
                r.get<float>();           // distance
                r.get<std::uint32_t>();   // direct control
                r.get<std::uint32_t>();   // level vertex
                r.get<std::uint32_t>();   // flags
                r.str();                  // custom data
                r.get<std::uint32_t>();   // story id
                r.get<std::uint32_t>();   // spawn story id
                if (r.ok() && r.tell() < stateEnd) {
                    std::string v = r.str();
                    bool printable = !v.empty();
                    for (unsigned char ch : v) printable &= ch >= 32 && ch < 127;
                    if (r.ok() && r.tell() <= stateEnd && printable) s.visual = v;
                }
            }
            out.push_back(std::move(s));
        }
    }

}

bool readLevel(const FileSystem& fs, const std::string& name, Level& out, std::string& error) {
    out = Level{};
    out.name = name;
    const std::string dir = "levels/" + name + "/";
    std::vector<std::uint8_t> level, geom;
    if (!readFile(fs, dir + "level", level, error)) return false;
    if (!readFile(fs, dir + "level.geom", geom, error)) return false;

    std::deque<std::vector<std::uint8_t>> store;
    const std::vector<Chunk> lc = chunksOf(level.data(), level.size(), store);
    if (const Chunk* h = find(lc, 1); h && h->size >= 2) {
        std::uint16_t v;
        std::memcpy(&v, h->data, 2);
        out.version = v;
    }
    if (out.version < 12) {
        error = "level format " + std::to_string(out.version) +
                " (only xrLC 12-14: Clear Sky, Call of Pripyat, the Enhanced Editions)";
        return false;
    }

    // Shaders: "engine/texture[,lightmap,lightmap]". Index 0 is the empty one.
    if (const Chunk* sc = find(lc, 2)) {
        Reader r(*sc);
        const std::uint32_t n = r.get<std::uint32_t>();
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
            const std::string s = r.str();
            LevelShader sh;
            const std::size_t slash = s.find('/');
            sh.engine = s.substr(0, slash);
            if (slash != std::string::npos) {
                std::string rest = s.substr(slash + 1);
                std::size_t at = 0;
                bool first = true;
                while (true) {
                    const std::size_t comma = rest.find(',', at);
                    std::string part = rest.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                    if (first) sh.texture = part;
                    else sh.more.push_back(part);
                    first = false;
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
            }
            out.shaders.push_back(std::move(sh));
        }
    }

    std::deque<std::vector<std::uint8_t>> gstore;
    Geometry g;
    if (!readGeometry(chunksOf(geom.data(), geom.size(), gstore), g, error)) {
        error = "level.geom: " + error;
        return false;
    }

    const Chunk* vc = find(lc, 3);
    if (!vc) { error = "level: no visuals"; return false; }
    std::deque<std::vector<std::uint8_t>> vstore;
    for (const Chunk& v : chunksOf(vc->data, vc->size, vstore)) {
        LevelMesh m;
        bool leaf = false;
        if (readVisual(v.data, v.size, g, m, leaf)) {
            if (m.shader < 0 || m.shader >= static_cast<int>(out.shaders.size())) m.shader = 0;
            out.meshes.push_back(std::move(m));
        } else if (leaf) {
            ++out.skippedVisuals;
        }
    }

    // Lights: a controller id, then D3DLIGHT9.
    if (const Chunk* l = find(lc, 6)) {
        Reader r(*l);
        while (r.left() >= 108) {
            LevelLight L;
            L.controller = r.get<std::uint32_t>();
            L.type = static_cast<int>(r.get<std::uint32_t>());
            const float d[4] = {r.get<float>(), r.get<float>(), r.get<float>(), r.get<float>()};
            L.color = glm::vec3(d[0], d[1], d[2]);
            r.skip(32);   // specular, ambient
            L.position = axes(r.vec3());
            L.direction = axes(r.vec3());
            L.range = r.get<float>();
            L.falloff = r.get<float>();
            for (float& a : L.attenuation) a = r.get<float>();
            L.theta = r.get<float>();
            L.phi = r.get<float>();
            if (!r.ok()) break;
            out.lights.push_back(L);
        }
    }
    if (const Chunk* gl = find(lc, 7)) {
        Reader r(*gl);
        while (r.left() >= 18) {
            LevelGlow G;
            G.position = axes(r.vec3());
            G.radius = r.get<float>();
            G.shader = r.get<std::uint16_t>();
            out.glows.push_back(G);
        }
    }

    readSpawnFile(fs, dir, out.spawns);

    // level.game: the patrol paths, one chunk each inside chunk 0x1000.
    std::vector<std::uint8_t> game;
    if (fs.read(dir + "level.game", game)) {
        std::deque<std::vector<std::uint8_t>> gstore;
        for (const Chunk& top : chunksOf(game.data(), game.size(), gstore)) {
            if (top.id != 0x1000) continue;
            for (const Chunk& pc : chunksOf(top.data, top.size, gstore)) {
                const std::vector<Chunk> parts = chunksOf(pc.data, pc.size, gstore);
                const Chunk* name = find(parts, 5);
                const Chunk* pts = find(parts, 2);
                if (!name || !pts) continue;
                PatrolPath path;
                path.name = Reader(*name).str();
                Reader pr(*pts);
                const std::uint16_t n = pr.get<std::uint16_t>();
                for (std::uint16_t i = 0; i < n && pr.ok(); ++i) {
                    PatrolPoint pt;
                    pt.position = axes(pr.vec3());
                    pt.flags = pr.get<std::uint32_t>();
                    pt.name = pr.str();
                    if (pr.ok()) path.points.push_back(std::move(pt));
                }
                if (const Chunk* links = find(parts, 3)) {
                    Reader lr(*links);
                    const std::uint16_t nl = lr.get<std::uint16_t>();
                    for (std::uint16_t i = 0; i < nl && lr.ok(); ++i) {
                        PatrolPath::Link l;
                        l.from = lr.get<std::uint16_t>();
                        l.to = lr.get<std::uint16_t>();
                        l.probability = lr.get<float>();
                        if (lr.ok() && l.from < static_cast<int>(path.points.size()) &&
                            l.to < static_cast<int>(path.points.size()))
                            path.links.push_back(l);
                    }
                }
                if (!path.points.empty()) out.paths.push_back(std::move(path));
            }
        }
    }

    // level.snd_static: a chunk per sound, its fields in sub-chunk 0.
    std::vector<std::uint8_t> snd;
    if (fs.read(dir + "level.snd_static", snd)) {
        std::deque<std::vector<std::uint8_t>> sstore2;
        for (const Chunk& c : chunksOf(snd.data(), snd.size(), sstore2)) {
            const std::vector<Chunk> parts = chunksOf(c.data, c.size, sstore2);
            const Chunk* body = find(parts, 0);
            if (!body) continue;
            Reader r(*body);
            StaticSound ss;
            ss.name = r.str();
            ss.position = axes(r.vec3());
            ss.volume = r.get<float>();
            ss.frequency = r.get<float>();
            for (std::uint32_t* t : {ss.activeTime, ss.playTime, ss.pauseTime}) {
                t[0] = r.get<std::uint32_t>();
                t[1] = r.get<std::uint32_t>();
            }
            if (r.ok() && !ss.name.empty()) out.sounds.push_back(std::move(ss));
        }
    }
    return true;
}

bool readSpawns(const FileSystem& fs, const std::string& level, std::vector<LevelSpawn>& out) {
    out.clear();
    readSpawnFile(fs, "levels/" + level + "/", out);
    return !out.empty();
}

bool readState(const LevelSpawn& sp, StateLayout layout, SpawnState& out) {
    out = SpawnState{};
    Reader r(sp.state.data(), sp.state.size());
    const int v = sp.version;
    if (v <= 111) return false;   // older packets lay their objects out otherwise
    auto object = [&] {
        r.get<std::uint16_t>();   // game vertex
        r.get<float>();           // distance
        r.get<std::uint32_t>();   // direct control
        r.get<std::uint32_t>();   // level vertex
        r.get<std::uint32_t>();   // flags
        out.custom = r.str();
        out.story = r.get<std::uint32_t>();
        r.get<std::uint32_t>();   // spawn story id
    };
    auto visual = [&] {
        std::string vis = r.str();
        if (v > 103) r.get<std::uint8_t>();
        bool printable = !vis.empty();
        for (unsigned char ch : vis) printable &= ch >= 32 && ch < 127;
        if (r.ok() && printable) out.visual = vis;
    };
    auto trader = [&] {
        r.get<std::uint32_t>();   // money
        r.str();                  // specific character
        r.get<std::uint32_t>();   // trader flags
        out.profile = r.str();
        r.get<std::int32_t>();    // community
        r.get<std::int32_t>();    // rank
        r.get<std::int32_t>();    // reputation
        if (v > 104) r.str();     // character name
    };
    auto shape = [&] {
        const std::uint8_t n = r.get<std::uint8_t>();
        for (std::uint8_t i = 0; i < n && r.ok(); ++i) {
            SpawnShape sh;
            const std::uint8_t type = r.get<std::uint8_t>();
            if (type == 0) {
                sh.center = r.vec3();
                sh.radius = r.get<float>();
            } else if (type == 1) {
                sh.box = true;
                const glm::vec3 i3 = r.vec3(), j3 = r.vec3(), k3 = r.vec3();
                sh.axes = glm::mat3(i3, j3, k3);
                sh.offset = r.vec3();
            } else {
                r.skip(r.left() + 1);   // unknown: fail
            }
            if (r.ok()) out.shapes.push_back(sh);
        }
        if (v > 74) out.restrictorType = r.get<std::uint8_t>();
    };
    switch (layout) {
        case StateLayout::Object:
            object();
            if (r.ok() && r.left() > 0) visual();
            return r.ok();
        case StateLayout::Human:
            trader();
            object();
            visual();
            return r.ok();
        case StateLayout::Trader:
            object();
            visual();
            trader();
            return r.ok();
        case StateLayout::Restrictor:
            object();
            shape();
            return r.ok();
        case StateLayout::LevelChanger:
            object();
            shape();
            r.get<std::uint16_t>();   // destination game vertex
            r.get<std::uint32_t>();   // destination level vertex
            out.destPosition = r.vec3();
            out.destDirection = r.vec3();
            out.destLevel = r.str();
            out.destPoint = r.str();
            return r.ok();
        case StateLayout::Zone:
            object();
            shape();
            out.power = r.get<float>();
            return r.ok();
    }
    return false;
}

} // namespace xray
