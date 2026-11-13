// Strahl's lexer and parser: source text to the tree in StrahlInternal.hpp.
//
// Statements need no separator. A line break ends nothing by itself -- an
// operator at the end of a line carries on onto the next -- with one exception
// that keeps the common cases unambiguous: a `(` or `[` at the START of a line
// opens a new expression instead of calling or indexing the one above. So
//
//     sphere { radius: 1 }
//     [0, 1, 0]           -- a new (positional) value, not an index
//
// reads the way it looks. `;` and `,` are accepted anywhere a statement ends,
// which is what lets `sphere { [0, 1, 0], 0.5, gold }` read like POV-Ray.

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "StrahlInternal.hpp"

namespace strahl {

const char* tokName(int t) {
    switch (t) {
    case T_EOF: return "end of file";
    case T_NUM: return "a number";
    case T_STR: return "a string";
    case T_COLOR: return "a colour";
    case T_IDENT: return "a name";
    case T_LPAREN: return "'('";
    case T_RPAREN: return "')'";
    case T_LBRACK: return "'['";
    case T_RBRACK: return "']'";
    case T_LBRACE: return "'{'";
    case T_RBRACE: return "'}'";
    case T_COMMA: return "','";
    case T_SEMI: return "';'";
    case T_COLON: return "':'";
    case T_DOT: return "'.'";
    case T_DOTDOT: return "'..'";
    case T_DOTDOTEQ: return "'..='";
    case T_ARROW: return "'=>'";
    case T_PLUS: return "'+'";
    case T_MINUS: return "'-'";
    case T_STAR: return "'*'";
    case T_SLASH: return "'/'";
    case T_PERCENT: return "'%'";
    case T_CARET: return "'^'";
    case T_CONCAT: return "'++'";
    case T_EQ: return "'=='";
    case T_NE: return "'!='";
    case T_LT: return "'<'";
    case T_LE: return "'<='";
    case T_GT: return "'>'";
    case T_GE: return "'>='";
    case T_NOT: return "'!'";
    case T_ANDAND: return "'&&'";
    case T_OROR: return "'||'";
    case T_ASSIGN: return "'='";
    case T_PLUSEQ: return "'+='";
    case T_MINUSEQ: return "'-='";
    case T_STAREQ: return "'*='";
    case T_SLASHEQ: return "'/='";
    case K_LET: return "'let'";
    case K_FN: return "'fn'";
    case K_IF: return "'if'";
    case K_ELSE: return "'else'";
    case K_FOR: return "'for'";
    case K_IN: return "'in'";
    case K_WHILE: return "'while'";
    case K_RETURN: return "'return'";
    case K_BREAK: return "'break'";
    case K_CONTINUE: return "'continue'";
    case K_IMPORT: return "'import'";
    case K_TRUE: return "'true'";
    case K_FALSE: return "'false'";
    case K_NIL: return "'nil'";
    case K_AND: return "'and'";
    case K_OR: return "'or'";
    case K_NOT: return "'not'";
    default: return "?";
    }
}

namespace {

struct Token {
    int         t = T_EOF;
    double      num = 0.0;
    glm::dvec3  color{0.0};
    std::string s;
    Loc         loc;
    bool        nl = false;   // a line break came before it
};

struct Keyword { const char* word; int tok; };
constexpr Keyword kKeywords[] = {
    {"let", K_LET}, {"fn", K_FN}, {"if", K_IF}, {"else", K_ELSE}, {"for", K_FOR},
    {"in", K_IN}, {"while", K_WHILE}, {"return", K_RETURN}, {"break", K_BREAK},
    {"continue", K_CONTINUE}, {"import", K_IMPORT}, {"true", K_TRUE}, {"false", K_FALSE},
    {"nil", K_NIL}, {"and", K_AND}, {"or", K_OR}, {"not", K_NOT},
};

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::vector<Token> lex(const std::string& src, int file) {
    std::vector<Token> out;
    std::size_t i = 0;
    int line = 1, col = 1;
    bool nl = true;
    const auto adv = [&](std::size_t k = 1) {
        for (std::size_t j = 0; j < k && i < src.size(); ++j) {
            if (src[i] == '\n') { ++line; col = 1; nl = true; }
            else ++col;
            ++i;
        }
    };
    const auto peek = [&](std::size_t k) { return i + k < src.size() ? src[i + k] : '\0'; };

    while (i < src.size()) {
        const char c = src[i];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { adv(); continue; }
        if (c == '/' && peek(1) == '/') {
            while (i < src.size() && src[i] != '\n') adv();
            continue;
        }
        if (c == '/' && peek(1) == '*') {
            const Loc at{file, line, col};
            adv(2);
            while (i < src.size() && !(src[i] == '*' && peek(1) == '/')) adv();
            if (i >= src.size()) throw Error(at, "this comment is never closed (missing */)");
            adv(2);
            continue;
        }

        Token tk;
        tk.loc = Loc{file, line, col};
        tk.nl  = nl;
        nl = false;

        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))) {
            std::size_t j = i;
            while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) ++j;
            // A '.' belongs to the number only when a digit follows: "0..10"
            // is a range, not the number 0. followed by .10.
            if (j < src.size() && src[j] == '.' && j + 1 < src.size() &&
                std::isdigit(static_cast<unsigned char>(src[j + 1]))) {
                ++j;
                while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) ++j;
            }
            if (j < src.size() && (src[j] == 'e' || src[j] == 'E')) {
                std::size_t k = j + 1;
                if (k < src.size() && (src[k] == '+' || src[k] == '-')) ++k;
                if (k < src.size() && std::isdigit(static_cast<unsigned char>(src[k]))) {
                    j = k;
                    while (j < src.size() && std::isdigit(static_cast<unsigned char>(src[j]))) ++j;
                }
            }
            tk.t = T_NUM;
            tk.num = std::strtod(src.substr(i, j - i).c_str(), nullptr);
            adv(j - i);
            if (i < src.size() && (std::isalpha(static_cast<unsigned char>(src[i])) || src[i] == '_'))
                throw Error(tk.loc, "a number runs straight into a name here -- missing an operator?");
            out.push_back(tk);
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < src.size() && (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_')) ++j;
            tk.s = src.substr(i, j - i);
            tk.t = T_IDENT;
            for (const Keyword& k : kKeywords)
                if (tk.s == k.word) { tk.t = k.tok; break; }
            adv(j - i);
            out.push_back(tk);
            continue;
        }
        if (c == '#') {
            std::size_t j = i + 1;
            while (j < src.size() && hexDigit(src[j]) >= 0) ++j;
            const std::size_t n = j - i - 1;
            if (n != 3 && n != 6 && n != 8)
                throw Error(tk.loc, "a colour is # and 3 or 6 hex digits, like #f80 or #ff8800");
            int rgb[3];
            for (int k = 0; k < 3; ++k) {
                if (n == 3) rgb[k] = hexDigit(src[i + 1 + k]) * 17;
                else        rgb[k] = hexDigit(src[i + 1 + 2 * k]) * 16 + hexDigit(src[i + 2 + 2 * k]);
            }
            tk.t = T_COLOR;
            tk.color = glm::dvec3(rgb[0], rgb[1], rgb[2]) / 255.0;
            adv(j - i);
            out.push_back(tk);
            continue;
        }
        if (c == '"' || c == '\'') {
            const char q = c;
            adv();
            std::string s;
            while (i < src.size() && src[i] != q) {
                if (src[i] == '\n') throw Error(tk.loc, "this string is never closed");
                if (src[i] == '\\' && i + 1 < src.size()) {
                    const char e = src[i + 1];
                    s += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                    adv(2);
                    continue;
                }
                s += src[i];
                adv();
            }
            if (i >= src.size()) throw Error(tk.loc, "this string is never closed");
            adv();
            tk.t = T_STR;
            tk.s = std::move(s);
            out.push_back(tk);
            continue;
        }

        struct Sym { const char* text; int tok; };
        static constexpr Sym kSyms[] = {
            {"..=", T_DOTDOTEQ}, {"..", T_DOTDOT}, {"=>", T_ARROW}, {"++", T_CONCAT},
            {"==", T_EQ}, {"!=", T_NE}, {"<=", T_LE}, {">=", T_GE}, {"&&", T_ANDAND},
            {"||", T_OROR}, {"+=", T_PLUSEQ}, {"-=", T_MINUSEQ}, {"*=", T_STAREQ},
            {"/=", T_SLASHEQ},
            {"(", T_LPAREN}, {")", T_RPAREN}, {"[", T_LBRACK}, {"]", T_RBRACK},
            {"{", T_LBRACE}, {"}", T_RBRACE}, {",", T_COMMA}, {";", T_SEMI}, {":", T_COLON},
            {".", T_DOT}, {"+", T_PLUS}, {"-", T_MINUS}, {"*", T_STAR}, {"/", T_SLASH},
            {"%", T_PERCENT}, {"^", T_CARET}, {"<", T_LT}, {">", T_GT}, {"!", T_NOT},
            {"=", T_ASSIGN},
        };
        bool matched = false;
        for (const Sym& s : kSyms) {
            const std::size_t len = std::strlen(s.text);
            if (src.compare(i, len, s.text) == 0) {
                tk.t = s.tok;
                adv(len);
                out.push_back(tk);
                matched = true;
                break;
            }
        }
        if (!matched) {
            std::string ch(1, c);
            throw Error(tk.loc, "unexpected character '" + ch + "'");
        }
    }
    Token eof;
    eof.t = T_EOF;
    eof.loc = Loc{file, line, col};
    eof.nl = true;
    out.push_back(eof);
    return out;
}

