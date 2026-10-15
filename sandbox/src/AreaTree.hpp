#pragma once

#include <string>
#include <vector>

// The editor's screen, cut into areas the way Blender cuts it: a binary tree
// whose leaves are areas and whose inner nodes are cuts. Every area shows ONE
// editor (or none); splitting an area cuts it in two, joining one gives its
// space back to the neighbour it was cut from.
//
// This file is only the bookkeeping -- the tree, its rectangles and its text
// form. Drawing it, the header with the editor menu and the dividers you drag
// are Areas.cpp. Kept apart so the tree can be checked without a window
// (areacheck): an area that silently drops its editor on a join, or a layout
// that comes back from imgui.ini with a different shape, is not something you
// notice until the panel you wanted is gone.
namespace areas {

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// Which way a cut runs. Columns: the two halves sit side by side (the cut is
// vertical). Rows: one above the other (the cut is horizontal).
enum class Cut { Columns, Rows };

class Tree {
public:
    // One area filling the screen, showing nothing.
    Tree();

    // --- Structure ---------------------------------------------------------
    int  root() const { return m_root; }
    bool isLeaf(int n) const;
    int  first(int n) const;            // left / top child of a cut
    int  second(int n) const;           // right / bottom child of a cut
    int  parent(int n) const;
    Cut  cut(int n) const;
    float ratio(int n) const;           // share of the first child, 0..1
    void  setRatio(int n, float r);

    // Every area, in reading order (left to right, top to bottom per cut).
    std::vector<int> leaves() const;
    // Every cut, parents before children.
    std::vector<int> cuts() const;

    // --- Areas -------------------------------------------------------------
    // An area's id: stable across splits, joins and save/load, so whatever is
    // keyed on it (the dock node its editor sits in) survives them. Node
    // indices do not.
    int id(int leaf) const;
    int leafById(int id) const;          // -1 if none

    // The editor an area shows, by its window title; empty = none.
    const std::string& editor(int leaf) const;
    void setEditor(int leaf, const std::string& title);
    // The area showing `title`, or -1.
    int findEditor(const std::string& title) const;

    // Cut `leaf` in two. It keeps its editor and its id in the FIRST half
    // (left / top); the second half is a new, empty area, whose leaf index is
    // returned. `ratio` is the first half's share.
    int split(int leaf, Cut how, float ratio = 0.5f);

    // Remove `leaf`: the neighbour it was cut from takes its whole space.
    // `droppedEditor` gets the title the area was showing, so the caller can
    // close that window. False (and nothing changes) for the last area.
    bool join(int leaf, std::string* droppedEditor = nullptr);

    int areaCount() const;

    // --- Geometry ----------------------------------------------------------
    // Lay every node out inside `r`, with `gap` pixels between the halves of a
    // cut. A half is never squeezed below `minSize` while the cut has room for
    // both; a window too small for that gets proportional halves instead.
    void layout(const Rect& r, float gap, float minSize);
    const Rect& rect(int n) const;
    // The strip between the two halves of cut `n` (the divider you drag).
    Rect divider(int n) const;

    // --- Text form ---------------------------------------------------------
    // One line, for imgui.ini. A cut is C|R ratio ( first , second ), an area
    // is A id [title]. Window titles may contain anything but ']'.
    std::string save() const;
    // Replace this tree with the one in `text`. On a malformed string the
    // tree is left as it was and false is returned.
    bool load(const std::string& text);

private:
    struct Node {
        int         parent = -1;
        int         a = -1, b = -1;     // children; -1 on a leaf
        Cut         cut = Cut::Columns;
        float       ratio = 0.5f;
        int         id = 0;             // leaves only
        std::string editor;             // leaves only
        Rect        rect;
        bool        alive = true;
    };

    int  newNode();
    void writeNode(int n, std::string& out) const;
    int  readNode(const std::string& s, std::size_t& i, int parent, int& maxId);
    void layoutNode(int n, const Rect& r, float gap, float minSize);

    std::vector<Node> m_nodes;
    int m_root   = 0;
    int m_nextId = 1;
    float m_gap  = 0.0f;
};

} // namespace areas
