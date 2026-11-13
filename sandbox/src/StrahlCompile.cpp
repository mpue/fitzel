// Strahl functions to the tracer's VM (TraceFunc.hpp).
//
// A material's colour, a bump map, an isosurface's field: functions of the
// point that the tracer calls hundreds of millions of times. They are written
// in the same language as the rest of the scene and compiled here, once, to a
// straight line of vec3 instructions.
//
// EVERYTHING THAT CAN BE KNOWN IS WORKED OUT NOW. Each expression compiles to
// an operand that is either a register (it depends on the point) or a value
// already computed -- by the interpreter itself, so the two cannot disagree.
// That is what makes the prelude's patterns cheap and what lets a function
// use things no per-ray code could: `checker(red, marble())` is a call that
// returns a function, made once here; the list of colours a ramp() is given
// is built here; an image is loaded here. Only arithmetic on the point is left
// for the tracer.
//
// WHAT CANNOT BE COMPILED is reported, with the place: a while loop (its
// length is not known), a for loop over something that depends on the point,
// recursion, building lists or objects per ray. Calls are inlined, `for` loops
// over known ranges unrolled, `if` on a known condition decided now and on the
// point compiled to a jump.

#include <algorithm>
#include <climits>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

#include "PathTrace.hpp"
#include "StrahlInternal.hpp"

namespace strahl {

namespace {

using Op = pathtrace::Func::Op;

constexpr int kNoReg = INT_MIN;

struct RawIns {
    Op  op = Op::Mov;
    int dst = 0, a = 0, b = 0, c = 0;
    int imm = 0;
};

// A compiled expression: a known value, or a register at run time, or both
// (a known number that has been given a constant register).
struct Operand {
    bool  known = false;
    Value val;
    int   reg = kNoReg;
    int   width = 1;     // 1 scalar, 2, 3 -- for registers and known numerics
};

struct Scope {
    std::unordered_map<std::string, Operand> vars;
};

// One function being inlined: its own locals, and the closure environment
// names that are not locals are looked up (and folded) in.
struct Frame {
    std::vector<Scope> scopes;
    EnvP               env;
    std::unordered_set<std::string> assigned;   // names some statement assigns to
    // Returns: the register they write, and the jumps to patch to the end.
    int  retReg = kNoReg;
    int  retWidth = 1;
    std::vector<std::size_t> retJumps;
    std::string name;
};

// Which built-ins are single VM instructions, and their shape.
enum class Shape { Unary, Binary, Ternary, Width1, WidthOct, Special };
struct VmFn { const char* name; Op op; Shape shape; };
constexpr VmFn kVm[] = {
    {"sin", Op::Sin, Shape::Unary}, {"cos", Op::Cos, Shape::Unary}, {"tan", Op::Tan, Shape::Unary},
    {"asin", Op::Asin, Shape::Unary}, {"acos", Op::Acos, Shape::Unary}, {"atan", Op::Atan, Shape::Unary},
    {"sqrt", Op::Sqrt, Shape::Unary}, {"exp", Op::Exp, Shape::Unary}, {"log", Op::Log, Shape::Unary},
    {"abs", Op::Abs, Shape::Unary}, {"sign", Op::Sign, Shape::Unary}, {"floor", Op::Floor, Shape::Unary},
    {"ceil", Op::Ceil, Shape::Unary}, {"round", Op::Round, Shape::Unary}, {"fract", Op::Fract, Shape::Unary},
    {"atan2", Op::Atan2, Shape::Binary}, {"pow", Op::Pow, Shape::Binary}, {"mod", Op::Mod, Shape::Binary},
    {"step", Op::Step, Shape::Binary},
    {"mix", Op::Mix, Shape::Ternary}, {"lerp", Op::Mix, Shape::Ternary},
    {"smoothstep", Op::Smoothstep, Shape::Ternary},
    {"noise", Op::Noise, Shape::Width1}, {"voronoi", Op::Voronoi, Shape::Width1},
    {"cell_id", Op::VoronoiId, Shape::Width1}, {"hash", Op::Hash, Shape::Width1},
    {"parity", Op::Checker, Shape::Width1},
    {"fbm", Op::Fbm, Shape::WidthOct}, {"turbulence", Op::Turbulence, Shape::WidthOct},
    // Special: argument handling of their own.
    {"min", Op::Min, Shape::Special}, {"max", Op::Max, Shape::Special},
    {"clamp", Op::Clamp, Shape::Special}, {"saturate", Op::Clamp, Shape::Special},
    {"length", Op::Length, Shape::Special}, {"normalize", Op::Normalize, Shape::Special},
    {"dot", Op::Dot, Shape::Special}, {"distance", Op::Distance, Shape::Special},
    {"cross", Op::Cross, Shape::Special}, {"reflect", Op::Reflect, Shape::Special},
    {"vec", Op::Combine, Shape::Special}, {"rgb", Op::Combine, Shape::Special},
    {"gray", Op::Combine, Shape::Special},
    {"radians", Op::Mul, Shape::Special}, {"degrees", Op::Mul, Shape::Special},
    {"log2", Op::Log, Shape::Special}, {"log10", Op::Log, Shape::Special},
    {"ramp", Op::Ramp, Shape::Special}, {"texture", Op::Texture, Shape::Special},
    {"random", Op::Hash, Shape::Special}, {"at", Op::Mov, Shape::Special},
};

const VmFn* vmFn(const std::string& name) {
    for (const VmFn& f : kVm)
        if (name == f.name) return &f;
    return nullptr;
}

} // namespace

bool isVmBuiltin(const std::string& name) { return vmFn(name) != nullptr; }

class Compiler {
public:
    Compiler(Interp& in, const Loc& loc) : m_in(in), m_loc(loc) {}

