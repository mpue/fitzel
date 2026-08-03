#include "fitzel/ui/Gui.hpp"

#include "fitzel/core/Window.hpp"

#include <fstream>
#include <utility>

#include <imgui.h>
#include <imgui_internal.h> // window list + per-window rect/rounding for drop shadows
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

namespace fitzel {

namespace {

// A soft, modern dark theme: calmer blue-grey backgrounds, a single azure
// accent, rounded corners and roomier spacing for readability.
//
// The surfaces are a deliberate elevation ladder -- dock backdrop < window <
// child/card < input frame -- so a panel reads as a stack of surfaces rather
// than one flat wash. The accent is spent only on state that is *live* (checked,
// dragged, selected, focused); everything at rest is neutral grey-blue, which is
// what keeps a dense editor from looking like a toy.
void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 8.0f;
    s.ChildRounding     = 6.0f;
    s.FrameRounding     = 6.0f;
    s.PopupRounding     = 8.0f;
    s.GrabRounding      = 4.0f;
    s.TabRounding       = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.WindowPadding     = ImVec2(12.0f, 10.0f);
    s.FramePadding      = ImVec2(10.0f, 6.0f);
    s.CellPadding       = ImVec2(8.0f, 5.0f);
    s.ItemSpacing       = ImVec2(9.0f, 7.0f);
    s.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    s.IndentSpacing     = 18.0f;
    s.ScrollbarSize     = 11.0f;   // slimmer: a scrollbar is chrome, not content
    s.GrabMinSize       = 10.0f;
    s.WindowBorderSize  = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.PopupBorderSize   = 1.0f;
    s.FrameBorderSize   = 1.0f;    // hairline edge: reads as a crisp control
    s.TabBarBorderSize  = 1.0f;
    s.TabBarOverlineSize = 2.0f;   // accent underline under the selected tab
    s.DockingSeparatorSize = 2.0f;
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextPadding = ImVec2(16.0f, 4.0f);
    s.WindowTitleAlign  = ImVec2(0.02f, 0.5f);
    s.DisabledAlpha     = 0.45f;
    // Invisible hit tolerance: every widget reacts a few pixels beyond its drawn
    // edge, so a click that lands slightly off still counts. Nothing moves --
    // this only widens the reactive box, which is why it costs no screen space
    // and no muscle memory. Kept small on purpose: overlapping widgets resolve in
    // favour of the first one, so a large value would start stealing clicks.
    s.TouchExtraPadding = ImVec2(4.0f, 3.0f);
    s.WindowMenuButtonPosition = ImGuiDir_None; // drop the collapse-arrow clutter
    // Anti-aliased curves everywhere, so rounded corners don't fray at 4K.
    s.CircleTessellationMaxError = 0.15f;
    s.CurveTessellationTol       = 1.0f;

