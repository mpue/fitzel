// imagecheck -- the image editor's core (ImageDoc) without a window.
//
// Every operation the Image editor window offers, run on small synthetic
// pictures and checked by numbers: strokes write absolutely (the tremor test),
// undo/redo restore exactly and share memory, blend modes composite as the spec
// says, selections confine paint, transforms keep pixels, filters stay in range,
// a tileable result really wraps, files survive a round trip, text renders.
//
//   imagecheck [outDir]      (writes a few PNGs there to look at; default: temp)

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/ImageDoc.hpp"

namespace fs = std::filesystem;
using namespace img;

static int g_fail = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) { ++g_fail; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
                       std::printf(__VA_ARGS__); std::printf("\n"); }      \
    } while (0)

static const std::uint8_t* at(const Document& d, int layer, int x, int y) {
    return d.pixels(layer).data() + (std::size_t(y) * d.width() + x) * 4;
}

static int diff(const Pixels& a, const Pixels& b) {
    if (a.size() != b.size()) return 9999;
    int m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    return m;
}

int main(int argc, char** argv) {
    const fs::path out = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "imagecheck";
    fs::create_directories(out);

    // --- The tremor test: a stroke is a function of where it went -----------------
    {
        Document a, b;
        a.create(64, 64, {1, 1, 1, 1});
        b.create(64, 64, {1, 1, 1, 1});
        Brush br; br.radius = 6; br.hardness = 0.5f; br.opacity = 0.5f;
        Stroke s;
        s.begin(a, PaintMode::Paint, {0, 0, 0, 1}, br);
        s.to(10, 32); s.to(54, 32);
        s.end();
        // The same path, but the hand shakes back and forth over it twenty times.
        s.begin(b, PaintMode::Paint, {0, 0, 0, 1}, br);
        s.to(10, 32);
        for (int k = 0; k < 20; ++k) { s.to(54, 32.4f); s.to(10, 31.6f); }
        s.to(54, 32);
        s.end();
        const int mid = at(a, 0, 32, 32)[0];
        CHECK(std::abs(mid - 128) <= 2, "half-opacity stroke centre should be ~128, is %d", mid);
        CHECK(std::abs(int(at(b, 0, 32, 32)[0]) - mid) <= 2,
              "twenty passes should leave what one leaves: %d vs %d", at(b, 0, 32, 32)[0], mid);
        CHECK(a.canUndo() && a.undoLabel() == "Brush", "a stroke is one undo step (%s)", a.undoLabel().c_str());
        a.undo();
        CHECK(at(a, 0, 32, 32)[0] == 255, "undo puts the white back");
        a.redo();
        CHECK(at(a, 0, 32, 32)[0] == mid, "redo puts the stroke back");
        std::printf("[imagecheck] stroke: centre %d, after 20 shaky passes %d\n", mid, at(b, 0, 32, 32)[0]);
    }

    // --- Undo shares pixels; a cancelled edit leaves no step --------------------------
    {
        Document d;
        d.create(32, 32, {0, 0, 0, 0});
        d.addLayer("A");
        d.addLayer("B");
        CHECK(d.layerCount() == 3 && d.active() == 2, "two new layers on top (%d, active %d)", d.layerCount(), d.active());
        const std::uint8_t* before = d.pixels(0).data();
        d.fillSelection({1, 0, 0, 1});   // on B
        CHECK(d.pixels(0).data() == before, "painting B does not copy the background");
        Stroke s;
        s.begin(d, PaintMode::Paint, {0, 1, 0, 1}, Brush{});
        s.end();  // never moved: nothing painted
        CHECK(d.undoLabel() == "Fill", "an empty stroke leaves no undo step (%s)", d.undoLabel().c_str());
        int applied = 0;
        auto hist = d.history(applied);
        CHECK(applied == 3 && hist.size() == 3, "history: 3 applied (%d of %zu)", applied, hist.size());
        d.jumpTo(1);
        CHECK(d.layerCount() == 2, "jump back to after the first new layer (%d layers)", d.layerCount());
        d.jumpTo(3);
        CHECK(d.layerCount() == 3 && at(d, 2, 5, 5)[0] == 255, "and forward again");
        CHECK(d.modified(), "a changed document is modified");
    }

    // --- Blend modes ----------------------------------------------------------------
    {
        Document d;
        d.create(4, 4, {0.5f, 0.5f, 0.5f, 1});
        d.addLayer();
        d.fillSelection({0.5f, 0.25f, 1.0f, 1});
        auto px = [&](Blend b) {
            d.setBlend(1, b);
            Pixels f = d.flattened();
            return std::array<int, 3>{f[0], f[1], f[2]};
        };
        auto m = px(Blend::Multiply);
        CHECK(std::abs(m[0] - 64) <= 1 && std::abs(m[1] - 32) <= 1 && std::abs(m[2] - 128) <= 1,
              "multiply %d %d %d", m[0], m[1], m[2]);
        auto sc = px(Blend::Screen);
        CHECK(std::abs(sc[0] - 191) <= 1 && std::abs(sc[2] - 255) <= 1, "screen %d %d", sc[0], sc[2]);
        auto df = px(Blend::Difference);
        CHECK(df[0] <= 1 && std::abs(df[2] - 127) <= 1, "difference %d %d", df[0], df[2]);
        d.setBlend(1, Blend::Normal);
        d.setOpacity(1, 0.5f);
        d.setOpacity(1, 0.25f);
        CHECK(d.undoLabel() == "Layer opacity", "opacity steps fold into one (%s)", d.undoLabel().c_str());
        d.undo();
        CHECK(d.layer(1).opacity == 1.0f, "one undo takes all the opacity clicks back (%.2f)", d.layer(1).opacity);
        d.setOpacity(1, 0.5f);
        d.mergeDown();
        CHECK(d.layerCount() == 1, "merged");
        CHECK(std::abs(int(at(d, 0, 1, 1)[0]) - 128) <= 1 && std::abs(int(at(d, 0, 1, 1)[2]) - 191) <= 1,
              "merge keeps the look: %d %d", at(d, 0, 1, 1)[0], at(d, 0, 1, 1)[2]);
    }

    // --- Selections confine paint; move carries the selection -------------------------
    {
        Document d;
        d.create(40, 40, {0, 0, 0, 1});
        d.selectRect({10, 10, 20, 20}, SelOp::Replace, false);
        d.fillSelection({1, 1, 1, 1});
        CHECK(at(d, 0, 15, 15)[0] == 255 && at(d, 0, 5, 5)[0] == 0, "fill stays inside the selection");
        d.selectRect({15, 10, 30, 20}, SelOp::Subtract, false);
        CHECK(d.selectionBounds().x1 == 15, "subtract trims the box (x1 %d)", d.selectionBounds().x1);
        d.selectRect({10, 10, 20, 20}, SelOp::Replace, false);
        d.moveSelected(5, 0);
        CHECK(at(d, 0, 24, 15)[0] == 255 && at(d, 0, 12, 15)[3] == 0, "move lifts and sets down (%d, a %d)",
              at(d, 0, 24, 15)[0], at(d, 0, 12, 15)[3]);
        CHECK(d.selectionBounds().x0 == 15, "the selection moves with it (%d)", d.selectionBounds().x0);
        d.deselect();
        d.selectColor(1, 1, 0.1f, true, false, SelOp::Replace);
        CHECK(d.hasSelection() && d.selectionBounds().w() == 40, "wand takes the black around");
        d.invertSelection();
        CHECK(d.selectionBounds().x0 >= 10, "and its inverse is the moved square plus the hole");
        d.selectRect({0, 0, 40, 40}, SelOp::Replace, true);
        CHECK(d.selAt(0) == 0 && d.selAt(20 * 40 + 20) == 255, "an ellipse leaves the corners out");
    }

    // --- Transforms -------------------------------------------------------------------
    {
        Document d;
        d.create(30, 20, {0, 0, 1, 1});
        d.pixelsMut(0)[0] = 255;   // top-left pixel red-ish
        d.rotate90(true);
        CHECK(d.width() == 20 && d.height() == 30, "rotate swaps the sides");
        CHECK(at(d, 0, 19, 0)[0] == 255, "top-left goes to the top right turning right");
        d.rotate90(false);
        CHECK(at(d, 0, 0, 0)[0] == 255, "and back");
        d.flipImage(true);
        CHECK(at(d, 0, 29, 0)[0] == 255, "flip horizontal");
        d.resize(60, 40);
        CHECK(d.width() == 60 && at(d, 0, 30, 20)[2] == 255, "resize keeps the colour of flat areas");
        d.canvasSize(80, 40, 1, 1);
        CHECK(d.width() == 80 && at(d, 0, 2, 20)[3] == 0 && at(d, 0, 40, 20)[3] == 255, "canvas grows around the middle");
        d.crop({10, 0, 70, 40});
        CHECK(d.width() == 60 && at(d, 0, 0, 20)[3] == 255, "crop");
        const auto shapes = d.shapeVersion();
        d.undo();
        CHECK(d.width() == 80 && d.shapeVersion() != shapes, "undo of a crop changes the size back");
    }

    // --- Filters ----------------------------------------------------------------------
    {
        Document d;
        const int W = 64, H = 48;
        d.create(W, H, {0, 0, 0, 1});
        std::uint8_t* p = d.pixelsMut(0);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                std::uint8_t* q = p + (std::size_t(y) * W + x) * 4;
                q[0] = std::uint8_t(x * 4); q[1] = std::uint8_t(y * 5); q[2] = std::uint8_t((x * y) / 12);
            }
        const Pixels orig = d.pixels(0);
        for (int f = 0; f < int(Filter::Count); ++f) {
            const FilterInfo& fi = filterInfo(Filter(f));
            Pixels o;
            applyFilter(Filter(f), fi.def, orig, o, W, H, nullptr, 7);
            CHECK(o.size() == orig.size(), "%s keeps the size", fi.name);
            const bool identity = std::string(fi.group) == "Adjust" && fi.params > 0 && Filter(f) != Filter::Levels;
            if (identity || Filter(f) == Filter::Levels)
                CHECK(diff(o, orig) <= 1, "%s at its defaults changes nothing (max diff %d)", fi.name, diff(o, orig));
        }
        // Live preview: changing the parameter recomputes from the base.
        d.beginEdit("Blur");
        float big[3] = {20, 0, 0}, small[3] = {1, 0, 0};
        d.filter(Filter::Blur, big);
        d.filter(Filter::Blur, small);
        Pixels once;
        applyFilter(Filter::Blur, small, orig, once, W, H, nullptr);
        CHECK(diff(d.pixels(0), once) == 0, "a preview is redone from the base, not stacked");
        d.cancelEdit();
        CHECK(diff(d.pixels(0), orig) == 0 && !d.canUndo(), "cancel restores exactly and leaves no step");

        // Tileable: the left and right columns must meet like neighbours.
        float t[3] = {35, 0, 0};
        Pixels tile;
        applyFilter(Filter::Tileable, t, orig, tile, W, H, nullptr);
        int seam = 0, inner = 0;
        for (int y = 0; y < H; ++y)
            for (int c = 0; c < 3; ++c) {
                seam = std::max(seam, std::abs(int(tile[(std::size_t(y) * W + W - 1) * 4 + c]) - int(tile[(std::size_t(y) * W) * 4 + c])));
                inner = std::max(inner, std::abs(int(orig[(std::size_t(y) * W + W - 1) * 4 + c]) - int(orig[(std::size_t(y) * W) * 4 + c])));
            }
        CHECK(seam < 24, "tileable wraps: seam step %d (was %d)", seam, inner);
        std::printf("[imagecheck] tileable seam step %d (before %d)\n", seam, inner);

        // Normal map of a bump: left of it points left, below it points down.
        Pixels bump(std::size_t(W) * H * 4, 0), nm;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float r = std::hypot(x - 32.0f, y - 24.0f);
                const std::uint8_t v = std::uint8_t(std::max(0.0f, 255.0f - r * 12.0f));
                std::uint8_t* q = &bump[(std::size_t(y) * W + x) * 4];
                q[0] = q[1] = q[2] = v; q[3] = 255;
            }
        float nmp[3] = {6, 0, 0};
        applyFilter(Filter::NormalMap, nmp, bump, nm, W, H, nullptr);
        CHECK(nm[(std::size_t(24) * W + 26) * 4 + 0] < 110, "left slope points left (r %d)", nm[(std::size_t(24) * W + 26) * 4]);
        CHECK(nm[(std::size_t(30) * W + 32) * 4 + 1] < 110, "lower slope points down, green up (g %d)", nm[(std::size_t(30) * W + 32) * 4 + 1]);
        stbi_write_png((out / "normal.png").string().c_str(), W, H, 4, nm.data(), W * 4);
        stbi_write_png((out / "tile.png").string().c_str(), W, H, 4, tile.data(), W * 4);
    }

    // --- Fill, shapes, gradient, clone ----------------------------------------------------
    {
        Document d;
        d.create(50, 50, {1, 1, 1, 1});
        Stroke s;
        s.begin(d, PaintMode::Paint, {0, 0, 0, 1}, Brush{}, 0, 0, "Rectangle");
        s.shape(Stroke::Shape::Rect, 10, 10, 40, 40, 2);
        s.shape(Stroke::Shape::Ellipse, 5, 5, 45, 45, 2);   // the preview moved on
        s.shape(Stroke::Shape::Rect, 10, 10, 40, 40, 2);    // ... and back
        s.end();
        CHECK(at(d, 0, 10, 25)[0] < 40 && at(d, 0, 25, 25)[0] == 255, "rectangle outline (%d / %d)", at(d, 0, 10, 25)[0], at(d, 0, 25, 25)[0]);
        CHECK(at(d, 0, 5, 25)[0] == 255, "the ellipse it passed through is gone (%d)", at(d, 0, 5, 25)[0]);
        d.floodFill(25, 25, {1, 0, 0, 1}, 0.1f, true, false, 1.0f);
        CHECK(at(d, 0, 25, 25)[1] == 0 && at(d, 0, 2, 2)[1] == 255, "bucket fills inside the outline only");
        d.beginEdit("Gradient");
        gradient(d, 0, 0, 50, 0, {0, 0, 0, 1}, {1, 1, 1, 1}, false, 1.0f);
        d.endEdit();
        CHECK(at(d, 0, 1, 1)[0] < 15 && at(d, 0, 48, 1)[0] > 240, "gradient runs black to white");
        Document c;
        c.create(20, 20, {0, 0, 0, 1});
        c.pixelsMut(0)[(std::size_t(5) * 20 + 5) * 4] = 255;
        Brush cb; cb.radius = 2; cb.hardness = 1;
        s.begin(c, PaintMode::Clone, {}, cb, -10, -10);
        s.to(15, 15);   // samples (5, 5)
        s.end();
        CHECK(at(c, 0, 15, 15)[0] == 255, "clone stamp copies from the offset (%d)", at(c, 0, 15, 15)[0]);
        s.begin(c, PaintMode::Erase, {}, cb);
        s.to(15.5f, 15.5f);
        s.end();
        CHECK(at(c, 0, 15, 15)[3] == 0, "eraser clears to transparent");
    }

    // --- Copy / paste ---------------------------------------------------------------------
    {
        Document d;
        d.create(30, 30, {0, 1, 0, 1});
        d.selectRect({5, 5, 15, 10}, SelOp::Replace, false);
        Clip c = d.copy(false);
        CHECK(c.valid() && c.w == 10 && c.h == 5 && c.x == 5, "copy takes the selection's box");
        d.paste(c);
        CHECK(d.layerCount() == 2 && at(d, 1, 6, 6)[3] == 255 && at(d, 1, 20, 20)[3] == 0, "paste lands in place on a new layer");
    }

    // --- Files ---------------------------------------------------------------------------
    {
        Document d;
        d.create(17, 9, {0.2f, 0.4f, 0.6f, 0.5f});
        std::string err;
        const std::string png = (out / "round.png").string(), jpg = (out / "round.jpg").string();
        CHECK(d.save(png, err), "save png: %s", err.c_str());
        CHECK(!d.modified(), "saved means not modified");
        Document e;
        CHECK(e.load(png, err), "load png: %s", err.c_str());
        CHECK(e.width() == 17 && e.height() == 9 && diff(e.pixels(0), d.pixels(0)) == 0, "png round trip is exact");
        CHECK(d.save(jpg, err), "save jpg: %s", err.c_str());
        CHECK(e.load(jpg, err) && at(e, 0, 3, 3)[3] == 255, "jpg loads opaque");
        CHECK(!d.save((out / "x.webp").string(), err), "an unknown format is refused");
        CHECK(!fs::exists(png + ".saving"), "no temp file left behind");
    }

    // --- Text ----------------------------------------------------------------------------
    {
        const auto& fonts = systemFonts();
        std::printf("[imagecheck] %zu system fonts\n", fonts.size());
        CHECK(!fonts.empty(), "the system has fonts");
        std::string arial;
        for (const auto& f : fonts) if (f.name == "Arial") arial = f.path;
        if (arial.empty() && !fonts.empty()) arial = fonts[0].path;
        Pixels t; int w = 0, h = 0;
        CHECK(renderText(arial, "Fitzel\nBild", 48, {1, 0, 0, 1}, t, w, h), "render text with %s", arial.c_str());
        int ink = 0;
        for (std::size_t i = 3; i < t.size(); i += 4) ink += t[i] > 128;
        CHECK(w > 100 && h > 90 && ink > 500, "two lines of 48 px text (%dx%d, %d inked)", w, h, ink);
        if (w > 0) stbi_write_png((out / "text.png").string().c_str(), w, h, 4, t.data(), w * 4);
    }

    std::printf(g_fail ? "[imagecheck] %d FAILED\n" : "[imagecheck] all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