    std::shared_ptr<const pathtrace::Func> run(const Value& fnValue) {
        if (fnValue.t != T::Func)
            throw Error(m_loc, "only a function written in Strahl can be compiled for the tracer");
        const Closure& c = fnValue.closure();
        const std::size_t np = c.fn->params.size();
        std::vector<Operand> args;
        // The tracer passes (point, normal, uv).
        static const int kWidths[3] = {3, 3, 2};
        for (std::size_t i = 0; i < np && i < 3; ++i) {
            Operand o;
            o.reg   = m_temps++;
            o.width = kWidths[i];
            args.push_back(o);
        }
        m_params = static_cast<int>(args.size());
        const Operand r = inlineCall(fnValue, args, {}, m_loc);
        if (r.known && !r.val.isNumeric())
            throw Error(m_loc, "a per-ray function must return a number or a colour, this one returns a " +
                                   std::string(typeName(r.val.t)));
        Operand res = r;
        const int reg = regOf(res, m_loc);
        return finish(reg, res.width);
    }

private:
    friend class Interp;
    Interp&              m_in;
    Loc                  m_loc;
    std::vector<RawIns>  m_code;
    std::vector<glm::vec3> m_consts;
    std::vector<pathtrace::Ramp> m_ramps;
    std::vector<std::shared_ptr<const pathtrace::Image>> m_images;
    std::vector<Frame>   m_frames;
    int                  m_temps  = 0;
    int                  m_params = 0;

    // --- Registers and instructions ---------------------------------------
    // During compilation constants are numbered -1, -2, ... and temporaries
    // 0, 1, ...; finish() lays them out as the VM wants (constants, then the
    // parameters, then the rest).
    int constReg(const glm::vec3& v) {
        for (std::size_t i = 0; i < m_consts.size(); ++i)
            if (m_consts[i] == v) return -static_cast<int>(i) - 1;
        m_consts.push_back(v);
        return -static_cast<int>(m_consts.size());
    }
    int temp() { return m_temps++; }

    int emit(Op op, int a = 0, int b = 0, int c = 0, int imm = 0) {
        const int d = temp();
        m_code.push_back({op, d, a, b, c, imm});
        return d;
    }
    void emitTo(int dst, Op op, int a = 0, int b = 0, int c = 0, int imm = 0) {
        m_code.push_back({op, dst, a, b, c, imm});
    }

    // The register an operand lives in, giving a known number one.
    int regOf(Operand& o, const Loc& loc) {
        if (o.reg != kNoReg) return o.reg;
        if (o.known) {
            if (o.val.t == T::Num || o.val.t == T::Bool) {
                o.reg = constReg(glm::vec3(static_cast<float>(o.val.v[0])));
                o.width = 1;
                return o.reg;
            }
            if (o.val.t == T::Vec) {
                if (o.val.n > 3)
                    throw Error(loc, "per-ray functions work with up to 3 components; this vector has 4");
                o.reg = constReg(glm::vec3(o.val.vec3()));
                o.width = o.val.n;
                return o.reg;
            }
            throw Error(loc, std::string("a ") + typeName(o.val.t) +
                                 " cannot be used here in a per-ray function (only numbers and vectors can)");
        }
        throw Error(loc, "internal: operand without a value");
    }

    static Operand knownOf(const Value& v) {
        Operand o;
        o.known = true;
        o.val = v;
        o.width = v.t == T::Vec ? v.n : 1;
        return o;
    }
    static Operand regOperand(int reg, int width) {
        Operand o;
        o.reg = reg;
        o.width = width;
        return o;
    }

    std::shared_ptr<const pathtrace::Func> finish(int resultReg, int resultWidth) {
        auto f = std::make_shared<pathtrace::Func>();
        const int C = static_cast<int>(m_consts.size());
        const auto map = [C](int r) { return r < 0 ? -r - 1 : C + r; };
        f->consts = m_consts;
        f->params = m_params;
        f->registers = C + m_temps;
        if (f->registers > pathtrace::Func::kMaxRegisters)
            throw Error(m_loc, "this function is too big to compile (" + std::to_string(f->registers) +
                                   " registers; the limit is " +
                                   std::to_string(pathtrace::Func::kMaxRegisters) + ")");
        f->code.reserve(m_code.size());
        for (const RawIns& r : m_code) {
            pathtrace::Func::Ins ins;
            ins.op  = r.op;
            ins.dst = static_cast<std::uint16_t>(map(r.dst));
            ins.a   = static_cast<std::uint16_t>(map(r.a));
            ins.b   = static_cast<std::uint16_t>(map(r.b));
            ins.c   = static_cast<std::uint16_t>(map(r.c));
            ins.imm = r.imm;
            f->code.push_back(ins);
        }
        f->result = map(resultReg);
        f->resultWidth = resultWidth;
        f->ramps  = std::move(m_ramps);
        f->images = std::move(m_images);
        ++m_in.compiled;
        return f;
    }

