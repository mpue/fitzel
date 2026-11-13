#include "TraceFunc.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "PathTrace.hpp"

namespace pathtrace {
namespace {

constexpr float kDeg = 3.14159265358979323846f / 180.0f;

// --- Noise ------------------------------------------------------------------
// Ken Perlin's improved noise (2002), with his permutation. A fixed table, not
// a seeded one: a scene that renders differently on another machine, or after
// an unrelated change moved a random seed, is a scene you cannot iterate on.
constexpr unsigned char kPerm[256] = {
    151,160,137,91,90,15,131,13,201,95,96,53,194,233,7,225,140,36,103,30,69,142,
    8,99,37,240,21,10,23,190,6,148,247,120,234,75,0,26,197,62,94,252,219,203,117,
    35,11,32,57,177,33,88,237,149,56,87,174,20,125,136,171,168,68,175,74,165,71,
    134,139,48,27,166,77,146,158,231,83,111,229,122,60,211,133,230,220,105,92,41,
    55,46,245,40,244,102,143,54,65,25,63,161,1,216,80,73,209,76,132,187,208,89,
    18,169,200,196,135,130,116,188,159,86,164,100,109,198,173,186,3,64,52,217,226,
    250,124,123,5,202,38,147,118,126,255,82,85,212,207,206,59,227,47,16,58,17,182,
    189,28,42,223,183,170,213,119,248,152,2,44,154,163,70,221,153,101,155,167,43,
    172,9,129,22,39,253,19,98,108,110,79,113,224,232,178,185,112,104,218,246,97,
    228,251,34,242,193,238,210,144,12,191,179,162,241,81,51,145,235,249,14,239,
    107,49,192,214,31,181,199,106,157,184,84,204,176,115,121,50,45,127,4,150,254,
    138,236,205,93,222,114,67,29,24,72,243,141,128,195,78,66,215,61,156,180};

inline int perm(int i) { return kPerm[i & 255]; }

inline float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

inline float grad(int h, float x, float y, float z) {
    const int   hh = h & 15;
    const float u  = hh < 8 ? x : y;
    const float v  = hh < 4 ? y : (hh == 12 || hh == 14 ? x : z);
    return ((hh & 1) ? -u : u) + ((hh & 2) ? -v : v);
}

std::uint32_t mix32(std::uint32_t h) {
    h ^= h >> 16; h *= 0x7feb352dU;
    h ^= h >> 15; h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

std::uint32_t cellHash(int x, int y, int z, std::uint32_t salt) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 0x8da6b343U ^
                      static_cast<std::uint32_t>(y) * 0xd8163841U ^
                      static_cast<std::uint32_t>(z) * 0xcb1ab31fU ^ salt;
    return mix32(h);
}

inline float unit(std::uint32_t h) { return static_cast<float>(h >> 8) * 0x1p-24f; }

// The nearest feature point: its squared distance, and the cell it lives in.
void nearestFeature(const glm::vec3& p, float& d2, glm::ivec3& cell) {
    const glm::ivec3 c(static_cast<int>(std::floor(p.x)), static_cast<int>(std::floor(p.y)),
                       static_cast<int>(std::floor(p.z)));
    d2 = 1e30f;
    cell = c;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                const glm::ivec3 k = c + glm::ivec3(dx, dy, dz);
                const glm::vec3 f(static_cast<float>(k.x) + unit(cellHash(k.x, k.y, k.z, 1u)),
                                  static_cast<float>(k.y) + unit(cellHash(k.x, k.y, k.z, 2u)),
                                  static_cast<float>(k.z) + unit(cellHash(k.x, k.y, k.z, 3u)));
                const glm::vec3 d = f - p;
                const float e = glm::dot(d, d);
                if (e < d2) { d2 = e; cell = k; }
            }
}

// Zero the components past `width`: a vec2's z, a scalar's y and z.
inline glm::vec3 narrow(glm::vec3 v, int width) {
    if (width < 3) v.z = 0.0f;
    if (width < 2) v.y = 0.0f;
    return v;
}

inline glm::vec3 bcast(float f) { return glm::vec3(f); }

inline float fmodPos(float a, float b) {
    if (b == 0.0f) return 0.0f;
    return a - b * std::floor(a / b);
}

} // namespace

