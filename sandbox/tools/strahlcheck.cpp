// The Strahl check.
//
// A scene language has two ways of being wrong that look fine: a script that
// means something other than it says (an object put in the scene twice, a
// `let` that leaks one, a default that never applies), and a picture that is
// subtly not the scene -- a pattern compiled for the tracer that disagrees
// with the same function run by the interpreter, a CSG surface missing where
// two operands meet, a normal pointing into the solid. Neither shows as an
// error, and both show as a render that is merely odd. So:
//
//   * the language: small scripts whose values are known, and where their
//     objects end up;
//   * interpreter against compiler: every prelude pattern and a set of
//     functions using every construct the compiler handles, evaluated both
//     ways at a few hundred points -- they must agree;
//   * shapes and CSG: random rays against every primitive and combination,
//     checked against the shape's own inside() -- a crossing must change
//     inside to outside, its normal must point out, and a ray that reports no
//     hit must not pass through the solid;
//   * light: a white furnace around an exact sphere comes back at radiance 1;
//   * the reference (docs/strahl.md) has a row for every name the language
//     knows, and describes no call that is gone;
//   * every bundled example builds.
//
// Console program, non-zero on failure. Writes small renders of the examples
// into [outDir] when given one.
//   build/release/bin/strahlcheck [outDir]

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#include <tinyexr.h>

#include "../src/DocPanel.hpp"
#include "../src/PathTrace.hpp"
#include "../src/StrahlComplete.hpp"
#include "../src/StrahlInternal.hpp"

namespace fs = std::filesystem;
using namespace strahl;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what.c_str(), detail.empty() ? "" : " -- ",
                detail.c_str());
    if (!ok) ++g_failures;
}

std::string num(double x) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", x);
    return b;
}

// Run a script; hand back the interpreter so its globals and scene can be read.
std::unique_ptr<Interp> run(const std::string& src, std::string* error = nullptr) {
    static Options opts;
    auto in = std::make_unique<Interp>(opts);
    try {
        in->runSource(src, "<test>");
    } catch (const Error& e) {
        if (error) *error = std::to_string(e.loc.line) + ":" + std::to_string(e.loc.col) + ": " + e.what();
        else std::printf("      script error %d:%d: %s\n", e.loc.line, e.loc.col, e.what());
        return nullptr;
    }
    return in;
}

Value global(Interp& in, const std::string& name) {
    Value* v = in.globals->find(name);
    return v ? *v : Value::nil();
}

bool numIs(const Value& v, double x, double tol = 1e-9) {
    return v.t == T::Num && std::fabs(v.v[0] - x) <= tol;
}

bool vecIs(const Value& v, glm::dvec3 x, double tol = 1e-9) {
    return v.t == T::Vec && v.n == 3 && glm::length(v.vec3() - x) <= tol;
}

// --- The language --------------------------------------------------------------
void language() {
    std::printf("\nthe language\n");
    auto in = run(R"(
        let a = 1 + 2 * 3 ^ 2 - -4 / 2          // 1 + 18 + 2
        let b = [1, 2, 3] * 2 + 1
        let c = b.zyx
        let d = (b.x + b.y) * 2
        fn add(x, y = 10) = x + y
        let e = add(1) + add(1, 2) + add(y: 5, x: 1)
        fn fact(n) { if n <= 1 { 1 } else { n * fact(n - 1) } }
        let f = fact(6)
        fn counter() { let n = 0; fn() { n = n + 1; n } }
        let g = counter()
        g(); g()
        let h = g()
        let l = []
        for i in 0..5 { push(l, i * i) }
        let li = l[-1] + len(l)
        let s = 0
        for i, v in [10, 20, 30] { s += i * v }
        let w = 0
        while w < 100 { w = w * 2 + 1 }
        let r = len(0..10) + len(0..=10) + len(range(0, 1, 0.25))
        let m = sum(map(filter(range(10), fn(x) => x % 2 == 0), fn(x) => x * x))
        let str1 = "a" ++ 1 ++ [2, 3]
        let t = if a > 20 { "big" } else { "small" }
        let br = 0
        for i in 0..100 { if i == 7 { break }; br = i }
        let mat = translation([1, 2, 3]) * rotation([0, 90, 0])
        let pt = mat * [1, 0, 0]
        let nested = [[1, 2], [3, 4]][1][0]
        let col = hsv(120, 1, 1)
        let sw = [1, 2].yx
    )");
    if (!in) { check(false, "the language script runs"); return; }
    check(numIs(global(*in, "a"), 21.0), "precedence: 1 + 2 * 3 ^ 2 - -4 / 2 is 21", in->describe(global(*in, "a")));
    check(vecIs(global(*in, "b"), {3, 5, 7}), "vectors: [1,2,3] * 2 + 1", in->describe(global(*in, "b")));
    check(vecIs(global(*in, "c"), {7, 5, 3}), "swizzle .zyx", in->describe(global(*in, "c")));
    check(numIs(global(*in, "d"), 16.0), "members in arithmetic", in->describe(global(*in, "d")));
    check(numIs(global(*in, "e"), 11 + 3 + 6), "defaults and named arguments", in->describe(global(*in, "e")));
    check(numIs(global(*in, "f"), 720.0), "recursion: fact(6)", in->describe(global(*in, "f")));
    check(numIs(global(*in, "h"), 3.0), "a closure keeps its own state", in->describe(global(*in, "h")));
    check(numIs(global(*in, "li"), 16 + 5), "lists: push, negative index, len", in->describe(global(*in, "li")));
    check(numIs(global(*in, "s"), 0 * 10 + 1 * 20 + 2 * 30), "for with an index", in->describe(global(*in, "s")));
    check(numIs(global(*in, "w"), 127.0), "while", in->describe(global(*in, "w")));
    check(numIs(global(*in, "r"), 10 + 11 + 4), "ranges: len of .., ..= and stepped", in->describe(global(*in, "r")));
    check(numIs(global(*in, "m"), 0 + 4 + 16 + 36 + 64), "map, filter, sum", in->describe(global(*in, "m")));
    const Value s1 = global(*in, "str1");
    check(s1.t == T::Str && s1.string() == "a1[2, 3]", "string concatenation", in->describe(s1));
    const Value t = global(*in, "t");
    check(t.t == T::Str && t.string() == "big", "if as a value", in->describe(t));
    check(numIs(global(*in, "br"), 6.0), "break inside an if inside a for", in->describe(global(*in, "br")));
    check(vecIs(global(*in, "pt"), {1, 2, 2}, 1e-9), "matrices: translation * rotation applied to a point",
          in->describe(global(*in, "pt")));
    check(numIs(global(*in, "nested"), 3.0), "nested lists", in->describe(global(*in, "nested")));
    check(vecIs(global(*in, "col"), {0, 1, 0}, 1e-9), "hsv(120) is green", in->describe(global(*in, "col")));
    const Value sw = global(*in, "sw");
    check(sw.t == T::Vec && sw.n == 2 && sw.v[0] == 2 && sw.v[1] == 1, "vec2 swizzle", in->describe(sw));
}

