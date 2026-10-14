// areacheck -- does the editor's area tree keep its editors and its shape?
//
// The editor's screen is a tree of areas (AreaTree.hpp): split one and it keeps
// its editor in one half, join one and its neighbour takes the space. Both are
// easy to get subtly wrong -- a join that hands the space to the wrong subtree,
// a split that loses the area's id (and with it the dock node its window sits
// in), a layout read back from imgui.ini in a different shape -- and every one
// of them looks on screen like "my panel is gone", not like a bug.
//
// So the operations are played out here and checked after each one.
//
// No GL, no window, no ImGui: this is bookkeeping about a tree.
//
//   build/linux/bin/areacheck
// Exits non-zero if any check fails.

#include <cmath>
#include <cstdio>
#include <string>

#include "../src/AreaTree.hpp"

namespace {

int g_fails = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    if (ok) std::printf("  ok   %s%s%s\n", what, detail.empty() ? "" : " -- ", detail.c_str());
    else  { std::printf("[FAIL] %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str()); ++g_fails; }
}

bool near(float a, float b) { return std::fabs(a - b) < 0.51f; }

} // namespace

int main() {
    using namespace areas;

    std::printf("split\n");
    Tree t;
    check(t.areaCount() == 1 && t.isLeaf(t.root()), "starts as one area");
    t.setEditor(t.root(), "Scene");
    const int firstId = t.id(t.root());
    const int right = t.split(t.root(), Cut::Columns, 0.25f);
    check(t.areaCount() == 2, "a split makes two areas");
    const int left = t.first(t.root());
    check(t.editor(left) == "Scene", "the first half keeps the editor");
    check(t.id(left) == firstId, "...and the id", std::to_string(t.id(left)));
    check(t.editor(right).empty(), "the second half starts empty");
    check(t.id(right) != firstId, "...with an id of its own");

    std::printf("layout\n");
    t.layout({0.0f, 0.0f, 1004.0f, 600.0f}, 4.0f, 50.0f);
    check(near(t.rect(left).w, 250.0f), "first half gets its ratio",
          std::to_string(t.rect(left).w));
    check(near(t.rect(right).x, 254.0f) && near(t.rect(right).w, 750.0f),
          "second half starts after the gap and fills the rest");
    const Rect d = t.divider(t.root());
    check(near(d.x, 250.0f) && near(d.w, 4.0f) && near(d.h, 600.0f), "divider sits in the gap");
    t.setRatio(t.root(), 0.0f);
    t.layout({0.0f, 0.0f, 1004.0f, 600.0f}, 4.0f, 50.0f);
    check(t.rect(left).w >= 50.0f, "a half is never squeezed under the minimum",
          std::to_string(t.rect(left).w));
    t.setRatio(t.root(), 0.25f);

    std::printf("nested split and join\n");
    t.setEditor(right, "Inspector");
    const int below = t.split(right, Cut::Rows, 0.4f);
    t.setEditor(below, "Terrain");
    check(t.areaCount() == 3, "three areas");
    check(t.findEditor("Inspector") >= 0 && t.findEditor("Terrain") == below, "editors findable");
    const int scene = t.findEditor("Scene");
    std::string dropped;
    check(t.join(scene, &dropped), "join the viewport away");
    check(dropped == "Scene", "join reports the editor it dropped", dropped);
    check(t.areaCount() == 2, "two areas left");
    check(!t.isLeaf(t.root()) && t.cut(t.root()) == Cut::Rows,
          "the neighbour (the rows cut) takes the root");
    t.layout({0.0f, 0.0f, 1000.0f, 600.0f}, 4.0f, 50.0f);
    const int insp = t.findEditor("Inspector");
    check(insp >= 0 && near(t.rect(insp).w, 1000.0f), "and the whole width");
    const int terr = t.findEditor("Terrain");
    check(t.join(terr) && t.areaCount() == 1, "join down to one area");
    check(!t.join(t.root()), "the last area cannot be joined");
    check(t.editor(t.root()) == "Inspector", "the survivor keeps its editor");

    std::printf("text form\n");
    Tree a;
    a.setEditor(a.root(), "Hierarchy");
    const int c = a.split(a.root(), Cut::Columns, 0.18f);
    a.setEditor(c, "Scene");
    const int r = a.split(c, Cut::Columns, 0.7f);
    a.setEditor(r, "Inspector");
    const int tools = a.split(r, Cut::Rows, 0.38f);
    a.setEditor(tools, "Sky & atmosphere");
    const std::string text = a.save();
    std::printf("       %s\n", text.c_str());
    Tree b;
    check(b.load(text), "reads back what it wrote");
    check(b.save() == text, "...into the same text");
    check(b.leafById(a.id(tools)) >= 0 &&
          b.editor(b.leafById(a.id(tools))) == "Sky & atmosphere",
          "area ids and titles survive");
    const int fresh = b.split(b.leafById(a.id(tools)), Cut::Columns);
    bool unique = true;
    for (int n : b.leaves())
        if (n != fresh && b.id(n) == b.id(fresh)) unique = false;
    check(unique, "a split after loading gets a new id");

    Tree keep;
    keep.setEditor(keep.root(), "Log");
    check(!keep.load("C0.5000(A1[x],"), "malformed text is refused");
    check(!keep.load("C0.5000(A1[x],A1[y])"), "two areas with one id are refused");
    check(keep.editor(keep.root()) == "Log", "...and the tree is left alone");
    Tree dup;
    check(dup.load("R0.5000(A1[Log],A2[Log])"), "a window named twice loads");
    check(dup.findEditor("Log") >= 0 &&
          (dup.editor(dup.first(dup.root())).empty() || dup.editor(dup.second(dup.root())).empty()),
          "...but only one area keeps it");

    std::printf(g_fails ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