    // --- Names ---------------------------------------------------------------
    Frame& frame() { return m_frames.back(); }

    Operand* local(const std::string& name) {
        auto& sc = frame().scopes;
        for (auto it = sc.rbegin(); it != sc.rend(); ++it) {
            auto f = it->vars.find(name);
            if (f != it->vars.end()) return &f->second;
        }
        return nullptr;
    }

    Operand lookup(const std::string& name, const Loc& loc) {
        if (Operand* o = local(name)) return *o;
        Value* v = frame().env->find(name);
        if (!v) throw Error(loc, "'" + name + "' is not defined");
        return knownOf(*v);
    }

    static void collectAssigned(const Node* n, std::unordered_set<std::string>& out) {
        if (!n) return;
        if (n->kind == N::Assign && n->a && n->a->kind == N::Ident) out.insert(n->a->str);
        if (n->kind == N::Lambda || n->kind == N::FnDecl) return;   // their own frames
        collectAssigned(n->a, out);
        collectAssigned(n->b, out);
        collectAssigned(n->c, out);
        for (const Node* e : n->list) collectAssigned(e, out);
        for (const Arg& a : n->args) collectAssigned(a.value, out);
    }

    // --- Calls ---------------------------------------------------------------
    Operand inlineCall(const Value& fnValue, std::vector<Operand> pos,
                       const std::vector<std::pair<std::string, Operand>>& named, const Loc& loc) {
        const Closure& c = fnValue.closure();
        const Node* fn = c.fn;
        const std::string name = c.name.empty() ? "a function" : c.name + "()";
        if (m_frames.size() > 32)
            throw Error(loc, name + " calls itself (or calls too deep) -- per-ray functions cannot recurse");
        if (pos.size() > fn->params.size())
            throw Error(loc, name + " takes " + std::to_string(fn->params.size()) + " arguments, not " +
                                 std::to_string(pos.size()));

        Frame fr;
        fr.env  = c.env;
        fr.name = name;
        fr.scopes.emplace_back();
        collectAssigned(fn->b, fr.assigned);
        m_frames.push_back(std::move(fr));

        for (std::size_t i = 0; i < fn->params.size(); ++i) {
            const Param& p = fn->params[i];
            Operand arg;
            bool have = false;
            if (i < pos.size()) { arg = pos[i]; have = true; }
            for (const auto& [k, v] : named)
                if (k == p.name) { arg = v; have = true; }
            if (!have) {
                if (!p.def) {
                    // The tracer offers (p, n, uv); a function asking for
                    // more than that has nothing to be given.
                    m_frames.pop_back();
                    throw Error(loc, name + " is missing its argument '" + p.name + "'");
                }
                arg = expr(p.def);
            }
            bindLocal(p.name, arg, loc);
        }

        Operand result;
        if (fn->flag) {
            result = expr(fn->b);
        } else {
            result = block(fn->b, false);
        }
        Frame& f = frame();
        if (f.retReg != kNoReg) {
            // Some path returned early: everything ends in the return register.
            if (!result.known || result.val.isNumeric()) {
                if (result.known || result.reg != kNoReg) {
                    const int r = regOf(result, loc);
                    emitTo(f.retReg, Op::Mov, r);
                    f.retWidth = std::max(f.retWidth, result.width);
                }
            }
            for (const std::size_t j : f.retJumps) m_code[j].imm = static_cast<int>(m_code.size());
            result = regOperand(f.retReg, f.retWidth);
        }
        m_frames.pop_back();
        return result;
    }

    void bindLocal(const std::string& name, Operand v, const Loc& loc) {
        // A name that is assigned to somewhere gets a register of its own, so
        // the assignment -- possibly in one branch of an if -- has somewhere
        // to write that the other branch reads.
        if (frame().assigned.count(name)) {
            const int r = regOf(v, loc);
            const int own = temp();
            emitTo(own, Op::Mov, r);
            v = regOperand(own, v.width);
        }
        frame().scopes.back().vars[name] = v;
    }