// Where objects go.
void emission() {
    std::printf("\nwhere objects go\n");
    auto in = run(R"(
        let kept = sphere { radius: 1 }             // a let puts nothing in the scene
        sphere { radius: 2 }                        // a statement does
        fn ring(n) { for i in 0..n { sphere { center: [i, 0, 0], radius: 0.1 } } }
        ring(4)                                      // one group of four
        let r = ring(3)                              // nothing
        if true { box { size: 1 } } else { box { size: 2 } }
        let chosen = if false { box { size: 1 } } else { box { size: 3 } }   // nothing
        union { kept; kept { translate: [1, 0, 0] }; [sphere { radius: 0.5 }, sphere { radius: 0.6 }] }
        fn two() { sphere { radius: 1 }; 42 }
        let tv = two()
        fn num() = 3
        num()                                        // a number statement: nothing
    )");
    if (!in) { check(false, "the emission script runs"); return; }
    std::vector<std::string> kinds;
    for (const Value& v : in->scene) kinds.push_back(v.t == T::Obj ? v.objRef().kind : "?");
    std::string got;
    for (const std::string& k : kinds) got += k + " ";
    check(kinds.size() == 4 && kinds[0] == "sphere" && kinds[1] == "group" && kinds[2] == "box" &&
              kinds[3] == "union",
          "statements emit; lets, ifs used as values and number calls do not", got);
    if (kinds.size() >= 2)
        check(in->scene[1].objRef().children.size() == 4, "a function returns what it made as one group",
              std::to_string(in->scene[1].objRef().children.size()));
    if (kinds.size() >= 4)
        check(in->scene[3].objRef().children.size() == 4, "a block's statements and lists become its children",
              std::to_string(in->scene[3].objRef().children.size()));
    const Value tv = global(*in, "tv");
    check(tv.t == T::Obj && tv.objRef().kind == "sphere", "a function that made something returns it, not its last value");
    const Value chosen = global(*in, "chosen");
    check(chosen.t == T::Obj && chosen.objRef().find("size") && chosen.objRef().find("size")->value.v[0] == 3,
          "an if used as a value yields its branch's object");

    // Modifiers copy: the original is untouched.
    auto in2 = run(R"(
        let base = sphere { radius: 1, center: [0, 0, 0] }
        let moved = base { translate: [5, 0, 0], radius: 2 }
        let pos = sphere([1, 2, 3], 4)
        let gm = sphere { [0, 1, 0], 0.5, gold }
        let based = material { matte(red), roughness: 0.2 }
        let recol = material { gold, #ff0000 }
        let pure = #ff0000
    )");
    if (in2) {
        const Obj& b = global(*in2, "base").objRef();
        const Obj& m = global(*in2, "moved").objRef();
        check(b.find("radius")->value.v[0] == 1.0 && b.xf[3][0] == 0.0 &&
                  m.find("radius")->value.v[0] == 2.0 && m.xf[3][0] == 5.0,
              "a modifier block makes a changed copy");
        const Obj& p = global(*in2, "pos").objRef();
        check(p.find("center") && vecIs(p.find("center")->value, {1, 2, 3}) && numIs(p.find("radius")->value, 4),
              "positional values map to the kind's parameters");
        const Obj& g = global(*in2, "gm").objRef();
        check(g.find("material") != nullptr, "a material among unnamed values is the material");
        const Obj& bm = global(*in2, "based").objRef();
        check(bm.find("roughness") && numIs(bm.find("roughness")->value, 0.2) && bm.find("color"),
              "a material among a material's values is what it starts from, changed by the block");
        const Obj& rc = global(*in2, "recol").objRef();
        check(rc.find("color") && vecIs(rc.find("color")->value, global(*in2, "pure").vec3()) &&
                  rc.find("metallic") && numIs(rc.find("metallic")->value, 1.0),
              "...and a colour after it replaces its colour");
    } else {
        check(false, "the modifier script runs");
    }

    // -D: a name given from outside wins over the script's own let.
    Options o;
    o.defines.emplace_back("quality", "7");
    Interp d(o);
    d.runSource("let quality = 1\nlet q2 = quality * 2", "<test>");
    check(numIs(global(d, "q2"), 14.0), "a define overrides the script's let", d.describe(global(d, "q2")));
}

// --- The compiler against the interpreter ------------------------------------
void compiler() {
    std::printf("\ninterpreter and compiled functions agree\n");
    const char* fns[][2] = {
        {"checker", "checker(red, blue, size: 0.3)"},
        {"stripes", "stripes(white, black, 0.2, [1, 1, 0])"},
        {"rings", "rings(white, brown, 0.1)"},
        {"gradient", "gradient(black, white, -1, 1, [0, 1, 0])"},
        {"clouds", "clouds()"},
        {"marble", "marble()"},
        {"wood", "wood()"},
        {"granite", "granite()"},
        {"spots", "spots()"},
        {"bricks", "bricks()"},
        {"tiles", "tiles(white, gray(0.3))"},
        {"nested", "checker(marble(), wood(), size: 0.5)"},
        {"bumps", "bumps()"},
        {"wrinkles", "wrinkles()"},
        {"dents", "dents()"},
        {"ripples", "ripples()"},
        {"waves", "waves()"},
        {"hsv per ray", "fn(p) => hsv(p.x * 90, 0.5 + 0.3 * sin(p.y * 40), 1)"},
        {"kelvin per ray", "fn(p) => kelvin(4000 + 3000 * noise(p))"},
        {"rounded box sdf", "rounded_box(size: [1, 0.6, 0.8], radius: 0.1).function"},
        {"blob field", "blob([[0, 0, 0], [0.5, 0.2, 0]], 0.4).function"},
        {"if / return / assign / for",
         R"(fn(p) {
                let acc = 0
                let amp = 1
                for i in 0..4 { acc += amp * noise(p * pow(2, i)); amp *= 0.5 }
                if acc > 0.1 { return [acc, 0, 1] }
                let v = [p.x, p.y, acc]
                if p.z < 0 { v = v.zyx } else { v = v * 2 }
                v
            })"},
        {"maths", "fn(p) => [atan2(p.y, p.x) / 180, clamp(p.z, -0.3, 0.3) + fract(p.x * 3), "
                  "smoothstep(-1, 1, p.y) * step(0, p.x) + mod(p.z, 0.7)]"},
        {"vectors", "fn(p, n, uv) => normalize(cross(p, [0.3, 1, 0.2])) * dot(p, n) + reflect(p, n) * length(uv)"},
        {"cells", "fn(p) => [voronoi(p * 3), cell_id(p * 3), hash(floor(p * 4))]"},
        {"random", "fn(p) => random(floor(p * 5), -1, 1)"},
        {"ramp", "fn(p) => ramp(0.5 + 0.5 * noise(p * 2), [black, [0.3, red], [0.6, yellow], white])"},
        {"min max lists", "fn(p) => min(p.x, p.y, p.z) + max(p, 0.2)"},
    };
    int compiled = 0;
    for (const auto& f : fns) {
        std::string err;
        auto in = run(std::string("let f = ") + f[1], &err);
        if (!in) { check(false, std::string(f[0]) + " runs", err); continue; }
        const Value fv = global(*in, "f");
        std::shared_ptr<const pathtrace::Func> vm;
        try {
            vm = compile(*in, fv, Loc{});
        } catch (const Error& e) {
            check(false, std::string(f[0]) + " compiles", e.what());
            continue;
        }
        ++compiled;
        const std::size_t np = fv.closure().fn->params.size();
        double worst = 0.0;
        glm::vec3 worstAt(0.0f);
        for (int i = 0; i < 300; ++i) {
            const glm::vec3 p(pathtrace::fn::hash(glm::vec3(i, 1, 7)) * 4 - 2,
                              pathtrace::fn::hash(glm::vec3(i, 2, 7)) * 4 - 2,
                              pathtrace::fn::hash(glm::vec3(i, 3, 7)) * 4 - 2);
            const glm::vec3 n = glm::normalize(glm::vec3(pathtrace::fn::hash(glm::vec3(i, 4, 7)) - 0.5f, 1.0f, 0.3f));
            const glm::vec3 uv(pathtrace::fn::hash(glm::vec3(i, 5, 7)), pathtrace::fn::hash(glm::vec3(i, 6, 7)), 0.0f);
            std::vector<Value> args = {Value::vec(glm::dvec3(p)), Value::vec(glm::dvec3(n)),
                                       Value::vec(glm::dvec3(uv.x, uv.y, 0.0))};
            args[2].n = 2;
            args.resize(std::min<std::size_t>(np, 3));
            Value r;
            try {
                r = in->callValue(fv, args, Loc{});
            } catch (const Error& e) {
                check(false, std::string(f[0]) + " interprets", e.what());
                worst = -1;
                break;
            }
            const glm::vec3 c = vm->eval(p, n, uv);
            glm::dvec3 want = r.vec3();
            if (r.t == T::Vec && r.n == 2) want.z = 0.0;
            const double d = glm::length(glm::dvec3(c) - want);
            if (d > worst) { worst = d; worstAt = p; }
        }
        if (worst < 0) continue;
        check(worst < 2e-3, std::string(f[0]) + " (" + std::to_string(vm->code.size()) + " instructions)",
              "worst difference " + num(worst) + " at " + num(worstAt.x) + "," + num(worstAt.y) + "," + num(worstAt.z));
    }

    // What must be refused, refused with a reason.
    const char* bad[][2] = {
        {"while", "fn(p) { let i = 0; while i < 3 { i += 1 }; i }"},
        {"loop over the point", "fn(p) { let s = 0; for i in 0..p.x { s += 1 }; s }"},
        {"recursion", "fn(p) { fn g(x) = g(x); g(p) }"},
        {"an object", "fn(p) => sphere { radius: p.x }"},
    };
    for (const auto& b : bad) {
        auto in = run(std::string("let f = ") + b[1]);
        if (!in) { check(false, std::string("refuses ") + b[0] + ": runs"); continue; }
        bool refused = false;
        std::string why;
        try { compile(*in, global(*in, "f"), Loc{}); }
        catch (const Error& e) { refused = true; why = e.what(); }
        check(refused, std::string("refuses ") + b[0], why);
    }
}

