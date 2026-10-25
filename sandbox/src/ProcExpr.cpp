// The procedural graphs' parameters and expressions (see ProcGraph.hpp): the
// little formula language a node's setting can follow, the object's own
// controls it reads, and the bookkeeping that keeps the two in step when a
// parameter is renamed, removed or promoted out of a node.

#include "ProcGraph.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace proc {

// --- Parameters -------------------------------------------------------------------

const std::vector<std::string>& parmTypeLabels() {
    static const std::vector<std::string> labels = {
        "Float", "Integer", "Toggle", "Vector", "Menu", "String", "Material", "Prefab",
        "Label", "Separator",
    };
    return labels;
}

bool parmHasValue(ParmType t) { return t != ParmType::Label && t != ParmType::Separator; }
bool parmIsText(ParmType t) {
    return t == ParmType::String || t == ParmType::Material || t == ParmType::Prefab;
}

Parm* Graph::parm(const std::string& name) {
    for (Parm& p : parms)
        if (p.name == name) return &p;
    return nullptr;
}
const Parm* Graph::parm(const std::string& name) const {
    for (const Parm& p : parms)
        if (p.name == name) return &p;
    return nullptr;
}

namespace {

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identChar(char c)  { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

// Words the formulas keep for themselves: a parameter called "pi" could never
// be read.
bool reserved(const std::string& w) {
    static const char* const words[] = {"pi", "tau", "true", "false", "ch", "if"};
    for (const char* k : words)
        if (w == k) return true;
    return false;
}

} // namespace

std::string Graph::uniqueParmName(const std::string& base) const {
    // "Ring radius" -> "ring_radius": what a formula can say, lower case.
    std::string stem;
    for (char c : base) {
        if (identChar(c)) stem += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        else if (!stem.empty() && stem.back() != '_') stem += '_';
    }
    while (!stem.empty() && stem.back() == '_') stem.pop_back();
    if (stem.empty() || !identStart(stem[0])) stem = "parm" + stem;
    auto taken = [&](const std::string& nm) {
        if (reserved(nm)) return true;
        for (const Parm& p : parms)
            if (p.name == nm) return true;
        return false;
    };
    if (!taken(stem)) return stem;
    while (!stem.empty() && std::isdigit(static_cast<unsigned char>(stem.back()))) stem.pop_back();
    for (int k = 2;; ++k) {
        const std::string nm = stem + std::to_string(k);
        if (!taken(nm)) return nm;
    }
}

// --- Reading and writing a setting --------------------------------------------------

namespace {

const Property* findProp(const Node& n, const std::string& key) {
    for (const Property& p : n.props())
        if (p.key == key) return &p;
    return nullptr;
}

// "center.y" -> ("center", 1); "radius" -> ("radius", -1).
std::pair<std::string, int> splitKey(const std::string& key) {
    if (key.size() > 2 && key[key.size() - 2] == '.') {
        const char c = key.back();
        const int k = c == 'x' ? 0 : c == 'y' ? 1 : c == 'z' ? 2 : -1;
        if (k >= 0) return {key.substr(0, key.size() - 2), k};
    }
    return {key, -1};
}

int componentOf(const std::string& s) {
    if (s == "x" || s == "r") return 0;
    if (s == "y" || s == "g") return 1;
    if (s == "z" || s == "b") return 2;
    return -1;
}

double readNumber(const Property& pr, const Node& n, int comp) {
    void* f = pr.field(const_cast<Node*>(&n));
    switch (pr.kind) {
        case PropKind::Float:   return *static_cast<const float*>(f);
        case PropKind::Int:
        case PropKind::EnumInt: return *static_cast<const int*>(f);
        case PropKind::Bool:    return *static_cast<const bool*>(f) ? 1.0 : 0.0;
        case PropKind::Vec3:
        case PropKind::Color:   return (*static_cast<const glm::vec3*>(f))[comp < 0 ? 0 : comp];
        default:                return 0.0;
    }
}

void writeNumber(const Property& pr, Node& n, int comp, double v) {
    if (!std::isfinite(v)) v = 0.0;
    void* f = pr.field(&n);
    const bool bounded = pr.max > pr.min;
    switch (pr.kind) {
        case PropKind::Float: {
            float x = static_cast<float>(v);
            if (bounded) x = std::clamp(x, pr.min, pr.max);
            *static_cast<float*>(f) = x;
            break;
        }
        case PropKind::Int: {
            double r = std::round(v);
            if (bounded) r = std::clamp(r, static_cast<double>(pr.min), static_cast<double>(pr.max));
            *static_cast<int*>(f) = static_cast<int>(std::clamp(r, -2.0e9, 2.0e9));
            break;
        }
        case PropKind::EnumInt: {
            const int count = std::max(1, static_cast<int>(pr.enumLabels.size()));
            *static_cast<int*>(f) = std::clamp(static_cast<int>(std::lround(std::clamp(v, -1.0e6, 1.0e6))),
                                               0, count - 1);
            break;
        }
        case PropKind::Bool: *static_cast<bool*>(f) = v != 0.0; break;
        case PropKind::Vec3:
        case PropKind::Color:
            if (comp >= 0 && comp < 3) {
                float x = static_cast<float>(v);
                if (bounded) x = std::clamp(x, pr.min, pr.max);
                (*static_cast<glm::vec3*>(f))[comp] = x;
            }
            break;
        default: break;
    }
}

std::string fmtNumber(double v) {
    char b[48];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// --- The evaluator ------------------------------------------------------------------
// Recursive descent over the text, straight to a value: formulas are a line
// long, and parsing one again on every cook costs nothing next to the cook.
// Errors are thrown as text and caught at the top.

struct ExprError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Ctx {
    const Graph& g;
    // The settings being worked out right now, "node:key": one that comes
    // round to itself ("a.x" = "b.x", "b.x" = "a.x") is a loop, said as such.
    std::vector<std::string> stack;
};

double settingValue(Ctx& c, const Node& n, const std::string& key, int comp);
std::string settingText(Ctx& c, const Node& n, const std::string& key);
double evalIn(Ctx& c, const std::string& expr);

class Parser {
public:
    Parser(Ctx& c, const std::string& s) : m_c(c), m_s(s) {}

    double run() {
        const double v = ternary();
        skip();
        if (m_i < m_s.size()) throw ExprError("unexpected '" + m_s.substr(m_i, 8) + "'");
        return v;
    }

private:
    Ctx&               m_c;
    const std::string& m_s;
    std::size_t        m_i = 0;

    void skip() {
        while (m_i < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_i]))) ++m_i;
    }
    bool eat(const char* tok) {
        skip();
        const std::size_t n = std::strlen(tok);
        if (m_s.compare(m_i, n, tok) != 0) return false;
        // "<" is not the start of "<=", "&" never stands alone.
        if (n == 1 && m_i + 1 < m_s.size()) {
            const char next = m_s[m_i + 1];
            if ((tok[0] == '<' || tok[0] == '>' || tok[0] == '!' || tok[0] == '=') && next == '=') return false;
        }
        m_i += n;
        return true;
    }
    void expect(const char* tok) {
        if (!eat(tok)) throw ExprError(std::string("expected '") + tok + "'");
    }

