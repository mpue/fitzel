#include "AreaTree.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace areas {

namespace {

const std::string kNone;

// A ratio as the text form writes it ("0.3812"), read without strtof: that one
// follows the C locale, and a desktop set to German would turn the dot into a
// stop sign and every saved layout into a malformed one.
bool readRatio(const std::string& s, std::size_t& i, float& out) {
    double v = 0.0, scale = 1.0;
    bool   digits = false, frac = false;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c >= '0' && c <= '9') {
            digits = true;
            if (frac) { scale *= 0.1; v += (c - '0') * scale; }
            else      v = v * 10.0 + (c - '0');
        } else if (c == '.' && !frac) {
            frac = true;
        } else {
            break;
        }
    }
    out = static_cast<float>(v);
    return digits;
}

bool readInt(const std::string& s, std::size_t& i, int& out) {
    const std::size_t start = i;
    long v = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9' && v < 1000000000L)
        v = v * 10 + (s[i++] - '0');
    out = static_cast<int>(v);
    return i > start;
}

} // namespace

Tree::Tree() {
    m_nodes.push_back(Node{});
    m_nodes[0].id = m_nextId++;
    m_root = 0;
}

bool  Tree::isLeaf(int n) const  { return m_nodes[n].a < 0; }
int   Tree::first(int n) const   { return m_nodes[n].a; }
int   Tree::second(int n) const  { return m_nodes[n].b; }
int   Tree::parent(int n) const  { return m_nodes[n].parent; }
Cut   Tree::cut(int n) const     { return m_nodes[n].cut; }
float Tree::ratio(int n) const   { return m_nodes[n].ratio; }
void  Tree::setRatio(int n, float r) { m_nodes[n].ratio = std::clamp(r, 0.02f, 0.98f); }
int   Tree::id(int leaf) const   { return m_nodes[leaf].id; }
const Rect& Tree::rect(int n) const { return m_nodes[n].rect; }

const std::string& Tree::editor(int leaf) const {
    return isLeaf(leaf) ? m_nodes[leaf].editor : kNone;
}

void Tree::setEditor(int leaf, const std::string& title) {
    if (isLeaf(leaf)) m_nodes[leaf].editor = title;
}

std::vector<int> Tree::leaves() const {
    std::vector<int> out, stack{m_root};
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        if (isLeaf(n)) { out.push_back(n); continue; }
        stack.push_back(m_nodes[n].b);   // second popped after first
        stack.push_back(m_nodes[n].a);
    }
    return out;
}

std::vector<int> Tree::cuts() const {
    std::vector<int> out, stack{m_root};
    while (!stack.empty()) {
        const int n = stack.back();
        stack.pop_back();
        if (isLeaf(n)) continue;
        out.push_back(n);
        stack.push_back(m_nodes[n].b);
        stack.push_back(m_nodes[n].a);
    }
    return out;
}

int Tree::leafById(int areaId) const {
    for (int n : leaves())
        if (m_nodes[n].id == areaId) return n;
    return -1;
}

int Tree::findEditor(const std::string& title) const {
    if (title.empty()) return -1;
    for (int n : leaves())
        if (m_nodes[n].editor == title) return n;
    return -1;
}

int Tree::areaCount() const { return static_cast<int>(leaves().size()); }

int Tree::newNode() {
    m_nodes.push_back(Node{});
    return static_cast<int>(m_nodes.size()) - 1;
}

int Tree::split(int leaf, Cut how, float r) {
    if (!isLeaf(leaf)) return -1;
    // The leaf turns into the cut, in place -- so its parent's link to it stays
    // right -- and its contents move down into a new first child.
    const int keep  = newNode();
    const int fresh = newNode();
    Node& k = m_nodes[keep];
    Node& f = m_nodes[fresh];
    Node& c = m_nodes[leaf];
    k.parent = leaf;
    k.id     = c.id;
    k.editor = std::move(c.editor);
    k.rect   = c.rect;
    f.parent = leaf;
    f.id     = m_nextId++;
    c.editor.clear();
    c.id    = 0;
    c.a     = keep;
    c.b     = fresh;
    c.cut   = how;
    c.ratio = std::clamp(r, 0.02f, 0.98f);
    return fresh;
}

bool Tree::join(int leaf, std::string* droppedEditor) {
    if (!isLeaf(leaf)) return false;
    const int p = m_nodes[leaf].parent;
    if (p < 0) return false;                       // the last area stays
    const int sib = m_nodes[p].a == leaf ? m_nodes[p].b : m_nodes[p].a;
    const int gp  = m_nodes[p].parent;

    if (droppedEditor) *droppedEditor = m_nodes[leaf].editor;

    // The sibling takes the cut's place: in the grandparent's link, or as root.
    m_nodes[sib].parent = gp;
    m_nodes[sib].rect   = m_nodes[p].rect;
    if (gp < 0) m_root = sib;
    else if (m_nodes[gp].a == p) m_nodes[gp].a = sib;
    else                         m_nodes[gp].b = sib;

    m_nodes[leaf].alive = false;
    m_nodes[p].alive    = false;
    m_nodes[leaf].editor.clear();
    return true;
}

