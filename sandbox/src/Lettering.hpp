#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "PolyFill.hpp"

// Text as shapes, from any TrueType or OpenType font on the machine: lines laid
// out in millimetres, every glyph's outline (curves cut fine enough that a
// printed letter shows no facets) filled once and kept, so a card that is
// rebuilt on every key press does not cut the same "e" twice.
//
// A glyph that fills cleanly (PolyFill::fill, sealed) is triangles over its own
// corners -- walled round its outline it is a closed solid. One whose outlines
// overlap (some fonts draw an "x" as two strokes) falls back to trapezoids.
//
// The font is read through a private copy of stb_truetype, so this stands on its
// own in the player too. Pure data: no GPU, no ImGui.
namespace lettering {

struct Font;

// The font at `path`, read on first use and kept; null when it is no font.
Font* load(const std::string& path);

// A plain sans-serif the system has -- DejaVu Sans, Liberation Sans, Noto Sans,
// Arial, Segoe UI -- or "" when there is none of them.
std::string defaultFontPath();

// One glyph, font units, y up from the baseline.
struct Shape {
    polyfill::Fill              fill;    // when it filled cleanly
    std::vector<polyfill::Trap> traps;   // otherwise
    bool fellBack = false;
};

// Saved as an index: append only.
enum class Align { Left, Centre, Right, Count };
const char* alignName(Align a);

struct Style {
    float capHeight = 3.5f;   // a capital's height (mm)
    float headline  = 1.0f;   // the first line this much larger
    float spacing   = 1.0f;   // line distance, times the font's own
    Align align     = Align::Left;
};

// A glyph in place: millimetres = at + scale * font units.
struct Placed {
    const Shape* shape = nullptr;
    double       scale = 0.0;
    glm::dvec2   at{0.0};
};

// A block of text in millimetres. Its box runs from x 0 to `width` and from y 0
// -- the top of the first line's capitals -- down to -`height`, the last line's
// baseline: what the eye aligns, descenders hanging below it.
struct Block {
    std::vector<Placed> glyphs;
    double width = 0.0, height = 0.0;
    double descent = 0.0;   // how far the last line's descenders reach below (mm)
    int    missing = 0;     // characters the font does not have
    int    fellBack = 0;    // glyphs made of trapezoids
};

Block layout(Font& f, const std::string& utf8, const Style& s);

// The glyph for a character (null when the font has none), and its outlines
// as read (font units). For the check.
const Shape* shapeOf(Font& f, char32_t c);
std::vector<std::vector<polyfill::DV>> outlinesOf(Font& f, char32_t c);

} // namespace lettering