    double ternary() {
        const double c = orExpr();
        if (eat("?")) {
            const double a = ternary();
            expect(":");
            const double b = ternary();
            return c != 0.0 ? a : b;
        }
        return c;
    }
    double orExpr() {
        double v = andExpr();
        while (eat("||")) { const double r = andExpr(); v = (v != 0.0 || r != 0.0) ? 1.0 : 0.0; }
        return v;
    }
    double andExpr() {
        double v = equality();
        while (eat("&&")) { const double r = equality(); v = (v != 0.0 && r != 0.0) ? 1.0 : 0.0; }
        return v;
    }
    double equality() {
        double v = compare();
        for (;;) {
            if (eat("=="))      v = std::fabs(v - compare()) < 1e-9 ? 1.0 : 0.0;
            else if (eat("!=")) v = std::fabs(v - compare()) < 1e-9 ? 0.0 : 1.0;
            else return v;
        }
    }
    double compare() {
        double v = additive();
        for (;;) {
            if (eat("<="))     v = v <= additive() ? 1.0 : 0.0;
            else if (eat(">=")) v = v >= additive() ? 1.0 : 0.0;
            else if (eat("<"))  v = v <  additive() ? 1.0 : 0.0;
            else if (eat(">"))  v = v >  additive() ? 1.0 : 0.0;
            else return v;
        }
    }
    double additive() {
        double v = term();
        for (;;) {
            if (eat("+"))      v += term();
            else if (eat("-")) v -= term();
            else return v;
        }
    }
    double term() {
        double v = unary();
        for (;;) {
            if (eat("*")) v *= unary();
            else if (eat("/")) {
                const double d = unary();
                if (d == 0.0) throw ExprError("division by zero");
                v /= d;
            } else if (eat("%")) {
                const double d = unary();
                if (d == 0.0) throw ExprError("division by zero");
                v = std::fmod(v, d);
            } else return v;
        }
    }
    double unary() {
        if (eat("-")) return -unary();
        if (eat("+")) return unary();
        if (eat("!")) return unary() == 0.0 ? 1.0 : 0.0;
        return power();
    }
    double power() {
        const double b = primary();
        // Right to left, and above the sign: -2^2 is -4, 2^3^2 is 512.
        if (eat("^")) return std::pow(b, unary());
        return b;
    }

