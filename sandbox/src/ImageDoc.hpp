#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The picture behind the editor's "Image editor" window: a stack of RGBA layers
// with blend modes, a selection, undo -- and the tools that write into it
// (strokes, shapes, fills, gradients, filters, transforms). Pure CPU: no GL, no
// ImGui, so imagecheck can drive every operation without a window.
//
// Pixels are 8-bit straight (not premultiplied) RGBA, top row first, the way
// stb_image hands a file over and stb_image_write takes it back.
//
// Undo is a stack of whole-document snapshots that SHARE pixel buffers: a layer
// holds its pixels by shared_ptr, a snapshot copies the pointer, and the first
// write after a snapshot clones the buffer (pixelsMut). A stroke on one layer of
// five therefore costs one layer's worth of memory, not five.
//
// Two rules from the editor's accessibility goal (parkinson-tauglicher Editor)
// are built into the core rather than the window:
//   * a stroke writes ABSOLUTELY: every pixel remembers the strongest coverage it
//     has had in this stroke and is recomputed from the layer as it was before
//     the stroke -- a trembling hand that passes over the same spot ten times
//     leaves what one pass leaves (see Stroke);
//   * every live edit (shape, gradient, move, filter preview) is a rewrite from
//     that same "before", so dragging a shape around and coming back costs
//     nothing, and cancelling is exact.
namespace img {

using Pixels = std::vector<std::uint8_t>;  // RGBA8, row-major, top row first
using Mask   = std::vector<std::uint8_t>;  // one byte per pixel, 255 = fully in

struct Color {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;  // straight alpha, 0..1
};

// A pixel rectangle, half-open: [x0, x1) x [y0, y1).
struct Rect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    int  w() const { return x1 - x0; }
    int  h() const { return y1 - y0; }
    void add(const Rect& o);                  // union (an empty side is ignored)
    Rect clipped(int w, int h) const;         // to [0,w) x [0,h)
    static Rect around(float x, float y, float r);  // the pixels a disc of radius r touches
};

enum class Blend { Normal, Multiply, Screen, Overlay, SoftLight, Add, Subtract,
                   Darken, Lighten, Difference, Color, Count };
const char* blendName(Blend b);

struct Layer {
    std::string             name;
    std::shared_ptr<Pixels> px;
    float                   opacity = 1.0f;
    Blend                   blend   = Blend::Normal;
    bool                    visible = true;
};

enum class SelOp { Replace, Add, Subtract, Intersect };

// What Copy puts on the clipboard: a piece of a layer (or of the picture as
// seen) and where it came from, so Paste lands it in the same place.
struct Clip {
    int    x = 0, y = 0, w = 0, h = 0;
    Pixels px;
    bool valid() const { return w > 0 && h > 0 && px.size() == std::size_t(w) * h * 4; }
};

// Adjustments and filters: one table so the window can lay out any of them
// (name, up to three numbers with their range and step) without knowing it.
enum class Filter {
    BrightnessContrast, HueSaturation, Levels, ColorBalance, Exposure,
    Blur, Sharpen, Noise, Pixelate, Posterize, Threshold,
    Invert, Desaturate, Tileable, NormalMap, Count
};
struct FilterInfo {
    const char* name;
    const char* group;   // "Adjust" or "Filter" -- which menu
    int         params;  // 0..3; 0 = applies in one click
    const char* label[3];
    float       lo[3], hi[3], def[3], step[3];
    const char* fmt[3];
    const char* tip;
};
const FilterInfo& filterInfo(Filter f);
// `dst` = `src` run through the filter, mixed back into `src` by the selection
// (null = everywhere). `dst` is resized to fit. Deterministic for a given seed.
void applyFilter(Filter f, const float* params, const Pixels& src, Pixels& dst,
                 int w, int h, const Mask* sel, std::uint32_t seed = 1);

class Document {
public:
    // --- Making one ---------------------------------------------------------------
    void create(int w, int h, Color fill);      // one layer, "Background"
    bool load(const std::string& path, std::string& err);
    // Writes the picture as seen (all visible layers) to PNG / JPG / TGA / BMP by
    // the extension. Marks the document saved and remembers the path.
    bool save(const std::string& path, std::string& err, int jpgQuality = 92);
    // A picture that is not a file yet (Paste as new, the harness).
    void adopt(int w, int h, Pixels px, const std::string& name);

    const std::string& path() const { return m_path; }
    std::string        title() const;           // file name, or "Untitled"
    bool               modified() const { return m_version != m_savedVersion; }
    int                width()  const { return m_w; }
    int                height() const { return m_h; }
    // Bumped whenever the canvas changes size -- the window re-creates its texture.
    std::uint32_t      shapeVersion() const { return m_shapeVersion; }