    // Call a value the script had at hand: a built-in becomes instructions, a
    // Strahl function is inlined -- or, with every argument known, simply
    // called by the interpreter now.
    Operand call(const Operand& callee, std::vector<Operand>& pos,
                 std::vector<std::pair<std::string, Operand>>& named, const Loc& loc) {
        if (!callee.known)
            throw Error(loc, "only a named function can be called in a per-ray function");
        const Value& fv = callee.val;
        bool allKnown = true;
        for (const Operand& o : pos) allKnown = allKnown && o.known;
        for (const auto& [k, o] : named) allKnown = allKnown && o.known;

        if (fv.t == T::Native && fv.native().kind.empty()) {
            const std::string& name = fv.native().name;
            if (allKnown && name != "at") return knownOf(nativeNow(fv, pos, named, loc));
            const VmFn* vm = vmFn(name);
            if (!vm) throw Error(loc, name + "() cannot be used per ray -- only with arguments known before the render");
            return builtin(*vm, pos, named, loc);
        }
        if (fv.t == T::Func) {
            if (allKnown) return knownOf(nativeNow(fv, pos, named, loc));
            return inlineCall(fv, pos, named, loc);
        }
        if (fv.t == T::Native)
            throw Error(loc, "objects (" + fv.native().kind + ") cannot be made per ray");
        throw Error(loc, std::string("a ") + typeName(fv.t) + " cannot be called");
    }

    Value nativeNow(const Value& fv, const std::vector<Operand>& pos,
                    const std::vector<std::pair<std::string, Operand>>& named, const Loc& loc) {
        Args a;
        a.loc = loc;
        for (const Operand& o : pos) { a.pos.push_back(o.val); a.posLoc.push_back(loc); }
        for (const auto& [k, o] : named) a.named.emplace_back(k, o.val);
        return m_in.call(fv, a, loc);
    }

    const Operand& arg(std::vector<Operand>& pos, std::vector<std::pair<std::string, Operand>>& named,
                       std::size_t i, const char* name, const char* fn, const Loc& loc) {
        if (i < pos.size()) return pos[i];
        for (auto& [k, v] : named) if (k == name) return v;
        throw Error(loc, std::string(fn) + "() is missing its argument '" + name + "'");
    }
    const Operand* argOpt(std::vector<Operand>& pos, std::vector<std::pair<std::string, Operand>>& named,
                          std::size_t i, const char* name) {
        if (i < pos.size()) return &pos[i];
        for (auto& [k, v] : named) if (k == name) return &v;
        return nullptr;
    }

    static int scalarFlag(int width) { return width == 1 ? pathtrace::Func::kScalar : 0; }

    int widthOfBinary(int a, int b, const Loc& loc, const char* what) {
        if (a == b) return a;
        if (a == 1) return b;
        if (b == 1) return a;
        throw Error(loc, std::string("cannot ") + what + " a vector of " + std::to_string(a) +
                             " and one of " + std::to_string(b));
    }

