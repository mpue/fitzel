#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include "ImageDoc.hpp"

// The editor's "Image editor" window (Assets menu): a small Photoshop for the
// textures a project is made of. Open a picture (file dialog, the project's
// image list, a file dropped on the window, a screenshot pasted from the
// clipboard), paint on it in layers, select, fill, filter, transform, resize --
// and save it back, where the editor's asset watcher hands the new pixels to
// every material that uses them. A .psd keeps its layers.
//
// Laid out the way Photoshop is, so a hand that knows Photoshop finds its way:
// the tools in a column of icons on the left (the same letters on the keyboard),
// the active tool's options in a bar across the top, Color / Swatches and
// Layers / History on the right, the same menus and shortcuts (Ctrl+T, Ctrl+J,
// Ctrl+L, Ctrl+M, Alt+Backspace ...). Every icon says in its tooltip what it is
// and which key it has.
//
// Several pictures at once, one tab each. The picture itself and every tool
// that writes into it live in ImageDoc.hpp (no GL, no ImGui); this file is the
// window: the canvas, the tool column, the options bar, the panels and dialogs.
//
// Made to be used without a steady hand (see parkinson-tauglicher Editor):
//   * strokes write absolutely -- going over a spot again does not darken it;
//   * the brush trails the pointer on a string (stabiliser), and a line mode
//     paints straight lines from click to click, no dragging at all;
//   * selections, shapes, crops and gradients go click -- click (dragging works
//     too), the polygonal lasso is click by click, with an optional snap grid and
//     a "constrain" switch instead of Shift;
//   * every amount is a stepper, every action one undo step, nothing asks
//     "are you sure" -- closing a changed picture is the one second click.
//
// While the window has the keyboard it claims it the way a text field does
// (io.WantTextInput), so the editor's own shortcuts -- Ctrl+Z on the scene,
// Q/W/E, V, G -- stand aside while you paint.
namespace imageui {

// The tool column, in Photoshop's order.
enum class Tool { Move, Select, Lasso, Wand, Crop, Eyedropper, Brush, Clone, Eraser, Gradient, Fill,
                  Blur, Dodge, Text, Shape, Hand, Zoom, Count };

class ImageEditor {
public:
    struct Deps {
        const std::string&        currentProject;   // the open .fitzel ("" = none)
        std::string&              status;           // the editor's footer line
        std::vector<std::string>* drops = nullptr;  // files dropped on the editor this frame
        const float*              dropX = nullptr;  // ...and where (screen pixels)
        const float*              dropY = nullptr;
    };
    explicit ImageEditor(Deps d);
    ~ImageEditor();
    ImageEditor(const ImageEditor&)            = delete;
    ImageEditor& operator=(const ImageEditor&) = delete;

    void panel(bool& show);
    // A tab for this file (the one it already has, if it is open). False + a
    // status line when it cannot be read.
    bool open(const std::string& path);
    void newImage(int w, int h, img::Color fill);
    // The window had the keyboard last frame.
    bool hasKeyboard() const;

    // For imagepanelcheck.
    img::Document* document();
    void setTool(Tool t);
    void setFirstSize(float w, float h) { m_firstW = w; m_firstH = h; }
    void setZoom(float z);
    void setColors(img::Color fg, img::Color bg) { m_fg = fg; m_bg = bg; }
    void setLineMode(bool on) { m_lineMode = on; }
    void setShape(int s) { m_shape = s; }
    void setPolyLasso(bool on) { m_polyLasso = on; }
    void setTone(int t) { m_tone = t; }
    void beginTransform() { if (View* v = cur()) startTransform(*v); }
    bool transforming() const { return m_xf.on; }
    bool cropping() const { return m_crop.on; }
    // Where a picture point was on screen in the last frame drawn.
    ImVec2 toScreen(ImVec2 p) const;
    // Open an adjustment or filter on the current picture, as its menu entry does.
    void startFilter(img::Filter f) { if (View* v = cur()) openFilter(*v, f); }
    void startCurves() { if (View* v = cur()) openCurves(*v); }

private:
    struct View {
        img::Document              doc;
        std::uint32_t              tex = 0;      // the picture as seen (GL)
        std::uint32_t              shape = 0;    // doc.shapeVersion() the texture was made for
        std::vector<std::uint8_t>  comp;         // CPU copy of it
        float                      zoom = 1.0f;
        ImVec2                     pan{0.0f, 0.0f};
        bool                       fitPending = true;
        std::uint32_t              selVer = 0;   // the selection the outline was traced for
        std::vector<ImVec4>        outline;      // its edges, image pixels (x0, y0, x1, y1)
        bool                       outlineBox = false;  // too many edges: the bounds instead
        int                        id = 0;
        bool                       closeArmed = false;
        // The Layers panel's pictures, one small texture per layer.
        std::vector<std::uint32_t> thumbs;
        std::uint32_t              thumbVer = 0;
        double                     thumbTime = -1.0;
        int                        thumbW = 0, thumbH = 0;
        bool                       hasLastPaint = false;   // Shift+click paints a line from here
        ImVec2                     lastPaint{0, 0};
    };
    enum class Pane { None, Filter, Curves, Resize, Canvas, New, Modify, Fill, Stroke };

