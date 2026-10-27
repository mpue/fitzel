// assistcheck -- the language model's help, driven the way a hand would drive
// it, against a real Ollama: the Lua editor's suggestions as you type (Tab
// takes them) and Ctrl+I (an instruction, a preview, Accept), then the
// Assistant window working a scene through its tools -- an object made,
// coloured and given a component, a script written and hung on it, undo.
// Keys and characters go into ImGui's input queue frame by frame; pictures of
// the windows are written along the way.
//
//   cd build/linux/bin && ./assistcheck [outDir] [ollama url] [editor]
//
// "editor" stops after the code editor's part. "probe <file>" asks the
// Assistant each line of the file in a fresh conversation and prints what it
// did and said -- for judging what the model knows.
//
// Everything happens in a scratch folder under outDir; the user's settings
// (ollama.json) are read but not written. Exit code 0 = all passed, 3 = no
// model server to test against.

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/core/Window.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/scene/Camera.hpp>
#include <fitzel/ui/Gui.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/AssistantPanel.hpp"
#include "../src/AssistantTools.hpp"
#include "../src/Command.hpp"
#include "../src/CodeAssist.hpp"
#include "../src/Component.hpp"
#include "../src/EditorContext.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/ScriptEditor.hpp"
#include "../src/ScriptSystem.hpp"

namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        if (!(cond)) { ++g_fail; std::printf("  FAIL %s:%d  ", __FILE__, __LINE__); \
                       std::printf(__VA_ARGS__); std::printf("\n"); }      \
        else { std::printf("  ok   "); std::printf(__VA_ARGS__); std::printf("\n"); } \
    } while (0)