    const ImVec4 accent    = ImVec4(0.31f, 0.58f, 0.90f, 1.00f);
    const ImVec4 accentHi  = ImVec4(0.42f, 0.68f, 0.98f, 1.00f);
    const ImVec4 accentDim = ImVec4(0.31f, 0.58f, 0.90f, 0.45f);
    // Elevation ladder (dark -> light).
    const ImVec4 bgSunken  = ImVec4(0.075f, 0.083f, 0.100f, 1.00f); // behind everything
    const ImVec4 bgWindow  = ImVec4(0.113f, 0.124f, 0.148f, 1.00f); // panels
    const ImVec4 bgCard    = ImVec4(0.140f, 0.153f, 0.181f, 1.00f); // child regions
    const ImVec4 bgFrame   = ImVec4(0.172f, 0.189f, 0.223f, 1.00f); // inputs
    const ImVec4 bgFrameHi = ImVec4(0.212f, 0.232f, 0.272f, 1.00f);
    const ImVec4 line      = ImVec4(0.290f, 0.315f, 0.365f, 0.55f); // borders

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = ImVec4(0.90f, 0.92f, 0.95f, 1.00f);
    c[ImGuiCol_TextDisabled]         = ImVec4(0.48f, 0.51f, 0.57f, 1.00f);
    c[ImGuiCol_WindowBg]             = bgWindow;
    c[ImGuiCol_ChildBg]              = bgCard;
    c[ImGuiCol_PopupBg]              = ImVec4(0.128f, 0.140f, 0.168f, 0.99f);
    c[ImGuiCol_Border]               = line;
    c[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg]              = bgFrame;
    c[ImGuiCol_FrameBgHovered]       = bgFrameHi;
    c[ImGuiCol_FrameBgActive]        = ImVec4(0.245f, 0.270f, 0.315f, 1.00f);
    c[ImGuiCol_TitleBg]              = bgSunken;
    c[ImGuiCol_TitleBgActive]        = ImVec4(0.145f, 0.165f, 0.205f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.075f, 0.083f, 0.100f, 0.80f);
    c[ImGuiCol_MenuBarBg]            = ImVec4(0.098f, 0.108f, 0.130f, 1.00f);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.16f);
    c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.290f, 0.315f, 0.365f, 0.85f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.360f, 0.395f, 0.455f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = accent;
    c[ImGuiCol_CheckMark]            = accentHi;
    c[ImGuiCol_SliderGrab]           = accent;
    c[ImGuiCol_SliderGrabActive]     = accentHi;
    // Buttons sit one step above their frame and lean on the accent only while
    // pressed -- a panel full of blue buttons has no hierarchy left to give.
    c[ImGuiCol_Button]               = ImVec4(0.196f, 0.216f, 0.256f, 1.00f);
    c[ImGuiCol_ButtonHovered]        = ImVec4(0.250f, 0.320f, 0.420f, 1.00f);
    c[ImGuiCol_ButtonActive]         = accent;
    c[ImGuiCol_Header]               = ImVec4(0.200f, 0.270f, 0.370f, 0.75f);
    c[ImGuiCol_HeaderHovered]        = ImVec4(0.235f, 0.330f, 0.460f, 0.90f);
    c[ImGuiCol_HeaderActive]         = ImVec4(0.270f, 0.400f, 0.560f, 1.00f);
    c[ImGuiCol_Separator]            = ImVec4(0.290f, 0.315f, 0.365f, 0.45f);
    c[ImGuiCol_SeparatorHovered]     = accentDim;
    c[ImGuiCol_SeparatorActive]      = accent;
    c[ImGuiCol_ResizeGrip]           = ImVec4(0.290f, 0.315f, 0.365f, 0.40f);
    c[ImGuiCol_ResizeGripHovered]    = accentDim;
    c[ImGuiCol_ResizeGripActive]     = accent;
    c[ImGuiCol_Tab]                  = ImVec4(0.113f, 0.124f, 0.148f, 1.00f);
    c[ImGuiCol_TabHovered]           = ImVec4(0.190f, 0.230f, 0.300f, 1.00f);
    c[ImGuiCol_TabSelected]          = ImVec4(0.163f, 0.185f, 0.228f, 1.00f);
    c[ImGuiCol_TabSelectedOverline]  = accentHi;   // the accent line on the active tab
    c[ImGuiCol_TabDimmed]            = ImVec4(0.095f, 0.105f, 0.126f, 1.00f);
    c[ImGuiCol_TabDimmedSelected]    = ImVec4(0.135f, 0.150f, 0.180f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.31f, 0.58f, 0.90f, 0.35f);
    c[ImGuiCol_DockingPreview]       = accentDim;
    c[ImGuiCol_DockingEmptyBg]       = bgSunken;
    c[ImGuiCol_TableHeaderBg]        = ImVec4(0.150f, 0.165f, 0.198f, 1.00f);
    c[ImGuiCol_TableBorderStrong]    = line;
    c[ImGuiCol_TableBorderLight]     = ImVec4(0.290f, 0.315f, 0.365f, 0.28f);
    c[ImGuiCol_TableRowBg]           = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_TableRowBgAlt]        = ImVec4(1.00f, 1.00f, 1.00f, 0.022f);
    c[ImGuiCol_TextSelectedBg]       = accentDim;
    c[ImGuiCol_NavCursor]            = accentHi;
    c[ImGuiCol_ModalWindowDimBg]     = ImVec4(0.04f, 0.045f, 0.055f, 0.65f);
}