    // --- Layers -------------------------------------------------------------------
    int          layerCount() const { return int(m_layers.size()); }
    const Layer& layer(int i) const { return m_layers[std::size_t(i)]; }
    int          active() const { return m_active; }
    void         setActive(int i);
    // Property edits go through these so they are undoable and repaint. Repeated
    // clicks on the same layer's opacity fold into one undo step.
    void setOpacity(int i, float o);
    void setBlend(int i, Blend b);
    void setVisible(int i, bool v);
    void rename(int i, const std::string& name, bool undoable = true);
    void addLayer(const std::string& name = {});  // empty, above the active one
    void duplicateLayer();
    void deleteLayer();                           // never the last one
    void moveLayer(int dir);                      // +1 = up the stack
    void mergeDown();
    void flatten();

    const Pixels& pixels(int i) const { return *m_layers[std::size_t(i)].px; }
    // The layer's pixels to write into -- cloned first if a snapshot shares them.
    // Call dirty() for what you changed.
    std::uint8_t* pixelsMut(int i);

    // --- The picture as seen --------------------------------------------------------
    // Composites the visible layers over transparency into `out` (w*h*4, same
    // layout as a layer), within `r` only.
    void   composite(const Rect& r, std::uint8_t* out) const;
    Pixels flattened() const;
    // Colour at a pixel (eyedropper): the picture as seen, or the active layer.
    Color  sample(int x, int y, bool merged) const;

    void dirty(const Rect& r) { m_dirty.add(r.clipped(m_w, m_h)); }
    void dirtyAll() { m_dirty = {0, 0, m_w, m_h}; }
    Rect takeDirty() { Rect r = m_dirty; m_dirty = {}; return r; }

    // --- Selection ----------------------------------------------------------------
    bool        hasSelection() const { return m_sel && !m_sel->empty(); }
    const Mask* selection() const { return hasSelection() ? m_sel.get() : nullptr; }
    Rect        selectionBounds() const { return m_selBounds; }
    std::uint32_t selectionVersion() const { return m_selVersion; }
    // How much pixel i is selected, 0..255 (255 everywhere with no selection).
    int         selAt(std::size_t i) const { return hasSelection() ? (*m_sel)[i] : 255; }
    void selectRect(const Rect& r, SelOp op, bool ellipse);
    // Magic wand: the pixels within `tolerance` (0..1) of the colour at (x, y),
    // joined to it (contiguous) or anywhere.
    void selectColor(int x, int y, float tolerance, bool contiguous, bool merged, SelOp op);
    void selectAll();
    void deselect();
    void invertSelection();
    void selectLayerAlpha(SelOp op);              // "select what the layer has painted"

    // --- Undo ---------------------------------------------------------------------
    // Remember the document as it is now under `label` (the action about to
    // happen). Clears redo. `mergeKey` != "" folds into the previous step when
    // that step had the same key and nothing else happened in between.
    void checkpoint(const std::string& label, const std::string& mergeKey = {});
    bool undo();
    bool redo();
    bool canUndo() const { return !m_undo.empty(); }
    bool canRedo() const { return !m_redo.empty(); }
    std::string undoLabel() const { return m_undo.empty() ? std::string() : m_undo.back().label; }
    std::string redoLabel() const { return m_redo.empty() ? std::string() : m_redo.back().label; }
    // The History list: the undo labels oldest first, then the redo labels in the
    // order they would be redone. `applied` = how many of them are in effect.
    std::vector<std::string> history(int& applied) const;
    void jumpTo(int applied);                      // undo/redo until `applied` steps are in

    // --- Live edits of the active layer -------------------------------------------
    // A stroke, shape, gradient, move or filter preview: checkpoint, then keep the
    // layer as it was ("base") to recompute from. endEdit keeps the result,
    // cancelEdit puts the base back and forgets the step.
    void          beginEdit(const std::string& label);
    bool          editing() const { return m_editing; }
    const Pixels& editBase() const { return *m_base; }
    void          restoreFromBase(const Rect& r);
    void          endEdit();
    void          cancelEdit();

    // --- Whole-picture operations (each one undo step) ------------------------------
    void resize(int w, int h);                     // resample every layer
    void canvasSize(int w, int h, int anchorX, int anchorY);  // anchor 0/1/2 = left/centre/right
    void crop(const Rect& r);
    void rotate90(bool clockwise);
    void rotate180();
    void flipImage(bool horizontal);
    void flipLayer(bool horizontal);

    // Move the active layer's selected pixels (the whole layer with no selection)
    // by (dx, dy), the selection with them. Inside a live edit ("Move") it is
    // recomputed from the base, so a drag can call it every frame.
    void moveSelected(int dx, int dy);
    void offsetLayer(int dx, int dy);              // wrapping -- for checking a tile's seams

    // Fill the selection (everything with none) of the active layer.
    void fillSelection(Color c);
    void clearSelection();                         // to transparent
    // Paint bucket: the pixels near the clicked colour, filled with `c`.
    void floodFill(int x, int y, Color c, float tolerance, bool contiguous, bool merged,
                   float opacity);
    // Filter the active layer (inside the selection). With a live edit open it
    // is recomputed from the edit's base -- the preview path.
    void filter(Filter f, const float* params, std::uint32_t seed = 1);