    Operand builtin(const VmFn& vm, std::vector<Operand>& pos,
                    std::vector<std::pair<std::string, Operand>>& named, const Loc& loc) {
        const std::string name = vm.name;
        const auto A = [&](std::size_t i, const char* n) -> Operand {
            return arg(pos, named, i, n, name.c_str(), loc);
        };
        switch (vm.shape) {
        case Shape::Unary: {
            Operand x = A(0, "x");
            const int r = regOf(x, loc);
            return regOperand(emit(vm.op, r, 0, 0, scalarFlag(x.width)), x.width);
        }
        case Shape::Binary: {
            Operand x = A(0, "x"), y = A(1, "y");
            const int w = widthOfBinary(x.width, y.width, loc, name.c_str());
            const int rx = regOf(x, loc), ry = regOf(y, loc);
            return regOperand(emit(vm.op, rx, ry, 0, scalarFlag(w)), w);
        }
        case Shape::Ternary: {
            Operand a = A(0, "a"), b = A(1, "b"), t = A(2, "t");
            const int w = widthOfBinary(widthOfBinary(a.width, b.width, loc, name.c_str()), t.width, loc,
                                        name.c_str());
            const int ra = regOf(a, loc), rb = regOf(b, loc), rt = regOf(t, loc);
            return regOperand(emit(vm.op, ra, rb, rt), w);
        }
        case Shape::Width1: {
            Operand p = A(0, "p");
            const int r = regOf(p, loc);
            return regOperand(emit(vm.op, r, 0, 0, p.width), 1);
        }
        case Shape::WidthOct: {
            Operand p = A(0, "p");
            Operand oct = argOpt(pos, named, 1, "octaves") ? *argOpt(pos, named, 1, "octaves")
                                                           : knownOf(Value::num(5.0));
            const int r = regOf(p, loc), ro = regOf(oct, loc);
            return regOperand(emit(vm.op, r, ro, 0, p.width), 1);
        }
        case Shape::Special: break;
        }

        if (name == "min" || name == "max") {
            if (pos.empty()) throw Error(loc, name + "() needs something to compare");
            Operand acc = pos[0];
            for (std::size_t i = 1; i < pos.size(); ++i) {
                Operand b = pos[i];
                const int w = widthOfBinary(acc.width, b.width, loc, name.c_str());
                const int ra = regOf(acc, loc), rb = regOf(b, loc);
                acc = regOperand(emit(vm.op, ra, rb), w);
            }
            return acc;
        }
        if (name == "clamp" || name == "saturate") {
            Operand x = A(0, "x");
            Operand lo = name == "clamp" && argOpt(pos, named, 1, "lo") ? *argOpt(pos, named, 1, "lo")
                                                                       : knownOf(Value::num(0.0));
            Operand hi = name == "clamp" && argOpt(pos, named, 2, "hi") ? *argOpt(pos, named, 2, "hi")
                                                                       : knownOf(Value::num(1.0));
            const int rx = regOf(x, loc), rl = regOf(lo, loc), rh = regOf(hi, loc);
            return regOperand(emit(Op::Clamp, rx, rl, rh), x.width);
        }
        if (name == "length" || name == "normalize") {
            Operand v = A(0, "v");
            const int r = regOf(v, loc);
            if (v.width == 1) return regOperand(emit(name == "length" ? Op::Abs : Op::Sign, r), 1);
            return regOperand(emit(vm.op, r, 0, 0, v.width), name == "length" ? 1 : v.width);
        }
        if (name == "dot" || name == "distance") {
            Operand a = A(0, "a"), b = A(1, "b");
            const int w = widthOfBinary(a.width, b.width, loc, name.c_str());
            const int ra = regOf(a, loc), rb = regOf(b, loc);
            if (w == 1) {
                const int d = name == "dot" ? emit(Op::Mul, ra, rb) : emit(Op::Abs, emit(Op::Sub, ra, rb));
                return regOperand(d, 1);
            }
            return regOperand(emit(vm.op, ra, rb, 0, w), 1);
        }
        if (name == "cross" || name == "reflect") {
            Operand a = A(0, "a"), b = A(1, "b");
            const int ra = regOf(a, loc), rb = regOf(b, loc);
            return regOperand(emit(vm.op, ra, rb), 3);
        }
        if (name == "vec" || name == "rgb" || name == "gray") {
            // Gather components: each argument gives as many as it has.
            std::vector<std::pair<int, int>> comps;   // (register, component)
            for (Operand& o : pos) {
                const int r = regOf(o, loc);
                for (int k = 0; k < o.width; ++k) comps.emplace_back(r, o.width == 1 ? 0 : k);
            }
            if (comps.empty()) throw Error(loc, name + "() needs a value");
            if (comps.size() == 1 || name == "gray") {
                const auto c = comps[0];
                return regOperand(emit(Op::Combine, c.first, c.first, c.first,
                                       c.second | c.second << 2 | c.second << 4), 3);
            }
            if (comps.size() > 3)
                throw Error(loc, "per-ray functions work with up to 3 components");
            const auto c0 = comps[0], c1 = comps[1];
            const auto c2 = comps.size() > 2 ? comps[2] : comps[0];
            return regOperand(emit(Op::Combine, c0.first, c1.first, c2.first,
                                   c0.second | c1.second << 2 | c2.second << 4),
                              static_cast<int>(comps.size()));
        }
        if (name == "radians" || name == "degrees") {
            Operand x = A(0, "x");
            Operand k = knownOf(Value::num(name == "radians" ? 3.14159265358979 / 180.0
                                                             : 180.0 / 3.14159265358979));
            const int rx = regOf(x, loc), rk = regOf(k, loc);
            return regOperand(emit(Op::Mul, rx, rk), x.width);
        }
        if (name == "log2" || name == "log10") {
            Operand x = A(0, "x");
            Operand k = knownOf(Value::num(name == "log2" ? 1.0 / std::log(2.0) : 1.0 / std::log(10.0)));
            const int rx = regOf(x, loc), rk = regOf(k, loc);
            return regOperand(emit(Op::Mul, emit(Op::Log, rx), rk), x.width);
        }
        if (name == "ramp") {
            Operand t = A(0, "t");
            const Operand& stops = A(1, "colors");
            if (!stops.known || stops.val.t != T::List)
                throw Error(loc, "ramp(): the colours must be a list known before the render");
            pathtrace::Ramp ramp;
            const List& l = stops.val.listRef();
            for (std::size_t i = 0; i < l.size(); ++i) {
                const Value& s = l[i];
                Value colour = s;
                float at = l.size() > 1 ? static_cast<float>(i) / static_cast<float>(l.size() - 1) : 0.0f;
                if (s.t == T::List && s.listRef().size() == 2) {
                    at = static_cast<float>(s.listRef()[0].v[0]);
                    colour = s.listRef()[1];
                }
                if (!colour.isNumeric()) throw Error(loc, "ramp(): every stop needs a colour");
                ramp.at.push_back(at);
                ramp.color.push_back(glm::vec3(colour.vec3()));
            }
            if (ramp.at.empty()) throw Error(loc, "ramp() needs at least one colour");
            for (std::size_t i = 1; i < ramp.at.size(); ++i)
                if (ramp.at[i] < ramp.at[i - 1])
                    throw Error(loc, "ramp(): the stops must be in rising order");
            m_ramps.push_back(std::move(ramp));
            const int rt = regOf(t, loc);
            return regOperand(emit(Op::Ramp, rt, 0, 0, static_cast<int>(m_ramps.size() - 1)), 3);
        }
        if (name == "texture") {
            const Operand& img = A(0, "image");
            Operand uv = A(1, "uv");
            if (!img.known || img.val.t != T::Image)
                throw Error(loc, "texture() wants an image from load_image()");
            m_images.push_back(std::static_pointer_cast<const pathtrace::Image>(img.val.p));
            const int ru = regOf(uv, loc);
            return regOperand(emit(Op::Texture, ru, 0, 0, static_cast<int>(m_images.size() - 1)), 3);
        }
        if (name == "random") {
            // As the interpreter's random(): the hash of the seed, nudged.
            Operand s = A(0, "seed");
            Operand off = knownOf(Value::vec(glm::dvec3(0.0, 0.0, 13.37)));
            const int rs = regOf(s, loc);
            // Pad the seed out to three components before adding, so a scalar
            // seed hashes as (s, 0, 0) as it does in the interpreter.
            const int padded = s.width == 1
                             ? emit(Op::Combine, rs, constReg(glm::vec3(0.0f)), constReg(glm::vec3(0.0f)), 0)
                             : rs;
            const int h = emit(Op::Hash, emit(Op::Add, padded, regOf(off, loc)), 0, 0, 3);
            const Operand* lo = argOpt(pos, named, 1, "lo");
            const Operand* hi = argOpt(pos, named, 2, "hi");
            if (!lo && !hi) return regOperand(h, 1);
            Operand l = lo ? *lo : knownOf(Value::num(0.0));
            Operand u = hi ? *hi : knownOf(Value::num(1.0));
            return regOperand(emit(Op::Mix, regOf(l, loc), regOf(u, loc), h), 1);
        }
        if (name == "at") {
            const Operand& c = A(0, "pattern");
            if (!c.known) return c;   // already a colour computed per ray
            if (c.val.t == T::Func) {
                std::vector<Operand> rest(pos.begin() + 1, pos.end());
                const std::size_t want = c.val.closure().fn->params.size();
                if (rest.size() > want) rest.resize(want);
                return inlineCall(c.val, rest, {}, loc);
            }
            if (c.val.t == T::Native) {
                std::vector<Operand> rest(pos.begin() + 1, pos.end());
                std::vector<std::pair<std::string, Operand>> none;
                return call(c, rest, none, loc);
            }
            return c;
        }
        throw Error(loc, name + "() cannot be used per ray");
    }