// The UI typefaces we offer, best-first. Each is a regular + semibold pair (the
// semibold is what section headings use). Families whose files aren't on this
// machine are skipped at load, so one list covers every platform. Verdana and
// the DejaVu/Liberation pair are in here on purpose: they have a large x-height
// and open letterforms, which read better at small sizes than the sleeker
// system UI fonts -- worth offering even though Segoe UI looks tidier.
struct FontFamilySpec {
    const char* name;
    const char* regular;
    const char* semibold; // may be null: the regular is then used for headings
};
const FontFamilySpec kFamilies[] = {
    // Windows
    {"Segoe UI", "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\seguisb.ttf"},
    {"Verdana",  "C:\\Windows\\Fonts\\verdana.ttf", "C:\\Windows\\Fonts\\verdanab.ttf"},
    {"Tahoma",   "C:\\Windows\\Fonts\\tahoma.ttf",  "C:\\Windows\\Fonts\\tahomabd.ttf"},
    {"Calibri",  "C:\\Windows\\Fonts\\calibri.ttf", "C:\\Windows\\Fonts\\calibrib.ttf"},
    {"Arial",    "C:\\Windows\\Fonts\\arial.ttf",   "C:\\Windows\\Fonts\\arialbd.ttf"},
    // macOS
    {"San Francisco", "/System/Library/Fonts/SFNS.ttf", nullptr},
    {"Helvetica", "/System/Library/Fonts/Helvetica.ttc", nullptr},
    // Linux
    {"DejaVu Sans", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"},
    {"Liberation Sans", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
                        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf"},
};

ImFont* addFont(ImGuiIO& io, const char* path, float px) {
    if (!path || !std::ifstream(path).good()) return nullptr;
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    return io.Fonts->AddFontFromFileTTF(path, px, &cfg);
}

// Load a fixed-width font for code views (the Lua editor). Returns the loaded
// ImFont, or nullptr if no monospace TTF is present (caller then falls back to
// the proportional UI font). Added after the UI font, so it is not the default.
ImFont* loadMonoFont(ImGuiIO& io, float scale) {
    const char* candidates[] = {
        // Windows
        "C:\\Windows\\Fonts\\consola.ttf",   // Consolas
        "C:\\Windows\\Fonts\\cour.ttf",      // Courier New
        // macOS
        "/System/Library/Fonts/Menlo.ttc",
        "/System/Library/Fonts/SFNSMono.ttf",
        // Linux
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
    };
    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    for (const char* path : candidates)
        if (std::ifstream(path).good())
            return io.Fonts->AddFontFromFileTTF(path, 18.0f * scale, &cfg);
    return nullptr;
}

// Input tolerances that make the editor forgiving of an unsteady hand (tremor,
// slow or imprecise clicks) WITHOUT changing any layout, shortcut or interaction
// model -- every widget still looks and works exactly as before, the thresholds
// around it are simply wider. `scale` is the display's content scale, so the
// pixel distances mean the same thing physically on a 4K screen as on a 1080p
// one. Deliberately not a "mode": there is nothing to switch on, so no workflow
// has to be relearned and no habit from another editor stops working.
void applyInputTolerances(ImGuiIO& io, float scale) {
    // A click that drifts a little is still a click, not the start of a drag.
    // This is the single most valuable one: it stops stray motion during a press
    // from turning into an accidental drag of whatever sits under the cursor.
    io.MouseDragThreshold      = 12.0f * scale;  // ImGui default: 6
    // Double-clicks may be slower and less repeatable in position.
    io.MouseDoubleClickTime    = 0.50f;          // default: 0.30 s
    io.MouseDoubleClickMaxDist = 12.0f * scale;  // default: 6
    // Click a DragFloat (without moving) to type the number instead of dragging
    // it. Dragging keeps working untouched -- this only ADDS the keyboard path
    // for the moments when dragging to a precise value is the harder way.
    io.ConfigDragClickToInputText = true;
    // Only the title bar moves a panel. Otherwise a click that slips on a
    // panel's background drags the whole panel out of the layout.
    io.ConfigWindowsMoveFromTitleBarOnly = true;
}

} // namespace

Gui::Gui(Window& window) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; // allow panels to dock

    // HiDPI: scale fonts + the whole style by the monitor's content scale so the
    // UI isn't tiny on 4K/high-DPI displays. GLFW reports e.g. 1.5 at 150% Windows
    // scaling (and 1.0 on a normal display). Clamp to a sane range.
    float sx = 1.0f, sy = 1.0f;
    if (GLFWwindow* w = window.nativeHandle())
        glfwGetWindowContentScale(w, &sx, &sy);
    float scale = (sx > 0.0f) ? sx : 1.0f;
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 4.0f) scale = 4.0f;

    m_dpiScale = scale;

    // Bake every available family at the default size scaled for this display.
    // ImGui 1.92 rasterizes on demand, so changing the size later stays crisp --
    // this is only the starting point, not a ceiling.
    for (const FontFamilySpec& spec : kFamilies) {
        ImFont* reg = addFont(io, spec.regular, kDefaultFontSize * scale);
        if (!reg) continue;
        ImFont* bold = addFont(io, spec.semibold, kDefaultFontSize * scale);
        m_families.push_back(FontFamily{spec.name, reg, bold ? bold : reg});
    }
    m_mono = loadMonoFont(io, scale);
    applyTheme();
    ImGui::GetStyle().ScaleAllSizes(scale); // padding/rounding/scrollbars/etc.
    applyInputTolerances(io, scale);        // after the style: these are io, not style
    if (!m_families.empty()) setFontFamily(0);
    setFontSize(kDefaultFontSize);

    // install_callbacks = true: ImGui chains the callbacks Input already set.
    ImGui_ImplGlfw_InitForOpenGL(window.nativeHandle(), true);
    ImGui_ImplOpenGL3_Init("#version 330");

    m_active = true;
}