namespace fn {

float noise(const glm::vec3& p) {
    const float fx = std::floor(p.x), fy = std::floor(p.y), fz = std::floor(p.z);
    const int X = static_cast<int>(fx) & 255, Y = static_cast<int>(fy) & 255,
              Z = static_cast<int>(fz) & 255;
    const float x = p.x - fx, y = p.y - fy, z = p.z - fz;
    const float u = fade(x), v = fade(y), w = fade(z);
    const int A = perm(X) + Y, AA = perm(A) + Z, AB = perm(A + 1) + Z;
    const int B = perm(X + 1) + Y, BA = perm(B) + Z, BB = perm(B + 1) + Z;
    const auto lerp = [](float t, float a, float b) { return a + t * (b - a); };
    return lerp(w, lerp(v, lerp(u, grad(perm(AA), x, y, z), grad(perm(BA), x - 1, y, z)),
                           lerp(u, grad(perm(AB), x, y - 1, z), grad(perm(BB), x - 1, y - 1, z))),
                   lerp(v, lerp(u, grad(perm(AA + 1), x, y, z - 1),
                                grad(perm(BA + 1), x - 1, y, z - 1)),
                        lerp(u, grad(perm(AB + 1), x, y - 1, z - 1),
                             grad(perm(BB + 1), x - 1, y - 1, z - 1))));
}

float fbm(const glm::vec3& p, int octaves) {
    octaves = std::clamp(octaves, 1, 12);
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    glm::vec3 q = p;
    for (int i = 0; i < octaves; ++i) {
        sum  += amp * noise(q);
        norm += amp;
        amp  *= 0.5f;
        // Rotated a little per octave as well as doubled, so the lattices of
        // successive octaves never line up into visible grid lines.
        q = glm::vec3(q.x * 1.6f + q.z * 1.2f, q.y * 2.0f, -q.x * 1.2f + q.z * 1.6f) + 17.13f;
    }
    return sum / norm;
}

float turbulence(const glm::vec3& p, int octaves) {
    octaves = std::clamp(octaves, 1, 12);
    float sum = 0.0f, amp = 0.5f, norm = 0.0f;
    glm::vec3 q = p;
    for (int i = 0; i < octaves; ++i) {
        sum  += amp * std::fabs(noise(q));
        norm += amp;
        amp  *= 0.5f;
        q = glm::vec3(q.x * 1.6f + q.z * 1.2f, q.y * 2.0f, -q.x * 1.2f + q.z * 1.6f) + 17.13f;
    }
    // |noise| averages about a third of its range; this puts the result
    // across most of [0, 1] rather than hugging the bottom of it.
    return std::min(1.0f, 1.6f * sum / norm);
}

float voronoi(const glm::vec3& p) {
    float d2;
    glm::ivec3 c;
    nearestFeature(p, d2, c);
    return std::sqrt(d2);
}

float voronoiId(const glm::vec3& p) {
    float d2;
    glm::ivec3 c;
    nearestFeature(p, d2, c);
    return unit(cellHash(c.x, c.y, c.z, 7u));
}

float hash(const glm::vec3& p) {
    std::uint32_t b[3];
    // +0.0f: -0 and 0 are the same number and must hash alike.
    const float v[3] = {p.x + 0.0f, p.y + 0.0f, p.z + 0.0f};
    std::memcpy(b, v, sizeof b);
    return unit(mix32(b[0] * 0x9e3779b9U ^ mix32(b[1] * 0x85ebca6bU ^ mix32(b[2] + 0x27d4eb2fU))));
}

float checker(const glm::vec3& p) {
    const long long s = static_cast<long long>(std::floor(p.x)) +
                        static_cast<long long>(std::floor(p.y)) +
                        static_cast<long long>(std::floor(p.z));
    return (s & 1) ? 1.0f : 0.0f;
}

} // namespace fn

glm::vec3 Ramp::lookup(float t) const {
    if (at.empty()) return glm::vec3(0.0f);
    if (!(t == t)) t = 0.0f;
    if (t <= at.front()) return color.front();
    if (t >= at.back())  return color.back();
    const auto it = std::upper_bound(at.begin(), at.end(), t);
    const std::size_t i = static_cast<std::size_t>(it - at.begin());
    const float a = at[i - 1], b = at[i];
    const float f = b > a ? (t - a) / (b - a) : 0.0f;
    return glm::mix(color[i - 1], color[i], f);
}