    View* cur() { return m_cur >= 0 && m_cur < int(m_views.size()) ? m_views[std::size_t(m_cur)].get() : nullptr; }
    std::string projectDir() const;
    std::string startDir() const;
    void scanImages();

    // --- window parts (ImageEditPanel.cpp)
    void menuBar();
    void welcome();
    void optionsBar(View& v);
    void toolBar();
    void fgBg();
    void colorPanel();
    void swatchesPanel();
    void layersPanel(View& v);
    void historyPanel(View& v);
    void dialog(View* v);
    void filterDialog(View& v);
    void curvesDialog(View& v);
    void statusLine(View& v);
    void shortcuts(View* v);
    void colorPopup(const char* id, img::Color& c, const char* title);
    void updateThumbs(View& v);
    const char* toolHint() const;

    // --- actions
    void openDialog();
    void placeDialog(View& v);
    bool save(View& v, bool as);
    void revert(View& v);
    void requestClose(int index);
    void closeView(int index);
    void copy(View& v, bool merged, bool cut);
    void paste(View* v, bool asNew);
    void openFilter(View& v, img::Filter f);
    void openCurves(View& v);
    void applyCurvesPreview(View& v);
    void repeatFilter(View& v);
    void closePane(View& v, bool apply);
    void rememberColor(const img::Color& c);
    void selectLayer(View& v, int i);

    // --- the canvas (ImageCanvas.cpp)
    void canvas(View& v);
    void upload(View& v);
    void traceOutline(View& v);
    void drawOverlay(View& v, ImDrawList* dl, ImVec2 origin, ImVec2 mouseImg, bool hovered);
    void pointerDown(View& v, ImVec2 p, ImVec2 screen, bool right);
    void pointerDrag(View& v, ImVec2 p, ImVec2 screen);
    void pointerUp(View& v, ImVec2 p, ImVec2 screen);
    void doubleClick(View& v, ImVec2 p);
    void finishAnchor(View& v, ImVec2 p);
    void cancelGesture(View& v);
    void startAnchor(View& v, ImVec2 p, ImVec2 screen);
    img::SelOp gestureOp() const;           // the selection mode, Shift / Alt included
    ImVec2 snapped(ImVec2 p) const;
    ImVec2 constrained(ImVec2 a, ImVec2 p, bool box) const;
    void fit(View& v, ImVec2 size);
    void zoomAt(View& v, float factor, ImVec2 screen);
    void textUpdate(View& v);
    void textEnd(bool keep);
    float textPad() const;
    // Polygonal / freehand lasso.
    void lassoClose(View& v);
    void lassoCancel();
    // Crop tool.
    void cropCommit(View& v);
    void cropCancel();
    // Free transform (Ctrl+T).
    void startTransform(View& v);
    void transformCommit(View& v);
    void transformCancel(View& v);
    void transformApply(View& v);
    void transformCorners(ImVec2 out[4]) const;   // image space, clockwise from top left
    int  transformHit(View& v, ImVec2 screen) const;
    void nudge(View& v, int dx, int dy);

    Deps m_d;
    std::vector<std::unique_ptr<View>> m_views;
    int  m_cur = -1, m_nextId = 1, m_selectTab = -1;
    std::vector<std::uint32_t> m_retired;   // textures to free once the frame that drew them is done
    std::uint32_t m_checker = 0;