Gui::~Gui() {
    if (m_active) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
}

void Gui::setFontFamily(int index) {
    if (index < 0 || index >= static_cast<int>(m_families.size())) return;
    m_family = index;
    ImGui::GetIO().FontDefault = m_families[index].regular;
    m_bold = m_families[index].bold;
}

void Gui::setFontSize(float px) {
    if (px < 10.0f) px = 10.0f;
    if (px > 32.0f) px = 32.0f;
    m_fontSize = px;
    // FontSizeBase is the pre-DPI size ImGui rasterizes at; the display scale is
    // ours to apply, the same one the baked sizes and ScaleAllSizes() used.
    ImGui::GetStyle().FontSizeBase = px * m_dpiScale;
}

const char* Gui::fontFamilyName(int index) const {
    if (index < 0 || index >= static_cast<int>(m_families.size())) return "";
    return m_families[index].name;
}

Gui::Gui(Gui&& other) noexcept
    : m_active(std::exchange(other.m_active, false)),
      m_mono(std::exchange(other.m_mono, nullptr)),
      m_bold(std::exchange(other.m_bold, nullptr)),
      m_families(std::move(other.m_families)),
      m_family(other.m_family),
      m_fontSize(other.m_fontSize),
      m_dpiScale(other.m_dpiScale) {}

Gui& Gui::operator=(Gui&& other) noexcept {
    if (this != &other) {
        if (m_active) {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
        }
        m_active   = std::exchange(other.m_active, false);
        m_mono     = std::exchange(other.m_mono, nullptr);
        m_bold     = std::exchange(other.m_bold, nullptr);
        m_families = std::move(other.m_families);
        m_family   = other.m_family;
        m_fontSize = other.m_fontSize;
        m_dpiScale = other.m_dpiScale;
    }
    return *this;
}

void Gui::beginFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

unsigned int Gui::dockspace() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    // A borderless, transparent host that can't itself be docked or focused, so
    // it acts purely as a backdrop for the dockspace over the live scene.
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoDocking;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DockHost", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockId = ImGui::GetID("FitzelDock");
    // PassthruCentralNode keeps the empty centre transparent (scene visible).
    ImGui::DockSpace(dockId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_PassthruCentralNode);
    ImGui::End();
    return dockId;
}