class Parser {
public:
    Parser(Program& prog, std::vector<Token> toks) : m_prog(prog), m_t(std::move(toks)) {}

    Node* program() {
        Node* b = node(N::Block, cur().loc);
        while (cur().t != T_EOF) {
            b->list.push_back(statement());
            separators();
        }
        return b;
    }

private:
    Program&           m_prog;
    std::vector<Token> m_t;
    std::size_t        m_i = 0;
    // Inside the head of an `if`, `while` or `for`, a `{` starts the body
    // rather than building an object from the expression before it. Reset
    // inside any bracket, where no body can start.
    bool               m_noBrace = false;

    const Token& cur() const { return m_t[m_i]; }
    const Token& ahead(std::size_t k = 1) const {
        return m_t[std::min(m_i + k, m_t.size() - 1)];
    }
    bool at(int t) const { return cur().t == t; }
    Token next() { Token t = m_t[m_i]; if (m_i + 1 < m_t.size()) ++m_i; return t; }
    bool accept(int t) { if (at(t)) { next(); return true; } return false; }
    Token expect(int t, const char* what) {
        if (!at(t))
            throw Error(cur().loc, std::string("expected ") + tokName(t) + " " + what +
                                       ", found " + describe(cur()));
        return next();
    }
    static std::string describe(const Token& t) {
        if (t.t == T_IDENT) return "'" + t.s + "'";
        if (t.t == T_NUM) return "a number";
        return tokName(t.t);
    }
    Node* node(N k, const Loc& loc) {
        m_prog.nodes.emplace_back();
        Node* n = &m_prog.nodes.back();
        n->kind = k;
        n->loc  = loc;
        return n;
    }
    void separators() { while (at(T_SEMI) || at(T_COMMA)) next(); }

