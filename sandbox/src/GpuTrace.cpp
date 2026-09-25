#include "GpuTrace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

#include <glad/gl.h>
#include <glm/gtc/constants.hpp>

#include <fitzel/core/GlCaps.hpp>

namespace gputrace {
namespace {

// The GPU's view of the scene. Every member padded to a vec4 rather than packed
// tight: std430 would allow tighter, and the tightest layout is where a GPU
// port spends its debugging time. A vec3 inside an array is 16 bytes on the
// device and 12 on the host, and the picture that comes out of getting that
// wrong looks like a broken accelerator rather than like a padding mistake.
// What a hit needs once it has been FOUND: normals, UVs, material. The corners
// are deliberately not here -- they live in their own tight buffer (`pos`, in
// upload) because the traversal reads nothing but corners and reads them
// constantly, while this is opened once per bounce. Same bytes per triangle
// either way; the difference is how many cache lines a leaf full of them
// touches.
struct GpuTri {
    float n0[4], n1[4], n2[4];
    float uv01[4];
    float uv2mat[4];
};
static_assert(sizeof(GpuTri) == 80, "GpuTri must match its std430 counterpart");

struct GpuNode {
    float lo[4];   // .w = leftFirst, bit-cast
    float hi[4];   // .w = count, bit-cast
};
static_assert(sizeof(GpuNode) == 32, "GpuNode must match its std430 counterpart");

struct GpuMat {
    float albedoRough[4];
    float emissionRefl[4];
    float misc[4];
    float tint[4];      // rgb: multiplies the base-colour map; a = detail scale
    float extra[4];     // x = translucency, y = additive (1 = a glow)
    float absorb[4];    // rgb = extinction per metre of the body (glass only)
    float medium[4];    // rgb = the body's own colour, sRGB
    // An enum in its own integer rather than squeezed into a float and rounded
    // back: that trick works until the day somebody adds a fourth mode.
    // y is the base-colour map, -1 for a material that has none.
    int   modeTex[4];   // x = AlphaMode (0 opaque, 1 cutout, 2 blend), y = texture
};
static_assert(sizeof(GpuMat) == 128, "GpuMat must match its std430 counterpart");

// One placement: world -> object as three rows of a 3x4, the normal matrix
// (object -> world, inverse transpose) as three rows, and where the mesh's tree
// starts plus the material that overrides its own.
struct GpuInst {
    float toObj[3][4];
    float nrm[3][4];
    int   meta[4];      // x = root node, y = material override (-1 none)
};
static_assert(sizeof(GpuInst) == 112, "GpuInst must match its std430 counterpart");

// The widest panorama handed to the card. A 4K HDRI is a hundred megabytes of
// floats, which is more than a preview has any business taking and more than
// some drivers will put in one buffer; halving it costs detail in the sky's
// background and nothing in its light, which comes from the coarse
// distribution either way.
constexpr int kMaxPanoramaWidth = 2048;

// Box-halve an RGB float panorama until it fits.
void shrinkPanorama(std::vector<float>& px, int& w, int& h) {
    while (w > kMaxPanoramaWidth && h > 1) {
        const int nw = w / 2, nh = h / 2;
        std::vector<float> out(static_cast<std::size_t>(nw) * nh * 3);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 3; ++c) {
                    auto at = [&](int xx, int yy) {
                        return px[(static_cast<std::size_t>(yy) * w + xx) * 3 + c];
                    };
                    out[(static_cast<std::size_t>(y) * nw + x) * 3 + c] =
                        0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) +
                                 at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
                }
        px.swap(out);
        w = nw;
        h = nh;
    }
}

// One terrain layer: the height/slope window it covers, how it tiles, and which
// map it is. Kept in one flat array for the whole scene, with each material
// naming a run of it -- the same shape as a BVH's index list, and for the same
// reason: an array of arrays is not a thing std430 hands over cheaply.
struct GpuLayer {
    float band[4];      // height start/end, slope start/end (degrees)
    float scaleTex[4];  // x = world->texture scale, y = texture index, as a float,
                        // z = the material's height blend, w = 1 on the forest floor
};
static_assert(sizeof(GpuLayer) == 32, "GpuLayer must match its std430 counterpart");

// One texture's place in the pixel blob: where it starts (in TEXELS, not bytes)
// and how big it is. The blob is one buffer for all of them because a compute
// shader cannot index an array of samplers by a value it computed -- and the
// material index at a hit is exactly such a value.
struct GpuTexMeta {
    int off, w, h, pad;
};
static_assert(sizeof(GpuTexMeta) == 16, "GpuTexMeta must match its std430 counterpart");

// The ceiling on the pixel blob, in texels. A scene with fifty 2K maps would
// otherwise ask the driver for more than it will hand out in one buffer, and the
// failure mode of THAT is the whole upload failing -- no preview at all -- rather
// than a few surfaces falling back to their flat colour. Textures past the line
// are dropped and the caller is told how many.
//
// Asked of the driver rather than assumed, because the answer varies by an order
// of magnitude between cards, with our own ceiling on top: a buffer the driver
// would accept can still be more memory than a preview has any business taking.
std::size_t maxTextureTexels() {
    GLint64 maxBlock = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &maxBlock);
    if (maxBlock <= 0) maxBlock = 128 * 1024 * 1024;         // a cautious default
    const std::size_t byDriver = static_cast<std::size_t>(maxBlock) / 4u;
    return std::min<std::size_t>(byDriver, 256u * 1024u * 1024u / 4u);  // 256 MB
}

struct GpuLamp {
    float posRange[4];
    float dirRadius[4];
    float color[4];
    float cone[4];
};
static_assert(sizeof(GpuLamp) == 64, "GpuLamp must match its std430 counterpart");

float asFloat(int i) {
    float f = 0.0f;
    static_assert(sizeof(f) == sizeof(i), "int and float must be the same width");
    std::memcpy(&f, &i, sizeof f);
    return f;
}