    // --- Expressions ------------------------------------------------------
    Operand expr(const Node* n) {
        switch (n->kind) {
        case N::Num:   return knownOf(Value::num(n->num));
        case N::Bool:  return knownOf(Value::boolean(n->num != 0.0));
        case N::Nil:   return knownOf(Value::nil());
        case N::Str:   return knownOf(Value::str(n->str));
        case N::Color: return knownOf(Value::vec(n->color));
        case N::Ident: return lookup(n->str, n->loc);
        case N::List: {
            std::vector<Operand> items;
            bool allKnown = true;
            for (const Node* e : n->list) {
                items.push_back(expr(e));
                allKnown = allKnown && items.back().known;
            }
            if (allKnown) {
                // As the interpreter reads a list literal: two to four numbers
                // are a vector, anything else a list.
                List l;
                bool numeric = items.size() >= 2 && items.size() <= 4;
                for (const Operand& o : items) {
                    l.push_back(o.val);
                    numeric = numeric && o.val.t == T::Num;
                }
                if (numeric) {
                    double c[4];
                    for (std::size_t i = 0; i < l.size(); ++i) c[i] = l[i].v[0];
                    return knownOf(Value::vec(c, static_cast<int>(l.size())));
                }
                return knownOf(Value::list(std::move(l)));
            }
            // [a, b, c] with something per ray in it: a vector built per ray.
            std::vector<std::pair<std::string, Operand>> nn;
            if (items.size() < 2 || items.size() > 3)
                throw Error(n->loc, "a list made per ray must be a vector of 2 or 3 numbers");
            for (const Operand& o : items)
                if (o.width != 1) throw Error(n->loc, "a vector is made of numbers; use vec() to join vectors");
            const VmFn* v = vmFn("vec");
            return builtin(*v, items, nn, n->loc);
        }
        case N::Unary: {
            Operand a = expr(n->a);
            if (a.known) {
                if (n->op == T_NOT) return knownOf(Value::boolean(!a.val.truthy()));
                return knownOf(m_in.evalBinary(T_STAR, a.val, Value::num(-1.0), n->loc));
            }
            const int r = regOf(a, n->loc);
            if (n->op == T_NOT) return regOperand(emit(Op::Not, r), 1);
            return regOperand(emit(Op::Neg, r), a.width);
        }
        case N::Binary: {
            Operand a = expr(n->a), b = expr(n->b);
            if (a.known && b.known) return knownOf(m_in.evalBinary(n->op, a.val, b.val, n->loc));
            Op op;
            switch (n->op) {
            case T_PLUS: op = Op::Add; break;
            case T_MINUS: op = Op::Sub; break;
            case T_STAR: op = Op::Mul; break;
            case T_SLASH: op = Op::Div; break;
            case T_PERCENT: op = Op::Mod; break;
            case T_CARET: op = Op::Pow; break;
            case T_LT: op = Op::Lt; break;
            case T_LE: op = Op::Le; break;
            case T_GT: op = Op::Gt; break;
            case T_GE: op = Op::Ge; break;
            case T_EQ: op = Op::Eq; break;
            case T_NE: op = Op::Ne; break;
            default:
                throw Error(n->loc, std::string(tokName(n->op)) + " cannot be used per ray");
            }
            const int w = widthOfBinary(a.width, b.width, n->loc, "combine");
            const int ra = regOf(a, n->loc), rb = regOf(b, n->loc);
            return regOperand(emit(op, ra, rb, 0, op == Op::Pow || op == Op::Mod ? scalarFlag(w) : 0), w);
        }
        case N::And:
        case N::Or: {
            Operand a = expr(n->a);
            if (a.known) {
                // Short-circuit what can be decided now.
                if (n->kind == N::And && !a.val.truthy()) return knownOf(Value::boolean(false));
                if (n->kind == N::Or && a.val.truthy()) return knownOf(Value::boolean(true));
                Operand b = expr(n->b);
                if (b.known) return knownOf(Value::boolean(b.val.truthy()));
                const int rb = regOf(b, n->loc);
                return regOperand(emit(Op::Ne, rb, constReg(glm::vec3(0.0f))), 1);
            }
            Operand b = expr(n->b);
            const int ra = regOf(a, n->loc), rb = regOf(b, n->loc);
            return regOperand(emit(n->kind == N::And ? Op::And : Op::Or, ra, rb), 1);
        }
        case N::Member: {
            Operand a = expr(n->a);
            if (a.known) return knownOf(m_in.evalMember(a.val, n->str, n->loc));
            return swizzle(a, n->str, n->loc);
        }
        case N::Index: {
            Operand a = expr(n->a), i = expr(n->b);
            if (a.known && i.known) return knownOf(m_in.evalIndex(a.val, i.val, n->loc));
            if (!i.known || i.val.t != T::Num)
                throw Error(n->loc, "per ray, an index must be a number known before the render");
            int k = static_cast<int>(i.val.v[0]);
            if (k < 0) k += a.width;
            if (k < 0 || k >= a.width) throw Error(n->loc, "index out of range");
            const int r = regOf(a, n->loc);
            return regOperand(emit(Op::Combine, r, r, r, k | k << 2 | k << 4), 1);
        }
        case N::Call: {
            Operand callee = expr(n->a);
            std::vector<Operand> pos;
            std::vector<std::pair<std::string, Operand>> named;
            for (const Arg& a : n->args) {
                if (a.name.empty()) pos.push_back(expr(a.value));
                else named.emplace_back(a.name, expr(a.value));
            }
            return call(callee, pos, named, n->loc);
        }
        case N::If: return ifExpr(n);
        case N::Range: {
            Operand a = expr(n->a), b = expr(n->b);
            if (!a.known || !b.known || a.val.t != T::Num || b.val.t != T::Num)
                throw Error(n->loc, "a range per ray must run between numbers known before the render");
            Value r;
            r.t = T::Range;
            r.v[0] = a.val.v[0];
            r.v[1] = b.val.v[0];
            r.v[2] = 1.0;
            r.n = n->flag ? 1 : 0;
            return knownOf(r);
        }
        case N::Lambda:
            throw Error(n->loc, "a function cannot be defined inside a per-ray function -- "
                                "define it outside and call it");
        case N::Construct:
            throw Error(n->loc, "objects cannot be made inside a per-ray function");
        case N::Block: return block(n, true);
        default:
            throw Error(n->loc, "this cannot be used inside a per-ray function");
        }
    }

