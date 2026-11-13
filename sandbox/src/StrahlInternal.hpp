#pragma once

// Strahl's insides, shared by its translation units and by tools/strahlcheck.cpp
// (which tests the interpreter and the compiler against each other). Nothing
// outside the language includes this; the public face is Strahl.hpp.

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "Strahl.hpp"

namespace strahl {

// --- Source positions and errors ---------------------------------------------

struct Loc {
    int file = 0;   // index into Interp::files
    int line = 0, col = 0;
};

// Everything that stops a script: a syntax error, a type error, an unknown
// name. Thrown, caught once at the top, turned into a Diagnostic.
struct Error : std::runtime_error {
    Loc loc;
    Error(const Loc& l, const std::string& msg) : std::runtime_error(msg), loc(l) {}
};

// --- The syntax tree ---------------------------------------------------------

enum class N : std::uint8_t {
    // expressions
    Num, Str, Bool, Nil, Color, Ident, List, Unary, Binary, And, Or, Call, Index, Member,
    Construct, Lambda, If, Range, Block,
    // statements
    Let, Assign, FnDecl, For, While, Return, Break, Continue, Import, Prop, ExprStmt,
};

struct Node;

struct Param {
    std::string name;
    Node*       def = nullptr;   // default value, or null: required
};

struct Arg {
    std::string name;            // empty: positional
    Node*       value = nullptr;
    Loc         loc;
};

struct Node {
    N           kind = N::Nil;
    Loc         loc;
    double      num = 0.0;            // Num, Bool
    glm::dvec3  color{0.0};           // Color
    std::string str;                  // Str, Ident, Member, Prop, Let/FnDecl name, For var, Import path
    std::string str2;                 // For: the value variable when there is an index one
    int         op = 0;               // Unary / Binary / Assign: the operator (Tok)
    Node*       a = nullptr;          // operand / callee / condition / target / value
    Node*       b = nullptr;          // operand / then-block / body / index
    Node*       c = nullptr;          // else-branch
    std::vector<Node*> list;          // List elements, Block statements
    std::vector<Arg>   args;          // Call
    std::vector<Param> params;        // Lambda, FnDecl
    bool        flag = false;         // Range: inclusive
};

// One parsed file. The nodes live as long as the interpreter does: closures
// point into them.
struct Program {
    std::string       path;
    std::string       source;
    std::deque<Node>  nodes;
    Node*             root = nullptr; // a Block
};

// Tokens (also the operator codes in Node::op).
enum Tok : int {
    T_EOF, T_NUM, T_STR, T_COLOR, T_IDENT,
    T_LPAREN, T_RPAREN, T_LBRACK, T_RBRACK, T_LBRACE, T_RBRACE,
    T_COMMA, T_SEMI, T_COLON, T_DOT, T_DOTDOT, T_DOTDOTEQ, T_ARROW,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_CARET, T_CONCAT,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_NOT, T_ANDAND, T_OROR,
    T_ASSIGN, T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ,
    // keywords
    K_LET, K_FN, K_IF, K_ELSE, K_FOR, K_IN, K_WHILE, K_RETURN, K_BREAK, K_CONTINUE,
    K_IMPORT, K_TRUE, K_FALSE, K_NIL, K_AND, K_OR, K_NOT,
};
const char* tokName(int t);

// Parse a whole file into `prog` (prog.source must be set). Throws Error.
void parse(Program& prog, int fileIndex);

// --- Values ------------------------------------------------------------------

enum class T : std::uint8_t {
    Nil, Bool, Num, Vec, Str, List, Range, Func, Native, Obj, Image, Mat,
};
const char* typeName(T t);

struct Value;
struct Env;
struct Obj;
struct Closure;
struct NativeFn;
class Interp;

using EnvP = std::shared_ptr<Env>;
using List = std::vector<Value>;

struct Value {
    T            t = T::Nil;
    std::uint8_t n = 0;                 // Vec: 2..4 components; Range: 1 = inclusive
    double       v[4] = {0.0, 0.0, 0.0, 0.0};
    std::shared_ptr<void> p;            // Str, List, Func, Native, Obj, Image, Mat

