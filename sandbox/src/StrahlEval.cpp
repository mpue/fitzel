// Strahl's interpreter: runs a script and collects the scene objects its
// statements make.
//
// WHERE OBJECTS GO. A statement whose value is a scene object -- a shape, a
// light, the camera -- puts it in the scene, as a POV-Ray statement does. Not
// every evaluation is a statement, though, and the rules are the ones that
// keep `let` and function calls predictable:
//
//   * at the top level, objects go into the scene;
//   * inside an object's { }, they become its children (a CSG's operands, a
//     group's members) and other bare values its positional arguments;
//   * inside a function, they are gathered, and the call RETURNS them: one
//     object as itself, several as a group. So a function that makes a ring of
//     pillars is called like any constructor, and `ring(8) { translate: ... }`
//     moves the lot;
//   * a value that is only being computed -- the right of a `let`, an argument,
//     an `if` used as an expression -- puts nothing anywhere.
//
// Values are immutable except lists, which are shared and can be pushed to:
// that is what lets a loop build a mesh's vertex list without copying it on
// every step.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <glm/gtc/matrix_transform.hpp>

#include "StrahlInternal.hpp"
#include "TraceFunc.hpp"

namespace strahl {

const char* typeName(T t) {
    switch (t) {
    case T::Nil:    return "nil";
    case T::Bool:   return "bool";
    case T::Num:    return "number";
    case T::Vec:    return "vector";
    case T::Str:    return "string";
    case T::List:   return "list";
    case T::Range:  return "range";
    case T::Func:   return "function";
    case T::Native: return "function";
    case T::Obj:    return "object";
    case T::Image:  return "image";
    case T::Mat:    return "matrix";
    }
    return "?";
}

bool Value::truthy() const {
    switch (t) {
    case T::Nil:  return false;
    case T::Bool: return v[0] != 0.0;
    case T::Num:  return v[0] != 0.0;
    default:      return true;
    }
}

namespace {

constexpr double kDeg = 3.14159265358979323846 / 180.0;

std::string fmtNum(double x) {
    if (x == std::floor(x) && std::fabs(x) < 1e15) {
        char b[32];
        std::snprintf(b, sizeof b, "%.0f", x);
        return b;
    }
    char b[32];
    std::snprintf(b, sizeof b, "%.6g", x);
    return b;
}

// --- Arithmetic on numbers and vectors ---------------------------------------
// One function for every element-wise operation: a number with a number, a
// vector with a number (the number goes to every component) or two vectors of
// the same size.
template <typename F>
Value elementwise(const Value& a, const Value& b, const Loc& loc, const char* what, F f) {
    if (a.t == T::Num && b.t == T::Num) return Value::num(f(a.v[0], b.v[0]));
    if (a.isNumeric() && b.isNumeric()) {
        const int n = a.t == T::Vec ? a.n : b.n;
        if (a.t == T::Vec && b.t == T::Vec && a.n != b.n)
            throw Error(loc, std::string("cannot ") + what + " a vector of " +
                                 std::to_string(a.n) + " and one of " + std::to_string(b.n));
        Value r;
        r.t = T::Vec;
        r.n = static_cast<std::uint8_t>(n);
        for (int i = 0; i < n; ++i)
            r.v[i] = f(a.t == T::Vec ? a.v[i] : a.v[0], b.t == T::Vec ? b.v[i] : b.v[0]);
        return r;
    }
    throw Error(loc, std::string("cannot ") + what + " " + typeName(a.t) + " and " + typeName(b.t));
}

template <typename F>
Value unaryNum(const Value& a, const Loc& loc, const char* name, F f) {
    if (a.t == T::Num) return Value::num(f(a.v[0]));
    if (a.t == T::Vec) {
        Value r = a;
        for (int i = 0; i < a.n; ++i) r.v[i] = f(a.v[i]);
        return r;
    }
    throw Error(loc, std::string(name) + "() wants a number or a vector, not " + typeName(a.t));
}

double fmodPos(double a, double b) {
    if (b == 0.0) return 0.0;
    return a - b * std::floor(a / b);
}

glm::dvec3 needVec3(const Value& v, const Loc& loc, const char* what) {
    if (v.t == T::Num) return glm::dvec3(v.v[0]);
    if (v.t == T::Vec && v.n >= 2) return v.vec3();
    throw Error(loc, std::string(what) + " wants a vector, not " + typeName(v.t));
}

int widthOf(const Value& v) { return v.t == T::Vec ? v.n : 1; }

// A vector's components as the VM's noise sees them: the unused ones zero.
glm::vec3 asPoint(const Value& v) {
    if (v.t == T::Num) return glm::vec3(static_cast<float>(v.v[0]), 0.0f, 0.0f);
    return glm::vec3(static_cast<float>(v.v[0]), static_cast<float>(v.n > 1 ? v.v[1] : 0.0),
                     static_cast<float>(v.n > 2 ? v.v[2] : 0.0));
}

} // namespace

// --- Arguments -----------------------------------------------------------------
// The helpers built-ins read their arguments with: by position, or by name, or
// a default -- with the error naming the function and the argument.
namespace {

struct ArgReader {
    Interp&     in;
    Args&       a;
    const char* fn;