    Operand swizzle(Operand& a, const std::string& name, const Loc& loc) {
        if (name.size() > 3) throw Error(loc, "per ray, a swizzle has at most 3 components");
        int idx[3] = {0, 0, 0};
        for (std::size_t i = 0; i < name.size(); ++i) {
            int k;
            switch (name[i]) {
            case 'x': case 'r': k = 0; break;
            case 'y': case 'g': k = 1; break;
            case 'z': case 'b': k = 2; break;
            default: throw Error(loc, "a vector has no '." + name + "'");
            }
            if (k >= a.width && a.width > 1)
                throw Error(loc, "a vector of " + std::to_string(a.width) + " has no ." + std::string(1, name[i]));
            idx[i] = a.width == 1 ? 0 : k;
        }
        if (name.size() == 1) idx[1] = idx[2] = idx[0];
        else if (name.size() == 2) idx[2] = idx[0];
        const int r = regOf(a, loc);
        return regOperand(emit(Op::Combine, r, r, r, idx[0] | idx[1] << 2 | idx[2] << 4),
                          static_cast<int>(name.size()));
    }

    Operand ifExpr(const Node* n) {
        Operand cond = expr(n->a);
        if (cond.known) {
            const Node* br = cond.val.truthy() ? n->b : n->c;
            if (!br) return knownOf(Value::nil());
            return br->kind == N::If ? ifExpr(br) : block(br, true);
        }
        const int rc = regOf(cond, n->loc);
        const int out = temp();
        const std::size_t jz = m_code.size();
        emitTo(0, Op::JumpIfZero, rc);
        Operand t = n->b->kind == N::If ? ifExpr(n->b) : block(n->b, true);
        int width = 1;
        bool any = false;
        if (t.known && t.val.t == T::Nil) {
        } else {
            emitTo(out, Op::Mov, regOf(t, n->loc));
            width = t.width;
            any = true;
        }
        const std::size_t jmp = m_code.size();
        emitTo(0, Op::Jump);
        m_code[jz].imm = static_cast<int>(m_code.size());
        if (n->c) {
            Operand e = n->c->kind == N::If ? ifExpr(n->c) : block(n->c, true);
            if (!(e.known && e.val.t == T::Nil)) {
                emitTo(out, Op::Mov, regOf(e, n->loc));
                width = any ? widthOfBinary(width, e.width, n->loc, "choose between") : e.width;
                any = true;
            }
        }
        m_code[jmp].imm = static_cast<int>(m_code.size());
        if (!any) return knownOf(Value::nil());
        return regOperand(out, width);
    }