    std::string stringLit() {
        skip();
        if (m_i >= m_s.size() || (m_s[m_i] != '"' && m_s[m_i] != '\'')) throw ExprError("expected a \"text\"");
        const char q = m_s[m_i++];
        const std::size_t end = m_s.find(q, m_i);
        if (end == std::string::npos) throw ExprError("a text without its closing quote");
        std::string out = m_s.substr(m_i, end - m_i);
        m_i = end + 1;
        return out;
    }

    double primary() {
        skip();
        if (m_i >= m_s.size()) throw ExprError("the formula ends too soon");
        const char c = m_s[m_i];
        if (c == '(') {
            ++m_i;
            const double v = ternary();
            expect(")");
            return v;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            const char* begin = m_s.c_str() + m_i;
            char* end = nullptr;
            const double v = std::strtod(begin, &end);
            if (end == begin) throw ExprError("a number that is not one");
            m_i += static_cast<std::size_t>(end - begin);
            return v;
        }
        if (identStart(c)) {
            // A path: "radius", "size.x", "hub.center.y".
            std::size_t j = m_i;
            while (j < m_s.size() && (identChar(m_s[j]) || (m_s[j] == '.' && j + 1 < m_s.size() &&
                                                             identStart(m_s[j + 1]))))
                ++j;
            const std::string path = m_s.substr(m_i, j - m_i);
            m_i = j;
            skip();
            if (m_i < m_s.size() && m_s[m_i] == '(') {
                ++m_i;
                return call(path);
            }
            return lookup(m_c, path);
        }
        throw ExprError(std::string("unexpected '") + c + "'");
    }