// --- Shapes and CSG ------------------------------------------------------------
// Random rays against a scene of one shape, held against its own inside().
struct ShapeStats { int rays = 0, hits = 0, badCross = 0, badNormal = 0, missedSolid = 0; };

ShapeStats probeShape(const pathtrace::Shapes& s, int root, const glm::vec3& centre, float extent,
                      bool solid) {
    ShapeStats st;
    for (int i = 0; i < 2000; ++i) {
        const glm::vec3 a(pathtrace::fn::hash(glm::vec3(i, 11, 3)) * 2 - 1,
                          pathtrace::fn::hash(glm::vec3(i, 12, 3)) * 2 - 1,
                          pathtrace::fn::hash(glm::vec3(i, 13, 3)) * 2 - 1);
        const glm::vec3 b(pathtrace::fn::hash(glm::vec3(i, 14, 3)) * 2 - 1,
                          pathtrace::fn::hash(glm::vec3(i, 15, 3)) * 2 - 1,
                          pathtrace::fn::hash(glm::vec3(i, 16, 3)) * 2 - 1);
        // From outside, aimed through the shape's middle region.
        const glm::vec3 o = centre + glm::normalize(a + glm::vec3(1e-3f)) * extent * 3.0f;
        const glm::vec3 target = centre + b * extent * 0.7f;
        const glm::vec3 d = glm::normalize(target - o);
        ++st.rays;
        pathtrace::ShapeHit h;
        const float eps = 2e-4f * extent;
        if (s.closest(root, o, d, 1e-4f, 1e9f, h)) {
            ++st.hits;
            const glm::vec3 P = o + d * h.t;
            if (solid) {
                // Where a CSG cut meets a face the solid tapers to an edge,
                // and a ray can cross a sliver thinner than the probe step.
                // That is the shape being thin, not the crossing being wrong:
                // it shows as a second crossing straight after the first.
                pathtrace::ShapeHit h2;
                const bool sliver = s.closest(root, o, d, h.t + 1e-6f, h.t + 2.0f * eps, h2);
                const bool before = s.inside(root, o + d * (h.t - eps));
                const bool after  = s.inside(root, o + d * (h.t + eps));
                if (!sliver && (before || !after)) ++st.badCross;   // from outside, the first crossing goes in
                if (!sliver && (s.inside(root, P + h.n * eps) || !s.inside(root, P - h.n * eps))) ++st.badNormal;
            }
            // Nothing solid between the ray's start and its first hit.
            for (int k = 1; k < 200 && solid; ++k) {
                const float t = h.t * static_cast<float>(k) / 200.0f;
                if (t > h.t - eps) break;
                if (s.inside(root, o + d * t)) { ++st.missedSolid; break; }
            }
        } else if (solid) {
            for (int k = 0; k < 600; ++k) {
                const float t = extent * 6.0f * static_cast<float>(k) / 600.0f;
                if (s.inside(root, o + d * t)) { ++st.missedSolid; break; }
            }
        }
    }
    return st;
}