    // A block's statements; its value is the last expression's.
    Operand block(const Node* b, bool newScope) {
        if (newScope) frame().scopes.emplace_back();
        Operand last = knownOf(Value::nil());
        for (const Node* s : b->list) last = stmt(s);
        if (newScope) frame().scopes.pop_back();
        return last;
    }

    Operand stmt(const Node* s) {
        switch (s->kind) {
        case N::Let:
            bindLocal(s->str, expr(s->a), s->loc);
            return knownOf(Value::nil());
        case N::FnDecl:
            throw Error(s->loc, "a function cannot be defined inside a per-ray function");
        case N::Assign: {
            Operand* slot = local(s->a->kind == N::Ident ? s->a->str : std::string());
            if (!slot || slot->reg == kNoReg || slot->known)
                throw Error(s->loc, "per ray, only a local declared with let can be assigned to");
            Operand v = expr(s->b);
            if (s->op != T_ASSIGN) {
                const Op op = s->op == T_PLUSEQ ? Op::Add : s->op == T_MINUSEQ ? Op::Sub
                            : s->op == T_STAREQ ? Op::Mul : Op::Div;
                const int r = regOf(v, s->loc);
                v = regOperand(emit(op, slot->reg, r), widthOfBinary(slot->width, v.width, s->loc, "assign"));
            }
            emitTo(slot->reg, Op::Mov, regOf(v, s->loc));
            slot->width = std::max(slot->width, v.width);
            return knownOf(Value::nil());
        }
        case N::For: {
            Operand it = expr(s->a);
            if (!it.known) throw Error(s->a->loc, "per ray, a for loop must run over something known before the render");
            std::vector<Value> items;
            const Value& v = it.val;
            if (v.t == T::Range) {
                const double a = v.v[0], b = v.v[1], step = v.v[2] != 0.0 ? v.v[2] : 1.0;
                for (std::size_t k = 0;; ++k) {
                    const double x = a + step * static_cast<double>(k);
                    const bool in = step > 0 ? (v.n ? x <= b + 1e-9 : x < b - 1e-9)
                                             : (v.n ? x >= b - 1e-9 : x > b + 1e-9);
                    if (!in) break;
                    items.push_back(Value::num(x));
                    if (items.size() > 256) break;
                }
            } else if (v.t == T::List) {
                items = v.listRef();
            } else if (v.t == T::Vec) {
                for (int k = 0; k < v.n; ++k) items.push_back(Value::num(v.v[k]));
            } else {
                throw Error(s->a->loc, std::string("cannot loop over a ") + typeName(v.t));
            }
            if (items.size() > 256)
                throw Error(s->loc, "per ray, a loop is unrolled and may run at most 256 times");
            for (std::size_t k = 0; k < items.size(); ++k) {
                frame().scopes.emplace_back();
                if (s->str2.empty()) frame().scopes.back().vars[s->str] = knownOf(items[k]);
                else {
                    frame().scopes.back().vars[s->str]  = knownOf(Value::num(static_cast<double>(k)));
                    frame().scopes.back().vars[s->str2] = knownOf(items[k]);
                }
                block(s->b, false);
                frame().scopes.pop_back();
            }
            return knownOf(Value::nil());
        }
        case N::While:
            throw Error(s->loc, "per ray there are no while loops (a for over a range works)");
        case N::Return: {
            Frame& f = frame();
            Operand v = s->a ? expr(s->a) : knownOf(Value::num(0.0));
            if (f.retReg == kNoReg) f.retReg = temp();
            emitTo(f.retReg, Op::Mov, regOf(v, s->loc));
            f.retWidth = std::max(f.retWidth, v.width);
            f.retJumps.push_back(m_code.size());
            emitTo(0, Op::Jump);
            return knownOf(Value::nil());
        }
        case N::Break:
        case N::Continue:
            throw Error(s->loc, "per ray there is no break or continue");
        case N::Prop:
            throw Error(s->loc, "a property cannot be set inside a per-ray function");
        case N::Import:
            throw Error(s->loc, "import belongs at the top of a file");
        case N::ExprStmt:
            return expr(s->a);
        default:
            return expr(s);
        }
    }
};

std::shared_ptr<const pathtrace::Func> compile(Interp& in, const Value& closure, const Loc& loc) {
    Compiler c(in, loc);
    return c.run(closure);
}

} // namespace strahl