    std::size_t count() const { return a.pos.size(); }
    const Value* get(std::size_t i, const char* name) const {
        if (i < a.pos.size()) return &a.pos[i];
        if (name)
            for (auto& [k, v] : a.named)
                if (k == name) return &v;
        return nullptr;
    }
    const Loc& locOf(std::size_t i) const { return i < a.posLoc.size() ? a.posLoc[i] : a.loc; }
    const Value& need(std::size_t i, const char* name) const {
        const Value* v = get(i, name);
        if (!v) throw Error(a.loc, std::string(fn) + "() is missing its argument '" + name + "'");
        return *v;
    }
    double num(std::size_t i, const char* name) const {
        const Value& v = need(i, name);
        if (v.t != T::Num && v.t != T::Bool)
            throw Error(locOf(i), std::string(fn) + "(): '" + name + "' should be a number, not " +
                                     typeName(v.t));
        return v.v[0];
    }
    double num(std::size_t i, const char* name, double def) const {
        return get(i, name) ? num(i, name) : def;
    }
    const Value& numeric(std::size_t i, const char* name) const {
        const Value& v = need(i, name);
        if (!v.isNumeric())
            throw Error(locOf(i), std::string(fn) + "(): '" + name + "' should be a number or vector, not " +
                                     typeName(v.t));
        return v;
    }
    glm::dvec3 vec3(std::size_t i, const char* name) const {
        return needVec3(need(i, name), locOf(i), fn);
    }
    List& list(std::size_t i, const char* name) const {
        const Value& v = need(i, name);
        if (v.t != T::List)
            throw Error(locOf(i), std::string(fn) + "(): '" + name + "' should be a list, not " +
                                     typeName(v.t));
        return v.listRef();
    }
    // Anything a for loop can go over, as a list: a list, a range, a
    // vector's components, a string's characters.
    List items(std::size_t i, const char* name) const {
        const Value& v = need(i, name);
        List out;
        if (v.t == T::List) return v.listRef();
        if (v.t == T::Range) {
            const double lo = v.v[0], hi = v.v[1], step = v.v[2] != 0.0 ? v.v[2] : 1.0;
            for (std::size_t k = 0;; ++k) {
                const double x = lo + step * static_cast<double>(k);
                const bool inside = step > 0 ? (v.n ? x <= hi + 1e-9 : x < hi - 1e-9)
                                             : (v.n ? x >= hi - 1e-9 : x > hi + 1e-9);
                if (!inside) break;
                if (out.size() >= 50000000) throw Error(a.loc, std::string(fn) + "(): that range is too long");
                out.push_back(Value::num(x));
            }
            return out;
        }
        if (v.t == T::Vec) {
            for (int k = 0; k < v.n; ++k) out.push_back(Value::num(v.v[k]));
            return out;
        }
        if (v.t == T::Str) {
            for (char c : v.string()) out.push_back(Value::str(std::string(1, c)));
            return out;
        }
        throw Error(locOf(i), std::string(fn) + "(): '" + name + "' should be a list or a range, not " +
                                 typeName(v.t));
    }
    std::string string(std::size_t i, const char* name) const {
        const Value& v = need(i, name);
        if (v.t != T::Str)
            throw Error(locOf(i), std::string(fn) + "(): '" + name + "' should be a string, not " +
                                     typeName(v.t));
        return v.string();
    }
};

} // namespace

// --- The interpreter -----------------------------------------------------------

Interp::Interp(const Options& opts) : options(opts) {
    globals = std::make_shared<Env>();
    installBuiltins();
    installPrelude();
    globals->vars["time"]   = Value::num(options.time);
    globals->vars["frame"]  = Value::num(options.frame);
    globals->vars["frames"] = Value::num(options.frames);
    for (const auto& [name, src] : options.defines) {
        Program& p = load("let " + name + " = (" + src + ")", "<define " + name + ">");
        Value last;
        m_collect.push_back({&scene, nullptr});
        execBlock(p.root, globals, Mode::Stmt, last, false);
        m_collect.pop_back();
        m_defined[name] = true;
    }
}

Program& Interp::load(const std::string& source, const std::string& path) {
    m_programs.push_back(std::make_unique<Program>());
    Program& p = *m_programs.back();
    p.path   = path;
    p.source = source;
    files.push_back(path);
    parse(p, static_cast<int>(files.size()) - 1);
    return p;
}

void Interp::installPrelude() {
    Program& p = load(preludeSource(), "<prelude>");
    Value last;
    m_collect.push_back({&scene, nullptr});
    execBlock(p.root, globals, Mode::Stmt, last, false);
    m_collect.pop_back();
}

void Interp::runSource(const std::string& source, const std::string& path) {
    Program& p = load(source, path);
    m_imported[path] = true;
    Value last;
    m_collect.push_back({&scene, nullptr});
    const Flow f = execBlock(p.root, globals, Mode::Stmt, last, false);
    m_collect.pop_back();
    if (f == Flow::Break || f == Flow::Continue)
        throw Error(p.root->loc, "'break' or 'continue' outside a loop");
}

std::string Interp::where(const Loc& loc) const {
    const std::string f = loc.file >= 0 && loc.file < static_cast<int>(files.size())
                        ? files[static_cast<std::size_t>(loc.file)] : std::string("?");
    return f + ":" + std::to_string(loc.line);
}

std::filesystem::path Interp::dirOf(const Loc& loc) const {
    if (loc.file >= 0 && loc.file < static_cast<int>(files.size())) {
        const std::string& f = files[static_cast<std::size_t>(loc.file)];
        if (!f.empty() && f[0] != '<') return std::filesystem::path(f).parent_path();
    }
    if (!options.file.empty()) return options.file.parent_path();
    return std::filesystem::current_path();
}

std::filesystem::path Interp::resolve(const std::string& path, const Loc& loc) const {
    namespace fs = std::filesystem;
    const fs::path p(path);
    if (p.is_absolute()) return p;
    const fs::path here = dirOf(loc) / p;
    std::error_code ec;
    if (fs::exists(here, ec)) return here;
    for (const fs::path& d : options.libraryDirs) {
        const fs::path q = d / p;
        if (fs::exists(q, ec)) return q;
    }
    return here;
}

void Interp::checkCancel(const Loc& loc) {
    if ((++m_steps & 4095) == 0 && options.cancelled && options.cancelled())
        throw Error(loc, "stopped");
}

void Interp::importFile(const std::string& path, const Loc& loc) {
    namespace fs = std::filesystem;
    fs::path p = resolve(path, loc);
    if (!fs::exists(p) && p.extension().empty()) p = resolve(path + ".strahl", loc);
    std::error_code ec;
    const std::string key = fs::weakly_canonical(p, ec).string();
    if (m_imported[key]) return;   // once, however many files ask for it
    std::ifstream f(p, std::ios::binary);
    if (!f) throw Error(loc, "cannot open '" + path + "' (looked beside the script and in the library)");
    std::stringstream ss;
    ss << f.rdbuf();
    m_imported[key] = true;
    Program& prog = load(ss.str(), p.string());
    Value last;
    // Imported definitions land in the globals; objects it makes, in the scene.
    m_collect.push_back({&scene, nullptr});
    execBlock(prog.root, globals, Mode::Stmt, last, false);
    m_collect.pop_back();
}

std::string Interp::describe(const Value& v) const {
    switch (v.t) {
    case T::Nil:  return "nil";
    case T::Bool: return v.v[0] != 0.0 ? "true" : "false";
    case T::Num:  return fmtNum(v.v[0]);
    case T::Vec: {
        std::string s = "[";
        for (int i = 0; i < v.n; ++i) s += (i ? ", " : "") + fmtNum(v.v[i]);
        return s + "]";
    }
    case T::Str:  return v.string();
    case T::List: {
        std::string s = "[";
        const List& l = v.listRef();
        for (std::size_t i = 0; i < l.size(); ++i) {
            if (i) s += ", ";
            if (i >= 16) { s += "... (" + std::to_string(l.size()) + " items)"; break; }
            s += l[i].t == T::Str ? "\"" + l[i].string() + "\"" : describe(l[i]);
        }
        return s + "]";
    }
    case T::Range:
        return fmtNum(v.v[0]) + (v.n ? "..=" : "..") + fmtNum(v.v[1]);
    case T::Func: {
        const Closure& c = v.closure();
        return c.name.empty() ? "<function>" : "<function " + c.name + ">";
    }
    case T::Native: return "<" + v.native().name + ">";
    case T::Obj:    return "<" + v.objRef().kind + ">";
    case T::Image:  return "<image>";
    case T::Mat:    return "<matrix>";
    }
    return "?";
}

bool Interp::emittable(const Value& v) {
    if (v.t == T::List) {
        for (const Value& e : v.listRef())
            if (emittable(e)) return true;
        return false;
    }
    if (v.t != T::Obj) return false;
    return v.objRef().kind != "material";
}

void Interp::emit(const Value& v, const Loc& loc) {
    if (v.t == T::List) {
        for (const Value& e : v.listRef())
            if (emittable(e)) emit(e, loc);
        return;
    }
    Collector& c = m_collect.back();
    if (c.obj) c.obj->children.push_back(v);
    else       c.items->push_back(v);
}

// --- Statements ----------------------------------------------------------------

Interp::Flow Interp::execBlock(const Node* block, const EnvP& env, Mode mode, Value& last,
                               bool newScope) {
    // The top level of a file shares the globals (newScope false), so its lets
    // and fns are what an import provides. Every other block is a scope of its
    // own.
    const EnvP s = newScope ? std::make_shared<Env>(env) : env;
    last = Value::nil();
    for (const Node* st : block->list) {
        const Flow f = exec(st, s, mode, last);
        if (f != Flow::Next) return f;
    }
    return Flow::Next;
}

Interp::Flow Interp::exec(const Node* s, const EnvP& env, Mode mode, Value& last) {
    checkCancel(s->loc);
    switch (s->kind) {
    case N::Let: {
        // A name given on the command line wins over the script's own value
        // for it, so `-D samples=16` works on any script that says
        // `let samples = 256` at the top.
        if (env == globals && m_defined.count(s->str)) return Flow::Next;
        env->vars[s->str] = eval(s->a, env);
        last = Value::nil();
        return Flow::Next;
    }
    case N::FnDecl: {
        auto c = std::make_shared<Closure>();
        c->fn   = s;
        c->env  = env;
        c->name = s->str;
        Value v;
        v.t = T::Func;
        v.p = c;
        env->vars[s->str] = v;
        last = Value::nil();
        return Flow::Next;
    }
    case N::Import:
        importFile(s->str, s->loc);
        return Flow::Next;
    case N::Assign: {
        Value v = eval(s->b, env);
        if (s->op != T_ASSIGN) {
            const Value cur = eval(s->a, env);
            const int op = s->op == T_PLUSEQ ? T_PLUS : s->op == T_MINUSEQ ? T_MINUS
                         : s->op == T_STAREQ ? T_STAR : T_SLASH;
            v = evalBinary(op, cur, v, s->loc);
        }
        assign(s->a, env, v);
        return Flow::Next;
    }
    case N::Prop: {
        if (m_collect.empty() || !m_collect.back().obj)
            throw Error(s->loc, "'" + s->str + ":' sets a property, which only works inside an object's { }");
        setProp(*m_collect.back().obj, s->str, eval(s->a, env), s->loc);
        return Flow::Next;
    }
    case N::For: {
        const Value it = eval(s->a, env);
        const bool two = !s->str2.empty();
        const auto body = [&](const Value& key, const Value& val) -> int {
            const EnvP scope = std::make_shared<Env>(env);
            if (two) { scope->vars[s->str] = key; scope->vars[s->str2] = val; }
            else       scope->vars[s->str] = val;
            Value l;
            const Flow f = execBlock(s->b, scope, mode, l, false);
            if (f == Flow::Break) return 1;
            if (f == Flow::Return) return 2;
            return 0;
        };
        int r = 0;
        if (it.t == T::Range) {
            const double a = it.v[0], b = it.v[1], step = it.v[2] != 0.0 ? it.v[2] : 1.0;
            std::size_t k = 0;
            for (double x = a; step > 0 ? (it.n ? x <= b + 1e-9 : x < b - 1e-9)
                                        : (it.n ? x >= b - 1e-9 : x > b + 1e-9);
                 x = a + step * static_cast<double>(++k)) {
                r = body(Value::num(static_cast<double>(k)), Value::num(x));
                if (r) break;
            }
        } else if (it.t == T::List) {
            const List items = it.listRef();   // a copy: the body may push to it
            for (std::size_t k = 0; k < items.size(); ++k) {
                r = body(Value::num(static_cast<double>(k)), items[k]);
                if (r) break;
            }
        } else if (it.t == T::Vec) {
            for (int k = 0; k < it.n; ++k) {
                r = body(Value::num(k), Value::num(it.v[k]));
                if (r) break;
            }
        } else if (it.t == T::Str) {
            const std::string str = it.string();
            for (std::size_t k = 0; k < str.size(); ++k) {
                r = body(Value::num(static_cast<double>(k)), Value::str(std::string(1, str[k])));
                if (r) break;
            }
        } else if (it.t == T::Num) {
            throw Error(s->a->loc, "a for loop goes over a range, a list or a vector -- "
                                   "did you mean 0.." + describe(it) + "?");
        } else {
            throw Error(s->a->loc, std::string("cannot loop over a ") + typeName(it.t));
        }
        return r == 2 ? Flow::Return : Flow::Next;
    }
    case N::While: {
        long long guard = 0;
        while (eval(s->a, env).truthy()) {
            if (++guard > 100000000LL) throw Error(s->loc, "this loop ran a hundred million times");
            Value l;
            const Flow f = execBlock(s->b, env, mode, l);
            if (f == Flow::Break) break;
            if (f == Flow::Return) return f;
        }
        return Flow::Next;
    }
    case N::Return:
        m_ret = s->a ? eval(s->a, env) : Value::nil();
        return Flow::Return;
    case N::Break:    return Flow::Break;
    case N::Continue: return Flow::Continue;
    case N::ExprStmt: {
        const Node* e = s->a;
        if (e->kind == N::If) {
            // An if as a statement: its branches are statements too, and a
            // break or return in one is the enclosing loop's or function's.
            Flow f = Flow::Next;
            last = evalIf(e, env, mode, &f);
            return f;
        }
        Value v = eval(e, env);
        if (mode == Mode::Stmt) {
            if (emittable(v)) emit(v, e->loc);
            else if (!m_collect.empty() && m_collect.back().obj && v.t != T::Nil)
                m_collect.back().obj->positional.push_back(v);
        }
        last = std::move(v);
        return Flow::Next;
    }
    default:
        last = eval(s, env);
        return Flow::Next;
    }
}

void Interp::assign(const Node* target, const EnvP& env, const Value& v) {
    if (target->kind == N::Ident) {
        Value* slot = env->find(target->str);
        if (!slot) throw Error(target->loc, "'" + target->str + "' is not defined -- declare it with let first");
        *slot = v;
        return;
    }
    // a[i] = v: a list is changed in place; a vector held in a name is copied,
    // changed and written back.
    const Value idx = eval(target->b, env);
    if (idx.t != T::Num) throw Error(target->b->loc, "an index is a number");
    if (target->a->kind == N::Ident) {
        Value* slot = env->find(target->a->str);
        if (!slot) throw Error(target->a->loc, "'" + target->a->str + "' is not defined");
        if (slot->t == T::Vec) {
            int i = static_cast<int>(idx.v[0]);
            if (i < 0) i += slot->n;
            if (i < 0 || i >= slot->n) throw Error(target->loc, "index out of range");
            if (v.t != T::Num) throw Error(target->loc, "a vector component is a number");
            slot->v[i] = v.v[0];
            return;
        }
    }
    const Value base = eval(target->a, env);
    if (base.t != T::List) throw Error(target->loc, std::string("cannot assign into a ") + typeName(base.t));
    List& l = base.listRef();
    long long i = static_cast<long long>(idx.v[0]);
    if (i < 0) i += static_cast<long long>(l.size());
    if (i < 0 || i >= static_cast<long long>(l.size())) throw Error(target->loc, "index out of range");
    l[static_cast<std::size_t>(i)] = v;
}

Value Interp::evalIf(const Node* n, const EnvP& env, Mode mode, Flow* flow) {
    const Node* branch = eval(n->a, env).truthy() ? n->b : n->c;
    if (!branch) return Value::nil();
    if (branch->kind == N::If) return evalIf(branch, env, mode, flow);
    Value last;
    const Flow f = execBlock(branch, env, mode, last);
    if (f != Flow::Next) {
        if (!flow)
            throw Error(n->loc, "'return', 'break' or 'continue' inside an if that is used as a value");
        *flow = f;
    }
    return last;
}

// --- Expressions -----------------------------------------------------------------

Value Interp::eval(const Node* n, const EnvP& env) {
    switch (n->kind) {
    case N::Num:   return Value::num(n->num);
    case N::Bool:  return Value::boolean(n->num != 0.0);
    case N::Nil:   return Value::nil();
    case N::Str:   return Value::str(n->str);
    case N::Color: return Value::vec(n->color);
    case N::Ident: {
        Value* v = env->find(n->str);
        if (!v) throw Error(n->loc, "'" + n->str + "' is not defined");
        return *v;
    }
    case N::List: {
        List items;
        items.reserve(n->list.size());
        bool numeric = n->list.size() >= 2 && n->list.size() <= 4;
        for (const Node* e : n->list) {
            items.push_back(eval(e, env));
            if (items.back().t != T::Num) numeric = false;
        }
        // Two to four numbers are a vector, which arithmetic works on; any
        // other list is a list.
        if (numeric) {
            double c[4];
            for (std::size_t i = 0; i < items.size(); ++i) c[i] = items[i].v[0];
            return Value::vec(c, static_cast<int>(items.size()));
        }
        return Value::list(std::move(items));
    }
    case N::Unary: {
        const Value a = eval(n->a, env);
        if (n->op == T_NOT) return Value::boolean(!a.truthy());
        return unaryNum(a, n->loc, "-", [](double x) { return -x; });
    }
    case N::Binary:
        return evalBinary(n->op, eval(n->a, env), eval(n->b, env), n->loc);
    case N::And: {
        const Value a = eval(n->a, env);
        if (!a.truthy()) return Value::boolean(false);
        return Value::boolean(eval(n->b, env).truthy());
    }
    case N::Or: {
        const Value a = eval(n->a, env);
        if (a.truthy()) return Value::boolean(true);
        return Value::boolean(eval(n->b, env).truthy());
    }
    case N::Range: {
        const Value a = eval(n->a, env), b = eval(n->b, env);
        if (a.t != T::Num || b.t != T::Num) throw Error(n->loc, "a range runs between two numbers");
        Value r;
        r.t = T::Range;
        r.v[0] = a.v[0];
        r.v[1] = b.v[0];
        r.v[2] = 1.0;
        r.n = n->flag ? 1 : 0;
        return r;
    }
    case N::Call: {
        const Value callee = eval(n->a, env);
        Args args;
        args.loc = n->loc;
        for (const Arg& a : n->args) {
            if (a.name.empty()) {
                args.pos.push_back(eval(a.value, env));
                args.posLoc.push_back(a.loc);
            } else {
                args.named.emplace_back(a.name, eval(a.value, env));
            }
        }
        return call(callee, args, n->loc);
    }
    case N::Index:  return evalIndex(eval(n->a, env), eval(n->b, env), n->loc);
    case N::Member: return evalMember(eval(n->a, env), n->str, n->loc);
    case N::Lambda: {
        auto c = std::make_shared<Closure>();
        c->fn  = n;
        c->env = env;
        Value v;
        v.t = T::Func;
        v.p = c;
        return v;
    }
    case N::If:
        return evalIf(n, env, Mode::Value, nullptr);
    case N::Construct:
        return evalConstruct(n, env);
    case N::Block: {
        Value last;
        execBlock(n, env, Mode::Value, last);
        return last;
    }
    default:
        throw Error(n->loc, "this is a statement, not a value");
    }
}

Value Interp::evalBinary(int op, const Value& a, const Value& b, const Loc& loc) {
    switch (op) {
    case T_PLUS:
        if (a.t == T::Str || b.t == T::Str) return Value::str(describe(a) + describe(b));
        if (a.t == T::Mat || b.t == T::Mat) break;
        return elementwise(a, b, loc, "add", [](double x, double y) { return x + y; });
    case T_MINUS:
        return elementwise(a, b, loc, "subtract", [](double x, double y) { return x - y; });
    case T_STAR:
        if (a.t == T::Mat && b.t == T::Mat) return Value::mat(a.matrix() * b.matrix());
        if (a.t == T::Mat && b.isVec())
            return Value::vec(glm::dvec3(a.matrix() * glm::dvec4(b.vec3(), 1.0)));
        return elementwise(a, b, loc, "multiply", [](double x, double y) { return x * y; });
    case T_SLASH:
        return elementwise(a, b, loc, "divide", [](double x, double y) { return y != 0.0 ? x / y : 0.0; });
    case T_PERCENT:
        return elementwise(a, b, loc, "take the remainder of", fmodPos);
    case T_CARET:
        return elementwise(a, b, loc, "raise", [](double x, double y) { return std::pow(x, y); });
    case T_CONCAT: {
        if (a.t == T::Str || b.t == T::Str) return Value::str(describe(a) + describe(b));
        List l;
        const auto add = [&](const Value& v) {
            if (v.t == T::List) l.insert(l.end(), v.listRef().begin(), v.listRef().end());
            else if (v.t == T::Vec) for (int i = 0; i < v.n; ++i) l.push_back(Value::num(v.v[i]));
            else l.push_back(v);
        };
        add(a);
        add(b);
        return Value::list(std::move(l));
    }
    case T_EQ:
    case T_NE: {
        bool eq;
        if (a.t != b.t) eq = (a.isNumeric() && b.isNumeric()) ? false : false;
        else if (a.t == T::Nil) eq = true;
        else if (a.t == T::Num || a.t == T::Bool) eq = a.v[0] == b.v[0];
        else if (a.t == T::Vec) {
            eq = a.n == b.n;
            for (int i = 0; eq && i < a.n; ++i) eq = a.v[i] == b.v[i];
        } else if (a.t == T::Str) eq = a.string() == b.string();
        else eq = a.p == b.p;
        return Value::boolean(op == T_EQ ? eq : !eq);
    }
    case T_LT: case T_LE: case T_GT: case T_GE: {
        if (a.t == T::Str && b.t == T::Str) {
            const int c = a.string().compare(b.string());
            return Value::boolean(op == T_LT ? c < 0 : op == T_LE ? c <= 0 : op == T_GT ? c > 0 : c >= 0);
        }
        if (a.t != T::Num || b.t != T::Num)
            throw Error(loc, std::string("cannot compare ") + typeName(a.t) + " and " + typeName(b.t) +
                                 " with " + tokName(op));
        const double x = a.v[0], y = b.v[0];
        return Value::boolean(op == T_LT ? x < y : op == T_LE ? x <= y : op == T_GT ? x > y : x >= y);
    }
    default: break;
    }
    throw Error(loc, std::string("cannot apply ") + tokName(op) + " to " + typeName(a.t) + " and " +
                         typeName(b.t));
}

Value Interp::evalMember(const Value& a, const std::string& name, const Loc& loc) {
    if (a.t == T::Vec || a.t == T::Num) {
        // Swizzles: .x .y .z .w, .r .g .b .a, and any combination of up to four.
        if (name.size() <= 4) {
            double c[4];
            bool ok = true;
            for (std::size_t i = 0; i < name.size() && ok; ++i) {
                int k = -1;
                switch (name[i]) {
                case 'x': case 'r': k = 0; break;
                case 'y': case 'g': k = 1; break;
                case 'z': case 'b': k = 2; break;
                case 'w': case 'a': k = 3; break;
                default: ok = false; break;
                }
                const int n = a.t == T::Vec ? a.n : 1;
                if (ok && k >= n)
                    throw Error(loc, "a vector of " + std::to_string(n) + " has no ." + std::string(1, name[i]));
                if (ok) c[i] = a.t == T::Vec ? a.v[k] : a.v[0];
            }
            if (ok) {
                if (name.size() == 1) return Value::num(c[0]);
                return Value::vec(c, static_cast<int>(name.size()));
            }
        }
        if (name == "len" || name == "length")
            return Value::num(a.t == T::Vec ? glm::length(a.vec3()) : std::fabs(a.v[0]));
    }
    if (a.t == T::List && (name == "len" || name == "length"))
        return Value::num(static_cast<double>(a.listRef().size()));
    if (a.t == T::Str && (name == "len" || name == "length"))
        return Value::num(static_cast<double>(a.string().size()));
    if (a.t == T::Obj) {
        const Obj& o = a.objRef();
        if (name == "kind") return Value::str(o.kind);
        if (const Prop* p = o.find(name)) return p->value;
        throw Error(loc, "this " + o.kind + " has no '" + name + "' set");
    }
    throw Error(loc, std::string("a ") + typeName(a.t) + " has no '." + name + "'");
}

Value Interp::evalIndex(const Value& a, const Value& i, const Loc& loc) {
    if (i.t != T::Num) throw Error(loc, std::string("an index is a number, not ") + typeName(i.t));
    long long k = static_cast<long long>(std::floor(i.v[0]));
    const auto check = [&](long long size) {
        if (k < 0) k += size;
        if (k < 0 || k >= size)
            throw Error(loc, "index " + fmtNum(i.v[0]) + " is out of range (size " + std::to_string(size) + ")");
    };
    if (a.t == T::List) {
        check(static_cast<long long>(a.listRef().size()));
        return a.listRef()[static_cast<std::size_t>(k)];
    }
    if (a.t == T::Vec) {
        check(a.n);
        return Value::num(a.v[k]);
    }
    if (a.t == T::Str) {
        check(static_cast<long long>(a.string().size()));
        return Value::str(std::string(1, a.string()[static_cast<std::size_t>(k)]));
    }
    throw Error(loc, std::string("cannot index a ") + typeName(a.t));
}

Value Interp::evalConstruct(const Node* n, const EnvP& env) {
    const Value head = eval(n->a, env);
    std::shared_ptr<Obj> o;
    if (head.t == T::Native && !head.native().kind.empty()) {
        o = std::make_shared<Obj>();
        o->kind = head.native().kind;
    } else {
        Value base = head;
        if (head.t == T::Func || head.t == T::Native) base = callValue(head, {}, n->loc);
        if (base.t != T::Obj)
            throw Error(n->loc, std::string("{ } builds or changes an object, and this is a ") +
                                    typeName(base.t));
        // A modified copy: the original stays as it was, so one declared
        // shape can be placed many times with different changes.
        o = std::make_shared<Obj>(base.objRef());
        o->positional.clear();
    }
    o->loc = n->loc;
    const EnvP scope = std::make_shared<Env>(env);
    m_collect.push_back({nullptr, o.get()});
    Value last;
    Flow f;
    try {
        f = execBlock(n->b, scope, Mode::Stmt, last);
    } catch (...) {
        m_collect.pop_back();
        throw;
    }
    m_collect.pop_back();
    if (f != Flow::Next)
        throw Error(n->loc, "'return', 'break' or 'continue' cannot leave an object's { }");
    finishObj(*o);
    return Value::obj(o);
}

Value Interp::callValue(const Value& callee, std::vector<Value> pos, const Loc& loc) {
    Args a;
    a.loc = loc;
    a.pos = std::move(pos);
    return call(callee, a, loc);
}

Value Interp::call(const Value& callee, Args& args, const Loc& loc) {
    if (callee.t == T::Native) {
        NativeFn& f = callee.native();
        if (!f.kind.empty()) {
            // A constructor called like a function: sphere([0, 1, 0], 0.5).
            auto o = std::make_shared<Obj>();
            o->kind = f.kind;
            o->loc = loc;
            o->positional = args.pos;
            for (auto& [k, v] : args.named) setProp(*o, k, v, loc);
            finishObj(*o);
            return Value::obj(o);
        }
        return f.fn(*this, args);
    }
    if (callee.t == T::Func) return callClosure(callee, args, loc);
    if (callee.t == T::Obj) {
        if (!args.pos.empty())
            throw Error(loc, "an object called like a function only takes named properties");
        auto o = std::make_shared<Obj>(callee.objRef());
        o->loc = loc;
        o->positional.clear();
        for (auto& [k, v] : args.named) setProp(*o, k, v, loc);
        finishObj(*o);
        return Value::obj(o);
    }
    throw Error(loc, std::string("a ") + typeName(callee.t) + " cannot be called");
}

Value Interp::callClosure(const Value& callee, Args& args, const Loc& loc) {
    const Closure& c = callee.closure();
    const Node* fn = c.fn;
    const std::string name = c.name.empty() ? "this function" : c.name + "()";
    if (args.pos.size() > fn->params.size())
        throw Error(loc, name + " takes " + std::to_string(fn->params.size()) + " argument" +
                             (fn->params.size() == 1 ? "" : "s") + ", not " +
                             std::to_string(args.pos.size()));
    for (const auto& [k, v] : args.named) {
        bool known = false;
        for (const Param& p : fn->params) known = known || p.name == k;
        if (!known) throw Error(loc, name + " has no parameter '" + k + "'");
    }
    // Deep enough for any tree a scene grows, shallow enough for a 1 MB stack.
    if (++m_depth > 200) {
        m_depth = 0;
        throw Error(loc, "functions call each other too deeply (endless recursion?)");
    }
    struct DepthGuard { int& d; ~DepthGuard() { if (d > 0) --d; } } guard{m_depth};

    const EnvP scope = std::make_shared<Env>(c.env);
    for (std::size_t i = 0; i < fn->params.size(); ++i) {
        const Param& p = fn->params[i];
        if (i < args.pos.size()) { scope->vars[p.name] = args.pos[i]; continue; }
        bool given = false;
        for (const auto& [k, v] : args.named)
            if (k == p.name) { scope->vars[p.name] = v; given = true; }
        if (given) continue;
        if (!p.def) throw Error(loc, name + " is missing its argument '" + p.name + "'");
        scope->vars[p.name] = eval(p.def, scope);
    }

    if (fn->flag) return eval(fn->b, scope);

    std::vector<Value> made;
    m_collect.push_back({&made, nullptr});
    Value last;
    Flow f;
    try {
        f = execBlock(fn->b, scope, Mode::Stmt, last, false);
    } catch (...) {
        m_collect.pop_back();
        throw;
    }
    m_collect.pop_back();
    if (f == Flow::Return) { Value r = m_ret; m_ret = Value::nil(); return r; }
    if (f == Flow::Break || f == Flow::Continue)
        throw Error(loc, "'break' or 'continue' outside a loop in " + name);
    if (made.size() == 1) return made[0];
    if (!made.empty()) {
        auto g = std::make_shared<Obj>();
        g->kind = "group";
        g->loc = loc;
        g->children = std::move(made);
        return Value::obj(g);
    }
    return last;
}

// --- Built-in functions ---------------------------------------------------------

void Interp::installBuiltins() {
    const auto def = [&](const char* name, std::function<Value(Interp&, Args&)> f) {
        auto nf = std::make_shared<NativeFn>();
        nf->name = name;
        nf->fn   = std::move(f);
        Value v;
        v.t = T::Native;
        v.p = nf;
        globals->vars[name] = v;
    };
    // Unary maths, element-wise over vectors.
    struct U { const char* name; double (*f)(double); };
    static const U kUnary[] = {
        {"sin",   [](double x) { return std::sin(x * kDeg); }},
        {"cos",   [](double x) { return std::cos(x * kDeg); }},
        {"tan",   [](double x) { return std::tan(x * kDeg); }},
        {"asin",  [](double x) { return std::asin(std::clamp(x, -1.0, 1.0)) / kDeg; }},
        {"acos",  [](double x) { return std::acos(std::clamp(x, -1.0, 1.0)) / kDeg; }},
        {"atan",  [](double x) { return std::atan(x) / kDeg; }},
        {"sqrt",  [](double x) { return std::sqrt(std::max(x, 0.0)); }},
        {"exp",   [](double x) { return std::exp(x); }},
        {"log",   [](double x) { return std::log(std::max(x, 1e-300)); }},
        {"log2",  [](double x) { return std::log2(std::max(x, 1e-300)); }},
        {"log10", [](double x) { return std::log10(std::max(x, 1e-300)); }},
        {"abs",   [](double x) { return std::fabs(x); }},
        {"sign",  [](double x) { return x > 0.0 ? 1.0 : x < 0.0 ? -1.0 : 0.0; }},
        {"floor", [](double x) { return std::floor(x); }},
        {"ceil",  [](double x) { return std::ceil(x); }},
        {"round", [](double x) { return std::round(x); }},
        {"fract", [](double x) { return x - std::floor(x); }},
        {"saturate", [](double x) { return std::clamp(x, 0.0, 1.0); }},
        {"radians",  [](double x) { return x * kDeg; }},
        {"degrees",  [](double x) { return x / kDeg; }},
    };
    for (const U& u : kUnary) {
        const auto f = u.f;
        const std::string nm = u.name;
        def(u.name, [f, nm](Interp& in, Args& a) {
            ArgReader r{in, a, nm.c_str()};
            return unaryNum(r.numeric(0, "x"), a.loc, nm.c_str(), f);
        });
    }
    def("atan2", [](Interp& in, Args& a) {
        ArgReader r{in, a, "atan2"};
        return elementwise(r.numeric(0, "y"), r.numeric(1, "x"), a.loc, "atan2",
                           [](double y, double x) { return std::atan2(y, x) / kDeg; });
    });
    def("pow", [](Interp& in, Args& a) {
        ArgReader r{in, a, "pow"};
        return elementwise(r.numeric(0, "x"), r.numeric(1, "y"), a.loc, "pow",
                           [](double x, double y) { return std::pow(x, y); });
    });
    def("mod", [](Interp& in, Args& a) {
        ArgReader r{in, a, "mod"};
        return elementwise(r.numeric(0, "x"), r.numeric(1, "y"), a.loc, "mod", fmodPos);
    });
    def("step", [](Interp& in, Args& a) {
        ArgReader r{in, a, "step"};
        return elementwise(r.numeric(0, "edge"), r.numeric(1, "x"), a.loc, "step",
                           [](double e, double x) { return x >= e ? 1.0 : 0.0; });
    });
    const auto minmax = [&](const char* name, bool isMin) {
        def(name, [name, isMin](Interp& in, Args& a) {
            ArgReader r{in, a, name};
            std::vector<Value> xs = a.pos;
            if (xs.size() == 1 && xs[0].t == T::List) xs = xs[0].listRef();
            if (xs.empty()) throw Error(a.loc, std::string(name) + "() needs something to compare");
            Value acc = xs[0];
            for (std::size_t i = 1; i < xs.size(); ++i)
                acc = elementwise(acc, xs[i], a.loc, name, [isMin](double x, double y) {
                    return isMin ? std::min(x, y) : std::max(x, y);
                });
            return acc;
        });
    };
    minmax("min", true);
    minmax("max", false);
    def("clamp", [](Interp& in, Args& a) {
        ArgReader r{in, a, "clamp"};
        const Value lo = r.get(1, "lo") ? r.numeric(1, "lo") : Value::num(0.0);
        const Value hi = r.get(2, "hi") ? r.numeric(2, "hi") : Value::num(1.0);
        const Value x = elementwise(r.numeric(0, "x"), lo, a.loc, "clamp",
                                    [](double v, double l) { return std::max(v, l); });
        return elementwise(x, hi, a.loc, "clamp", [](double v, double h) { return std::min(v, h); });
    });
    const auto mixFn = [](Interp& in, Args& a) {
        ArgReader r{in, a, "mix"};
        const Value& x = r.numeric(0, "a");
        const Value& y = r.numeric(1, "b");
        const Value& t = r.numeric(2, "t");
        const Value d = elementwise(y, x, a.loc, "mix", [](double p, double q) { return p - q; });
        const Value s = elementwise(d, t, a.loc, "mix", [](double p, double q) { return p * q; });
        return elementwise(x, s, a.loc, "mix", [](double p, double q) { return p + q; });
    };
    def("mix", mixFn);
    def("lerp", mixFn);
    def("smoothstep", [](Interp& in, Args& a) {
        ArgReader r{in, a, "smoothstep"};
        const Value& e0 = r.numeric(0, "edge0");
        const Value& e1 = r.numeric(1, "edge1");
        const Value& x  = r.numeric(2, "x");
        const Value t = elementwise(elementwise(x, e0, a.loc, "smoothstep",
                                                [](double p, double q) { return p - q; }),
                                    elementwise(e1, e0, a.loc, "smoothstep",
                                                [](double p, double q) { return p - q; }),
                                    a.loc, "smoothstep",
                                    [](double p, double q) { return q != 0.0 ? std::clamp(p / q, 0.0, 1.0) : 0.0; });
        return unaryNum(t, a.loc, "smoothstep", [](double u) { return u * u * (3.0 - 2.0 * u); });
    });

    // Vectors.
    def("vec", [](Interp&, Args& a) {
        double c[4];
        int n = 0;
        for (const Value& v : a.pos) {
            if (v.t == T::Num) { if (n < 4) c[n++] = v.v[0]; }
            else if (v.t == T::Vec) { for (int i = 0; i < v.n && n < 4; ++i) c[n++] = v.v[i]; }
            else throw Error(a.loc, std::string("vec() is made of numbers and vectors, not ") + typeName(v.t));
        }
        if (n == 1) { c[1] = c[2] = c[0]; n = 3; }
        if (n < 2) throw Error(a.loc, "vec() needs at least one number");
        return Value::vec(c, n);
    });
    def("rgb", [](Interp& in, Args& a) {
        ArgReader r{in, a, "rgb"};
        return Value::vec(glm::dvec3(r.num(0, "r"), r.num(1, "g"), r.num(2, "b")));
    });
    def("gray", [](Interp& in, Args& a) {
        ArgReader r{in, a, "gray"};
        return Value::vec(glm::dvec3(r.num(0, "v")));
    });
    def("length", [](Interp& in, Args& a) {
        ArgReader r{in, a, "length"};
        const Value& v = r.numeric(0, "v");
        if (v.t == T::Num) return Value::num(std::fabs(v.v[0]));
        double s = 0.0;
        for (int i = 0; i < v.n; ++i) s += v.v[i] * v.v[i];
        return Value::num(std::sqrt(s));
    });
    def("normalize", [](Interp& in, Args& a) {
        ArgReader r{in, a, "normalize"};
        Value v = r.numeric(0, "v");
        if (v.t == T::Num) return Value::num(v.v[0] > 0 ? 1.0 : v.v[0] < 0 ? -1.0 : 0.0);
        double s = 0.0;
        for (int i = 0; i < v.n; ++i) s += v.v[i] * v.v[i];
        s = std::sqrt(s);
        if (s > 0.0) for (int i = 0; i < v.n; ++i) v.v[i] /= s;
        return v;
    });
    def("dot", [](Interp& in, Args& a) {
        ArgReader r{in, a, "dot"};
        const Value p = elementwise(r.numeric(0, "a"), r.numeric(1, "b"), a.loc, "dot",
                                    [](double x, double y) { return x * y; });
        double s = 0.0;
        for (int i = 0; i < widthOf(p); ++i) s += p.v[i];
        return Value::num(s);
    });
    def("cross", [](Interp& in, Args& a) {
        ArgReader r{in, a, "cross"};
        return Value::vec(glm::cross(r.vec3(0, "a"), r.vec3(1, "b")));
    });
    def("distance", [](Interp& in, Args& a) {
        ArgReader r{in, a, "distance"};
        const Value d = elementwise(r.numeric(0, "a"), r.numeric(1, "b"), a.loc, "distance",
                                    [](double x, double y) { return x - y; });
        double s = 0.0;
        for (int i = 0; i < widthOf(d); ++i) s += d.v[i] * d.v[i];
        return Value::num(std::sqrt(s));
    });
    def("reflect", [](Interp& in, Args& a) {
        ArgReader r{in, a, "reflect"};
        const glm::dvec3 d = r.vec3(0, "d"), n = r.vec3(1, "n");
        return Value::vec(d - 2.0 * glm::dot(d, n) * n);
    });

    // Noise, shared with the VM (TraceFunc.hpp) so a function computes the
    // same numbers when the script calls it as when the tracer does.
    struct NoiseFn { const char* name; float (*f)(const glm::vec3&); };
    static const NoiseFn kNoise[] = {
        {"noise", pathtrace::fn::noise}, {"voronoi", pathtrace::fn::voronoi},
        {"cell_id", pathtrace::fn::voronoiId}, {"hash", pathtrace::fn::hash},
        {"parity", pathtrace::fn::checker},
    };
    for (const NoiseFn& nf : kNoise) {
        const auto f = nf.f;
        const std::string nm = nf.name;
        def(nf.name, [f, nm](Interp& in, Args& a) {
            ArgReader r{in, a, nm.c_str()};
            return Value::num(f(asPoint(r.numeric(0, "p"))));
        });
    }
    def("fbm", [](Interp& in, Args& a) {
        ArgReader r{in, a, "fbm"};
        return Value::num(pathtrace::fn::fbm(asPoint(r.numeric(0, "p")),
                                             static_cast<int>(r.num(1, "octaves", 5.0))));
    });
    def("turbulence", [](Interp& in, Args& a) {
        ArgReader r{in, a, "turbulence"};
        return Value::num(pathtrace::fn::turbulence(asPoint(r.numeric(0, "p")),
                                                    static_cast<int>(r.num(1, "octaves", 5.0))));
    });
    def("random", [](Interp& in, Args& a) {
        ArgReader r{in, a, "random"};
        const Value& s = r.numeric(0, "seed");
        // Hashed, not drawn from a generator: the same seed gives the same
        // number wherever and however often it is asked, so a scene full of
        // random placement renders the same every time.
        const double u = pathtrace::fn::hash(asPoint(s) + glm::vec3(0.0f, 0.0f, 13.37f));
        if (r.get(1, "lo") || r.get(2, "hi")) {
            const double lo = r.num(1, "lo", 0.0), hi = r.num(2, "hi", 1.0);
            return Value::num(lo + (hi - lo) * u);
        }
        return Value::num(u);
    });
    def("ramp", [](Interp& in, Args& a) {
        ArgReader r{in, a, "ramp"};
        const double t = r.num(0, "t");
        const List& stops = r.list(1, "colors");
        pathtrace::Ramp ramp;
        for (std::size_t i = 0; i < stops.size(); ++i) {
            const Value& s = stops[i];
            if (s.t == T::List && s.listRef().size() == 2) {
                ramp.at.push_back(static_cast<float>(s.listRef()[0].v[0]));
                ramp.color.push_back(glm::vec3(needVec3(s.listRef()[1], a.loc, "ramp")));
            } else {
                ramp.at.push_back(stops.size() > 1 ? static_cast<float>(i) / static_cast<float>(stops.size() - 1) : 0.0f);
                ramp.color.push_back(glm::vec3(needVec3(s, a.loc, "ramp")));
            }
        }
        return Value::vec(glm::dvec3(ramp.lookup(static_cast<float>(t))));
    });
    def("at", [](Interp& in, Args& a) {
        ArgReader r{in, a, "at"};
        const Value& c = r.need(0, "pattern");
        if (c.t != T::Func && c.t != T::Native) return c;
        // As many of (p, n, uv) as the function takes.
        std::vector<Value> pass(a.pos.begin() + 1, a.pos.end());
        if (c.t == T::Func) {
            const std::size_t want = c.closure().fn->params.size();
            if (pass.size() > want) pass.resize(want);
        }
        return in.callValue(c, pass, a.loc);
    });
    def("texture", [](Interp& in, Args& a) {
        ArgReader r{in, a, "texture"};
        const Value& img = r.need(0, "image");
        if (img.t != T::Image) throw Error(a.loc, "texture() wants an image from load_image()");
        const glm::dvec3 uv = r.vec3(1, "uv");
        const auto* im = static_cast<const pathtrace::Image*>(img.p.get());
        return Value::vec(glm::dvec3(glm::vec3(im->sample(static_cast<float>(uv.x),
                                                          1.0f - static_cast<float>(uv.y)))));
    });

    // Lists.
    def("len", [](Interp& in, Args& a) {
        ArgReader r{in, a, "len"};
        const Value& v = r.need(0, "x");
        if (v.t == T::List) return Value::num(static_cast<double>(v.listRef().size()));
        if (v.t == T::Vec)  return Value::num(v.n);
        if (v.t == T::Str)  return Value::num(static_cast<double>(v.string().size()));
        if (v.t == T::Range) {
            const double step = v.v[2] != 0.0 ? v.v[2] : 1.0;
            const double span = (v.v[1] - v.v[0]) / step;
            const double n = v.n ? std::floor(span + 1e-9) + 1.0 : std::ceil(span - 1e-9);
            return Value::num(std::max(0.0, n));
        }
        throw Error(a.loc, std::string("len() of a ") + typeName(v.t));
    });
    def("range", [](Interp& in, Args& a) {
        ArgReader r{in, a, "range"};
        Value v;
        v.t = T::Range;
        if (a.pos.size() == 1) { v.v[0] = 0.0; v.v[1] = r.num(0, "end"); }
        else { v.v[0] = r.num(0, "start"); v.v[1] = r.num(1, "end"); }
        v.v[2] = r.num(2, "step", 1.0);
        if (v.v[2] == 0.0) throw Error(a.loc, "range() with a step of 0 never ends");
        return v;
    });
    def("list", [](Interp& in, Args& a) {
        // list(n, f): [f(0), f(1), ... f(n-1)]; list(x...): the arguments.
        if (a.pos.size() == 2 && a.pos[0].t == T::Num &&
            (a.pos[1].t == T::Func || a.pos[1].t == T::Native)) {
            List l;
            const long long n = static_cast<long long>(a.pos[0].v[0]);
            for (long long i = 0; i < n; ++i) {
                l.push_back(in.callValue(a.pos[1], {Value::num(static_cast<double>(i))}, a.loc));
                in.checkCancel(a.loc);
            }
            return Value::list(std::move(l));
        }
        return Value::list(a.pos);
    });
    def("push", [](Interp& in, Args& a) {
        ArgReader r{in, a, "push"};
        List& l = r.list(0, "list");
        for (std::size_t i = 1; i < a.pos.size(); ++i) l.push_back(a.pos[i]);
        return a.pos[0];
    });
    def("pop", [](Interp& in, Args& a) {
        ArgReader r{in, a, "pop"};
        List& l = r.list(0, "list");
        if (l.empty()) throw Error(a.loc, "pop() from an empty list");
        Value v = l.back();
        l.pop_back();
        return v;
    });
    def("map", [](Interp& in, Args& a) {
        ArgReader r{in, a, "map"};
        const List items = r.items(0, "list");
        List out;
        out.reserve(items.size());
        for (const Value& v : items) out.push_back(in.callValue(r.need(1, "f"), {v}, a.loc));
        return Value::list(std::move(out));
    });
    def("filter", [](Interp& in, Args& a) {
        ArgReader r{in, a, "filter"};
        const List items = r.items(0, "list");
        List out;
        for (const Value& v : items)
            if (in.callValue(r.need(1, "f"), {v}, a.loc).truthy()) out.push_back(v);
        return Value::list(std::move(out));
    });
    def("reverse", [](Interp& in, Args& a) {
        ArgReader r{in, a, "reverse"};
        List l = r.items(0, "list");
        std::reverse(l.begin(), l.end());
        return Value::list(std::move(l));
    });
    def("sum", [](Interp& in, Args& a) {
        ArgReader r{in, a, "sum"};
        const List l = r.items(0, "list");
        Value acc = Value::num(0.0);
        for (const Value& v : l)
            acc = elementwise(acc, v, a.loc, "sum", [](double x, double y) { return x + y; });
        return acc;
    });
    def("contains", [](Interp& in, Args& a) {
        ArgReader r{in, a, "contains"};
        const List l = r.items(0, "list");
        const Value& x = r.need(1, "x");
        for (const Value& v : l)
            if (in.evalBinary(T_EQ, v, x, a.loc).truthy()) return Value::boolean(true);
        return Value::boolean(false);
    });

    // Strings, output, checks.
    def("str", [](Interp& in, Args& a) {
        std::string s;
        for (const Value& v : a.pos) s += in.describe(v);
        return Value::str(s);
    });
    def("print", [](Interp& in, Args& a) {
        std::string s;
        for (std::size_t i = 0; i < a.pos.size(); ++i) s += (i ? " " : "") + in.describe(a.pos[i]);
        in.log.push_back(s);
        if (in.options.print) in.options.print(s);
        return Value::nil();
    });
    def("error", [](Interp& in, Args& a) -> Value {
        ArgReader r{in, a, "error"};
        throw Error(a.loc, r.get(0, "message") ? in.describe(*r.get(0, "message")) : "error()");
    });
    def("assert", [](Interp& in, Args& a) {
        ArgReader r{in, a, "assert"};
        if (!r.need(0, "condition").truthy())
            throw Error(a.loc, r.get(1, "message") ? "assertion failed: " + in.describe(*r.get(1, "message"))
                                                    : "assertion failed");
        return Value::nil();
    });
    def("type", [](Interp& in, Args& a) {
        ArgReader r{in, a, "type"};
        const Value& v = r.need(0, "x");
        return Value::str(v.t == T::Obj ? v.objRef().kind : typeName(v.t));
    });

    // Matrices: for placing things by composing transforms in code, the way a
    // tree or an L-system wants to be written.
    def("identity", [](Interp&, Args&) { return Value::mat(glm::dmat4(1.0)); });
    def("translation", [](Interp& in, Args& a) {
        ArgReader r{in, a, "translation"};
        return Value::mat(glm::translate(glm::dmat4(1.0), r.vec3(0, "v")));
    });
    def("rotation", [](Interp& in, Args& a) {
        ArgReader r{in, a, "rotation"};
        const glm::dvec3 e = r.vec3(0, "degrees") * kDeg;
        glm::dmat4 m(1.0);
        m = glm::rotate(glm::dmat4(1.0), e.z, glm::dvec3(0, 0, 1)) *
            glm::rotate(glm::dmat4(1.0), e.y, glm::dvec3(0, 1, 0)) *
            glm::rotate(glm::dmat4(1.0), e.x, glm::dvec3(1, 0, 0));
        return Value::mat(m);
    });
    def("rotation_axis", [](Interp& in, Args& a) {
        ArgReader r{in, a, "rotation_axis"};
        const glm::dvec3 axis = r.vec3(0, "axis");
        if (glm::length(axis) < 1e-12) throw Error(a.loc, "rotation_axis() about a zero axis");
        return Value::mat(glm::rotate(glm::dmat4(1.0), r.num(1, "degrees") * kDeg, glm::normalize(axis)));
    });
    def("scaling", [](Interp& in, Args& a) {
        ArgReader r{in, a, "scaling"};
        return Value::mat(glm::scale(glm::dmat4(1.0), r.vec3(0, "s")));
    });
    def("inverse", [](Interp& in, Args& a) {
        ArgReader r{in, a, "inverse"};
        const Value& m = r.need(0, "m");
        if (m.t != T::Mat) throw Error(a.loc, "inverse() of a matrix");
        return Value::mat(glm::inverse(m.matrix()));
    });
    def("apply", [](Interp& in, Args& a) {
        ArgReader r{in, a, "apply"};
        const Value& m = r.need(0, "m");
        if (m.t != T::Mat) throw Error(a.loc, "apply() wants a matrix first");
        return Value::vec(glm::dvec3(m.matrix() * glm::dvec4(r.vec3(1, "point"), 1.0)));
    });
    def("apply_dir", [](Interp& in, Args& a) {
        ArgReader r{in, a, "apply_dir"};
        const Value& m = r.need(0, "m");
        if (m.t != T::Mat) throw Error(a.loc, "apply_dir() wants a matrix first");
        return Value::vec(glm::dvec3(m.matrix() * glm::dvec4(r.vec3(1, "direction"), 0.0)));
    });

    // Images, for texture().
    def("load_image", [](Interp& in, Args& a) {
        ArgReader r{in, a, "load_image"};
        return loadImage(in, r.string(0, "file"), a.loc);
    });

    // Constants.
    globals->vars["pi"]  = Value::num(3.14159265358979323846);
    globals->vars["tau"] = Value::num(6.28318530717958647692);
    globals->vars["e"]   = Value::num(2.71828182845904523536);
    globals->vars["inf"] = Value::num(1e30);
    globals->vars["x"]   = Value::vec(glm::dvec3(1, 0, 0));
    globals->vars["y"]   = Value::vec(glm::dvec3(0, 1, 0));
    globals->vars["z"]   = Value::vec(glm::dvec3(0, 0, 1));

    // The scene objects' constructors.
    for (const std::string& kind : objectKinds()) {
        auto nf = std::make_shared<NativeFn>();
        nf->name = kind;
        nf->kind = kind;
        Value v;
        v.t = T::Native;
        v.p = nf;
        globals->vars[kind] = v;
    }
}

} // namespace strahl