    Node* block() {
        const Token open = expect(T_LBRACE, "to open a block");
        const bool saved = m_noBrace;
        m_noBrace = false;
        Node* b = node(N::Block, open.loc);
        separators();
        while (!at(T_RBRACE)) {
            if (at(T_EOF))
                throw Error(open.loc, "this '{' is never closed");
            b->list.push_back(statement());
            separators();
        }
        next();
        m_noBrace = saved;
        return b;
    }

    std::vector<Param> params() {
        expect(T_LPAREN, "before the parameters");
        std::vector<Param> ps;
        const bool saved = m_noBrace;
        m_noBrace = false;
        while (!at(T_RPAREN)) {
            Param p;
            p.name = expect(T_IDENT, "as a parameter name").s;
            if (accept(T_ASSIGN)) p.def = expression();
            ps.push_back(p);
            if (!accept(T_COMMA)) break;
        }
        expect(T_RPAREN, "after the parameters");
        m_noBrace = saved;
        return ps;
    }

    Node* statement() {
        const Token& t = cur();
        switch (t.t) {
        case K_LET: {
            next();
            Node* n = node(N::Let, t.loc);
            n->str = expect(T_IDENT, "after 'let'").s;
            expect(T_ASSIGN, "after the name in 'let'");
            n->a = expression();
            return n;
        }
        case K_FN:
            if (ahead().t == T_IDENT) {
                next();
                Node* n = node(N::FnDecl, t.loc);
                n->str = next().s;
                n->params = params();
                if (accept(T_ASSIGN) || accept(T_ARROW)) {
                    n->b = expression();
                    n->flag = true;   // an expression body
                } else {
                    n->b = block();
                }
                return n;
            }
            break;
        case K_IMPORT: {
            next();
            Node* n = node(N::Import, t.loc);
            n->str = expect(T_STR, "naming the file to import").s;
            return n;
        }
        case K_FOR: {
            next();
            Node* n = node(N::For, t.loc);
            n->str = expect(T_IDENT, "as the loop variable").s;
            if (accept(T_COMMA)) n->str2 = expect(T_IDENT, "as the second loop variable").s;
            expect(K_IN, "in a for loop");
            const bool saved = m_noBrace;
            m_noBrace = true;
            n->a = expression();
            m_noBrace = saved;
            n->b = block();
            return n;
        }
        case K_WHILE: {
            next();
            Node* n = node(N::While, t.loc);
            const bool saved = m_noBrace;
            m_noBrace = true;
            n->a = expression();
            m_noBrace = saved;
            n->b = block();
            return n;
        }
        case K_RETURN: {
            next();
            Node* n = node(N::Return, t.loc);
            if (!at(T_RBRACE) && !at(T_SEMI) && !at(T_EOF) && !cur().nl) n->a = expression();
            return n;
        }
        case K_BREAK:    next(); return node(N::Break, t.loc);
        case K_CONTINUE: next(); return node(N::Continue, t.loc);
        case T_IDENT:
            if (ahead().t == T_COLON) {
                Node* n = node(N::Prop, t.loc);
                n->str = next().s;
                next();
                n->a = expression();
                return n;
            }
            break;
        default: break;
        }

        Node* e = expression();
        const int op = cur().t;
        if (op == T_ASSIGN || op == T_PLUSEQ || op == T_MINUSEQ || op == T_STAREQ ||
            op == T_SLASHEQ) {
            if (e->kind != N::Ident && e->kind != N::Index)
                throw Error(cur().loc, "only a name or a list element can be assigned to");
            const Token o = next();
            Node* n = node(N::Assign, o.loc);
            n->op = op;
            n->a  = e;
            n->b  = expression();
            return n;
        }
        Node* s = node(N::ExprStmt, e->loc);
        s->a = e;
        return s;
    }