    Clip copy(bool merged) const;                  // the selection's box (or all)
    void paste(const Clip& c, const std::string& name = "Pasted");  // a new layer

    // A new layer holding `px` (w x h) placed with its top left at (x, y), above
    // the active one. Not an undo step of its own -- the caller checkpoints.
    void insertLayer(const std::string& name, const Pixels& px, int w, int h, int x, int y);
    // Rewrite layer i as `px` (w x h) at (x, y) on transparency (live text).
    void placeOnLayer(int i, const Pixels& px, int w, int h, int x, int y);

private:
    struct State {
        int w = 0, h = 0;
        std::vector<Layer>    layers;
        int                   active = 0;
        std::shared_ptr<Mask> sel;
        Rect                  selBounds;
        std::uint32_t         version = 0;
        std::string           label, mergeKey;
    };
    State capture() const;
    void  restore(const State& s);
    void  setSelection(std::shared_ptr<Mask> m);   // recomputes bounds; null/empty = none
    void  combineSelection(Mask&& m, SelOp op);
    void  trimUndo();
    void  touched() { m_version = ++m_counter; }
    void  reshaped(int w, int h);                  // new canvas size

    int m_w = 0, m_h = 0;
    std::vector<Layer>    m_layers;
    int                   m_active = 0;
    std::shared_ptr<Mask> m_sel;
    Rect                  m_selBounds;
    std::uint32_t         m_selVersion = 1;
    std::string           m_path;
    std::uint32_t         m_version = 0, m_savedVersion = 0, m_counter = 0;
    std::uint32_t         m_shapeVersion = 1;
    Rect                  m_dirty;
    std::vector<State>    m_undo, m_redo;
    std::string           m_lastKey;               // merge key of the newest step
    bool                  m_editing = false;
    std::shared_ptr<Pixels> m_base;
    std::shared_ptr<Mask>   m_baseSel;             // the selection a Move started with
};

// --- Painting ----------------------------------------------------------------------

enum class PaintMode { Paint, Erase, Clone };

struct Brush {
    float radius   = 8.0f;   // pixels
    float hardness = 0.8f;   // 0 = all falloff, 1 = a hard disc
    float opacity  = 1.0f;   // the most one stroke can lay down
    float spacing  = 0.2f;   // dab distance, fraction of the radius
};

// One stroke of the brush, eraser or clone stamp on the document's active layer
// (inside its selection). Coverage is kept per pixel as the MAXIMUM any dab has
// given it, and the pixel recomputed from the layer as it was -- so the result
// is a function of where the stroke went, not of how long it stayed there.
class Stroke {
public:
    void begin(Document& d, PaintMode mode, Color c, const Brush& b,
               float cloneDx = 0.0f, float cloneDy = 0.0f, const char* label = nullptr);
    void to(float x, float y);       // dab along from the last point (the first call: one dab)
    void end();                      // keep it (one undo step)
    void cancel();
    bool active() const { return m_doc != nullptr; }

    // Shapes and lines through the same machinery: coverage from a distance field
    // instead of dabs, rewritten whole on every call (for the live preview).
    enum class Shape { Line, Rect, RectFill, Ellipse, EllipseFill };
    void shape(Shape s, float x0, float y0, float x1, float y1, float width);

private:
    void dab(float x, float y);
    void shade(const Rect& r);
    void clearCoverage();

    Document*  m_doc = nullptr;
    PaintMode  m_mode = PaintMode::Paint;
    Color      m_color;
    Brush      m_brush;
    float      m_cdx = 0.0f, m_cdy = 0.0f;
    Mask       m_cov;
    Rect       m_touched;
    float      m_lx = 0.0f, m_ly = 0.0f, m_carry = 0.0f;
    bool       m_first = true;
};

// A gradient across the active layer (inside the selection), rewritten from the
// edit's base on every call: open a live edit first (beginEdit("Gradient")).
void gradient(Document& d, float x0, float y0, float x1, float y1, Color a, Color b,
              bool radial, float opacity);

// --- Text -----------------------------------------------------------------------------

// The TrueType fonts the system has (name + file), for the Text tool's list.
struct FontFile { std::string name, path; };
const std::vector<FontFile>& systemFonts();
// `text` (UTF-8, '\n' breaks lines) set in `fontPath` at `sizePx`, as RGBA in
// colour `c` with the glyphs' coverage as alpha. False when the font cannot be read.
bool renderText(const std::string& fontPath, const std::string& text, float sizePx, Color c,
                Pixels& out, int& w, int& h);

// --- Small helpers shared with the window ---------------------------------------------
void        toHsv(const Color& c, float& h, float& s, float& v);
Color       fromHsv(float h, float s, float v, float a);
std::string hexOf(const Color& c);
bool        isImageFile(const std::string& path);   // the extensions load() reads

} // namespace img