struct CodeAssistProbe {
    CodeAssist& a;
    bool requesting() const { return (bool)a.m_req; }
    bool editOpen() const { return a.m_edit.open; }
    bool editResult() const { return a.m_edit.haveResult; }
    bool editRunning() const { return a.m_edit.reply && !a.m_edit.reply->finished(); }
    std::string editError() const { return a.m_edit.error; }
    std::string ghost() const { return a.m_ghost; }
    std::string state() const {
        return std::string("want=") + (a.m_wantSuggest ? "1" : "0") + " req=" + (a.m_req ? "1" : "0") +
               " err='" + a.m_lastError + "' ed=" + (a.m_ed ? "set" : "null");
    }
};
struct AssistantPanelProbe {
    AssistantPanel& p;
    void clear() { p.m_msgs.clear(); }
    // The conversation as text: questions, tool calls with what they got, answers.
    std::string transcript() const {
        std::string s;
        for (const auto& m : p.m_msgs) {
            if (m.role == "user") {
                s += "\n>>> " + m.content + "\n";
                // Which chapters went with it: the line after each [file] mark.
                for (std::size_t at = m.hidden.find("\n["); at != std::string::npos; at = m.hidden.find("\n[", at + 2)) {
                    const std::size_t eol = m.hidden.find('\n', at + 1);
                    const std::size_t eol2 = eol == std::string::npos ? eol : m.hidden.find('\n', eol + 1);
                    if (eol != std::string::npos && eol > at + 5 && m.hidden.compare(eol - 4, 4, ".md]") == 0) {
                        std::string h = m.hidden.substr(at + 1, eol2 - at - 1);
                        std::replace(h.begin(), h.end(), '\n', ' ');
                        s += "    (manual " + h + ")\n";
                    }
                }
            }
            else if (m.role == "tool") s += "  [" + m.toolName + " " + m.args.dump() + "] -> " + m.content.substr(0, 300) + "\n";
            else if (!m.content.empty()) s += m.content + "\n";
        }
        return s;
    }
};
struct ScriptEditorProbe {
    ScriptEditor& s;
    CodeEditor& ed() { return s.m_editor; }
};

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const fs::path outDir = argc > 1 ? fs::path(argv[1]) : fs::path("assistcheck-out");
    fs::create_directories(outDir);
    const fs::path work = fs::absolute(outDir / "project");
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work / "scripts", ec);

    ollama::Client& oc = ollama::client();
    oc.load();
    if (argc > 2) oc.settings.url = argv[2];
    oc.settings.inlineAuto  = true;
    oc.settings.inlineDelay = 0.3f;
    oc.settings.confirmChanges = false;

    const int w = 1500, h = 900;
    fitzel::Window window(fitzel::WindowConfig{
        .width = w, .height = h, .title = "assistcheck", .vsync = false, .maximized = false});
    fitzel::Gui gui(window);
    fitzel::rhi::Device& gpu = window.device();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplGlfw_CursorEnterCallback(window.nativeHandle(), 1);

    // Two trees to place (the engine's, copied: the database writes .meta files).
    fs::create_directories(work / "models", ec);
    for (const char* m : {"tree1.glb", "tree2.glb"})
        for (const char* dir : {"../../../content/models/", "content/models/"})
            if (fs::exists(fs::path(dir) / m, ec)) {
                fs::copy_file(fs::path(dir) / m, work / "models" / m, fs::copy_options::overwrite_existing, ec);
                fs::copy_file(fs::path(dir) / (std::string(m) + ".meta"), work / "models" / (std::string(m) + ".meta"),
                              fs::copy_options::overwrite_existing, ec);
                break;
            }

    // --- An editor around a scene ------------------------------------------------
    Document                  document;
    std::vector<Entity>&      entities  = document.entities();
    std::vector<MaterialDef>& materials = document.materials();
    Selection                 sel(entities);
    CommandStack              history;
    ModelLibrary              models;
    fitzel::AssetDatabase     assetDb{work.generic_string()};
    fitzel::Camera            camera({0.0f, 2.0f, 10.0f});
    int         matSel = -1, faceOwner = -1, faceSel = -1, nextId = 1;
    std::string status;
    assetDb.refresh();
    EditorContext ed{document, entities, sel, history, materials, matSel, assetDb, models,
                     camera, status, faceOwner, faceSel, nextId,
                     // main's addModelEntity, as far as a scene without a renderer needs it
                     [&](glm::vec3 at, int modelId) {
                         LoadedModel* lm = models.byId(modelId);
                         if (!lm) return;
                         Entity nb;
                         nb.type     = EntityType::Model;
                         auto mc     = std::make_unique<ModelComponent>();
                         mc->modelId = modelId;
                         mc->modelPath = lm->path;
                         nb.components.items.push_back(std::move(mc));
                         nb.half        = 0.5f * lm->size();
                         nb.localCenter = nb.center = glm::vec3(at.x, at.y + nb.half.y, at.z);
                         nb.id   = nextId++;
                         nb.name = lm->name + " " + std::to_string(nb.id);
                         history.push(std::make_unique<AddEntityCmd>(nb), document);
                         sel.select(nb.id);
                     },
                     [](glm::vec3, const std::string&) {},
                     [](const std::string&) { return false; }};
    {
        // A ground and a lamp, so the scene is not empty.
        Entity g;
        g.type = EntityType::Plane;
        g.name = "Ground";
        g.id   = nextId++;
        g.half = glm::vec3(20.0f, 0.05f, 20.0f);
        entities.push_back(g);
        Entity l;
        l.type = EntityType::Light;
        l.name = "Lamp";
        l.id   = nextId++;
        l.localCenter = l.center = glm::vec3(0.0f, 4.0f, 0.0f);
        l.components.items.push_back(std::make_unique<LightComponent>());
        entities.push_back(std::move(l));
    }

    ScriptSystem scripts;
    ScriptEditor scriptEditor;
    scriptEditor.scriptsDir = [&] { return (work / "scripts").generic_string(); };
    ScriptEditorProbe SP{scriptEditor};
    CodeAssistProbe   AP{scriptEditor.assist};

    AssistantPanel assistantPanel;
    int played = 0;
    assistant::Host host;
    host.scriptsDir    = [&] { return (work / "scripts").generic_string(); };
    host.projectFile   = [] { return std::string(); };
    host.csharp        = [] { return false; };
    host.playing       = [] { return false; };
    host.play          = [&] { ++played; };
    host.stop          = [] {};
    host.saveScene     = [] { return false; };
    host.openScript    = [&](const std::string& f) { scriptEditor.open(f); };
    host.reloadScripts = [&] { scripts.reset(); };
    host.scriptError   = [&] { return scripts.lastError(); };
    host.spawnPoint    = [] { return glm::vec3(0.0f, 0.0f, -5.0f); };
    host.frame         = [](int) {};
    // The rest of main, as stand-ins that remember what was asked of them.
    std::string openedPanel, weatherSet;
    float clock = 12.0f;
    host.panels = [] {
        return std::vector<std::string>{"World > Terrain", "World > Terrain sculpt", "World > Water",
                                        "World > Rivers & brooks", "World > Sky & atmosphere",
                                        "World > Weather & audio", "Planting > Vegetation",
                                        "Planting > Tree generator", "Construct > Roads",
                                        "Construct > Splines & bridges", "Construct > Town generator",
                                        "Construct > Vehicle", "Assets > Materials", "Assets > Models",
                                        "Assets > Prefabs", "Assets > Scripts", "Presentation > Camera",
                                        "Presentation > Animation graph"};
    };
    host.openPanel = [&](const std::string& n) {
        for (const std::string& p : host.panels())
            if (p.size() >= n.size() && p.compare(p.size() - n.size(), n.size(), n) == 0) { openedPanel = n; return true; }
        return false;
    };
    host.weatherPresets = [] {
        return std::vector<std::string>{"Sunny day", "Morning dew", "Overcast", "Heavy rain", "Storm"};
    };
    host.setWeather = [&](const std::string& n) {
        for (const std::string& p : host.weatherPresets())
            if (p.size() == n.size() && std::equal(p.begin(), p.end(), n.begin(), [](char a, char b) {
                    return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); })) { weatherSet = p; return true; }
        return false;
    };
    host.timeOfDay    = [&] { return clock; };
    host.setTimeOfDay = [&](float h) { clock = h; };
    std::vector<assistant::Host::Road> laidRoads;
    host.createRoad = [&](const assistant::Host::Road& r) {
        laidRoads.push_back(r);
        return static_cast<int>(laidRoads.size()) - 1;
    };
    host.roads = [&] {
        std::vector<std::string> out;
        for (const auto& r : laidRoads) out.push_back(r.name + ": " + std::to_string(r.points.size()) + " points");
        return out;
    };
    host.prefabs      = [] { return std::vector<std::pair<std::string, std::string>>{}; };
    host.placePrefab  = [](const std::string&, glm::vec3) { return -1; };
    assistantPanel.tools = assistant::makeTools(ed, host);
    assistantPanel.brief     = [&] { return assistant::brief(ed, host); };
    assistantPanel.reference = [&] { return assistant::reference(host); };
    assistantPanel.overview  = [&] { return assistant::overview(host); };
    assistantPanel.lookup    = [](const std::string& q) { return assistant::lookup(q, 6000); };
    scriptEditor.assist.ask = [&](const std::string& q) { assistantPanel.ask(q); };

    // A C# buffer with its own help, as the C# editor drives it (without the
    // language service: that is cseditorcheck's).
    CodeEditor csEd;
    CodeAssist csAssist;
    CodeAssistProbe CP{csAssist};
    CodeAssist::Doc csDoc;
    csDoc.ed        = &csEd;
    csDoc.language  = "csharp";
    csDoc.path      = "scripts/Mover.cs";
    csDoc.reference = CodeAssist::manual("csharp-scripting.md", "## 3.", "## 5.");
    csDoc.mono      = gui.monoFont();
    bool showCs = false;

    bool showScripts = true, showAssistant = false;
    auto draw = [&] {
        gui.beginFrame();
        if (showCs) {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(float(w), float(h)), ImGuiCond_Always);
            ImGui::Begin("C#");
            const bool took = csAssist.keys(csDoc, false);
            if (gui.monoFont()) ImGui::PushFont(gui.monoFont());
            if (took) csEd.SetHandleKeyboardInputs(false);
            csEd.Render("##cs");
            csEd.SetHandleKeyboardInputs(true);
            if (gui.monoFont()) ImGui::PopFont();
            csAssist.after(csDoc, false);
            ImGui::End();
        }
        if (showScripts) {
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(showAssistant ? 900.0f : float(w), float(h)), ImGuiCond_Always);
            scriptEditor.visible = true;
            scriptEditor.panel(scripts, gui);
        }
        ImGui::SetNextWindowPos(ImVec2(showScripts ? 900.0f : 0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(showScripts ? 600.0f : float(w), float(h)), ImGuiCond_Always);
        assistantPanel.visible = showAssistant;
        assistantPanel.panel(gui);
        gui.endFrame();
    };
    auto frame = [&] {
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        gpu.setViewport(0, 0, fw, fh);
        gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
        gpu.clear(fitzel::rhi::ClearColor);
        draw();
        window.swapBuffers();
    };
    auto shot = [&](const std::string& name) {
        window.pollEvents();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        gpu.setViewport(0, 0, fw, fh);
        gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
        gpu.clear(fitzel::rhi::ClearColor);
        draw();
        std::vector<unsigned char> px(static_cast<std::size_t>(fw) * fh * 4);
        gpu.readPixels({}, 0, fitzel::rhi::Rect{0, 0, fw, fh}, fitzel::rhi::Format::RGBA8, px.data());
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
            if (c == '\n') {
                io.AddKeyEvent(ImGuiKey_Enter, true);
                frame();
                io.AddKeyEvent(ImGuiKey_Enter, false);
            } else {
                io.AddInputCharacter(static_cast<unsigned>(static_cast<unsigned char>(c)));
            }
            frames(1);
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
    auto click = [&](ImVec2 p) {
        io.AddMousePosEvent(p.x, p.y);
        frame();
        io.AddMouseButtonEvent(0, true);
        frame();
        io.AddMouseButtonEvent(0, false);
        frames(2);
    };

    // --- Is there a model? --------------------------------------------------------
    std::printf("server %s\n", oc.settings.url.c_str());
    oc.refreshModels();
    pump([&] { oc.poll(); return !oc.modelsPending(); }, 10.0);
    if (oc.models().empty()) {
        std::printf("no model server to test against (%s)\n", oc.modelsError().c_str());
        return 3;
    }
    std::printf("chat model %s, code model %s\n", oc.chatModel().c_str(), oc.codeModel().c_str());

    // --- Probe: questions from a file, one fresh conversation each, the
    // transcripts printed -- for judging what the model knows, not a check.
    if (argc > 4 && std::string(argv[3]) == "probe") {
        AssistantPanelProbe PP{assistantPanel};
        showScripts   = false;
        showAssistant = true;
        std::ifstream in(argv[4]);
        std::string q;
        while (std::getline(in, q)) {
            if (q.empty() || q[0] == '#') continue;
            PP.clear();
            assistantPanel.ask(q);
            const auto t0 = std::chrono::steady_clock::now();
            pump([&] { return !assistantPanel.busy(); }, 300.0);
            const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf("==== %.1f s%s", sec, PP.transcript().c_str());
            if (!openedPanel.empty()) std::printf("  {panel opened: %s}\n", openedPanel.c_str());
            if (!weatherSet.empty()) std::printf("  {weather: %s, clock %.1f}\n", weatherSet.c_str(), clock);
            for (const auto& r : laidRoads) {
                std::printf("  {road '%s':", r.name.c_str());
                for (std::size_t k = 0; k < r.points.size(); ++k)
                    std::printf(" (%.0f,%.0f%s)", r.points[k].x, r.points[k].y,
                                r.lifts[k] != 0.0f ? (" +" + std::to_string((int)r.lifts[k])).c_str() : "");
                for (const auto& b : r.bridges) std::printf(" bridge %d-%d", b.first, b.second);
                std::printf("}\n");
            }
            laidRoads.clear();
            openedPanel.clear();
            weatherSet.clear();
            std::printf("\n");
        }
        return 0;
    }

    // --- The Lua editor ------------------------------------------------------------
    std::printf("Lua editor\n");
    {
        std::ofstream out(work / "scripts" / "mover.lua");
        out << "-- mover.lua: the player's box, driven with the keys\n"
               "local speed = 5\n"
               "\n"
               "function update(e, dt, t)\n"
               "end\n";
    }
    scriptEditor.open("mover.lua");
    frames(4);
    CodeEditor& code = SP.ed();
    // Focus the text: a click into it.
    click(ImVec2(400.0f, 200.0f));
    CHECK(code.IsFocused(), "the Lua buffer has the keyboard");
    CHECK(code.GetLanguageDefinition().mName == "Lua", "it reads Lua (%s)", code.GetLanguageDefinition().mName.c_str());

    // Ctrl+/ comments the Lua way.
    code.SetCursorPosition(CodeEditor::Coordinates(1, 0));
    frames(1);
    key(ImGuiKey_Slash, 1);
    CHECK(code.GetLineText(1) == "-- local speed = 5", "Ctrl+/ comments with -- ('%s')", code.GetLineText(1).c_str());
    key(ImGuiKey_Slash, 1);
    CHECK(code.GetLineText(1) == "local speed = 5", "and takes it away again ('%s')", code.GetLineText(1).c_str());

    // A suggestion as typing pauses, and Tab.
    code.SetCursorPosition(CodeEditor::Coordinates(3, (int)code.GetLineText(3).size()));   // after "function update(e, dt, t)"
    frames(1);
    type("\n    if game.keyDown(KEY_W) then\n        ");
    std::printf("  after typing: %s\n", AP.state().c_str());
    const bool asked = pump([&] { return AP.requesting() || code.HasGhostText(); }, 3.0);
    std::printf("  after the pause: %s\n", AP.state().c_str());
    CHECK(asked, "the pause asked the model");
    const bool ghost = pump([&] { return code.HasGhostText(); }, 30.0);
    CHECK(ghost, "a suggestion shows at the caret ('%s')", AP.ghost().substr(0, 70).c_str());
    shot("01_suggestion.png");
    if (ghost) {
        const std::string g = code.GetGhostText();
        const int ln = code.GetCursorPosition().mLine;
        key(ImGuiKey_Tab);
        CHECK(!code.HasGhostText(), "Tab takes it");
        const std::string first = g.substr(0, g.find('\n'));
        CHECK(code.GetLineText(ln).find(first) != std::string::npos, "it is in the text ('%s')",
              code.GetLineText(ln).c_str());
        CHECK(code.GetText().find("game.") != std::string::npos, "and uses the engine's API");
        shot("02_taken.png");
        key(ImGuiKey_Z, 1);
        CHECK(code.GetLineText(ln).find(first) == std::string::npos || first.empty(), "one Ctrl+Z takes it back");
    }
    // Esc lets a suggestion go.
    key(ImGuiKey_Backslash, 4);
    if (pump([&] { return code.HasGhostText(); }, 30.0)) {
        key(ImGuiKey_Escape);
        CHECK(!code.HasGhostText(), "Esc lets a suggestion go");
    }

    // Ctrl+I: the whole update function, an instruction, a preview, Accept.
    {
        std::ofstream out(work / "scripts" / "mover.lua");
        out << "-- mover.lua: the player's box, driven with the keys\n"
               "local speed = 5\n"
               "\n"
               "function update(e, dt, t)\n"
               "    if game.keyDown(KEY_W) then\n"
               "        e.z = e.z - speed * dt\n"
               "    end\n"
               "end\n";
    }
    scriptEditor.open("mover.lua");
    frames(2);
    click(ImVec2(400.0f, 200.0f));
    code.SetSelection(CodeEditor::Coordinates(3, 0), CodeEditor::Coordinates(7, 3));
    frames(1);
    key(ImGuiKey_I, 1);
    CHECK(AP.editOpen(), "Ctrl+I opens the instruction box");
    type("also move back with S, left with A and right with D");
    key(ImGuiKey_Enter);
    CHECK(AP.editRunning() || AP.editResult(), "Enter sends it");
    const bool wrote = pump([&] { return AP.editResult() || !AP.editError().empty(); }, 120.0);
    CHECK(wrote && AP.editError().empty(), "the model wrote the change (%s)", AP.editError().c_str());
    shot("03_ctrl_i_preview.png");
    if (wrote && AP.editError().empty()) {
        key(ImGuiKey_Enter, 1);
        CHECK(!AP.editOpen(), "Ctrl+Enter accepts");
        const std::string t = code.GetText();
        CHECK(t.find("KEY_S") != std::string::npos && t.find("KEY_A") != std::string::npos &&
                  t.find("KEY_D") != std::string::npos,
              "the function now has S, A and D");
        CHECK(t.find("local speed = 5") != std::string::npos, "and the rest of the file is as it was");
        CHECK(t.find("<<<") == std::string::npos && t.find("```") == std::string::npos, "with no markers or fences");
        shot("04_ctrl_i_accepted.png");
        std::printf("----\n%s----\n", t.c_str());
        key(ImGuiKey_Z, 1);
        CHECK(code.GetText().find("KEY_D") == std::string::npos, "one Ctrl+Z takes the whole change back");
    }

    // --- C# -----------------------------------------------------------------------------
    std::printf("C#\n");
    showScripts = false;
    showCs      = true;
    CHECK(!csDoc.reference.empty(), "the C# manual is beside the exe");
    csEd.SetText("using Fitzel;\n"
                 "\n"
                 "public class Mover : Behaviour\n"
                 "{\n"
                 "    public float Speed = 5f;\n"
                 "\n"
                 "    void Update()\n"
                 "    {\n"
                 "    }\n"
                 "}\n");
    frames(3);
    click(ImVec2(400.0f, 200.0f));
    csEd.SetCursorPosition(CodeEditor::Coordinates(7, 5));   // after the {
    frames(1);
    type("\nif (Input.GetKey(Key.W))\n");
    const bool csGhost = pump([&] { return csEd.HasGhostText(); }, 30.0);
    CHECK(csGhost, "a C# suggestion shows ('%s')", CP.ghost().substr(0, 70).c_str());
    shot("04b_csharp_suggestion.png");
    if (csGhost) {
        key(ImGuiKey_Tab);
        CHECK(!csEd.HasGhostText() && csEd.GetText().find(CP.ghost().substr(0, 5)) != std::string::npos,
              "Tab takes it");
        std::printf("----\n%s----\n", csEd.GetText().c_str());
    }
    showCs      = false;
    showScripts = true;

    // --- The Assistant ---------------------------------------------------------------
    if (argc > 3 && std::string(argv[3]) == "editor") {
        std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
        return g_fail ? 1 : 0;
    }
    std::printf("Assistant\n");
    showScripts   = false;
    showAssistant = true;
    frames(3);
    shot("05_assistant_empty.png");
    assistantPanel.ask("Make a red box called Crate at x=3, y=1, z=0 that is 2 metres on each side, "
                       "and give it a Spin component.");
    const bool done1 = pump([&] { return !assistantPanel.busy(); }, 240.0);
    CHECK(done1, "the turn ended");
    {
        const Entity* crate = nullptr;
        for (const Entity& e : entities)
            if (e.name == "Crate") crate = &e;
        CHECK(crate != nullptr, "there is a Crate");
        if (crate) {
            CHECK(crate->type == EntityType::Box, "a box");
            CHECK(glm::length(crate->localCenter - glm::vec3(3.0f, 1.0f, 0.0f)) < 0.01f, "at 3,1,0 (%.2f %.2f %.2f)",
                  crate->localCenter.x, crate->localCenter.y, crate->localCenter.z);
            CHECK(glm::length(crate->half - glm::vec3(1.0f)) < 0.01f, "2 m on each side (half %.2f)", crate->half.x);
            const auto* mc = crate->components.get<MaterialComponent>();
            const MaterialDef* md = nullptr;
            if (mc) for (const MaterialDef& m : materials) if (m.assetId == mc->material) md = &m;
            CHECK(md && md->albedo.r > 0.5f && md->albedo.g < 0.3f && md->albedo.b < 0.3f, "red (%s)",
                  md ? md->name.c_str() : "no material");
            CHECK(crate->components.get<SpinComponent>() != nullptr, "spinning");
        }
    }
    shot("06_assistant_crate.png");

    assistantPanel.ask("Write a Lua script bob.lua that makes an object bob up and down by half a metre, "
                       "and attach it to the Crate.");
    const bool done2 = pump([&] { return !assistantPanel.busy(); }, 300.0);
    CHECK(done2, "the second turn ended");
    {
        std::ifstream in(work / "scripts" / "bob.lua");
        std::stringstream ss;
        ss << in.rdbuf();
        CHECK(in.good() || !ss.str().empty(), "bob.lua was written");
        CHECK(ss.str().find("function update") != std::string::npos, "with an update function");
        CHECK(ss.str().find("e.y") != std::string::npos && ss.str().find("e.position") == std::string::npos,
              "moving the object through e.y, the engine's own field");
        // e is a new table on every call: a field of the script's own on it is lost.
        std::string own;
        {
            static const char* fields[] = {"x", "y", "z", "rx", "ry", "rz", "sx", "sy", "sz"};
            const std::string src = ss.str();
            for (std::size_t at = src.find("e."); at != std::string::npos; at = src.find("e.", at + 2)) {
                if (at > 0 && (std::isalnum((unsigned char)src[at - 1]) || src[at - 1] == '_' || src[at - 1] == '.')) continue;
                std::size_t k = at + 2;
                while (k < src.size() && (std::isalnum((unsigned char)src[k]) || src[k] == '_')) ++k;
                const std::string f = src.substr(at + 2, k - at - 2);
                std::size_t q = k;
                while (q < src.size() && src[q] == ' ') ++q;
                const bool assigned = q < src.size() && src[q] == '=' && (q + 1 >= src.size() || src[q + 1] != '=');
                bool known = false;
                for (const char* x : fields) known |= f == x;
                if (assigned && !known) own = f;
            }
        }
        CHECK(own.empty(), "keeping its own state in variables, not on e (e.%s)", own.c_str());
        bool attached = false;
        for (const Entity& e : entities)
            if (e.name == "Crate")
                for (const auto& c : e.components.items)
                    if (auto* s = dynamic_cast<const ScriptComponent*>(c.get())) attached |= s->file == "bob.lua";
        CHECK(attached, "and hung on the Crate");
        std::printf("----\n%s----\n", ss.str().c_str());
    }
    shot("07_assistant_script.png");

    // Everything it did undoes.
    while (history.canUndo()) history.undo(document);
    bool gone = true;
    for (const Entity& e : entities) gone &= e.name != "Crate";
    CHECK(gone, "undo takes the Crate away again");

    std::printf("%s (%d failed)\n", g_fail ? "FAILED" : "PASSED", g_fail);
    return g_fail ? 1 : 0;
}