void Tree::layout(const Rect& r, float gap, float minSize) {
    m_gap = gap;
    layoutNode(m_root, r, gap, minSize);
}

void Tree::layoutNode(int n, const Rect& r, float gap, float minSize) {
    m_nodes[n].rect = r;
    if (isLeaf(n)) return;
    const Node& c = m_nodes[n];
    const bool  cols   = c.cut == Cut::Columns;
    const float extent = std::max(0.0f, (cols ? r.w : r.h) - gap);
    float len = extent * c.ratio;
    if (extent >= 2.0f * minSize) len = std::clamp(len, minSize, extent - minSize);
    len = std::floor(len + 0.5f);
    Rect ra = r, rb = r;
    if (cols) {
        ra.w = len;
        rb.x = r.x + len + gap;
        rb.w = std::max(0.0f, r.w - len - gap);
    } else {
        ra.h = len;
        rb.y = r.y + len + gap;
        rb.h = std::max(0.0f, r.h - len - gap);
    }
    const int a = c.a, b = c.b;
    layoutNode(a, ra, gap, minSize);
    layoutNode(b, rb, gap, minSize);
}

Rect Tree::divider(int n) const {
    const Node& c = m_nodes[n];
    const Rect& a = m_nodes[c.a].rect;
    if (c.cut == Cut::Columns) return {a.x + a.w, c.rect.y, m_gap, c.rect.h};
    return {c.rect.x, a.y + a.h, c.rect.w, m_gap};
}

// --- Text form ---------------------------------------------------------------

void Tree::writeNode(int n, std::string& out) const {
    const Node& c = m_nodes[n];
    if (isLeaf(n)) {
        out += 'A';
        out += std::to_string(c.id);
        out += '[';
        out += c.editor;
        out += ']';
        return;
    }
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d.%04d", 0,
                  static_cast<int>(std::lround(std::clamp(c.ratio, 0.0f, 0.9999f) * 10000.0f)));
    out += c.cut == Cut::Columns ? 'C' : 'R';
    out += buf;
    out += '(';
    writeNode(c.a, out);
    out += ',';
    writeNode(c.b, out);
    out += ')';
}

std::string Tree::save() const {
    std::string out;
    writeNode(m_root, out);
    return out;
}

int Tree::readNode(const std::string& s, std::size_t& i, int parent, int& maxId) {
    if (i >= s.size()) return -1;
    const char kind = s[i++];
    if (kind == 'A') {
        int areaId = 0;
        if (!readInt(s, i, areaId) || areaId <= 0) return -1;
        if (i >= s.size() || s[i] != '[') return -1;
        const std::size_t close = s.find(']', ++i);
        if (close == std::string::npos) return -1;
        const int n = newNode();
        m_nodes[n].parent = parent;
        m_nodes[n].id     = areaId;
        m_nodes[n].editor = s.substr(i, close - i);
        i = close + 1;
        maxId = std::max(maxId, areaId);
        return n;
    }
    if (kind != 'C' && kind != 'R') return -1;
    float r = 0.5f;
    if (!readRatio(s, i, r)) return -1;
    if (i >= s.size() || s[i++] != '(') return -1;
    const int n = newNode();
    m_nodes[n].parent = parent;
    m_nodes[n].cut    = kind == 'C' ? Cut::Columns : Cut::Rows;
    m_nodes[n].ratio  = std::clamp(r, 0.02f, 0.98f);
    const int a = readNode(s, i, n, maxId);
    if (a < 0 || i >= s.size() || s[i++] != ',') return -1;
    const int b = readNode(s, i, n, maxId);
    if (b < 0 || i >= s.size() || s[i++] != ')') return -1;
    m_nodes[n].a = a;
    m_nodes[n].b = b;
    return n;
}

bool Tree::load(const std::string& text) {
    Tree t;
    t.m_nodes.clear();
    std::size_t i = 0;
    int maxId = 0;
    const int root = t.readNode(text, i, -1, maxId);
    if (root < 0 || i != text.size()) return false;
    // Two areas with one id would share a dock node -- and one editor would
    // vanish behind the other. Never written by save(), but imgui.ini is a text
    // file people edit.
    std::vector<int> ids;
    for (int n : t.leaves()) ids.push_back(t.m_nodes[n].id);
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) return false;
    // One window cannot sit in two areas: the later one comes up empty.
    for (int n : t.leaves())
        if (t.findEditor(t.m_nodes[n].editor) != n) t.m_nodes[n].editor.clear();
    t.m_root   = root;
    t.m_nextId = maxId + 1;
    *this = std::move(t);
    return true;
}

} // namespace areas