void put3(float* dst, const glm::vec3& v, float w = 0.0f) {
    dst[0] = v.x; dst[1] = v.y; dst[2] = v.z; dst[3] = w;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Empty GL's error queue before doing something whose success is about to be
// judged by it.
//
// The flag is global, sticky and shared with everything else drawing in this
// context. Inside the editor a whole frame has already gone by before the
// preview is serviced, and anything it left in there is still sitting in it --
// so reading glGetError() straight after a dispatch answers "did ANYBODY err
// recently", not "did this work". That is not a hypothetical: it read an
// unrelated GL_INVALID_ENUM as its own and switched the GPU tracer off on a
// machine that could run it perfectly well, on every frame, silently falling
// back to the CPU. tracecheck leaves an error behind on purpose now.
void drainErrors() {
    for (int i = 0; i < 64 && glGetError() != GL_NO_ERROR; ++i) {}
}

void setVec3(unsigned program, const char* name, const glm::vec3& v) {
    glUniform3f(glGetUniformLocation(program, name), v.x, v.y, v.z);
}
void setFloat(unsigned program, const char* name, float v) {
    glUniform1f(glGetUniformLocation(program, name), v);
}
void setInt(unsigned program, const char* name, int v) {
    glUniform1i(glGetUniformLocation(program, name), v);
}

} // namespace

bool available() { return fitzel::glcaps::compute(); }

Tracer::~Tracer() { release(); }

void Tracer::release() {
    for (auto& kv : m_programs) if (kv.second) glDeleteProgram(kv.second);
    m_programs.clear();
    if (m_resolve) { glDeleteProgram(m_resolve); m_resolve = 0; }
    if (m_ldr)     { glDeleteTextures(1, &m_ldr); m_ldr = 0; }
    if (m_accum)   glDeleteTextures(1, &m_accum);
    if (m_query)   { glDeleteQueries(1, &m_query); m_query = 0; m_queryPending = false; }
    for (std::uint32_t& b : m_ssbo)
        if (b) { glDeleteBuffers(1, &b); b = 0; }
    m_program = 0;
    m_accum   = 0;
}

bool Tracer::init(const std::string& shaderPath, const std::string& resolvePath) {
    if (!available()) {
        m_error = "this context is OpenGL " +
                  std::to_string(fitzel::glcaps::majorVersion()) + "." +
                  std::to_string(fitzel::glcaps::minorVersion()) +
                  "; the GPU tracer needs 4.3 (compute shaders)";
        return false;
    }
    m_source = readFile(shaderPath);
    if (m_source.empty()) {
        m_error = "could not read the kernel: " + shaderPath;
        return false;
    }
    // Built once here at the safe size, so init() still answers "does the kernel
    // build" before any scene exists. upload() rebuilds it smaller as soon as it
    // has a tree to measure.
    if (!build(64)) return false;

    // The resolve pass, beside the kernel unless told otherwise.
    std::string rp = resolvePath;
    if (rp.empty()) {
        const std::size_t slash = shaderPath.find_last_of("/\\");
        rp = (slash == std::string::npos ? std::string()
                                         : shaderPath.substr(0, slash + 1)) +
             "gpuresolve.comp";
    }
    const std::string rsrc = readFile(rp);
    if (rsrc.empty()) { m_error = "could not read the resolve pass: " + rp; return false; }
    const std::uint32_t traced = m_program;   // compile() writes into m_program
    if (!compile(rsrc)) return false;
    m_resolve = m_program;
    m_program = traced;
    return true;
}

// Compile the kernel with `stackSize` traversal entries per thread. Cheap
// enough to do on a scene change (tens of milliseconds) and worth it: half the
// speed rides on this number, and only a scene can say what it has to be.
bool Tracer::build(int stackSize) {
    if (m_source.empty()) { m_error = "no kernel source to build"; return false; }
    // Already built once at this size? Then it is a pointer, not a compile.
    // This driver takes the better part of a second over this kernel, and a
    // scene being edited crosses a depth bracket and crosses back -- paying that
    // twice for the same program would be a visible stall in the middle of
    // dragging something.
    auto cached = m_programs.find(stackSize);
    if (cached != m_programs.end()) {
        m_program   = cached->second;
        m_stackSize = stackSize;
        return true;
    }
    // The define goes AFTER the #version line, which GLSL insists comes first.
    std::string src = m_source;
    const std::string def =
        "#define TRAVERSAL_STACK " + std::to_string(stackSize) + "\n";
    const std::size_t nl = src.find('\n');
    if (nl != std::string::npos) src.insert(nl + 1, def);
    else                         src = def + src;
    if (!compile(src)) return false;
    m_programs[stackSize] = m_program;
    m_stackSize = stackSize;
    return true;
}

bool Tracer::compile(const std::string& src) {
    const GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    const char*  cstr   = src.c_str();
    glShaderSource(shader, 1, &cstr, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        // The log, verbatim. A shader that will not build is the one case where
        // a summary is worth nothing: the line number is the whole message.
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetShaderInfoLog(shader, len, nullptr, log.data());
        glDeleteShader(shader);
        m_error = "gputrace.comp did not compile:\n" + log;
        return false;
    }

    const GLuint prog = glCreateProgram();
    glAttachShader(prog, shader);
    glLinkProgram(prog);
    glDeleteShader(shader);
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        glDeleteProgram(prog);
        m_error = "gputrace.comp did not link:\n" + log;
        return false;
    }

    // Not deleted: the previous program is owned by m_programs (or is about to
    // be put there), and it is kept so that going back to that stack size is a
    // lookup rather than another compile.
    m_program = prog;
    return true;
}