namespace {
// --- Window drop shadows -----------------------------------------------------
// Mainline ImGui has no window shadow. We fake one per window and splice it into
// the render stream *just behind that window*, so panels lift off the scene and
// menus/popups cast onto whatever they float over. A single shared background
// list can't do this: a menu draws on top of everything, so its shadow would then
// hide behind the very panels it overlaps. Hence one scratch draw list per window,
// inserted right before the window's own list in ImDrawData (see injectWindowShadows).

// Scratch draw lists reused each frame (grown on demand; kept for the app's life).
ImVector<ImDrawList*> g_shadowPool;
int                   g_shadowUsed = 0;

ImDrawList* acquireShadowList() {
    ImGuiIO& io = ImGui::GetIO();
    if (g_shadowUsed >= g_shadowPool.Size)
        g_shadowPool.push_back(IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData()));
    ImDrawList* dl = g_shadowPool[g_shadowUsed++];
    dl->_ResetForNewFrame();
    dl->PushTexture(io.Fonts->TexRef);      // bind the atlas (white pixel for fills)
    dl->PushClipRectFullScreen();
    return dl;
}

// Soft shadow for `w`: a stack of expanding rounded rects, darkest against the
// window edge and fading outward, nudged down a touch as if lit from above.
void paintShadow(ImDrawList* dl, const ImGuiWindow* w) {
    const ImVec2 mn = w->Pos;
    const ImVec2 mx(w->Pos.x + w->Size.x, w->Pos.y + w->Size.y);
    const ImVec2 off(0.0f, 4.0f); // drop direction
    const float  spread = 16.0f;  // penumbra reach (px)
    const int    layers = 8;
    const float  rnd = w->WindowRounding;
    for (int i = layers; i >= 1; --i) {
        const float t    = static_cast<float>(i) / static_cast<float>(layers);
        const float grow = spread * t;                       // outer layers larger
        const int   a    = static_cast<int>(20.0f * (1.0f - t) + 3.0f); // + fainter
        dl->AddRectFilled(ImVec2(mn.x - grow + off.x, mn.y - grow + off.y),
                          ImVec2(mx.x + grow + off.x, mx.y + grow + off.y),
                          IM_COL32(0, 0, 0, a), rnd + grow);
    }
}

// Windows worth a shadow: real, visible, top-level ones with a body of their own.
// Skip child regions, the transparent dock backdrop, and dock-node hosts (the
// windows docked inside them are shadowed individually).
bool castsShadow(const ImGuiWindow* w) {
    if (!w->WasActive || w->Hidden) return false;
    if (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_NoBackground |
                    ImGuiWindowFlags_DockNodeHost))
        return false;
    return w->Size.x >= 8.0f && w->Size.y >= 8.0f;
}

// Rebuild the frame's draw-list array, inserting a shadow list in front of each
// shadow-casting window's own list. Runs after Render(), before the backend reads
// the data. Window order in ImDrawData is back-to-front, so a shadow sits above
// the windows below its owner and below the owner itself -- exactly a drop shadow.
void injectWindowShadows(ImDrawData* dd) {
    if (!dd || dd->CmdLists.Size == 0) return;
    ImGuiContext& g = *ImGui::GetCurrentContext();

    g_shadowUsed = 0;
    static ImVector<ImDrawList*> out; // kept static so its storage outlives the copy
    out.resize(0);
    dd->TotalVtxCount = 0;
    dd->TotalIdxCount = 0;

    for (ImDrawList* dl : dd->CmdLists) {
        const ImGuiWindow* win = nullptr;
        for (ImGuiWindow* w : g.Windows)
            if (w->DrawList == dl) { win = w; break; }
        if (win && castsShadow(win)) {
            ImDrawList* sh = acquireShadowList();
            paintShadow(sh, win);
            sh->_PopUnusedDrawCmd();
            ImGui::AddDrawListToDrawDataEx(dd, &out, sh);
        }
        ImGui::AddDrawListToDrawDataEx(dd, &out, dl);
    }
    dd->CmdLists = out; // copy the reordered pointers back into the frame's data
}
} // namespace

void Gui::endFrame() {
    ImGui::Render();
    injectWindowShadows(ImGui::GetDrawData());
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

bool Gui::wantsMouse() const {
    return ImGui::GetIO().WantCaptureMouse;
}

bool Gui::wantsKeyboard() const {
    return ImGui::GetIO().WantCaptureKeyboard;
}

} // namespace fitzel