    double call(const std::string& fn) {
        if (fn == "ch") {
            // Houdini's way of saying a name: ch("../hub/radius").
            std::string path = stringLit();
            expect(")");
            while (path.rfind("../", 0) == 0 || path.rfind("./", 0) == 0) path.erase(0, path.find('/') + 1);
            std::replace(path.begin(), path.end(), '/', '.');
            return lookup(m_c, path);
        }
        std::vector<double> a;
        skip();
        if (!eat(")")) {
            do a.push_back(ternary());
            while (eat(","));
            expect(")");
        }
        auto want = [&](std::size_t n) {
            if (a.size() != n)
                throw ExprError(fn + "() takes " + std::to_string(n) + (n == 1 ? " value" : " values"));
        };
        constexpr double kDeg = 3.14159265358979323846 / 180.0;
        if (fn == "sin")   { want(1); return std::sin(a[0] * kDeg); }
        if (fn == "cos")   { want(1); return std::cos(a[0] * kDeg); }
        if (fn == "tan")   { want(1); return std::tan(a[0] * kDeg); }
        if (fn == "asin")  { want(1); return std::asin(std::clamp(a[0], -1.0, 1.0)) / kDeg; }
        if (fn == "acos")  { want(1); return std::acos(std::clamp(a[0], -1.0, 1.0)) / kDeg; }
        if (fn == "atan")  { want(1); return std::atan(a[0]) / kDeg; }
        if (fn == "atan2") { want(2); return std::atan2(a[0], a[1]) / kDeg; }
        if (fn == "abs")   { want(1); return std::fabs(a[0]); }
        if (fn == "sign")  { want(1); return a[0] > 0.0 ? 1.0 : a[0] < 0.0 ? -1.0 : 0.0; }
        if (fn == "floor") { want(1); return std::floor(a[0]); }
        if (fn == "ceil")  { want(1); return std::ceil(a[0]); }
        if (fn == "round") { want(1); return std::round(a[0]); }
        if (fn == "frac")  { want(1); return a[0] - std::floor(a[0]); }
        if (fn == "sqrt")  { want(1); return std::sqrt(std::max(0.0, a[0])); }
        if (fn == "exp")   { want(1); return std::exp(a[0]); }
        if (fn == "log")   { want(1); if (a[0] <= 0.0) throw ExprError("log() of nothing above 0"); return std::log(a[0]); }
        if (fn == "pow")   { want(2); return std::pow(a[0], a[1]); }
        if (fn == "min" || fn == "max") {
            if (a.empty()) throw ExprError(fn + "() needs values");
            return fn == "min" ? *std::min_element(a.begin(), a.end()) : *std::max_element(a.begin(), a.end());
        }
        if (fn == "clamp") { want(3); return std::clamp(a[0], std::min(a[1], a[2]), std::max(a[1], a[2])); }
        if (fn == "lerp")  { want(3); return a[0] + (a[1] - a[0]) * a[2]; }
        if (fn == "if")    { want(3); return a[0] != 0.0 ? a[1] : a[2]; }
        if (fn == "fit" || fn == "fit01") {
            // Houdini's: where value sits between omin and omax, put between
            // nmin and nmax -- and no further.
            double v, omin, omax, nmin, nmax;
            if (fn == "fit") { want(5); v = a[0]; omin = a[1]; omax = a[2]; nmin = a[3]; nmax = a[4]; }
            else             { want(3); v = a[0]; omin = 0.0;  omax = 1.0;  nmin = a[1]; nmax = a[2]; }
            if (omax == omin) return nmin;
            const double t = std::clamp((v - omin) / (omax - omin), 0.0, 1.0);
            return nmin + (nmax - nmin) * t;
        }
        if (fn == "smooth") {
            // smooth(value, min, max): 0 below, 1 above, an S between.
            want(3);
            if (a[2] == a[1]) return a[0] < a[1] ? 0.0 : 1.0;
            const double t = std::clamp((a[0] - a[1]) / (a[2] - a[1]), 0.0, 1.0);
            return t * t * (3.0 - 2.0 * t);
        }
        if (fn == "rand") {
            // The same seed, the same number in [0, 1) -- on every cook.
            want(1);
            const double s = a[0];
            std::uint64_t bits = 0;
            std::memcpy(&bits, &s, sizeof bits);
            return random01(static_cast<int>(bits ^ (bits >> 32)), 0x5EED);
        }
        throw ExprError("no function called '" + fn + "'");
    }

public:
    // What a name says: a parameter first, then a word the language keeps,
    // then a node's setting.
    static double lookup(Ctx& c, const std::string& path) {
        std::vector<std::string> part;
        for (std::size_t a = 0;;) {
            const std::size_t b = path.find('.', a);
            part.push_back(path.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + 1;
        }
        if (const Parm* p = c.g.parm(part[0])) {
            if (!parmHasValue(p->type)) throw ExprError("'" + p->name + "' is a " +
                                                        parmTypeLabels()[static_cast<std::size_t>(p->type)] +
                                                        " -- it has no value");
            if (parmIsText(p->type)) throw ExprError("'" + p->name + "' is text, not a number");
            if (p->type == ParmType::Vector) {
                if (part.size() != 2 || componentOf(part[1]) < 0)
                    throw ExprError("'" + p->name + "' is a vector: say " + p->name + ".x, .y or .z");
                return p->value[componentOf(part[1])];
            }
            if (part.size() > 1) throw ExprError("'" + p->name + "' is one number, it has no ." + part[1]);
            return p->value.x;
        }
        if (part.size() == 1) {
            if (path == "pi")    return 3.14159265358979323846;
            if (path == "tau")   return 6.28318530717958647692;
            if (path == "true")  return 1.0;
            if (path == "false") return 0.0;
        }
        const Node* n = nullptr;
        for (const auto& o : c.g.nodes)
            if (o->name == part[0]) n = o.get();
        if (!n) throw ExprError("no parameter or node called '" + part[0] + "'");
        if (part.size() < 2) throw ExprError("'" + part[0] + "' is a node: say which setting (" + part[0] + ".radius)");
        const Property* pr = findProp(*n, part[1]);
        if (!pr) throw ExprError("node '" + part[0] + "' has no setting '" + part[1] + "'");
        int comp = -1;
        if (pr->kind == PropKind::Vec3 || pr->kind == PropKind::Color) {
            if (part.size() != 3 || (comp = componentOf(part[2])) < 0)
                throw ExprError("'" + part[0] + "." + part[1] + "' is a vector: say .x, .y or .z");
        } else if (part.size() > 2) {
            throw ExprError("'" + part[0] + "." + part[1] + "' is one value, it has no ." + part[2]);
        }
        if (pr->kind == PropKind::Text) throw ExprError("'" + part[0] + "." + part[1] + "' is text, not a number");
        return settingValue(c, *n, part[1], comp);
    }
};

// RAII for Ctx::stack: in and out of one setting, a loop refused.
struct Visit {
    Ctx& c;
    Visit(Ctx& ctx, const Node& n, const std::string& key) : c(ctx) {
        const std::string tag = std::to_string(n.id) + ":" + key;
        if (std::find(c.stack.begin(), c.stack.end(), tag) != c.stack.end())
            throw ExprError("'" + n.name + "." + key + "' comes round to itself");
        if (c.stack.size() > 64) throw ExprError("too many settings deep");
        c.stack.push_back(tag);
    }
    ~Visit() { c.stack.pop_back(); }
};

double settingValue(Ctx& c, const Node& n, const std::string& key, int comp) {
    const Property* pr = findProp(n, key);
    if (!pr) return 0.0;
    const std::string full = comp >= 0 ? key + "." + "xyz"[comp] : key;
    const auto it = n.exprs.find(full);
    if (it == n.exprs.end()) return readNumber(*pr, n, comp);
    Visit v(c, n, full);
    // Through a scratch node, so what reads it gets the value the cook will
    // use: clamped, rounded, one of the labels.
    std::unique_ptr<Node> tmp = n.clone();
    writeNumber(*pr, *tmp, comp, evalIn(c, it->second));
    return readNumber(*pr, *tmp, comp);
}

double evalIn(Ctx& c, const std::string& expr) {
    Parser p(c, expr);
    return p.run();
}

// A "${...}" or "$name" in a text: the text it stands for.
std::string pieceText(Ctx& c, const std::string& inner) {
    // A plain path to something that is text gives the text...
    bool isPath = !inner.empty() && identStart(inner[0]);
    for (char ch : inner) isPath = isPath && (identChar(ch) || ch == '.');
    if (isPath) {
        const std::size_t dot = inner.find('.');
        const std::string head = inner.substr(0, dot);
        if (const Parm* p = c.g.parm(head); p && parmIsText(p->type) && dot == std::string::npos)
            return p->text;
        if (!c.g.parm(head) && dot != std::string::npos) {
            for (const auto& o : c.g.nodes) {
                if (o->name != head) continue;
                const std::string key = inner.substr(dot + 1);
                if (const Property* pr = findProp(*o, key); pr && pr->kind == PropKind::Text)
                    return settingText(c, *o, key);
            }
        }
    }
    // ...anything else is a formula, written out.
    return fmtNumber(evalIn(c, inner));
}

std::string textIn(Ctx& c, const std::string& t) {
    std::string out;
    for (std::size_t i = 0; i < t.size();) {
        if (t[i] != '$') { out += t[i++]; continue; }
        if (i + 1 < t.size() && t[i + 1] == '$') { out += '$'; i += 2; continue; }
        if (i + 1 < t.size() && t[i + 1] == '{') {
            const std::size_t end = t.find('}', i + 2);
            if (end == std::string::npos) throw ExprError("a ${ without its }");
            out += pieceText(c, t.substr(i + 2, end - i - 2));
            i = end + 1;
            continue;
        }
        std::size_t j = i + 1;
        while (j < t.size() && (identChar(t[j]) || (t[j] == '.' && j + 1 < t.size() && identStart(t[j + 1])))) ++j;
        if (j == i + 1) { out += '$'; ++i; continue; }
        out += pieceText(c, t.substr(i + 1, j - i - 1));
        i = j;
    }
    return out;
}

std::string settingText(Ctx& c, const Node& n, const std::string& key) {
    const Property* pr = findProp(n, key);
    if (!pr || pr->kind != PropKind::Text) return {};
    const auto it = n.exprs.find(key);
    if (it == n.exprs.end()) return *static_cast<const std::string*>(pr->field(const_cast<Node*>(&n)));
    Visit v(c, n, key);
    return textIn(c, it->second);
}

// --- Finding names in a formula ---------------------------------------------------
// The places in `s` where a parameter (or node) is named, by the first word of
// its path: [begin, end) of each word. A Text setting's expression is a text
// with "$..." in it, everything else a formula; in a formula, ch("...") names
// one too.
void namesIn(const std::string& s, bool text, std::vector<std::pair<std::size_t, std::size_t>>& out,
             std::size_t base = 0);

void namesInFormula(const std::string& s, std::vector<std::pair<std::size_t, std::size_t>>& out,
                    std::size_t base) {
    for (std::size_t i = 0; i < s.size();) {
        const char c = s[i];
        if (c == '"' || c == '\'') {
            // ch("../hub/radius"): the word after any ../ is a name.
            const std::size_t end = s.find(c, i + 1);
            const std::size_t stop = end == std::string::npos ? s.size() : end;
            std::size_t a = i + 1;
            while (s.compare(a, 3, "../") == 0) a += 3;
            while (s.compare(a, 2, "./") == 0) a += 2;
            std::size_t b = a;
            while (b < stop && identChar(s[b])) ++b;
            if (b > a && identStart(s[a])) out.emplace_back(base + a, base + b);
            i = stop + 1;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            // A number, exponent and all: the "e" of 1e5 is no name.
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
            continue;
        }
        if (identStart(c) && (i == 0 || s[i - 1] != '.')) {
            std::size_t j = i;
            while (j < s.size() && identChar(s[j])) ++j;
            // A function's name is not a parameter's.
            std::size_t k = j;
            while (k < s.size() && std::isspace(static_cast<unsigned char>(s[k]))) ++k;
            if (k >= s.size() || s[k] != '(') out.emplace_back(base + i, base + j);
            // Skip the rest of the path: "hub.radius" names hub, not radius.
            while (j < s.size() && (identChar(s[j]) || s[j] == '.')) ++j;
            i = j;
            continue;
        }
        ++i;
    }
}

void namesIn(const std::string& s, bool text, std::vector<std::pair<std::size_t, std::size_t>>& out,
             std::size_t base) {
    if (!text) { namesInFormula(s, out, base); return; }
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] != '$') { ++i; continue; }
        if (i + 1 < s.size() && s[i + 1] == '$') { i += 2; continue; }
        if (i + 1 < s.size() && s[i + 1] == '{') {
            const std::size_t end = s.find('}', i + 2);
            const std::size_t stop = end == std::string::npos ? s.size() : end;
            namesInFormula(s.substr(i + 2, stop - i - 2), out, base + i + 2);
            i = stop + 1;
            continue;
        }
        std::size_t j = i + 1;
        while (j < s.size() && identChar(s[j])) ++j;
        if (j > i + 1 && identStart(s[i + 1])) out.emplace_back(base + i + 1, base + j);
        i = j;
    }
}

