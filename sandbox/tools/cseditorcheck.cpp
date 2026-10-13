// cseditorcheck -- drives the C# editor window the way a hand would and checks
// what came of it: the real CSharpEditor, through the editor's own Gui, against
// the real language service (Fitzel.LanguageServer from bin/dotnet) on a real
// C# project. Keys and characters go into ImGui's input queue frame by frame;
// pictures of the window are written along the way.
//
// Typing help (indent, closing brackets and quotes, stepping over them),
// completion after `.` with the list filtered as you type and Tab accepting,
// signature help inside a call, squiggles from the service, quick info on
// hover, F12, Ctrl+/ and format.
//
//   cd build/release/bin && cseditorcheck.exe <C# project dir> [outDir]
//   cd build/linux/bin && ./cseditorcheck <C# project dir> [outDir]
//
// The project is only read: nothing is saved. Exit code 0 = all passed.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <fitzel/core/Window.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/CSharpEditor.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
// A crash prints where: function and line of each frame.
static LONG WINAPI crashReport(EXCEPTION_POINTERS* ep) {
    HANDLE proc = GetCurrentProcess(), thread = GetCurrentThread();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    SymInitialize(proc, nullptr, TRUE);
    std::printf("CRASH 0x%08lx at %p\n", ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    CONTEXT ctx = *ep->ContextRecord;
    STACKFRAME64 f{};
    f.AddrPC.Offset = ctx.Rip; f.AddrPC.Mode = AddrModeFlat;
    f.AddrFrame.Offset = ctx.Rbp; f.AddrFrame.Mode = AddrModeFlat;
    f.AddrStack.Offset = ctx.Rsp; f.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 24 && StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &f, &ctx, nullptr,
                                           SymFunctionTableAccess64, SymGetModuleBase64, nullptr); ++i) {
        char buf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 d = 0;
        IMAGEHLP_LINE64 ln{};
        ln.SizeOfStruct = sizeof ln;
        DWORD dl = 0;
        const bool hasSym = SymFromAddr(proc, f.AddrPC.Offset, &d, sym);
        const bool hasLine = SymGetLineFromAddr64(proc, f.AddrPC.Offset, &dl, &ln);
        std::printf("  #%d %s  %s:%lu\n", i, hasSym ? sym->Name : "?", hasLine ? ln.FileName : "", hasLine ? ln.LineNumber : 0);
    }
    std::fflush(stdout);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) { ++g_fail; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
                       std::printf(__VA_ARGS__); std::printf("\n"); }      \
        else { std::printf("  ok   "); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

// The editor's insides, for the checks.
struct CSharpEditorProbe {
    CSharpEditor& e;
    CodeEditor& ed() { return e.active()->ed; }
    bool ready() { return e.m_lang.ready(); }
    bool compOpen() { return e.m_comp.open; }
    std::vector<std::string> shown() {
        std::vector<std::string> r;
        for (int i : e.m_comp.shown) r.push_back(e.m_comp.items[i].label);
        return r;
    }
    std::string selected() {
        return e.m_comp.open && !e.m_comp.shown.empty() ? e.m_comp.items[e.m_comp.shown[e.m_comp.sel]].label : "";
    }
    std::string desc() { return e.m_comp.desc; }
    bool sigOpen() { return e.m_sig.open; }
    std::string sigLabel() {
        if (!e.m_sig.open || !e.m_sig.data.contains("sigs") || e.m_sig.data["sigs"].empty()) return "";
        const int a = e.m_sig.data.value("active", 0);
        return e.m_sig.data["sigs"][a].value("label", std::string());
    }
    int sigParam() { return e.m_sig.data.value("param", -1); }
    int diagCount() { return (int)e.active()->diags.size(); }
    bool hasDiag(const std::string& code) {
        for (auto& d : e.active()->diags) if (d.code == code) return true;
        return false;
    }
    bool analyzed() { return !e.active()->needAnalyze && e.active()->analyzeId == 0; }
    bool hoverAnswered() { return e.m_hover.answered; }
    std::string hoverText() {
        std::string s;
        if (e.m_hover.data.contains("sections"))
            for (auto& x : e.m_hover.data["sections"]) s += x.value("text", std::string()) + "\n";
        return s;
    }
    int version() { return e.active()->version; }
    int refCount() { return (int)e.m_refs.size(); }
    int matchCount() { return (int)e.m_matches.size(); }
};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashReport);
#endif
    if (argc < 2) {
        std::printf("usage: cseditorcheck <C# project dir> [outDir]\n");
        return 2;
    }
    const std::string project = fs::absolute(argv[1]).generic_string();
    const fs::path outDir = argc > 2 ? fs::path(argv[2]) : fs::path(".");
    fs::create_directories(outDir);
    const int w = 1400, h = 860;

    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "cseditorcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplGlfw_CursorEnterCallback(window.nativeHandle(), 1);

    CSharpEditor editor;
    editor.projectDir = [&] { return project; };
    editor.visible = true;
    editor.sceneNames = [] {
        return std::vector<CSharpEditor::SceneName>{
            {"Player", "Model"}, {"Sun", "Sun"}, {"Kette_Haken", "Empty"}, {"Crate", "Box"}, {"Crate", "Box"}};
    };
    CSharpEditorProbe P{editor};

    auto frame = [&] {
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        gui.beginFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(float(w), float(h)), ImGuiCond_Always);
        editor.panel(gui);
        gui.endFrame();
        window.swapBuffers();
    };
    auto shot = [&](const std::string& name) {
        // The frame drawn again, read back before it is shown.
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        glViewport(0, 0, fw, fh);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        gui.beginFrame();
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(float(w), float(h)), ImGuiCond_Always);
        editor.panel(gui);
        gui.endFrame();
        std::vector<unsigned char> px(static_cast<std::size_t>(fw) * fh * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        stbi_flip_vertically_on_write(1);
        const std::string path = (outDir / name).string();
        stbi_write_png(path.c_str(), fw, fh, 4, px.data(), fw * 4);
        window.swapBuffers();
        std::printf("  [wrote %s]\n", path.c_str());
    };
    auto pump = [&](const std::function<bool()>& until, double seconds) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(seconds * 1000));
        while (std::chrono::steady_clock::now() < end) {
            frame();
            if (until()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
        return until();
    };
    auto frames = [&](int n) { for (int i = 0; i < n; ++i) frame(); };
    ImGuiIO& io = ImGui::GetIO();
    auto type = [&](const std::string& s) {
        for (char c : s) {
            io.AddInputCharacter(static_cast<unsigned>(static_cast<unsigned char>(c)));
            frames(2);
        }
    };
    auto key = [&](ImGuiKey k, int mods = 0) {
        if (mods & 1) io.AddKeyEvent(ImGuiMod_Ctrl, true);
        if (mods & 2) io.AddKeyEvent(ImGuiMod_Shift, true);
        if (mods & 4) io.AddKeyEvent(ImGuiMod_Alt, true);
        io.AddKeyEvent(k, true);
        frame();
        io.AddKeyEvent(k, false);
        if (mods & 4) io.AddKeyEvent(ImGuiMod_Alt, false);
        if (mods & 2) io.AddKeyEvent(ImGuiMod_Shift, false);
        if (mods & 1) io.AddKeyEvent(ImGuiMod_Ctrl, false);
        frames(2);
    };
    auto line = [&](int l) { return P.ed().GetLineText(l); };
    auto caretLine = [&] { return P.ed().GetCursorPosition().mLine; };
    auto findLine = [&](const std::string& needle) {
        for (int l = 0; l < P.ed().GetTotalLines(); ++l)
            if (line(l).find(needle) != std::string::npos) return l;
        return -1;
    };

    const fs::path script = fs::path(project) / "scripts" / "Rotator.cs";
    editor.open(script.string());
    frames(3);
    CHECK(pump([&] { return P.ready(); }, 40.0), "the language service started and warmed up");
    CHECK(pump([&] { return P.analyzed(); }, 20.0), "the file was analysed");
    CHECK(P.diagCount() == 0, "Rotator.cs has no problems (%d)", P.diagCount());
    shot("01_open.png");

    // --- Typing help -------------------------------------------------------------
    const int rot = findLine("Rotation +=");
    CHECK(rot >= 0, "found the Update body (line %d)", rot + 1);
    P.ed().JumpTo(CodeEditor::Coordinates(rot, (int)line(rot).size()));
    frames(3);
    key(ImGuiKey_Enter);
    CHECK(line(rot + 1) == "        " && caretLine() == rot + 1, "Enter keeps the indentation ('%s')", line(rot + 1).c_str());

    // --- Completion ----------------------------------------------------------------
    type("Time.");
    CHECK(pump([&] { return P.compOpen(); }, 10.0), "completion opened after 'Time.'");
    {
        const auto items = P.shown();
        bool dt = false;
        for (auto& s : items) dt |= s == "DeltaTime";
        CHECK(dt, "the list offers DeltaTime (%d items)", (int)items.size());
    }
    pump([&] { return !P.desc().empty(); }, 5.0);
    CHECK(P.desc().find("DeltaTime") != std::string::npos, "the highlighted item is described ('%s')",
          P.desc().substr(0, 60).c_str());
    shot("02_completion.png");
    type("De");
    CHECK(P.selected() == "DeltaTime", "typing 'De' filters to DeltaTime ('%s')", P.selected().c_str());
    key(ImGuiKey_Tab);
    CHECK(line(rot + 1) == "        Time.DeltaTime", "Tab accepts ('%s')", line(rot + 1).c_str());
    CHECK(!P.compOpen(), "and the list closes");
    type(";");

    // --- Brackets, quotes, signature help ----------------------------------------------
    key(ImGuiKey_Enter);
    type("Debug.Log(");
    CHECK(line(rot + 2) == "        Debug.Log()", "'(' brings its ')' ('%s')", line(rot + 2).c_str());
    CHECK(pump([&] { return P.sigOpen(); }, 10.0), "signature help opened inside the call");
    CHECK(P.sigLabel().find("Log(") != std::string::npos, "it shows Log ('%s')", P.sigLabel().c_str());
    shot("03_signature.png");
    type("\"hi");
    CHECK(line(rot + 2) == "        Debug.Log(\"hi\")", "'\"' brings its closing quote ('%s')", line(rot + 2).c_str());
    type("\")");
    CHECK(line(rot + 2) == "        Debug.Log(\"hi\")", "typing the closers steps over them ('%s')", line(rot + 2).c_str());
    type(";");
    pump([&] { return !P.sigOpen(); }, 3.0);

    // --- Scene names in Entity.Find("...") ------------------------------------------------
    key(ImGuiKey_Enter);
    type("var p = Entity.Find(\"");
    CHECK(pump([&] { return P.compOpen(); }, 10.0), "a quote in Entity.Find( offers the scene's objects");
    {
        const auto names = P.shown();
        CHECK(names.size() == 4, "each name once (%d: %s ...)", (int)names.size(), names.empty() ? "" : names[0].c_str());
    }
    frames(3);
    shot("04a_scenenames.png");
    type("Pl");
    CHECK(P.selected() == "Player", "typing filters to Player ('%s')", P.selected().c_str());
    key(ImGuiKey_Tab);
    CHECK(line(caretLine()) == "        var p = Entity.Find(\"Player\")", "Tab writes the name in ('%s')",
          line(caretLine()).c_str());
    key(ImGuiKey_End);
    type(";");

    // --- Squiggles -----------------------------------------------------------------
    key(ImGuiKey_Enter);
    type("float x = undefinedThing;");
    CHECK(pump([&] { return P.hasDiag("CS0103"); }, 15.0), "an unknown name is reported (CS0103)");
    frames(5);
    shot("04_squiggle.png");

    // --- Hover -----------------------------------------------------------------------
    {
        const int l = findLine("Rotation +=");
        const int c = (int)line(l).find("Rotation") + 3;
        const ImVec2 p = P.ed().CoordinatesToScreen(CodeEditor::Coordinates(l, c));
        io.AddMousePosEvent(p.x + 2.0f, p.y + P.ed().GetLineHeight() * 0.5f);
        CHECK(pump([&] { return P.hoverAnswered(); }, 10.0), "hovering a name asks for quick info");
        CHECK(P.hoverText().find("Rotation") != std::string::npos, "quick info names it ('%s')",
              P.hoverText().substr(0, 70).c_str());
        frames(4);
        shot("05_hover.png");
        io.AddMousePosEvent(5.0f, float(h) - 5.0f);
        frames(2);
    }

    // --- F12 ---------------------------------------------------------------------------
    {
        const int l = findLine("Rotation +=");
        const int c = (int)line(l).find("Speed") + 2;
        P.ed().JumpTo(CodeEditor::Coordinates(l, c));
        frames(2);
        key(ImGuiKey_F12);
        const int decl = findLine("public float Speed");
        CHECK(pump([&] { return caretLine() == decl; }, 10.0), "F12 on Speed goes to its declaration (line %d -> %d)",
              caretLine() + 1, decl + 1);
    }

    // --- Comment toggle, undo -----------------------------------------------------------
    {
        const int decl = findLine("public float Speed");
        const std::string before = line(decl);
        key(ImGuiKey_Slash, 1);
        CHECK(line(decl) == "    // " + before.substr(4), "Ctrl+/ comments the line ('%s')", line(decl).c_str());
        key(ImGuiKey_Slash, 1);
        CHECK(line(decl) == before, "and again uncomments it ('%s')", line(decl).c_str());
    }

    // --- References, rename, undo --------------------------------------------------------
    {
        const int decl = findLine("public float Speed");
        P.ed().JumpTo(CodeEditor::Coordinates(decl, (int)line(decl).find("Speed") + 1));
        frames(2);
        key(ImGuiKey_F12, 2);
        CHECK(pump([&] { return P.refCount() >= 2; }, 10.0), "Shift+F12 lists the references to Speed (%d)", P.refCount());
        key(ImGuiKey_F2);
        frames(3);
        type("TurnSpeed");
        key(ImGuiKey_Enter);
        CHECK(pump([&] { return line(decl).find("TurnSpeed") != std::string::npos; }, 10.0),
              "F2 renames the declaration ('%s')", line(decl).c_str());
        const int use = findLine("Rotation +=");
        CHECK(line(use).find("TurnSpeed") != std::string::npos, "...and its use ('%s')", line(use).c_str());
        frames(2);
        key(ImGuiKey_Z, 1);
        CHECK(line(decl).find("TurnSpeed") == std::string::npos && line(use).find("TurnSpeed") == std::string::npos,
              "one Ctrl+Z undoes the whole rename ('%s')", line(decl).c_str());
    }

    // --- Find -------------------------------------------------------------------------------
    {
        key(ImGuiKey_F, 1);
        type("speed");
        frames(2);
        CHECK(P.matchCount() == 2, "Ctrl+F finds 'speed' twice, case-insensitively (%d)", P.matchCount());
        key(ImGuiKey_Escape);
        frames(2);
    }

    // --- Format ---------------------------------------------------------------------------
    {
        const int l = findLine("float x = undefinedThing;");
        P.ed().JumpTo(CodeEditor::Coordinates(l, 0));
        frames(2);
        // Wreck the line's spacing, then format.
        P.ed().ReplaceRange(CodeEditor::Coordinates(l, 0), CodeEditor::Coordinates(l, (int)line(l).size()),
                            "float    x=undefinedThing ;");
        frames(2);
        const int v = P.version();
        key(ImGuiKey_F, 2 | 4);
        CHECK(pump([&] { return line(l) == "        float x = undefinedThing;"; }, 10.0), "Shift+Alt+F formats ('%s')",
              line(l).c_str());
        (void)v;
    }
    shot("06_final.png");

    std::printf("[cseditorcheck] %s\n", g_fail ? "FAILED" : "all passed");
    return g_fail ? 1 : 0;
}