    Node* expression() { return orExpr(); }

    Node* binary(N kind, int op, Node* a, Node* b, const Loc& loc) {
        Node* n = node(kind, loc);
        n->op = op;
        n->a = a;
        n->b = b;
        return n;
    }

    Node* orExpr() {
        Node* a = andExpr();
        while (at(K_OR) || at(T_OROR)) {
            const Token o = next();
            a = binary(N::Or, T_OROR, a, andExpr(), o.loc);
        }
        return a;
    }
    Node* andExpr() {
        Node* a = notExpr();
        while (at(K_AND) || at(T_ANDAND)) {
            const Token o = next();
            a = binary(N::And, T_ANDAND, a, notExpr(), o.loc);
        }
        return a;
    }
    Node* notExpr() {
        if (at(K_NOT)) {
            const Token o = next();
            Node* n = node(N::Unary, o.loc);
            n->op = T_NOT;
            n->a  = notExpr();
            return n;
        }
        return compare();
    }
    Node* compare() {
        Node* a = range();
        while (at(T_EQ) || at(T_NE) || at(T_LT) || at(T_LE) || at(T_GT) || at(T_GE)) {
            const Token o = next();
            a = binary(N::Binary, o.t, a, range(), o.loc);
        }
        return a;
    }
    Node* range() {
        Node* a = additive();
        if (at(T_DOTDOT) || at(T_DOTDOTEQ)) {
            const Token o = next();
            Node* n = node(N::Range, o.loc);
            n->a = a;
            n->b = additive();
            n->flag = o.t == T_DOTDOTEQ;
            return n;
        }
        return a;
    }
    Node* additive() {
        Node* a = multiplicative();
        while (at(T_PLUS) || at(T_MINUS) || at(T_CONCAT)) {
            const Token o = next();
            a = binary(N::Binary, o.t, a, multiplicative(), o.loc);
        }
        return a;
    }
    Node* multiplicative() {
        Node* a = unary();
        while (at(T_STAR) || at(T_SLASH) || at(T_PERCENT)) {
            const Token o = next();
            a = binary(N::Binary, o.t, a, unary(), o.loc);
        }
        return a;
    }
    Node* unary() {
        if (at(T_MINUS) || at(T_NOT) || at(T_PLUS)) {
            const Token o = next();
            Node* inner = unary();
            if (o.t == T_PLUS) return inner;
            // A negative literal folds now, so "-1" is a number rather than
            // an operation on one -- which the compiler then sees as such.
            if (o.t == T_MINUS && inner->kind == N::Num) { inner->num = -inner->num; return inner; }
            Node* n = node(N::Unary, o.loc);
            n->op = o.t;
            n->a  = inner;
            return n;
        }
        return power();
    }
    Node* power() {
        Node* a = postfix();
        if (at(T_CARET)) {
            const Token o = next();
            return binary(N::Binary, T_CARET, a, unary(), o.loc);
        }
        return a;
    }