    static Value nil() { return {}; }
    static Value num(double x) { Value r; r.t = T::Num; r.v[0] = x; return r; }
    static Value boolean(bool b) { Value r; r.t = T::Bool; r.v[0] = b ? 1.0 : 0.0; return r; }
    static Value vec(const glm::dvec3& x) { Value r; r.t = T::Vec; r.n = 3; r.v[0] = x.x; r.v[1] = x.y; r.v[2] = x.z; return r; }
    static Value vec(const double* x, int n) {
        Value r; r.t = T::Vec; r.n = static_cast<std::uint8_t>(n);
        for (int i = 0; i < n; ++i) r.v[i] = x[i];
        return r;
    }
    static Value str(std::string s) { Value r; r.t = T::Str; r.p = std::make_shared<std::string>(std::move(s)); return r; }
    static Value list(List l) { Value r; r.t = T::List; r.p = std::make_shared<List>(std::move(l)); return r; }
    static Value obj(std::shared_ptr<Obj> o);
    static Value mat(const glm::dmat4& m) { Value r; r.t = T::Mat; r.p = std::make_shared<glm::dmat4>(m); return r; }

    bool isNum() const { return t == T::Num; }
    bool isVec() const { return t == T::Vec; }
    bool isNumeric() const { return t == T::Num || t == T::Vec; }
    double numv() const { return v[0]; }
    glm::dvec3 vec3() const {
        if (t == T::Num) return glm::dvec3(v[0]);
        return glm::dvec3(v[0], n > 1 ? v[1] : 0.0, n > 2 ? v[2] : 0.0);
    }
    const std::string& string() const { return *static_cast<const std::string*>(p.get()); }
    List& listRef() const { return *static_cast<List*>(p.get()); }
    Obj& objRef() const { return *static_cast<Obj*>(p.get()); }
    std::shared_ptr<Obj> objPtr() const { return std::static_pointer_cast<Obj>(p); }
    Closure& closure() const { return *static_cast<Closure*>(p.get()); }
    NativeFn& native() const { return *static_cast<NativeFn*>(p.get()); }
    const glm::dmat4& matrix() const { return *static_cast<const glm::dmat4*>(p.get()); }
    bool truthy() const;
};

struct Env {
    EnvP parent;
    std::unordered_map<std::string, Value> vars;
    explicit Env(EnvP p = nullptr) : parent(std::move(p)) {}
    Value* find(const std::string& name) {
        for (Env* e = this; e; e = e->parent.get()) {
            auto it = e->vars.find(name);
            if (it != e->vars.end()) return &it->second;
        }
        return nullptr;
    }
};

struct Closure {
    const Node*  fn = nullptr;     // Lambda or FnDecl: params and body
    EnvP         env;
    std::string  name;             // "" for an anonymous function
};

// The arguments of one call, positional then named, as the callee sees them.
struct Args {
    std::vector<Value> pos;
    std::vector<std::pair<std::string, Value>> named;
    Loc loc;
    std::vector<Loc> posLoc;
};

struct NativeFn {
    std::string name;
    // A constructor for a kind of scene object (sphere, material, ...) rather
    // than a function. Empty for an ordinary built-in.
    std::string kind;
    std::function<Value(Interp&, Args&)> fn;
};

// A scene object: a shape, a material, a light, the camera ... As the script
// builds it -- a kind and the properties it was given, in order -- and only
// interpreted when the scene is built (StrahlScene.cpp), which is where a
// property is checked against what the kind accepts.
struct Prop {
    std::string name;
    Value       value;
    Loc         loc;
};

struct Obj {
    std::string        kind;
    std::vector<Prop>  props;       // last one wins
    std::vector<Value> children;    // objects inside a group, CSG or light list
    std::vector<Value> positional;  // values given without a name, in order
    glm::dmat4         xf{1.0};     // translate / rotate / scale, applied in order
    Loc                loc;

    const Prop* find(const std::string& name) const {
        for (auto it = props.rbegin(); it != props.rend(); ++it)
            if (it->name == name) return &*it;
        return nullptr;
    }
};

inline Value Value::obj(std::shared_ptr<Obj> o) { Value r; r.t = T::Obj; r.p = std::move(o); return r; }

// --- The interpreter ---------------------------------------------------------

class Interp {
public:
    explicit Interp(const Options& options);

    // Parse and run a file's statements at the top level. Throws Error.
    void runSource(const std::string& source, const std::string& path);

    // Everything the script put in the scene, in order.
    std::vector<Value> scene;

    std::vector<std::string> files;   // Loc::file -> path, for messages
    std::vector<std::string> log;
    const Options& options;
    EnvP globals;
    // Closures compiled for the tracer (counted for the report).
    int compiled = 0;

