#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

// A function the tracer calls per ray: a procedural colour, a roughness that
// varies over a surface, a bump map, the field an isosurface is the zero set
// of. Compiled once from a scene script (Strahl.hpp) and then only ever
// evaluated, from every render thread at once.
//
// WHY BYTECODE AND NOT THE SCRIPT'S OWN INTERPRETER. A marble pigment is
// evaluated at every path vertex that lands on the marble, and an isosurface
// at every step of every ray that crosses its bounds -- hundreds of millions of
// calls in one render. A tree-walking interpreter with dynamic values spends a
// microsecond or more on each of them, allocating as it goes; this spends a few
// nanoseconds per instruction and allocates nothing. The script stays the
// place the function is WRITTEN, and nothing that runs per ray knows it exists.
//
// ONE VALUE TYPE. Every register is a vec3. A scalar is held broadcast (x = y =
// z), so scalar and vector arithmetic are the same instruction, and a vec2
// keeps whatever its z happens to be: the instructions whose answer depends on
// how many components there are (length, dot, noise, ...) are told the width
// by the compiler instead. That keeps the interpreter loop to one switch with
// no type tags in it.
//
// NO LOOPS, NO CALLS. The compiler inlines every call and unrolls every loop
// (their bounds have to be constants), so a program is a straight line with
// forward jumps for `if`. Which also means it always terminates -- a render
// thread cannot be hung by a pattern.
//
// No GL, no script: plain arithmetic, like the rest of the tracer.
namespace pathtrace {

struct Image;

// One colour ramp: positions ascending in [0, 1], colours as authored.
struct Ramp {
    std::vector<float>     at;
    std::vector<glm::vec3> color;
    glm::vec3 lookup(float t) const;
};

class Func {
public:
    enum class Op : std::uint8_t {
        Mov,
        Add, Sub, Mul, Div, Mod, Pow, Neg,
        Min, Max, Lt, Le, Gt, Ge, Eq, Ne, And, Or, Not,
        Sin, Cos, Tan, Asin, Acos, Atan, Atan2,   // degrees, like the language
        Sqrt, Exp, Log, Abs, Sign, Floor, Ceil, Round, Fract,
        Clamp, Mix, Smoothstep, Step,
        Length, Normalize, Dot, Distance,         // imm = width
        Cross, Reflect,
        // dst = (a[i0], b[i1], c[i2]); imm = i0 | i1 << 2 | i2 << 4. The one
        // way vectors are built and taken apart (constructors, swizzles).
        Combine,
        Noise, Fbm, Turbulence, Voronoi, VoronoiId, Hash, Checker,  // imm = width
        Ramp,       // imm = index into ramps
        Texture,    // imm = index into images; a = uv
        Jump,       // to imm
        JumpIfZero, // to imm when a.x == 0
        Select,     // dst = a.x != 0 ? b : c
    };
    struct Ins {
        Op            op = Op::Mov;
        std::uint16_t dst = 0, a = 0, b = 0, c = 0;
        std::int32_t  imm = 0;
    };

    // Registers [0, consts.size()) hold the constants, the next `params` hold
    // the arguments; everything after is scratch.
    std::vector<Ins>       code;
    std::vector<glm::vec3> consts;
    int                    params    = 0;
    int                    registers = 0;
    int                    result    = 0;   // register holding the answer
    int                    resultWidth = 3; // 1 scalar, 2, 3
    std::vector<Ramp>      ramps;
    std::vector<std::shared_ptr<const Image>> images;

    static constexpr int kMaxRegisters = 1024;
    // In `imm` of an element-wise maths instruction (Pow .. Fract): its
    // operands are scalars, so it need only be worked out once.
    static constexpr int kScalar = 1;

    // Up to three arguments: the point, the normal, the uv (as a vec3, z = 0),
    // which is what every per-ray caller has to offer.
    glm::vec3 eval(const glm::vec3& a0, const glm::vec3& a1 = glm::vec3(0.0f),
                   const glm::vec3& a2 = glm::vec3(0.0f)) const;
    float scalar(const glm::vec3& p) const { return eval(p).x; }
};

// The noise the VM's instructions are made of, exposed so the script's
// interpreter computes exactly the same numbers when it calls them at
// build time, and so a test can hold the two against each other.
namespace fn {
float noise(const glm::vec3& p);                 // Perlin gradient noise, ~[-1, 1]
float fbm(const glm::vec3& p, int octaves);      // sum of octaves, ~[-1, 1]
float turbulence(const glm::vec3& p, int octaves); // sum of |noise|, [0, 1]
float voronoi(const glm::vec3& p);               // distance to the nearest feature point
float voronoiId(const glm::vec3& p);             // [0, 1), constant per cell
float hash(const glm::vec3& p);                  // [0, 1), of the exact value
float checker(const glm::vec3& p);               // 0 / 1
} // namespace fn

} // namespace pathtrace