std::shared_ptr<pathtrace::Scene> sceneOf(const std::string& src, std::string* err = nullptr) {
    Options o;
    Result r = build(src, o);
    if (!r.ok) {
        if (err) *err = r.errors.empty() ? "?" : r.errors[0].format();
        return nullptr;
    }
    return r.scene;
}

void shapes() {
    std::printf("\nexact shapes and CSG, against their own inside()\n");
    struct Case { const char* name; const char* src; bool solid; float extent; };
    const Case cases[] = {
        {"sphere", "sphere { center: [0.3, -0.2, 0.1], radius: 0.8 }", true, 1.0f},
        {"ellipsoid (scaled, rotated sphere)", "sphere { radius: 1, scale: [1, 0.4, 0.7], rotate: [20, 40, 10] }", true, 1.0f},
        {"box", "box { min: [-0.7, -0.3, -0.5], max: [0.6, 0.9, 0.4], rotate: [10, 30, 5] }", true, 1.0f},
        {"cylinder", "cylinder { from: [-0.5, -0.6, 0], to: [0.4, 0.7, 0.2], radius: 0.35 }", true, 1.0f},
        {"cone", "cone { from: [0, -0.7, 0], to: [0.2, 0.8, 0.1], radius: 0.6, top_radius: 0.15 }", true, 1.0f},
        {"pointed cone", "cone { from: [0, -0.7, 0], to: [0, 0.8, 0], radius: 0.6 }", true, 1.0f},
        {"torus", "torus { major: 0.7, minor: 0.22, rotate: [60, 10, 0] }", true, 1.0f},
        {"isosurface (sphere field)", "isosurface { function: fn(p) => length(p) - 0.8, bound: 1 }", true, 1.0f},
        {"isosurface (noisy)", "isosurface { function: fn(p) => length(p) - 0.7 + 0.1 * noise(p * 4), bound: 1 }", true, 1.0f},
        {"isosurface cut by its box", "isosurface { function: fn(p) => length(p) - 1.2, bound: 0.8 }", true, 1.0f},
        {"difference: box minus sphere", "difference { box { size: 1.6 }; sphere { radius: 1.0 } }", true, 1.0f},
        {"difference: three holes", "difference { box { size: 1.4 }; cylinder([0,-1,0],[0,1,0],0.3); "
                                    "cylinder([-1,0,0],[1,0,0],0.3); cylinder([0,0,-1],[0,0,1],0.3) }", true, 1.0f},
        {"intersection: lens", "intersection { sphere([0.4, 0, 0], 0.8); sphere([-0.4, 0, 0], 0.8) }", true, 1.0f},
        {"merge", "merge { sphere([0.3, 0, 0], 0.5); box { size: 0.7 } }", true, 1.0f},
        {"nested", "difference { intersection { box { size: 1.5 }; sphere { radius: 0.95 } }; "
                   "merge { cylinder([0,-1,0],[0,1,0],0.25); torus { major: 0.6, minor: 0.12 } } }", true, 1.0f},
        {"rounded box", "rounded_box(size: [1.2, 0.8, 1], radius: 0.15)", true, 1.0f},
        {"capsule", "capsule([0, -0.5, 0], [0.3, 0.5, 0], 0.3)", true, 1.0f},
    };
    for (const Case& c : cases) {
        std::string err;
        auto sc = sceneOf(c.src, &err);
        if (!sc || !sc->shapes || sc->shapes->roots.empty()) { check(false, c.name, "did not build: " + err); continue; }
        const int root = sc->shapes->roots[0];
        const ShapeStats st = probeShape(*sc->shapes, root, glm::vec3(0.0f), c.extent, c.solid);
        const bool ok = st.hits > 100 && st.badCross <= st.rays / 500 && st.badNormal <= st.rays / 500 &&
                        st.missedSolid <= st.rays / 500;
        check(ok, c.name,
              std::to_string(st.hits) + "/" + std::to_string(st.rays) + " hit, crossing wrong " +
                  std::to_string(st.badCross) + ", normal wrong " + std::to_string(st.badNormal) +
                  ", solid passed through " + std::to_string(st.missedSolid));
    }

    // Answers known in advance.
    auto sph = sceneOf("sphere { radius: 2 }");
    pathtrace::ShapeHit h;
    const bool hit = sph && sph->shapes->closest(sph->shapes->roots[0], glm::vec3(0, 0, -10), glm::vec3(0, 0, 1),
                                                 1e-4f, 1e9f, h);
    check(hit && std::fabs(h.t - 8.0f) < 1e-4f && glm::length(h.n - glm::vec3(0, 0, -1)) < 1e-4f,
          "a ray meets a sphere of radius 2 at distance 8, facing back", hit ? num(h.t) : "missed");
    auto cut = sceneOf("difference { box { size: 2 }; sphere { radius: 1.3 } }");
    const bool through = cut && cut->shapes->closest(cut->shapes->roots[0], glm::vec3(-10, 0, 0), glm::vec3(1, 0, 0),
                                                     1e-4f, 1e9f, h);
    check(!through, "down the axis of a cube with a bigger sphere cut out, there is nothing to hit");
    const glm::vec3 dd = glm::normalize(glm::vec3(1, 1, 1));
    const bool corner = cut && cut->shapes->closest(cut->shapes->roots[0], -dd * 10.0f, dd, 1e-4f, 1e9f, h);
    check(corner && std::fabs(h.t - (10.0f - std::sqrt(3.0f))) < 1e-3f,
          "but the cube's corner is still there", corner ? num(h.t) : "missed");
    auto tor = sceneOf("torus { major: 2, minor: 0.5 }");
    const bool th = tor && tor->shapes->closest(tor->shapes->roots[0], glm::vec3(-10, 0, 0), glm::vec3(1, 0, 0),
                                                1e-4f, 1e9f, h);
    check(th && std::fabs(h.t - 7.5f) < 1e-4f, "a torus of radii 2 and 0.5 is met at 7.5", th ? num(h.t) : "missed");
    const bool hole = tor && tor->shapes->closest(tor->shapes->roots[0], glm::vec3(0, 10, 0), glm::vec3(0, -1, 0),
                                                  1e-4f, 1e9f, h);
    check(!hole, "and its hole is empty");
}