glm::vec3 Func::eval(const glm::vec3& a0, const glm::vec3& a1, const glm::vec3& a2) const {
    // Raw storage, not vec3[]: an isosurface calls this a hundred times per ray,
    // and constructing twelve kilobytes of registers that the program is about
    // to overwrite would cost more than the program.
    alignas(glm::vec3) unsigned char storage[kMaxRegisters * sizeof(glm::vec3)];
    glm::vec3* r = reinterpret_cast<glm::vec3*>(storage);
    const std::size_t nc = consts.size();
    if (nc) std::memcpy(static_cast<void*>(r), consts.data(), nc * sizeof(glm::vec3));
    const glm::vec3 args[3] = {a0, a1, a2};
    for (int i = 0; i < params && i < 3; ++i) r[nc + static_cast<std::size_t>(i)] = args[i];

    const Ins* ins = code.data();
    const int  n   = static_cast<int>(code.size());
    for (int pc = 0; pc < n; ++pc) {
        const Ins& I = ins[pc];
        glm::vec3& d = r[I.dst];
        const glm::vec3& A = r[I.a];
        const glm::vec3& B = r[I.b];
        const glm::vec3& C = r[I.c];
        // A scalar operation (Func::kScalar) is worked out once, on x, and
        // broadcast: a sin of a number is one sin, not three.
        if (I.imm == kScalar) {
            const float x = A.x, y = B.x;
            bool done = true;
            float v = 0.0f;
            switch (I.op) {
            case Op::Pow:   v = std::pow(x, y); break;
            case Op::Sin:   v = std::sin(x * kDeg); break;
            case Op::Cos:   v = std::cos(x * kDeg); break;
            case Op::Tan:   v = std::tan(x * kDeg); break;
            case Op::Asin:  v = std::asin(std::clamp(x, -1.0f, 1.0f)) / kDeg; break;
            case Op::Acos:  v = std::acos(std::clamp(x, -1.0f, 1.0f)) / kDeg; break;
            case Op::Atan:  v = std::atan(x) / kDeg; break;
            case Op::Atan2: v = std::atan2(x, y) / kDeg; break;
            case Op::Sqrt:  v = std::sqrt(std::max(x, 0.0f)); break;
            case Op::Exp:   v = std::exp(x); break;
            case Op::Log:   v = std::log(std::max(x, 1e-30f)); break;
            case Op::Abs:   v = std::fabs(x); break;
            case Op::Sign:  v = x > 0.0f ? 1.0f : x < 0.0f ? -1.0f : 0.0f; break;
            case Op::Floor: v = std::floor(x); break;
            case Op::Ceil:  v = std::ceil(x); break;
            case Op::Round: v = std::round(x); break;
            case Op::Fract: v = x - std::floor(x); break;
            case Op::Mod:   v = fmodPos(x, y); break;
            default:        done = false; break;
            }
            if (done) { d = glm::vec3(v); continue; }
        }
        switch (I.op) {
        case Op::Mov:   d = A; break;
        case Op::Add:   d = A + B; break;
        case Op::Sub:   d = A - B; break;
        case Op::Mul:   d = A * B; break;
        case Op::Div:
            d = glm::vec3(B.x != 0.0f ? A.x / B.x : 0.0f, B.y != 0.0f ? A.y / B.y : 0.0f,
                          B.z != 0.0f ? A.z / B.z : 0.0f);
            break;
        case Op::Mod:
            d = glm::vec3(fmodPos(A.x, B.x), fmodPos(A.y, B.y), fmodPos(A.z, B.z));
            break;
        case Op::Pow:
            d = glm::vec3(std::pow(A.x, B.x), std::pow(A.y, B.y), std::pow(A.z, B.z));
            break;
        case Op::Neg:   d = -A; break;
        case Op::Min:   d = glm::min(A, B); break;
        case Op::Max:   d = glm::max(A, B); break;
        case Op::Lt:    d = glm::vec3(glm::lessThan(A, B)); break;
        case Op::Le:    d = glm::vec3(glm::lessThanEqual(A, B)); break;
        case Op::Gt:    d = glm::vec3(glm::greaterThan(A, B)); break;
        case Op::Ge:    d = glm::vec3(glm::greaterThanEqual(A, B)); break;
        case Op::Eq:    d = glm::vec3(glm::equal(A, B)); break;
        case Op::Ne:    d = glm::vec3(glm::notEqual(A, B)); break;
        case Op::And:   d = bcast((A.x != 0.0f && B.x != 0.0f) ? 1.0f : 0.0f); break;
        case Op::Or:    d = bcast((A.x != 0.0f || B.x != 0.0f) ? 1.0f : 0.0f); break;
        case Op::Not:   d = bcast(A.x == 0.0f ? 1.0f : 0.0f); break;
        case Op::Sin:   d = glm::sin(A * kDeg); break;
        case Op::Cos:   d = glm::cos(A * kDeg); break;
        case Op::Tan:   d = glm::tan(A * kDeg); break;
        case Op::Asin:  d = glm::asin(glm::clamp(A, -1.0f, 1.0f)) / kDeg; break;
        case Op::Acos:  d = glm::acos(glm::clamp(A, -1.0f, 1.0f)) / kDeg; break;
        case Op::Atan:  d = glm::atan(A) / kDeg; break;
        case Op::Atan2:
            d = glm::vec3(std::atan2(A.x, B.x), std::atan2(A.y, B.y), std::atan2(A.z, B.z)) / kDeg;
            break;
        case Op::Sqrt:  d = glm::sqrt(glm::max(A, glm::vec3(0.0f))); break;
        case Op::Exp:   d = glm::exp(A); break;
        case Op::Log:   d = glm::log(glm::max(A, glm::vec3(1e-30f))); break;
        case Op::Abs:   d = glm::abs(A); break;
        case Op::Sign:  d = glm::sign(A); break;
        case Op::Floor: d = glm::floor(A); break;
        case Op::Ceil:  d = glm::ceil(A); break;
        case Op::Round: d = glm::round(A); break;
        case Op::Fract: d = A - glm::floor(A); break;
        case Op::Clamp: d = glm::min(glm::max(A, B), C); break;
        case Op::Mix:   d = A + (B - A) * C; break;
        case Op::Smoothstep: {
            const glm::vec3 e = B - A;
            const glm::vec3 t = glm::clamp(glm::vec3(e.x != 0.0f ? (C.x - A.x) / e.x : 0.0f,
                                                     e.y != 0.0f ? (C.y - A.y) / e.y : 0.0f,
                                                     e.z != 0.0f ? (C.z - A.z) / e.z : 0.0f),
                                           0.0f, 1.0f);
            d = t * t * (3.0f - 2.0f * t);
            break;
        }
        case Op::Step:  d = glm::vec3(glm::greaterThanEqual(B, A)); break;
        case Op::Length:    d = bcast(glm::length(narrow(A, I.imm))); break;
        case Op::Normalize: {
            const glm::vec3 v = narrow(A, I.imm);
            const float l = glm::length(v);
            d = l > 0.0f ? v / l : glm::vec3(0.0f);
            break;
        }
        case Op::Dot:      d = bcast(glm::dot(narrow(A, I.imm), narrow(B, I.imm))); break;
        case Op::Distance: d = bcast(glm::length(narrow(A - B, I.imm))); break;
        case Op::Cross:    d = glm::cross(A, B); break;
        case Op::Reflect:  d = A - 2.0f * glm::dot(A, B) * B; break;
        case Op::Combine: {
            const glm::vec3 v(A[I.imm & 3], B[(I.imm >> 2) & 3], C[(I.imm >> 4) & 3]);
            d = v;
            break;
        }
        case Op::Noise:      d = bcast(fn::noise(narrow(A, I.imm))); break;
        case Op::Fbm:        d = bcast(fn::fbm(narrow(A, I.imm), static_cast<int>(B.x))); break;
        case Op::Turbulence: d = bcast(fn::turbulence(narrow(A, I.imm), static_cast<int>(B.x))); break;
        case Op::Voronoi:    d = bcast(fn::voronoi(narrow(A, I.imm))); break;
        case Op::VoronoiId:  d = bcast(fn::voronoiId(narrow(A, I.imm))); break;
        case Op::Hash:       d = bcast(fn::hash(narrow(A, I.imm))); break;
        case Op::Checker:    d = bcast(fn::checker(narrow(A, I.imm))); break;
        case Op::Ramp:
            d = ramps[static_cast<std::size_t>(I.imm)].lookup(A.x);
            break;
        case Op::Texture: {
            const Image* img = images[static_cast<std::size_t>(I.imm)].get();
            // Image rows run top-down; a uv with v up reads the picture the
            // right way up, as it does on the GPU.
            d = img ? glm::vec3(img->sample(A.x, 1.0f - A.y)) : glm::vec3(1.0f);
            break;
        }
        case Op::Jump:       pc = I.imm - 1; break;
        case Op::JumpIfZero: if (A.x == 0.0f) pc = I.imm - 1; break;
        case Op::Select:     d = A.x != 0.0f ? B : C; break;
        }
    }
    return narrow(r[result], resultWidth == 1 ? 3 : resultWidth);
}

} // namespace pathtrace