// How deep the tree actually goes, which is exactly how many entries a walk of
// it can have on its stack at once: one far child per level of the descent.
int treeDepth(const std::vector<pathtrace::BvhNode>& nodes) {
    if (nodes.empty()) return 0;
    // Iteratively, with an explicit stack of (node, depth): a tree deep enough
    // for this number to matter is deep enough not to recurse over.
    std::vector<std::pair<int, int>> work{{0, 1}};
    int deepest = 1;
    while (!work.empty()) {
        const int n = work.back().first, d = work.back().second;
        work.pop_back();
        deepest = std::max(deepest, d);
        if (n < 0 || n >= static_cast<int>(nodes.size())) continue;
        if (nodes[n].count > 0) continue;                    // a leaf
        work.push_back({nodes[n].leftFirst,     d + 1});
        work.push_back({nodes[n].leftFirst + 1, d + 1});
    }
    return deepest;
}

// The stack the kernel is built with for a tree of that depth. Rounded up to one
// of four sizes, so that editing a scene does not recompile the kernel every
// time a triangle moves, and capped where the CPU tracer's own stack is -- past
// that the two would be disagreeing about a tree neither of them can walk.
int stackSizeFor(int depth) {
    // Rounded up to a multiple of four rather than to a power of two: the cost
    // is smooth in this number, so a tree eighteen deep paying for thirty-two
    // entries is throwing away most of the difference between them for nothing.
    // Four wide keeps a scene being edited from recompiling every time its tree
    // shifts a level. Floor of eight, and capped where the CPU tracer's own
    // stack is -- past that the two would disagree about a tree neither can walk.
    const int rounded = ((std::max(depth, 8) + 3) / 4) * 4;
    return std::min(rounded, 128);
}

bool Tracer::uploadBuffer(std::uint32_t& id, int binding, const void* data,
                          std::size_t bytes) {
    drainErrors();
    if (!id) glGenBuffers(1, &id);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, id);
    // Never zero bytes: a binding of size 0 is not a buffer the driver has to
    // accept, and an empty lamp list is the normal case rather than an error.
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(std::max<std::size_t>(bytes, 16)),
                 bytes ? data : nullptr, GL_STATIC_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(binding), id);
    return glGetError() == GL_NO_ERROR;
}