// --- Light -----------------------------------------------------------------------
void furnace() {
    std::printf("\nlight\n");
    // A white sphere in a white world: whatever bounces where, every path
    // comes back with radiance 1. An integrator that loses or invents energy
    // on an exact surface -- a wrong normal, a self-intersection -- fails here.
    auto sc = sceneOf(R"(
        render { width: 48, height: 48, samples: 256, bounces: 32, clamp: 0 }
        camera { position: [0, 0, 4], look_at: [0, 0, 0], fov: 20 }
        sky { zenith: white, horizon: white, ground: white }
        sphere { radius: 1, material: material { color: white, roughness: 1 } }
    )");
    if (!sc) { check(false, "the furnace builds"); return; }
    pathtrace::Settings s;
    s.width = 48; s.height = 48; s.samples = 256; s.maxBounces = 32; s.clampIndirect = 0.0f;
    pathtrace::Job job;
    job.start(sc, s);
    while (job.running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::vector<float> hdr;
    job.snapshotHdr(hdr);
    double sum = 0.0;
    int n = 0;
    for (int y = 18; y < 30; ++y)
        for (int x = 18; x < 30; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * 48 + x) * 3;
            sum += (hdr[i] + hdr[i + 1] + hdr[i + 2]) / 3.0;
            ++n;
        }
    const double mean = sum / n;
    check(std::fabs(mean - 1.0) < 0.03, "a white furnace around an exact sphere comes back at 1", num(mean));
}