bool textKey(const Node& n, const std::string& key) {
    const Property* pr = findProp(n, splitKey(key).first);
    return pr && pr->kind == PropKind::Text;
}

bool mentions(const std::string& s, bool text, const std::string& name) {
    std::vector<std::pair<std::size_t, std::size_t>> at;
    namesIn(s, text, at);
    for (const auto& [a, b] : at)
        if (s.compare(a, b - a, name) == 0 && b - a == name.size()) return true;
    return false;
}

std::string renamed(const std::string& s, bool text, const std::string& from, const std::string& to) {
    std::vector<std::pair<std::size_t, std::size_t>> at;
    namesIn(s, text, at);
    std::string out;
    std::size_t last = 0;
    for (const auto& [a, b] : at) {
        if (b - a != from.size() || s.compare(a, b - a, from) != 0) continue;
        out += s.substr(last, a - last);
        out += to;
        last = b;
    }
    return out + s.substr(last);
}

} // namespace

// --- The public half ----------------------------------------------------------------

ExprResult evalExpr(const Graph& g, const std::string& expr) {
    ExprResult r;
    Ctx c{g, {}};
    try {
        r.value = evalIn(c, expr);
        if (!std::isfinite(r.value)) { r.value = 0.0; r.error = "the result is not a number"; }
    } catch (const ExprError& e) {
        r.error = e.what();
    }
    return r;
}