bool Tracer::upload(const pathtrace::Scene& scene) {
    if (!m_program) { m_error = "upload() before a kernel was built"; return false; }
    if (scene.triangleCount() <= 0) {
        m_error = "the scene has no triangles";
        return false;
    }

    // The tracer's own accelerator, both levels of it, not one of ours. See
    // BvhNode in PathTrace.hpp for why that matters more than it looks.
    const pathtrace::AccelData acc = pathtrace::buildAccel(scene);

    // --- One address space for every tree ---------------------------------
    // The world's triangles first, then each mesh's; the leaf slots (index /
    // corners) in the same order, since a tree has exactly one slot per
    // triangle; the nodes of the world's tree, then each mesh's, then the top
    // level's. Every tree's pointers are rebased onto that layout here, once,
    // so the kernel walks them all with the one loop.
    const int worldTris = static_cast<int>(scene.triangles.size());
    std::vector<int> triBase(scene.meshes.size(), 0), nodeBase(scene.meshes.size(), 0);
    int triCount  = worldTris;
    int nodeCount = static_cast<int>(acc.world.nodes.size());
    for (std::size_t k = 0; k < scene.meshes.size(); ++k) {
        triBase[k]  = triCount;
        nodeBase[k] = nodeCount;
        triCount   += static_cast<int>(scene.meshes[k].triangles.size());
        nodeCount  += static_cast<int>(acc.meshes[k].nodes.size());
    }
    const bool haveTop  = !acc.instances.nodes.empty();
    const int  tlasBase = nodeCount;
    nodeCount += static_cast<int>(acc.instances.nodes.size());

    // How deep the trees are decides how much scratch memory each thread needs,
    // and that decides how many threads the card runs at once. Measured rather
    // than assumed, and the kernel rebuilt when the answer changes bracket.
    int depth = treeDepth(acc.world.nodes);
    for (const pathtrace::BvhData& m : acc.meshes) depth = std::max(depth, treeDepth(m.nodes));
    m_bvhDepth  = depth;
    m_tlasDepth = haveTop ? treeDepth(acc.instances.nodes) : 0;
    // One stack walks both levels (see the kernel's closest()): the top
    // level's depth, the widest top-level leaf's enter markers, one exit
    // marker, and then the deepest mesh tree under it.
    int widestTop = 0;
    for (const pathtrace::BvhNode& n : acc.instances.nodes) widestTop = std::max(widestTop, n.count);
    const int want = stackSizeFor(m_bvhDepth + (haveTop ? m_tlasDepth + widestTop + 1 : 0));
    if (want != m_stackSize && !build(want)) return false;

    auto triAt = [&](int id) -> const pathtrace::Triangle& {
        if (id < worldTris) return scene.triangles[static_cast<std::size_t>(id)];
        // Which mesh: the last base not past the id.
        const auto it = std::upper_bound(triBase.begin(), triBase.end(), id);
        const std::size_t k = static_cast<std::size_t>(it - triBase.begin()) - 1;
        return scene.meshes[k].triangles[static_cast<std::size_t>(id - triBase[k])];
    };

    std::vector<GpuTri> tris(static_cast<std::size_t>(triCount));
    for (int i = 0; i < triCount; ++i) {
        const pathtrace::Triangle& t = triAt(i);
        GpuTri& g = tris[static_cast<std::size_t>(i)];
        put3(g.n0, t.n0); put3(g.n1, t.n1); put3(g.n2, t.n2);
        g.uv01[0] = t.uv0.x; g.uv01[1] = t.uv0.y;
        g.uv01[2] = t.uv1.x; g.uv01[3] = t.uv1.y;
        g.uv2mat[0] = t.uv2.x;
        g.uv2mat[1] = t.uv2.y;
        g.uv2mat[2] = static_cast<float>(t.material);
        g.uv2mat[3] = 0.0f;
    }

    // Leaf slots: which triangle each one is, and its corners as p0/e1/e2 --
    // the form the intersection wants, which also saves it two subtractions per
    // test. In slot order, so a leaf is one contiguous run; this is what the
    // traversal reads and all it reads.
    std::vector<int>   index(static_cast<std::size_t>(triCount));
    std::vector<float> pos(static_cast<std::size_t>(triCount) * 12);
    auto putSlots = [&](const pathtrace::BvhData& b, int base) {
        for (std::size_t s = 0; s < b.index.size(); ++s) {
            const int id = b.index[s] + base;
            const std::size_t slot = static_cast<std::size_t>(base) + s;
            index[slot] = id;
            const pathtrace::Triangle& t = triAt(id);
            put3(&pos[slot * 12 + 0], t.p0);
            put3(&pos[slot * 12 + 4], t.p1 - t.p0);
            put3(&pos[slot * 12 + 8], t.p2 - t.p0);
        }
    };
    putSlots(acc.world, 0);
    for (std::size_t k = 0; k < acc.meshes.size(); ++k)
        putSlots(acc.meshes[k], triBase[k]);

    std::vector<GpuNode> nodes(static_cast<std::size_t>(std::max(nodeCount, 1)));
    auto putNodes = [&](const std::vector<pathtrace::BvhNode>& src, int nodeOff,
                        int slotOff, bool top) {
        for (std::size_t i = 0; i < src.size(); ++i) {
            const pathtrace::BvhNode& n = src[i];
            const int lf = n.count > 0 ? n.leftFirst + slotOff : n.leftFirst + nodeOff;
            GpuNode& g = nodes[static_cast<std::size_t>(nodeOff) + i];
            put3(g.lo, n.lo, asFloat(lf));
            // A top-level leaf holds instances, and says so by the sign of its
            // count: the kernel walks both levels in one loop and this is how
            // it knows which kind of leaf it is standing on.
            put3(g.hi, n.hi, asFloat(top && n.count > 0 ? -n.count : n.count));
        }
    };
    putNodes(acc.world.nodes, 0, 0, false);
    for (std::size_t k = 0; k < acc.meshes.size(); ++k)
        putNodes(acc.meshes[k].nodes, nodeBase[k], triBase[k], false);
    // The top level's leaves name instance SLOTS -- the instances are laid out
    // below in its leaf order -- so only its inner pointers move.
    putNodes(acc.instances.nodes, tlasBase, 0, true);

    std::vector<GpuInst> insts(std::max<std::size_t>(acc.instances.index.size(), 1));
    for (std::size_t j = 0; j < acc.instances.index.size(); ++j) {
        const pathtrace::Instance& in =
            scene.instances[static_cast<std::size_t>(acc.instances.index[j])];
        const glm::mat4 inv = glm::inverse(in.transform);
        const glm::mat3 nrm = glm::transpose(glm::inverse(glm::mat3(in.transform)));
        GpuInst& g = insts[j];
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 4; ++c) g.toObj[r][c] = inv[c][r];
            for (int c = 0; c < 3; ++c) g.nrm[r][c] = nrm[c][r];
            g.nrm[r][3] = 0.0f;
        }
        g.meta[0] = nodeBase[static_cast<std::size_t>(in.mesh)];
        g.meta[1] = in.material;
        g.meta[2] = 0;
        g.meta[3] = 0;
    }

    std::vector<GpuMat> mats(std::max<std::size_t>(scene.materials.size(), 1));
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
        const pathtrace::Material& m = scene.materials[i];
        GpuMat& g = mats[i];
        put3(g.albedoRough, m.albedo, m.roughness);
        put3(g.emissionRefl, m.emission, m.reflectivity);
        g.misc[0] = m.opacity;
        g.misc[1] = m.emissionStrength;
        g.misc[2] = m.alphaCutoff;
        // The index of refraction doubles as the flag: 0 is "not glass",
        // and anything above 1 is both "glass" and how much it bends. One
        // slot, and no way for the two to disagree.
        g.misc[3] = m.glass ? std::max(1.0f, m.ior) : 0.0f;
        put3(g.tint, m.tint, m.detailScale);
        g.extra[0] = m.translucency;
        g.extra[1] = m.additive ? 1.0f : 0.0f;
        g.extra[2] = m.farGround ? 1.0f : 0.0f;
        g.extra[3] = 0.0f;
        put3(g.absorb, m.absorption);
        put3(g.medium, m.mediumColor);
        g.modeTex[0] = m.alphaMode;
        g.modeTex[1] = -1;              // filled below, once the blob is packed
        g.modeTex[2] = 0;               // ...and so are the layers
        g.modeTex[3] = 0;
    }

    // --- The textures, packed into one blob ---------------------------------
    // RGBA8 as one uint per texel, in the order the CPU tracer's Image holds
    // them, so the sampling code on the other side is the same arithmetic on the
    // same bytes rather than a second interpretation of them.
    //
    // What does not fit is SHRUNK, not dropped. Dropping was the first version,
    // and on a real landscape -- half a gigabyte of maps -- it took the ones that
    // happened to come last, which were the forest's: every tree in the preview
    // turned white. So each map gets a level (how many times it is halved), the
    // biggest map is halved until the whole set fits, and every surface keeps
    // its colour at whatever resolution the card has room for.
    std::vector<GpuTexMeta>    texMeta;
    std::vector<std::uint32_t> texels;
    std::vector<int>           texSlot(scene.textures.size(), -1);
    const std::size_t          texelCap = m_texelBudget > 0 ? m_texelBudget : maxTextureTexels();
    m_texturesDropped = 0;
    m_texturesShrunk  = 0;
    std::vector<int> level(scene.textures.size(), 0);
    auto dims = [&](std::size_t i) {
        const pathtrace::Image& img = scene.textures[i];
        return glm::ivec2(std::max(1, img.width >> level[i]), std::max(1, img.height >> level[i]));
    };
    auto usable = [&](std::size_t i) {
        const pathtrace::Image& img = scene.textures[i];
        return img.width > 0 && img.height > 0 &&
               img.pixels.size() >= static_cast<std::size_t>(img.width) * img.height * 4;
    };
    {
        std::size_t total = 0;
        for (std::size_t i = 0; i < scene.textures.size(); ++i)
            if (usable(i)) { const glm::ivec2 d = dims(i); total += static_cast<std::size_t>(d.x) * d.y; }
        while (total > texelCap) {
            std::size_t big = scene.textures.size();
            std::size_t bigCount = 0;
            for (std::size_t i = 0; i < scene.textures.size(); ++i) {
                if (!usable(i)) continue;
                const glm::ivec2 d = dims(i);
                const std::size_t c = static_cast<std::size_t>(d.x) * d.y;
                if (c > bigCount && d.x > 8 && d.y > 8) { bigCount = c; big = i; }
            }
            if (big == scene.textures.size()) break;   // nothing left worth halving
            ++level[big];
            const glm::ivec2 d = dims(big);
            total = total - bigCount + static_cast<std::size_t>(d.x) * d.y;
        }
    }
    for (std::size_t i = 0; i < scene.textures.size(); ++i) {
        if (!usable(i)) continue;
        const pathtrace::Image& img = scene.textures[i];
        const glm::ivec2 d = dims(i);
        const std::size_t count = static_cast<std::size_t>(d.x) * d.y;
        if (texels.size() + count > texelCap) { ++m_texturesDropped; continue; }
        if (level[i] > 0) ++m_texturesShrunk;
        texSlot[i] = static_cast<int>(texMeta.size());
        texMeta.push_back(GpuTexMeta{static_cast<int>(texels.size()), d.x, d.y, 0});
        texels.reserve(texels.size() + count);
        // A box filter over the 2^level square each texel stands for -- the
        // same averaging the harvest's own downsample does.
        const int f = 1 << level[i];
        for (int y = 0; y < d.y; ++y)
            for (int x = 0; x < d.x; ++x) {
                unsigned acc[4] = {0, 0, 0, 0};
                unsigned n = 0;
                for (int by = 0; by < f; ++by) {
                    const int sy = y * f + by;
                    if (sy >= img.height) break;
                    for (int bx = 0; bx < f; ++bx) {
                        const int sx = x * f + bx;
                        if (sx >= img.width) break;
                        const unsigned char* q =
                            &img.pixels[(static_cast<std::size_t>(sy) * img.width + sx) * 4];
                        for (int c = 0; c < 4; ++c) acc[c] += q[c];
                        ++n;
                    }
                }
                n = std::max(n, 1u);
                texels.push_back((acc[0] / n) | ((acc[1] / n) << 8) | ((acc[2] / n) << 16) |
                                 ((acc[3] / n) << 24));
            }
    }
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
        const int t = scene.materials[i].texture;
        mats[i].modeTex[1] = (t >= 0 && t < static_cast<int>(texSlot.size()))
                                 ? texSlot[t] : -1;
    }
    m_textureCount = static_cast<int>(texMeta.size());

    // --- Terrain layers -----------------------------------------------------
    // Flattened: every material's layers laid end to end, the material keeping
    // only where its run starts and how long it is.
    std::vector<GpuLayer> layers;
    for (std::size_t i = 0; i < scene.materials.size(); ++i) {
        const pathtrace::Material& m = scene.materials[i];
        if (m.layers.empty()) continue;
        mats[i].modeTex[2] = static_cast<int>(layers.size());
        int kept = 0;
        int li = -1;
        for (const pathtrace::TerrainLayer& L : m.layers) {
            ++li;
            // A layer whose map did not fit the blob is dropped rather than
            // left pointing at nothing: it would otherwise contribute white.
            const int slot = (L.texture >= 0 && L.texture < static_cast<int>(texSlot.size()))
                                 ? texSlot[L.texture] : -1;
            if (slot < 0) continue;
            GpuLayer g{};
            g.band[0] = L.band.x; g.band[1] = L.band.y;
            g.band[2] = L.band.z; g.band[3] = L.band.w;
            g.scaleTex[0] = L.scale;
            g.scaleTex[1] = static_cast<float>(slot);
            g.scaleTex[2] = m.heightBlend;   // the material's, repeated per layer
            // Flagged on the layer rather than named by index: a layer that did
            // not fit the blob is dropped, and every index after it would move.
            g.scaleTex[3] = li == scene.ground.forestLayer ? 1.0f : 0.0f;
            layers.push_back(g);
            ++kept;
        }
        mats[i].modeTex[3] = kept;
    }
    m_layerCount = static_cast<int>(layers.size());

    // --- The terrain's hand-painted weights ---------------------------------
    // Three per WORLD triangle, in the triangles' own order -- so a hit's
    // triangle index reaches them without a second table, exactly as paintAt()
    // does on the CPU. Absent on every scene whose terrain nobody has painted,
    // which is most of them, and then it is one dummy entry and a switched-off
    // flag.
    const bool havePaint = worldTris > 0 &&
        scene.vertexPaint.size() >= static_cast<std::size_t>(worldTris) * 3;
    m_havePaint = havePaint;
    std::vector<float> paint;
    if (havePaint) {
        paint.reserve(scene.vertexPaint.size() * 4);
        for (const glm::vec4& w : scene.vertexPaint) {
            paint.push_back(w.x); paint.push_back(w.y);
            paint.push_back(w.z); paint.push_back(w.w);
        }
    }

    // --- The sky --------------------------------------------------------------
    // The lighting panorama, the CPU's importance distribution over it (built
    // from the FULL panorama, so the card aims where the CPU aims), and the
    // backdrop, one after another in one float buffer.
    std::vector<float> env;
    m_envSize = glm::ivec2(0);
    m_distSize = glm::ivec2(0);
    m_backSize = glm::ivec2(0);
    m_envPixOff = m_distFuncOff = m_distCondOff = m_distMargOff = m_backOff = 0;
    m_distTotal = 0.0f;
    if (scene.env.hasMap()) {
        const pathtrace::EnvDistribution dist = pathtrace::buildEnvDistribution(scene.env);
        std::vector<float> px = scene.env.pixels;
        int w = scene.env.width, h = scene.env.height;
        shrinkPanorama(px, w, h);
        m_envPixOff = static_cast<int>(env.size());
        env.insert(env.end(), px.begin(), px.end());
        m_envSize = glm::ivec2(w, h);
        if (dist.valid()) {
            m_distFuncOff = static_cast<int>(env.size());
            env.insert(env.end(), dist.func.begin(), dist.func.end());
            m_distCondOff = static_cast<int>(env.size());
            env.insert(env.end(), dist.condCdf.begin(), dist.condCdf.end());
            m_distMargOff = static_cast<int>(env.size());
            env.insert(env.end(), dist.margCdf.begin(), dist.margCdf.end());
            m_distSize  = glm::ivec2(dist.w, dist.h);
            m_distTotal = dist.total;
        }
    }
    m_maskSize = glm::ivec2(0);
    m_maskOff  = 0;
    if (scene.sunMask.valid()) {
        m_maskOff  = static_cast<int>(env.size());
        env.insert(env.end(), scene.sunMask.values.begin(), scene.sunMask.values.end());
        m_maskSize = glm::ivec2(scene.sunMask.width, scene.sunMask.height);
        m_maskRect = glm::vec4(scene.sunMask.origin, 1.0f / std::max(scene.sunMask.size, 1e-3f),
                               scene.sunMask.refY);
        m_maskSun  = scene.sunMask.sunDir;
    }
    m_mediumGlow = pathtrace::mediumLight(scene);
    m_ground = scene.ground;
    m_ground.moisture.clear();       // lives in the buffer, not twice in memory
    m_moistOff = 0;
    if (scene.ground.moistSamples > 0 &&
        scene.ground.moisture.size() >= static_cast<std::size_t>(scene.ground.moistSamples) *
                                            scene.ground.moistSamples) {
        m_moistOff = static_cast<int>(env.size());
        env.insert(env.end(), scene.ground.moisture.begin(), scene.ground.moisture.end());
    } else {
        m_ground.moistSamples = 0;
    }
    if (scene.env.hasBackdrop()) {
        std::vector<float> px = scene.env.backdrop;
        int w = scene.env.backdropWidth, h = scene.env.backdropHeight;
        shrinkPanorama(px, w, h);
        m_backOff = static_cast<int>(env.size());
        env.insert(env.end(), px.begin(), px.end());
        m_backSize = glm::ivec2(w, h);
    }

    std::vector<GpuLamp> lamps(scene.lamps.size());
    for (std::size_t i = 0; i < scene.lamps.size(); ++i) {
        const pathtrace::Lamp& l = scene.lamps[i];
        GpuLamp& g = lamps[i];
        put3(g.posRange, l.position, l.range);
        put3(g.dirRadius, l.direction, l.radius);
        put3(g.color, l.color);
        g.cone[0] = l.cosInner;
        g.cone[1] = l.cosOuter;
        g.cone[2] = 0.0f;
        g.cone[3] = 0.0f;
    }

    bool ok = true;
    ok = ok && uploadBuffer(m_ssbo[0], 1, tris.data(),  tris.size()  * sizeof(GpuTri));
    ok = ok && uploadBuffer(m_ssbo[1], 2, nodes.data(), nodes.size() * sizeof(GpuNode));
    ok = ok && uploadBuffer(m_ssbo[2], 3, index.data(), index.size() * sizeof(int));
    ok = ok && uploadBuffer(m_ssbo[3], 4, mats.data(),  mats.size()  * sizeof(GpuMat));
    ok = ok && uploadBuffer(m_ssbo[4], 5, lamps.data(), lamps.size() * sizeof(GpuLamp));
    ok = ok && uploadBuffer(m_ssbo[5], 6, texMeta.data(),
                            texMeta.size() * sizeof(GpuTexMeta));
    ok = ok && uploadBuffer(m_ssbo[6], 7, texels.data(),
                            texels.size() * sizeof(std::uint32_t));
    ok = ok && uploadBuffer(m_ssbo[7], 8, layers.data(),
                            layers.size() * sizeof(GpuLayer));
    ok = ok && uploadBuffer(m_ssbo[8], 9, paint.data(), paint.size() * sizeof(float));
    ok = ok && uploadBuffer(m_ssbo[9], 10, pos.data(), pos.size() * sizeof(float));
    ok = ok && uploadBuffer(m_ssbo[10], 11, insts.data(), insts.size() * sizeof(GpuInst));
    ok = ok && uploadBuffer(m_ssbo[11], 12, env.data(), env.size() * sizeof(float));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (!ok) { m_error = "uploading the scene failed"; return false; }

    m_worldTris    = worldTris;
    m_haveWorld    = !acc.world.nodes.empty();
    m_tlasRoot     = haveTop ? tlasBase : -1;
    m_instCount    = static_cast<int>(acc.instances.index.size());
    m_camera       = scene.camera;
    m_sun          = scene.sun;
    m_fog          = scene.fog;
    m_envZenith    = scene.env.zenith;
    m_envHorizon   = scene.env.horizon;
    m_envGround    = scene.env.ground;
    m_envIntensity = scene.env.intensity;
    m_lampCount    = static_cast<int>(scene.lamps.size());
    m_triangles    = scene.triangleCount();
    m_haveScene    = true;
    m_samples      = 0;
    return true;
}