// --- Errors ---------------------------------------------------------------------
void errors() {
    std::printf("\nerrors point at the line\n");
    struct E { const char* src; int line; const char* says; };
    const E es[] = {
        {"sphere { radius: 1 }\nsphere { radus: 2 }", 2, "did you mean 'radius'"},
        {"let a = 1\nlet b = a + [1, 2]\nlet c = b + [1, 2, 3]", 3, "cannot add"},
        {"fn f(x) = x\n\nf(1, 2)", 3, "takes 1"},
        {"sphere {\n  radius: 1\n", 1, "never closed"},
        {"let m = material { color: fn(p) { while true { } } }\nsphere { material: m }", 1, "while"},
        {"box { color: red }", 1, "material"},
        {"sphere {\n  material: material { color: gold }\n}", 2, "not a material"},
    };
    for (const E& e : es) {
        Options o;
        Result r = build(e.src, o);
        const bool ok = !r.ok && !r.errors.empty() && r.errors[0].line == e.line &&
                        r.errors[0].message.find(e.says) != std::string::npos;
        check(ok, std::string("\"") + e.says + "\" on line " + std::to_string(e.line),
              r.errors.empty() ? "no error" : r.errors[0].format());
    }
}

// --- The bundled examples ----------------------------------------------------------
fs::path examplesDir() {
    for (const fs::path& p : {fs::path("strahl-lib/examples"), fs::path("sandbox/strahl/examples"),
                              fs::path("../../../sandbox/strahl/examples")})
        if (fs::is_directory(p)) return p;
    return {};
}