std::string evalText(const Graph& g, const std::string& tmpl, std::string* error) {
    Ctx c{g, {}};
    try {
        return textIn(c, tmpl);
    } catch (const ExprError& e) {
        if (error) *error = e.what();
        return {};
    }
}

std::unique_ptr<Node> resolved(const Graph& g, const Node& n, std::string* error) {
    if (n.exprs.empty()) return nullptr;
    std::unique_ptr<Node> out = n.clone();
    for (const auto& [key, expr] : n.exprs) {
        const auto [base, comp] = splitKey(key);
        const Property* pr = findProp(n, base);
        if (!pr) continue;   // a setting this kind no longer has: nothing to drive
        Ctx c{g, {}};
        try {
            if (pr->kind == PropKind::Text) {
                *static_cast<std::string*>(pr->field(out.get())) = settingText(c, n, base);
            } else {
                // Through settingValue, so a loop through this very setting
                // ("hub.radius" saying hub.radius) is caught like any other.
                writeNumber(*pr, *out, comp, settingValue(c, n, base, comp));
            }
        } catch (const ExprError& e) {
            if (error && error->empty()) *error = pr->label + ": " + e.what();
        }
    }
    return out;
}

std::string shownValue(const Graph& g, const Node& n, const std::string& key) {
    const auto [base, comp] = splitKey(key);
    const Property* pr = findProp(n, base);
    if (!pr) return {};
    const auto it = n.exprs.find(key);
    if (it != n.exprs.end()) {
        // Its own error, not the first of the node's.
        Ctx c{g, {}};
        try {
            if (pr->kind == PropKind::Text) settingText(c, n, base);
            else settingValue(c, n, base, comp);
        } catch (const ExprError& e) {
            return std::string("error: ") + e.what();
        }
    }
    const std::unique_ptr<Node> r = resolved(g, n);
    const Node& src = r ? *r : n;
    switch (pr->kind) {
        case PropKind::Text:    return *static_cast<const std::string*>(pr->field(const_cast<Node*>(&src)));
        case PropKind::Bool:    return readNumber(*pr, src, -1) != 0.0 ? "on" : "off";
        case PropKind::EnumInt: {
            const int v = static_cast<int>(readNumber(*pr, src, -1));
            return v >= 0 && v < static_cast<int>(pr->enumLabels.size()) ? pr->enumLabels[static_cast<std::size_t>(v)]
                                                                         : std::to_string(v);
        }
        default:                return fmtNumber(readNumber(*pr, src, comp));
    }
}