    Tool        m_tool = Tool::Brush;
    img::Color  m_fg{0.0f, 0.0f, 0.0f, 1.0f}, m_bg{1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<img::Color> m_recent;
    img::Brush  m_brush;
    float       m_retouch = 0.5f;             // strength of Blur / Sharpen / Dodge / Burn / Sponge
    float       m_stabilize = 14.0f;          // screen pixels of string
    bool        m_lineMode = false;
    bool        m_ellipse = false;            // the marquee's shape
    bool        m_polyLasso = false;
    int         m_sharpen = 0;                // the Blur tool: 0 blur, 1 sharpen
    int         m_tone = 0;                   // the Dodge tool: 0 dodge, 1 burn, 2 sponge, 3 saturate
    img::SelOp  m_selOp = img::SelOp::Replace;
    float       m_tolerance = 0.12f;
    bool        m_contiguous = true, m_sampleMerged = false;
    float       m_fillOpacity = 1.0f;
    bool        m_radial = false, m_reverse = false;
    int         m_shape = 1;                  // img::Stroke::Shape
    float       m_lineWidth = 4.0f;
    bool        m_constrain = false;
    bool        m_autoSelect = false;         // Move: take the layer under the pointer
    int         m_snap = 1;                   // grid for selections, shapes, text, moves (px)
    int         m_nudge = 1;
    bool        m_pixelGrid = true, m_tilePreview = false, m_showEdges = true;

    // gestures in progress
    img::Stroke m_stroke;
    bool   m_painting = false;
    ImVec2 m_lazy{0, 0};
    bool   m_anchorOn = false, m_anchorEdit = false, m_anchorHeld = false;
    ImVec2 m_anchor{0, 0}, m_pressScreen{0, 0}, m_lastQ{0, 0};
    img::SelOp m_anchorOp = img::SelOp::Replace;
    ImVec2 m_center{0, 0};                    // the canvas middle on screen (zoom buttons)
    bool   m_lineHas = false;
    ImVec2 m_lineLast{0, 0};
    bool   m_moving = false;
    ImVec2 m_moveStart{0, 0};
    int    m_moveDx = 0, m_moveDy = 0;
    bool   m_panning = false;
    bool   m_cloneHasSrc = false, m_clonePick = true, m_cloneAligned = false;
    ImVec2 m_cloneSrc{0, 0}, m_cloneOff{0, 0};
    // Dragging the marquee itself (a press inside the selection with a select tool).
    bool   m_selPending = false, m_selMoving = false;
    ImVec2 m_selPress{0, 0}, m_selPressScreen{0, 0};
    std::shared_ptr<img::Mask> m_selBase;

    struct Lasso {
        bool on = false, dragging = false;
        int viewId = -1;
        std::vector<ImVec2> pts;
        img::SelOp op = img::SelOp::Replace;
    } m_lasso;

    struct Crop {
        bool on = false;
        int viewId = -1;
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        int drag = -1;                        // handle 0..7, 8 = the whole box
        float sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
        ImVec2 press{0, 0};
    } m_crop;

    struct XformState {
        bool on = false;
        int viewId = -1;
        img::Xform xf, start;
        img::Rect src;
        int drag = -1;                        // handle 0..7, 8 = move, 9 = turn
        ImVec2 press{0, 0};
        float pressAngle = 0.0f;
        bool keepAspect = false;
        bool dirty = false;
    } m_xf;

    struct TextSession {
        bool   on = false;
        int    viewId = -1, layer = -1, layers = 0;
        ImVec2 pos{0, 0};
        bool   dirty = false;
        img::Color color;
    } m_text;
    char        m_textBuf[2048] = {};
    std::string m_font;                       // path; "" = the first sans we find
    std::string m_fontName = "Arial";
    float       m_textSize = 48.0f;
    char        m_fontSearch[64] = {};
    bool        m_focusText = false;

    Pane        m_pane = Pane::None;
    bool        m_paneOpened = false;         // the dialog's popup was opened
    img::Filter m_filter = img::Filter::Blur;
    float       m_fp[3] = {0, 0, 0};
    bool        m_preview = true;
    std::uint32_t m_seed = 1;
    bool        m_hasLastFilter = false;
    img::Filter m_lastFilter = img::Filter::Blur;
    float       m_lastFp[3] = {0, 0, 0};
    int         m_newW = 1024, m_newH = 1024, m_newFill = 0;
    int         m_resW = 0, m_resH = 0, m_ancX = 1, m_ancY = 1;
    bool        m_keepAspect = true;
    img::SelModify m_modOp = img::SelModify::Feather;
    float       m_modR = 4.0f;
    int         m_fillWhat = 0;               // Edit > Fill: 0 fg, 1 bg, 2 black, 3 50 % grey, 4 white
    float       m_fillDlgOpacity = 1.0f;
    float       m_strokeW = 4.0f;
    int         m_strokeWhere = 1;
    // Curves: points per channel (0 = RGB, then R, G, B), the one picked, the histogram.
    std::vector<std::pair<float, float>> m_curve[4];
    int         m_curveCh = 0, m_curveSel = -1, m_curveDrag = -1;
    std::uint32_t m_hist[4][256] = {};

    img::Clip     m_clip;
    std::uint32_t m_clipSeq = 0;              // the OS clipboard's number right after our own copy

    std::vector<std::string> m_images;        // the project's pictures
    std::string m_scannedFor = "\x01";
    char        m_search[64] = {};
    char        m_layerName[128] = {};
    int         m_renaming = -1;              // the layer being renamed in place
    bool        m_renameFocus = false;
    std::string m_lastDir;
    std::string m_hoverInfo;                  // the status line's pointer part

    float m_firstW = 1300.0f, m_firstH = 860.0f;
    int   m_focusFrame = -10;
};

} // namespace imageui