bool Tracer::resize(int width, int height) {
    const int w = std::max(1, width), h = std::max(1, height);
    if (m_accum && w == m_width && h == m_height) {
        // Same picture, new view: keep the texture, drop what is in it.
        const std::vector<float> zero(static_cast<std::size_t>(w) * h * 4, 0.0f);
        glBindTexture(GL_TEXTURE_2D, m_accum);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, zero.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        m_samples = 0;
        return true;
    }
    drainErrors();
    if (m_accum) glDeleteTextures(1, &m_accum);
    glGenTextures(1, &m_accum);
    glBindTexture(GL_TEXTURE_2D, m_accum);
    // Immutable storage: an image unit binds a level of a texture, and a
    // mutable one can have that level redefined under it.
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA32F, w, h);
    const std::vector<float> zero(static_cast<std::size_t>(w) * h * 4, 0.0f);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, zero.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    m_width   = w;
    m_height  = h;
    m_samples = 0;
    return glGetError() == GL_NO_ERROR;
}

bool Tracer::accumulate(int samples) {
    if (!m_program || !m_haveScene || !m_accum) {
        m_error = "accumulate() before init/upload/resize";
        return false;
    }
    if (samples <= 0) return true;

    drainErrors();
    glUseProgram(m_program);
    glBindImageTexture(0, m_accum, 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);

    glUniform2i(glGetUniformLocation(m_program, "uSize"), m_width, m_height);
    setInt(m_program, "uSampleBase", m_samples);
    setInt(m_program, "uSamples", samples);
    setInt(m_program, "uSeed", 1);
    setInt(m_program, "uLampCount", m_lampCount);
    setInt(m_program, "uTexCount", m_textureCount);
    setInt(m_program, "uHavePaint", m_havePaint ? 1 : 0);
    setInt(m_program, "uWorldTris", m_worldTris);
    setInt(m_program, "uHaveWorld", m_haveWorld ? 1 : 0);
    setInt(m_program, "uTlasRoot", m_tlasRoot);
    setInt(m_program, "uInstCount", m_instCount);
    setInt(m_program, "uMaxBounces", m_maxBounces);
    setFloat(m_program, "uClampIndirect", m_clampIndirect);

    const float halfV = std::tan(glm::radians(m_camera.fovDegrees) * 0.5f);
    const float aspect = static_cast<float>(m_width) / static_cast<float>(m_height);
    setVec3(m_program, "uCamPos", m_camera.position);
    setVec3(m_program, "uCamFwd", glm::normalize(m_camera.forward));
    setVec3(m_program, "uCamUp", glm::normalize(m_camera.up));
    setVec3(m_program, "uCamRight", glm::normalize(m_camera.right));
    setFloat(m_program, "uHalfV", halfV);
    setFloat(m_program, "uHalfU", halfV * aspect);
    setFloat(m_program, "uAperture", std::max(0.0f, m_camera.apertureRadius));
    setFloat(m_program, "uFocus", std::max(0.1f, m_camera.focusDistance));

    // The sun, resolved exactly as pathtrace::Tracer's constructor resolves it:
    // `color` is the irradiance the directional light was authored as, and
    // spreading it over the disc is what keeps a render at the same brightness
    // as the viewport whatever angle the disc is given.
    const glm::vec3 sunAxis = glm::normalize(m_sun.direction);
    const float cosMax = std::cos(glm::radians(std::max(0.0f, m_sun.angularRadiusDeg)));
    const float solid  = 2.0f * glm::pi<float>() * (1.0f - cosMax);
    const float sunPdf = solid > 1e-7f ? 1.0f / solid : 0.0f;
    const glm::vec3 sunRadiance = solid > 1e-7f ? m_sun.color / solid : glm::vec3(0.0f);
    setInt(m_program, "uSunOn", m_sun.enabled ? 1 : 0);
    setVec3(m_program, "uSunAxis", sunAxis);
    setVec3(m_program, "uSunDirRaw", m_sun.direction);
    setVec3(m_program, "uSunColor", m_sun.color);
    setVec3(m_program, "uSunRadiance", sunRadiance);
    setFloat(m_program, "uSunCosMax", cosMax);
    setFloat(m_program, "uSunPdf", sunPdf);

    setVec3(m_program, "uEnvZenith", m_envZenith);
    setVec3(m_program, "uEnvHorizon", m_envHorizon);
    setVec3(m_program, "uEnvGround", m_envGround);
    setFloat(m_program, "uEnvIntensity", m_envIntensity);
    glUniform2i(glGetUniformLocation(m_program, "uEnvSize"), m_envSize.x, m_envSize.y);
    setInt(m_program, "uEnvPixOff", m_envPixOff);
    glUniform2i(glGetUniformLocation(m_program, "uDistSize"), m_distSize.x, m_distSize.y);
    setInt(m_program, "uDistFuncOff", m_distFuncOff);
    setInt(m_program, "uDistCondOff", m_distCondOff);
    setInt(m_program, "uDistMargOff", m_distMargOff);
    setFloat(m_program, "uDistTotal", m_distTotal);
    glUniform2i(glGetUniformLocation(m_program, "uBackSize"), m_backSize.x, m_backSize.y);
    setInt(m_program, "uBackOff", m_backOff);
    glUniform2i(glGetUniformLocation(m_program, "uMaskSize"), m_maskSize.x, m_maskSize.y);
    setInt(m_program, "uMaskOff", m_maskOff);
    glUniform4f(glGetUniformLocation(m_program, "uMaskRect"),
                m_maskRect.x, m_maskRect.y, m_maskRect.z, m_maskRect.w);
    setVec3(m_program, "uMaskSun", m_maskSun);
    setVec3(m_program, "uMediumGlow", m_mediumGlow);
    const pathtrace::GroundLook& g = m_ground;
    glUniform4f(glGetUniformLocation(m_program, "uMeadow"),
                g.meadowNear, g.meadowFar, g.meadowAmount, g.meadowLush);
    setVec3(m_program, "uGrassTint", g.grassTint);
    setVec3(m_program, "uGrassTopDryWater", glm::vec3(g.grassTop, g.grassDry, g.waterLevel));
    setInt(m_program, "uMoistSamples", g.moistSamples);
    setInt(m_program, "uMoistOff", m_moistOff);
    setVec3(m_program, "uMoistRect", glm::vec3(g.moistOrigin, g.moistCell));
    setInt(m_program, "uWoodsOn", g.woodsOn() ? 1 : 0);
    setVec3(m_program, "uEco", glm::vec3(g.ecoCover, g.ecoStandSize, g.ecoSlopeLove));
    setVec3(m_program, "uCanopy", g.canopy);
    setVec3(m_program, "uFarLook", glm::vec3(g.ecoOn ? 1.0f : 0.0f, g.farTreeLine, g.farSnowLevel));
    setVec3(m_program, "uEco2", glm::vec3(g.ecoTreeLine, g.ecoWater, g.ecoSolitary));
    setVec3(m_program, "uAirSun", g.airSun);
    glUniform2f(glGetUniformLocation(m_program, "uCanopyRange"),
                std::min(g.canopyFrom, 1e30f), std::min(g.canopyTo, 1e30f));

    setVec3(m_program, "uFogColor", m_fog.color);
    setVec3(m_program, "uFogSunColor", m_fog.sunColor);
    setFloat(m_program, "uFogDensity", m_fog.density);
    setFloat(m_program, "uFogFalloff", m_fog.heightFalloff);
    setFloat(m_program, "uFogHeight", m_fog.height);

    // How long the last dispatch actually took, asked of the GPU rather than
    // timed on the CPU: a dispatch returns immediately and a wall clock around
    // it measures the driver, not the work. Read one dispatch late and only when
    // the answer is already there -- waiting for it would stall the very
    // pipeline the number exists to keep full.
    if (!m_query) glGenQueries(1, &m_query);
    if (m_queryPending) {
        GLuint ready = GL_FALSE;
        glGetQueryObjectuiv(m_query, GL_QUERY_RESULT_AVAILABLE, &ready);
        if (ready == GL_TRUE) {
            GLuint64 ns = 0;
            glGetQueryObjectui64v(m_query, GL_QUERY_RESULT, &ns);
            m_lastDispatchMs = static_cast<double>(ns) / 1.0e6;
            m_lastDispatchSamples = m_queriedSamples;
            m_queryPending = false;
        }
    }
    const bool timing = !m_queryPending;
    if (timing) { glBeginQuery(GL_TIME_ELAPSED, m_query); m_queriedSamples = samples; }

    const GLuint gx = static_cast<GLuint>((m_width  + 7) / 8);
    const GLuint gy = static_cast<GLuint>((m_height + 7) / 8);
    glDispatchCompute(gx, gy, 1);
    if (timing) { glEndQuery(GL_TIME_ELAPSED); m_queryPending = true; }
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    glUseProgram(0);

    const GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        m_error = "the dispatch reported GL error " + std::to_string(err);
        return false;
    }
    m_samples += samples;
    return true;
}