bool parmHidden(const Graph& g, const Parm& p) {
    if (p.hideWhen.empty()) return false;
    const ExprResult r = evalExpr(g, p.hideWhen);
    return r.ok() && r.value != 0.0;
}

bool parmDisabled(const Graph& g, const Parm& p) {
    if (p.disableWhen.empty()) return false;
    const ExprResult r = evalExpr(g, p.disableWhen);
    return r.ok() && r.value != 0.0;
}

std::vector<std::string> parmUsers(const Graph& g, const std::string& name) {
    std::vector<std::string> out;
    for (const auto& n : g.nodes)
        for (const auto& [key, expr] : n->exprs)
            if (mentions(expr, textKey(*n, key), name)) out.push_back(n->name + "." + key);
    return out;
}

void renameParm(Graph& g, const std::string& from, const std::string& to) {
    if (from == to) return;
    for (auto& n : g.nodes)
        for (auto& [key, expr] : n->exprs) expr = renamed(expr, textKey(*n, key), from, to);
    for (Parm& p : g.parms) {
        p.hideWhen    = renamed(p.hideWhen, false, from, to);
        p.disableWhen = renamed(p.disableWhen, false, from, to);
        if (p.name == from) p.name = to;
    }
}

void freezeExpr(const Graph& g, Node& n, const std::string& key) {
    const auto it = n.exprs.find(key);
    if (it == n.exprs.end()) return;
    const auto [base, comp] = splitKey(key);
    if (const Property* pr = findProp(n, base)) {
        Ctx c{g, {}};
        try {
            if (pr->kind == PropKind::Text) {
                const std::string t = settingText(c, n, base);
                *static_cast<std::string*>(pr->field(&n)) = t;
            } else {
                writeNumber(*pr, n, comp, settingValue(c, n, base, comp));
            }
        } catch (const ExprError&) {
            // A formula that does not work leaves the number it had.
        }
    }
    n.exprs.erase(key);
}