    // Evaluation, exposed for the scene builder and the compiler.
    Value eval(const Node* n, const EnvP& env);
    Value call(const Value& callee, Args& args, const Loc& loc);
    Value callValue(const Value& callee, std::vector<Value> pos, const Loc& loc);
    std::string describe(const Value& v) const;
    std::string where(const Loc& loc) const;
    void  checkCancel(const Loc& loc);
    // The folder relative paths in a script resolve against.
    std::filesystem::path dirOf(const Loc& loc) const;

    // Resolve a path the script gave against its own folder, then the library.
    std::filesystem::path resolve(const std::string& path, const Loc& loc) const;
    // Images the script loaded, by resolved path.
    std::unordered_map<std::string, std::shared_ptr<pathtrace::Image>> images;

    // Is this value something a statement puts in the scene (a shape, a light,
    // the camera ...) rather than an ordinary value?
    static bool emittable(const Value& v);
    static bool isShapeKind(const std::string& kind);

    // Finish an object a constructor or block made: positional values to
    // named properties, names checked. (StrahlScene.cpp knows the kinds.)
    void finishObj(Obj& o);
    // Apply one property to an object as a script sets it: transforms are
    // multiplied in, everything else recorded.
    void setProp(Obj& o, const std::string& name, const Value& v, const Loc& loc);

private:
    friend class Compiler;   // folds what it can with the interpreter's own arithmetic

    enum class Flow { Next, Return, Break, Continue };
    enum class Mode { Stmt, Value };

    struct Collector {
        std::vector<Value>* items = nullptr;  // where emitted objects go
        Obj*                obj   = nullptr;  // inside an object's block: its properties
    };
    std::vector<Collector> m_collect;
    Value m_ret;
    int   m_depth = 0;
    long long m_steps = 0;
    std::vector<std::unique_ptr<Program>> m_programs;
    std::unordered_map<std::string, bool> m_imported;
    std::unordered_map<std::string, bool> m_defined;   // names set from Options::defines

    Flow exec(const Node* s, const EnvP& env, Mode mode, Value& last);
    Flow execBlock(const Node* block, const EnvP& env, Mode mode, Value& last,
                   bool newScope = true);
    // `flow`: where a break/continue/return in the taken branch is reported;
    // null when the if is a value and such a thing is an error.
    Value evalIf(const Node* n, const EnvP& env, Mode mode, Flow* flow);
    Value evalConstruct(const Node* n, const EnvP& env);
    Value evalBinary(int op, const Value& a, const Value& b, const Loc& loc);
    Value evalMember(const Value& a, const std::string& name, const Loc& loc);
    Value evalIndex(const Value& a, const Value& i, const Loc& loc);
    Value callClosure(const Value& callee, Args& args, const Loc& loc);
    void  assign(const Node* target, const EnvP& env, const Value& v);
    void  emit(const Value& v, const Loc& loc);
    void  importFile(const std::string& path, const Loc& loc);
    void  installBuiltins();
    void  installPrelude();
    Program& load(const std::string& source, const std::string& path);
};

// Built-ins shared by the interpreter and the compiler: does this name map to
// a VM instruction, and how. Defined in StrahlCompile.cpp.
bool isVmBuiltin(const std::string& name);

// Compile a closure to the tracer's VM. `arity` is how many of (p, n, uv) the
// caller passes. Throws Error with the reason when the function uses something
// a per-ray function cannot (a list it builds, a while loop, recursion ...).
std::shared_ptr<const pathtrace::Func> compile(Interp& in, const Value& closure, const Loc& loc);

// load_image(): decode a picture the script names (cached per path).
Value loadImage(Interp& in, const std::string& path, const Loc& loc);

// Build the scene from what the script emitted. Throws Error.
void buildScene(Interp& in, Result& out);

// The prelude: the library every script starts with (colours, materials,
// patterns, compound shapes), written in Strahl itself.
const char* preludeSource();

// Schema queries for the vocabulary.
std::vector<std::string> objectKinds();
std::vector<std::string> propertyNames();

// One kind as completion sees it: what it takes by position, every property
// it accepts besides the transforms, and whether it is a shape (takes a
// material, has shape children).
struct KindSchema {
    std::string              name;
    std::vector<std::string> positional, props;
    bool                     shape = false;
};
std::vector<KindSchema> kindSchemas();

} // namespace strahl