    Node* postfix() {
        Node* e = primary();
        for (;;) {
            const Token& t = cur();
            if (t.t == T_LPAREN && !t.nl) {
                next();
                Node* c = node(N::Call, t.loc);
                c->a = e;
                const bool saved = m_noBrace;
                m_noBrace = false;
                while (!at(T_RPAREN)) {
                    Arg arg;
                    arg.loc = cur().loc;
                    if (at(T_IDENT) && ahead().t == T_COLON) {
                        arg.name = next().s;
                        next();
                    } else if (!c->args.empty() && !c->args.back().name.empty()) {
                        throw Error(cur().loc, "a positional argument cannot follow a named one");
                    }
                    arg.value = expression();
                    c->args.push_back(arg);
                    if (!accept(T_COMMA)) break;
                }
                expect(T_RPAREN, "to close the call");
                m_noBrace = saved;
                e = c;
                continue;
            }
            if (t.t == T_LBRACK && !t.nl) {
                next();
                Node* ix = node(N::Index, t.loc);
                ix->a = e;
                const bool saved = m_noBrace;
                m_noBrace = false;
                ix->b = expression();
                m_noBrace = saved;
                expect(T_RBRACK, "to close the index");
                e = ix;
                continue;
            }
            if (t.t == T_DOT) {
                next();
                Node* m = node(N::Member, t.loc);
                m->a = e;
                m->str = expect(T_IDENT, "after '.'").s;
                e = m;
                continue;
            }
            if (t.t == T_LBRACE && !m_noBrace) {
                Node* c = node(N::Construct, t.loc);
                c->a = e;
                c->b = block();
                e = c;
                continue;
            }
            return e;
        }
    }

    Node* primary() {
        const Token t = cur();
        switch (t.t) {
        case T_NUM:  { next(); Node* n = node(N::Num, t.loc); n->num = t.num; return n; }
        case T_STR:  { next(); Node* n = node(N::Str, t.loc); n->str = t.s; return n; }
        case T_COLOR:{ next(); Node* n = node(N::Color, t.loc); n->color = t.color; return n; }
        case K_TRUE: { next(); Node* n = node(N::Bool, t.loc); n->num = 1.0; return n; }
        case K_FALSE:{ next(); Node* n = node(N::Bool, t.loc); n->num = 0.0; return n; }
        case K_NIL:  { next(); return node(N::Nil, t.loc); }
        case T_IDENT:{ next(); Node* n = node(N::Ident, t.loc); n->str = t.s; return n; }
        case T_LPAREN: {
            next();
            const bool saved = m_noBrace;
            m_noBrace = false;
            Node* e = expression();
            m_noBrace = saved;
            expect(T_RPAREN, "to close the parenthesis");
            return e;
        }
        case T_LBRACK: {
            next();
            Node* n = node(N::List, t.loc);
            const bool saved = m_noBrace;
            m_noBrace = false;
            while (!at(T_RBRACK)) {
                n->list.push_back(expression());
                if (!accept(T_COMMA)) break;
            }
            m_noBrace = saved;
            expect(T_RBRACK, "to close the list");
            return n;
        }
        case K_FN: {
            next();
            Node* n = node(N::Lambda, t.loc);
            n->params = params();
            if (accept(T_ARROW) || accept(T_ASSIGN)) {
                const bool saved = m_noBrace;
                m_noBrace = false;
                n->b = expression();
                m_noBrace = saved;
                n->flag = true;
            } else {
                n->b = block();
            }
            return n;
        }
        case K_IF: return ifExpr();
        default: break;
        }
        throw Error(t.loc, "expected a value, found " + describe(t));
    }

    Node* ifExpr() {
        const Token t = expect(K_IF, "");
        Node* n = node(N::If, t.loc);
        const bool saved = m_noBrace;
        m_noBrace = true;
        n->a = expression();
        m_noBrace = saved;
        n->b = block();
        if (accept(K_ELSE)) n->c = at(K_IF) ? ifExpr() : block();
        return n;
    }
};

} // namespace

void parse(Program& prog, int fileIndex) {
    Parser p(prog, lex(prog.source, fileIndex));
    prog.root = p.program();
}

} // namespace strahl