void removeParm(Graph& g, const std::string& name) {
    if (!g.parm(name)) return;
    // Worked out while the parameter is still there to be read: a graph whose
    // node b follows a, and a follows the parameter, freezes both.
    std::vector<std::pair<Node*, std::string>> frozen;
    for (auto& n : g.nodes)
        for (const auto& [key, expr] : n->exprs)
            if (mentions(expr, textKey(*n, key), name)) frozen.emplace_back(n.get(), key);
    // Every setting it reaches gets its number first, then the formulas go.
    std::vector<std::unique_ptr<Node>> values;
    for (auto& [n, key] : frozen) {
        std::unique_ptr<Node> r = resolved(g, *n);
        values.push_back(std::move(r));
    }
    for (std::size_t i = 0; i < frozen.size(); ++i) {
        Node& n = *frozen[i].first;
        const std::string& key = frozen[i].second;
        const auto [base, comp] = splitKey(key);
        if (const Property* pr = findProp(n, base); pr && values[i]) {
            if (pr->kind == PropKind::Text)
                *static_cast<std::string*>(pr->field(&n)) = *static_cast<std::string*>(pr->field(values[i].get()));
            else
                writeNumber(*pr, n, comp, readNumber(*pr, *values[i], comp));
        }
    }
    for (auto& [n, key] : frozen) n->exprs.erase(key);
    for (Parm& p : g.parms) {
        if (mentions(p.hideWhen, false, name))    p.hideWhen.clear();
        if (mentions(p.disableWhen, false, name)) p.disableWhen.clear();
    }
    g.parms.erase(std::remove_if(g.parms.begin(), g.parms.end(),
                                 [&](const Parm& p) { return p.name == name; }),
                  g.parms.end());
}

bool linkable(PropKind k, ParmType t) {
    switch (k) {
        case PropKind::Text:  return parmIsText(t);
        case PropKind::Vec3:
        case PropKind::Color: return t == ParmType::Vector || t == ParmType::Float || t == ParmType::Int;
        case PropKind::Float:
        case PropKind::Int:
        case PropKind::Bool:
        case PropKind::EnumInt:
            return t == ParmType::Float || t == ParmType::Int || t == ParmType::Toggle || t == ParmType::Menu;
        default: return false;
    }
}

std::string linkExpr(const Parm& p, PropKind k, int component) {
    if (k == PropKind::Text) return "$" + p.name;
    if (p.type == ParmType::Vector) return p.name + "." + "xyz"[std::clamp(component, 0, 2)];
    return p.name;
}

std::string promote(Graph& g, Node& n, const Property& pr) {
    Parm p;
    switch (pr.kind) {
        case PropKind::Float:   p.type = ParmType::Float;  break;
        case PropKind::Int:     p.type = ParmType::Int;    break;
        case PropKind::Bool:    p.type = ParmType::Toggle; break;
        case PropKind::EnumInt: p.type = ParmType::Menu;   p.items = pr.enumLabels; break;
        case PropKind::Vec3:    p.type = ParmType::Vector; break;
        case PropKind::Text:
            if (pr.key == "points") return {};   // a curve's points are drawn, not dialled
            p.type = pr.key.rfind("material", 0) == 0 ? ParmType::Material
                   : pr.key == "prefab"               ? ParmType::Prefab
                                                      : ParmType::String;
            break;
        default: return {};
    }
    // Its name from the setting's key -- "radius" -- and the node's in front
    // when another parameter has that already: "spoke_radius".
    const std::string want = g.parm(pr.key) || reserved(pr.key) ? n.name + "_" + pr.key : pr.key;
    p.name  = g.uniqueParmName(want);
    p.label = g.parm(pr.key) ? n.name + " " + pr.label : pr.label;
    p.tip   = "Promoted from " + n.name + "." + pr.key;
    p.min   = pr.min;
    p.max   = pr.max;
    p.step  = pr.kind == PropKind::Int ? std::max(1.0f, pr.speed) : (pr.speed > 0.0f ? pr.speed : 0.1f);
    // The value it has now, expression and all: promoting changes nothing.
    const std::unique_ptr<Node> r = resolved(g, n);
    const Node& src = r ? *r : n;
    if (pr.kind == PropKind::Text) {
        p.text = *static_cast<const std::string*>(pr.field(const_cast<Node*>(&src)));
    } else if (pr.kind == PropKind::Vec3) {
        p.value = *static_cast<const glm::vec3*>(pr.field(const_cast<Node*>(&src)));
    } else {
        p.value.x = static_cast<float>(readNumber(pr, src, -1));
    }
    g.parms.push_back(p);
    if (pr.kind == PropKind::Vec3) {
        for (int k = 0; k < 3; ++k) n.exprs[pr.key + "." + "xyz"[k]] = linkExpr(p, pr.kind, k);
    } else {
        n.exprs[pr.key] = linkExpr(p, pr.kind);
    }
    return p.name;
}

} // namespace proc