bool Tracer::resolve(float exposure, const pathtrace::Grade& grade) {
    if (!m_resolve || !m_accum || m_samples <= 0) return false;

    drainErrors();
    // The eight-bit picture, made once per size. Nearest filtering: it is drawn
    // at its own size, and a preview that quietly resamples itself is a preview
    // whose noise nobody can judge.
    if (!m_ldr || m_ldrW != m_width || m_ldrH != m_height) {
        if (m_ldr) glDeleteTextures(1, &m_ldr);
        glGenTextures(1, &m_ldr);
        glBindTexture(GL_TEXTURE_2D, m_ldr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, m_width, m_height);
        glBindTexture(GL_TEXTURE_2D, 0);
        m_ldrW = m_width;
        m_ldrH = m_height;
    }

    glUseProgram(m_resolve);
    glBindImageTexture(0, m_accum, 0, GL_FALSE, 0, GL_READ_ONLY,  GL_RGBA32F);
    glBindImageTexture(1, m_ldr,   0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    glUniform2i(glGetUniformLocation(m_resolve, "uSize"), m_width, m_height);
    setInt(m_resolve, "uSampleCount", m_samples);
    setFloat(m_resolve, "uExposure", exposure);
    setFloat(m_resolve, "uHueShift", grade.hueShift);
    setFloat(m_resolve, "uSaturation", grade.saturation);
    setFloat(m_resolve, "uValue", grade.value);
    setFloat(m_resolve, "uWarmth", grade.warmth);
    setFloat(m_resolve, "uContrast", grade.contrast);
    setInt(m_resolve, "uCurve", grade.curve);
    setFloat(m_resolve, "uVignette", grade.vignette);
    glDispatchCompute(static_cast<GLuint>((m_width  + 7) / 8),
                      static_cast<GLuint>((m_height + 7) / 8), 1);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_UPDATE_BARRIER_BIT);
    glUseProgram(0);
    return glGetError() == GL_NO_ERROR;
}

bool Tracer::snapshotHdr(std::vector<float>& out) const {
    if (!m_accum || m_samples <= 0) return false;
    std::vector<float> rgba(static_cast<std::size_t>(m_width) * m_height * 4);
    glBindTexture(GL_TEXTURE_2D, m_accum);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    // Sums in, radiance out: the kernel only ever adds, so this is the one
    // place the sample count is divided out.
    const float inv = 1.0f / static_cast<float>(m_samples);
    out.resize(static_cast<std::size_t>(m_width) * m_height * 3);
    for (std::size_t i = 0, n = static_cast<std::size_t>(m_width) * m_height;
         i < n; ++i) {
        out[i * 3 + 0] = rgba[i * 4 + 0] * inv;
        out[i * 3 + 1] = rgba[i * 4 + 1] * inv;
        out[i * 3 + 2] = rgba[i * 4 + 2] * inv;
    }
    return true;
}

} // namespace gputrace