// Completion: what the editor offers at a caret, written as `|` in the text.
void completion() {
    std::printf("\ncompletion\n");
    const fs::path lib = examplesDir().empty() ? fs::path() : examplesDir().parent_path();
    const auto ask = [&](std::string text, bool manual = false) {
        complete::Request q;
        q.caret = text.find('|');
        text.erase(q.caret, 1);
        q.text = text;
        q.manual = manual;
        if (!lib.empty()) q.libraryDirs.push_back(lib);
        return complete::complete(q);
    };
    const auto first = [](const complete::Result& r) {
        return r.items.empty() ? std::string("(nothing)") : r.items[0].label;
    };
    const auto shown = [](const complete::Result& r, std::size_t n = 6) {
        std::string s;
        for (std::size_t i = 0; i < r.items.size() && i < n; ++i) s += (i ? " " : "") + r.items[i].label;
        return s.empty() ? std::string("(nothing)") : s;
    };
    const auto has = [](const complete::Result& r, const std::string& label, std::size_t within) {
        for (std::size_t i = 0; i < r.items.size() && i < within; ++i)
            if (r.items[i].label == label) return true;
        return false;
    };

    complete::Result r = ask("sphere { ra|");
    check(first(r) == "radius" && r.items[0].insert == "radius: ", "a sphere's block offers its properties",
          shown(r));
    r = ask("sphere {\n    |\n}", true);
    check(first(r) == "center" && has(r, "radius", 2), "asked for in an empty block: the positional ones first",
          shown(r));
    r = ask("sphere { material: go|");
    check(first(r) == "gold", "after material: a material", shown(r));
    r = ask("sphere { material: | }", true);
    check(has(r, "gold", 30) && has(r, "metal", 30) && !has(r, "sin", 30), "material: offers materials, not maths",
          shown(r, 10));
    r = ask("cylinder { op|");
    check(first(r) == "open", "a cylinder's open", shown(r));
    r = ask("cylinder { open: t|");
    check(first(r) == "true", "a flag wants true", shown(r));
    r = ask("material { bump: |", true);
    check(has(r, "fn(p) =>", 4) && has(r, "bumps", 12), "bump: offers a function and the bump maps", shown(r, 12));
    r = ask("render { tonemap: | }", true);
    check(first(r).front() == '"', "tonemap: offers the names", shown(r));
    r = ask("le|");
    check(first(r) == "let" && r.items[0].insert == "let ", "a statement starts with a keyword", shown(r));
    r = ask("sph|");
    check(first(r) == "sphere" && r.items[0].insert == "sphere {  }" && r.items[0].caret == 9,
          "a kind is inserted with its block, the caret inside", shown(r));
    r = ask("let s = sphere { }\ns { ra|");
    check(first(r) == "radius", "a block on a variable knows the variable's kind", shown(r));
    r = ask("fn ring(n = 8, radius = 2) { }\nri|");
    check(first(r) == "ring" && r.items[0].insert == "ring()" && r.items[0].caret == 5 &&
              r.items[0].detail == "(n = 8, radius = 2)",
          "the script's own functions, with their parameters", r.items.empty() ? "" : r.items[0].detail);
    r = ask("fn f(alpha, beta) {\n    al|\n}");
    check(first(r) == "alpha", "parameters inside the body", shown(r));
    r = ask("let g = fn(px) => p|");
    check(first(r) == "px", "parameters of an expression body", shown(r));
    r = ask("let g = fn(px) => 1\nlet h = p|");
    check(first(r) != "px", "...and not after it", shown(r));
    r = ask("for idx in 0..4 {\n    print(id|)\n}");
    check(first(r) == "idx", "a loop variable inside its loop", shown(r));
    r = ask("// sph|");
    check(r.items.empty(), "nothing in a comment", shown(r));
    r = ask("print(\"sph|\")");
    check(r.items.empty(), "nothing in a string", shown(r));
    r = ask("let v = [1, 2, 3]\nprint(v.x|)");
    check(r.items.empty(), "nothing after a dot (swizzles)", shown(r));
    r = ask("let ra|");
    check(r.items.empty(), "nothing where a new name is written", shown(r));
    r = ask("let col = #ff|");
    check(r.items.empty(), "nothing in a #colour", shown(r));

    r = ask("plane { material: matte(checker(#ddd, #555, si|");
    check(first(r) == "size" && r.items[0].insert == "size: ", "a call offers its parameters by name", shown(r));
    check(r.hasSignature && r.signature.label == "checker(a = white, b = black, size = 1)" && r.signature.active == 2,
          "the signature of the call, at the third parameter",
          r.signature.label + " @" + std::to_string(r.signature.active));
    r = ask("marble(scale: 2, |)", true);
    check(!has(r, "scale", 1000) && has(r, "base", 6), "a named argument given is not offered again", shown(r));
    r = ask("marble(scale: |)");
    check(r.hasSignature && r.signature.active >= 0 &&
              r.signature.label.substr(static_cast<std::size_t>(r.signature.params[static_cast<std::size_t>(r.signature.active)].first), 5) == "scale",
          "a named argument is the active parameter", r.signature.label);
    r = ask("sphere([0, 1, 0], |)");
    check(r.hasSignature && r.signature.label.rfind("sphere(center, radius", 0) == 0 && r.signature.active == 1,
          "a kind called like a function", r.signature.label + " @" + std::to_string(r.signature.active));
    r = ask("let x = fbm(p, oc|");
    check(first(r) == "octaves" && r.hasSignature && r.signature.label == "fbm(p, octaves = 5)",
          "built-ins have their parameter names", shown(r) + " / " + r.signature.label);

    if (!lib.empty()) {
        r = ask("import \"stu|\"");
        check(r.inString && first(r) == "studio", "import offers the library's files", shown(r));
        r = ask("import \"studio\"\ncyc|");
        check(first(r) == "cyclorama" && r.items[0].detail.find("width") != std::string::npos,
              "an import's functions, with their parameters", r.items.empty() ? "" : r.items[0].detail);
    }

    // The camera written back: the script's own camera moves, in whatever form it has.
    const glm::dvec3 P(1, 2, 3), T(0, 0.5, -1);
    struct C { const char* src; const char* expect; const char* what; };
    const C cs[] = {
        {"camera { position: [0, 1.5, 5], look_at: [0, 0.5, 0], fov: 40 }",
         "camera { position: [1, 2, 3], look_at: [0, 0.5, -1], fov: 40 }", "a block's position and look_at"},
        {"camera {\n    position: [0, 1.5, 5]\n    look_at: [0, 0.5, 0]\n}",
         "camera {\n    position: [1, 2, 3]\n    look_at: [0, 0.5, -1]\n}", "a block over several lines"},
        {"camera { [0, 1.5, 5], [0, 0.5, 0] }", "camera { [1, 2, 3], [0, 0.5, -1] }", "unnamed values by the kind's order"},
        {"camera([0, 1.5, 5], [0, 0.5, 0], fov: 40)", "camera([1, 2, 3], [0, 0.5, -1], fov: 40)", "a camera called like a function"},
        {"camera { position: [0, 1, 5], fov: 30 }", "camera { position: [1, 2, 3], fov: 30, look_at: [0, 0.5, -1] }",
         "a missing look_at is added"},
        {"camera { }", "camera { position: [1, 2, 3], look_at: [0, 0.5, -1] }", "an empty block gets both"},
        {"camera { position: [0, 1, 5], direction: [0, 0, -1] }",
         "camera { position: [1, 2, 3], direction: [-0.228, -0.342, -0.912] }", "a camera aimed by direction keeps it"},
    };
    for (const C& c : cs) {
        const complete::CameraEdit e = complete::setCamera(c.src, 1, 1, P, T);
        check(e.ok && e.text == c.expect, std::string("camera written: ") + c.what, e.ok ? e.text : e.why);
    }
    complete::CameraEdit e = complete::setCamera("camera { position: [0, 1, 5], look_at: [0, 0, 0] }", 1, 1, P, T, 35.5);
    check(e.ok && e.text == "camera { position: [1, 2, 3], look_at: [0, 0.5, -1], fov: 35.5 }",
          "camera written: a fov is added when given", e.ok ? e.text : e.why);
    const std::string anim = "camera { position: [5 * sin(time * 360), 2, 5], look_at: [0, 0, 0] }";
    e = complete::setCamera(anim, 1, 1, P, T);
    check(!e.ok && e.text == anim && e.why.find("position") != std::string::npos,
          "a computed position is left alone, and said so", e.why);
    e = complete::setCamera("render { width: 640 }\nsphere { }\n", 0, 0, P, T);
    check(e.ok && e.text == "render { width: 640 }\ncamera { position: [1, 2, 3], look_at: [0, 0.5, -1] }\nsphere { }\n",
          "no camera: one is added after render", e.text);
    e = complete::setCamera("sphere { }\ncamera { position: [0, 1, 5] }", 1, 1, P, T);
    check(!e.ok, "a stale place is refused", e.why);
    e = complete::setCamera("camera([0, 1, 5], [0, 0, 0]) { fov: 30 }", 1, 30, P, T);
    check(e.ok && e.text == "camera([1, 2, 3], [0, 0.5, -1]) { fov: 30 }",
          "camera written: found from the block after a call", e.ok ? e.text : e.why);
    e = complete::setCamera("let cam = camera { position: [0, 1, 5] }\ncam { fov: 30 }", 2, 5, P, T);
    check(e.ok && e.text == "let cam = camera { position: [0, 1, 5] }\ncam { fov: 30, position: [1, 2, 3], look_at: [0, 0.5, -1] }",
          "camera written: a changed copy gets its own values", e.ok ? e.text : e.why);
    // Round trip: what is written is where the built camera stands.
    {
        const std::string src = "let h = 2\nsphere { }\ncamera { position: [0, 1.5, 5], look_at: [0, 0.5, 0] }\n";
        Options o;
        const Result r0 = build(src, o);
        e = complete::setCamera(src, r0.camera.line, r0.camera.col, P, T);
        const Result r1 = e.ok ? build(e.text, o) : Result{};
        check(r0.ok && r0.camera.line == 3 && e.ok && r1.ok &&
                  glm::length(glm::dvec3(r1.scene->camera.position) - P) < 1e-4 &&
                  glm::length(r1.camera.target - T) < 1e-4,
              "the camera written is the camera built", e.ok ? "" : e.why);
    }

    // Every name the language knows is documented.
    std::string missing;
    const Vocabulary& v = vocabulary();
    for (const std::vector<std::string>* names : {&v.functions, &v.objects, &v.properties})
        for (const std::string& n : *names)
            if (complete::documentation(n).empty()) missing += " " + n;
    check(missing.empty(), "every kind, property and function has documentation", missing);
}

// The reference (docs/strahl.md, Help -> Strahl Reference): every name the
// language knows has a table row of its own -- the row F1 lands on, found by
// the same rule (DocPanel.hpp) -- and every call a row names still exists.
void reference() {
    std::printf("\nthe reference\n");
    fs::path file;
    for (const fs::path& p : {fs::path("strahl.md"), fs::path("docs/strahl.md"), fs::path("../../../docs/strahl.md")})
        if (fs::is_regular_file(p)) { file = p; break; }
    if (file.empty()) { check(false, "strahl.md is found"); return; }
    std::vector<std::string> lines;
    {
        std::ifstream in(file);
        for (std::string l; std::getline(in, l);) lines.push_back(l);
    }
    const auto defined = [&](const std::string& word) {
        for (const std::string& l : lines)
            if (docpanel::defines(l, word)) return true;
        return false;
    };

    const Vocabulary& v = vocabulary();
    std::string missing;
    for (const std::vector<std::string>* names : {&v.keywords, &v.objects, &v.properties})
        for (const std::string& n : *names)
            if (!defined(n)) missing += " " + n;
    for (const std::string& n : v.functions)
        if (!defined(n + "(")) missing += " " + n + "(";
    Options o;
    Interp in(o);
    std::vector<std::string> values;
    for (const auto& [name, val] : in.globals->vars)
        if (val.t != T::Func && val.t != T::Native) values.push_back(name);
    for (const std::string& n : values)
        if (!defined(n)) missing += " " + n;
    check(missing.empty(), "every keyword, kind, property, function and value has a row (" +
                               std::to_string(v.keywords.size() + v.objects.size() + v.properties.size() +
                                              v.functions.size() + values.size()) + " names)",
          missing);

    // The other way round: a row for a call that is gone sends F1 to a lie.
    // The library's own fns (import "studio") count as known.
    std::vector<std::string> known = v.functions;
    known.insert(known.end(), v.objects.begin(), v.objects.end());
    if (const fs::path lib = examplesDir(); !lib.empty())
        for (const auto& e : fs::directory_iterator(lib.parent_path())) {
            if (e.path().extension() != ".strahl") continue;
            std::ifstream f(e.path());
            for (std::string l; std::getline(f, l);)
                if (l.rfind("fn ", 0) == 0) known.push_back(l.substr(3, l.find('(') - 3));
        }
    std::string stale;
    int calls = 0;
    for (const std::string& l : lines)
        for (const std::string& span : docpanel::definedSpans(l)) {
            std::size_t e = 0;
            while (e < span.size() && (std::isalnum(static_cast<unsigned char>(span[e])) || span[e] == '_')) ++e;
            if (e == 0 || e >= span.size() || span[e] != '(') continue;
            ++calls;
            if (std::find(known.begin(), known.end(), span.substr(0, e)) == known.end()) stale += " " + span;
        }
    check(stale.empty() && calls > 0, "every call the reference describes exists (" + std::to_string(calls) + ")",
          stale);
}

void examples(const fs::path& outDir) {
    std::printf("\nthe examples\n");
    const fs::path dir = examplesDir();
    if (dir.empty()) { check(false, "the examples folder is found"); return; }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir))
        if (e.path().extension() == ".strahl") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    check(!files.empty(), "there are examples", dir.string());
    for (const fs::path& f : files) {
        Options o;
        o.libraryDirs.push_back(dir.parent_path());
        Result r = buildFile(f, o);
        check(r.ok, f.filename().string() + " builds",
              r.ok ? std::to_string(r.shapes) + " shapes, " + std::to_string(r.triangles) + " triangles, " +
                         std::to_string(r.functions) + " functions, " + num(r.seconds) + "s"
                   : r.errors[0].format());
        if (!r.ok || outDir.empty()) continue;
        pathtrace::Settings s = r.settings;
        const float aspect = static_cast<float>(s.width) / static_cast<float>(s.height);
        s.height = 180;
        s.width = static_cast<int>(180 * aspect);
        s.samples = 24;
        pathtrace::Job job;
        job.start(r.scene, s);
        while (job.running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::vector<unsigned char> px;
        job.snapshotLdr(px);
        const fs::path png = outDir / ("strahl-" + f.stem().string() + ".png");
        stbi_write_png(png.string().c_str(), s.width, s.height, 4, px.data(), s.width * 4);
        std::printf("      wrote %s (%.1fs)\n", png.string().c_str(), job.elapsedSeconds());
    }
}

} // namespace

int main(int argc, char** argv) {
    const fs::path outDir = argc > 1 ? fs::path(argv[1]) : fs::path();
    if (!outDir.empty()) fs::create_directories(outDir);
    std::printf("strahlcheck\n");
    language();
    emission();
    compiler();
    shapes();
    furnace();
    errors();
    completion();
    reference();
    examples(outDir);
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "passed", g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
