// csharpcheck: the C# script backend end to end, without the editor.
//
//   build/release/bin/csharpcheck.exe [project dir]
//   build/linux/bin/csharpcheck [project dir]
//
// Makes a C# project the way File > New Project does (csproject::create), adds
// test scripts, builds it with `dotnet build` through the editor's background
// builder, starts .NET in this process (CSharpScripts) and runs the scripts
// against real Entity objects and a ScriptHost stub: Start/Update, inspector
// fields and their overrides, the transform written in place, spawning, input,
// the HUD, an exception disabling only its own script, OnDestroy, a rebuilt
// assembly replacing the old one at the next Play, and a compile error coming
// back as file + line. Prints one line per check; exit code 0 = all passed.
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <nlohmann/json.hpp>

#include "CSharpHost.hpp"
#include "CSharpProject.hpp"
#include "Component.hpp"
#include "ScriptSystem.hpp"

namespace fs = std::filesystem;

namespace {

int g_failed = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("[%s] %s%s%s\n", ok ? " ok " : "FAIL", what, detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_failed;
}

void write(const fs::path& p, const std::string& text) {
    std::ofstream out(p, std::ios::binary);
    out << text;
}

const char* kProbe = R"CS(using Fitzel;
using System.Numerics;

public class Probe : Behaviour
{
    public int Frames;
    [SerializeField] string label = "probe";
    public Vector3 Offset = new Vector3(1, 2, 3);
    public Color Tint = Color.Red;
    [HideInInspector] public float Hidden = 7f;
    bool started;

    void Start()
    {
        started = true;
        Debug.Log($"Probe.Start on {Name} ({label})");
        var spinner = Entity.Find("Spinner");
        if (spinner == null) throw new System.Exception("Spinner not found");
        Entity.Spawn(new SpawnInfo { Name = "spawned", Type = EntityType.Box, Position = new Vector3(5, 0, 0) });
    }

    void Update()
    {
        if (!started) throw new System.Exception("Update before Start");
        Frames++;
        Position = Offset * Frames;
        if (Input.GetKey(Key.W)) Name = "w-held";
        Hud.DrawText(10, 10, $"frame {Frames}", 24, Color.White);
    }

    void OnDestroy()
    {
        Entity.Find("Spinner").Name = "bye";
    }
}
)CS";

const char* kBad = R"CS(using Fitzel;
using System.Numerics;

public class Bad : Behaviour
{
    void Update()
    {
        Position += Vector3.UnitX;
        throw new System.InvalidOperationException("on purpose");
    }
}
)CS";

// The rest of the API, through Lua's own functions: one line per call into a
// report file the check reads back ("name ok" / "name FAIL detail").
const char* kApiProbe = R"CS(using Fitzel;
using System;
using System.Collections.Generic;
using System.IO;
using System.Numerics;

public class ApiProbe : Behaviour
{
    public string Report = "";
    List<string> lines = new List<string>();

    void Check(string name, Func<bool> test)
    {
        try { lines.Add(name + (test() ? " ok" : " FAIL")); }
        catch (Exception e) { lines.Add(name + " FAIL " + e.GetType().Name + ": " + e.Message.Replace('\n', ' ')); }
    }

    void Start()
    {
        Check("entities", () => Entity.All().Count == 4);
        Check("entityInfo", () => Entity.Find("Spinner").Info.Name == "Spinner" && Entity.Find("Spinner").Info.Script == "Rotator.cs");
        Check("children", () => Entity.Find("Spinner").Children.Count == 1 && Entity.Find("Spinner").Children[0].Name == "ApiProbe");
        Check("setLight/getLight", () => {
            var s = Entity.Find("Spinner");
            s.Light = new LightSettings { Intensity = 7.5f, Color = new Color(1, 0.5f, 0.25f), Type = LightType.Spot };
            var l = s.Light;
            return l != null && Math.Abs(l.Intensity.Value - 7.5f) < 1e-4 && Math.Abs(l.Color.Value.G - 0.5f) < 1e-4 && l.Type == LightType.Spot;
        });
        Check("createMaterial", () => Materials.Create(new MaterialEdit { Name = "Lava", Emission = new Color(1, 0.3f, 0), EmissionStrength = 2 }) == "guid-Lava");
        Check("materialInfo", () => { var m = Materials.Info("guid-Lava"); return m != null && m.Name == "Lava" && Math.Abs(m.Emission.G - 0.3f) < 1e-4; });
        Check("saveData/loadData", () => {
            Game.SaveData("probe", new Dictionary<string, object> { ["level"] = 3, ["name"] = "Fitz", ["pos"] = new Vector3(1, 2, 3) });
            var d = Game.LoadData<Dictionary<string, System.Text.Json.JsonElement>>("probe");
            return d["level"].GetInt32() == 3 && d["name"].GetString() == "Fitz" && d["pos"][2].GetSingle() == 3f;
        });
        Check("shared", () => { Shared.Set("hp", 42); return Shared.Get<int>("hp") == 42; });
        Check("shared from Lua", () => Shared.Get<string>("fromLua") == "hello");
        Check("synth.note", () => Synth.Note("A4") == 69 && Synth.Note("nonsense") == null);
        Check("net.clock", () => Net.Clock > 0);
        Check("worldToHud", () => { var p = Hud.WorldToHud(new Vector3(1, 2, 3)); return p.HasValue && p.Value.X == 10 && p.Value.Y == 20; });
        Check("lua error -> exception", () => {
            try { LuaApi.Call("game", "getLight"); return false; }
            catch (InvalidOperationException e) { return e.Message.Contains("bad argument"); }
        });
        Check("unknown function", () => {
            try { LuaApi.Call("game", "noSuchThing"); return false; }
            catch (InvalidOperationException e) { return e.Message.Contains("does not exist"); }
        });
    }

    bool written;

    void Update()
    {
        // Hold the keys, every frame: the other C# script must not see W.
        bool held = Input.Capture();
        if (written) return;
        written = true;
        lines.Add("capture " + (held ? "ok" : "FAIL"));
        File.WriteAllLines(Report, lines);
    }
}
)CS";

const char* kKeyWatcher = R"CS(using Fitzel;

public class KeyWatcher : Behaviour
{
    void Update() { Name = Input.GetKey(Key.W) ? "sees-w" : "blind"; }
}
)CS";

} // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // Next to the exe is where bin/dotnet/Fitzel.dll is, as for the editor.
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    fs::current_path(fs::path(exe).parent_path());
#else
    std::error_code cwdEc;
    const fs::path self = fs::read_symlink("/proc/self/exe", cwdEc);
    if (!cwdEc) fs::current_path(self.parent_path(), cwdEc);
#endif
    const fs::path dir = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "fitzel_csharpcheck";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string D = dir.generic_string();

    // --- The project, as the New Project wizard makes it -------------------------
    std::string err;
    check(csproject::create(D, "CheckGame", err), "create project", err);
    check(fs::exists(dir / "CheckGame.sln"), "solution written");
    check(csproject::isCSharp(D), "project is C#");
    check(fs::exists(dir / "scripts" / "Rotator.cs"), "first script written");
    check(fs::exists(dir / "Library" / "Fitzel.dll"), "API copied into Library/");
    write(dir / "scripts" / "Probe.cs", kProbe);
    write(dir / "scripts" / "Bad.cs", kBad);

    csproject::Builder b;
    b.start(D);
    b.wait();
    check(b.ok(), "dotnet build", b.ok() ? std::string() : b.log());
    const std::string dll = CSharpScripts::assemblyFor(D);
    check(fs::exists(dll), "Assembly-CSharp.dll built");
    check(!fs::exists(dir / "obj"), "no obj/ in the project root");

    // --- The scene and the host --------------------------------------------------
    std::vector<Entity> entities(3);
    const char* names[3] = {"Spinner", "Probe", "Bad"};
    const char* files[3] = {"Rotator.cs", "Probe.cs", "Bad.cs"};
    for (int i = 0; i < 3; ++i) {
        entities[i].id   = i + 1;
        entities[i].name = names[i];
        auto sc  = std::make_unique<ScriptComponent>();
        sc->file = files[i];
        if (i == 0) {
            ScriptParam p;
            p.name = "Speed";
            p.type = ScriptParam::Type::Number;
            p.num  = 90.0;
            sc->params.push_back(p);
        }
        entities[i].components.items.push_back(std::move(sc));
    }
    ScriptHost host;
    std::vector<ScriptSpawn> spawned;
    host.findEntity = [&](const std::string& n) {
        for (const Entity& e : entities) if (e.name == n) return e.id;
        return -1;
    };
    host.setName = [&](int id, const std::string& n) {
        for (Entity& e : entities) if (e.id == id) e.name = n;
    };
    host.spawn   = [&](const ScriptSpawn& s) { spawned.push_back(s); return 100 + static_cast<int>(spawned.size()); };
    host.keyDown = [](int k) { return k == 'W'; };
    host.log     = [](const std::string& s) { std::printf("      log: %s\n", s.c_str()); };

    CSharpScripts cs;
    cs.setEntities(&entities);
    cs.setHost(&host);
    cs.setAssembly(dll);

    // --- Inspector fields -------------------------------------------------------
    std::string scanErr;
    const auto rot = cs.scanParams("Rotator.cs", &scanErr);
    check(rot.size() == 1 && rot[0].name == "Speed" && std::abs(rot[0].num - 45.0) < 1e-6,
          "Rotator's fields: Speed = 45", scanErr);
    const auto probe = cs.scanParams("Probe.cs", &scanErr);
    std::string seen;
    for (const auto& p : probe) seen += p.name + "(" + std::to_string(static_cast<int>(p.type)) + ") ";
    check(probe.size() == 4, "Probe's fields: Frames, label, Offset, Tint (not Hidden)", seen);
    bool tintIsColor = false;
    for (const auto& p : probe)
        if (p.name == "Tint") tintIsColor = p.type == ScriptParam::Type::Color && p.vec.x == 1.0f;
    check(tintIsColor, "a Color field scans as a colour");
    cs.scanParams("Nope.cs", &scanErr);
    check(!scanErr.empty(), "a missing class says so", scanErr);

    // --- Play ---------------------------------------------------------------------
    cs.reset();
    auto frames = [&](int n) {
        for (int f = 0; f < n; ++f) {
            host.hudCmds.clear();
            for (Entity& e : entities)
                for (const auto& c : e.components.items)
                    if (auto* sc = dynamic_cast<ScriptComponent*>(c.get()))
                        cs.update(e, *sc, 0.1f, static_cast<float>(f) * 0.1f);
        }
    };
    frames(10);
    check(std::abs(entities[0].localRotation.y - 90.0f) < 1e-3f, "Rotator turned 90 deg/s for 1 s (card value)",
          std::to_string(entities[0].localRotation.y));
    check(std::abs(entities[1].localCenter.x - 10.0f) < 1e-4f && std::abs(entities[1].localCenter.z - 30.0f) < 1e-4f,
          "Probe wrote its position in place", std::to_string(entities[1].localCenter.x));
    check(spawned.size() == 1 && spawned[0].name == "spawned" && spawned[0].type == 0, "Start spawned a box");
    check(entities[1].name == "w-held", "Input.GetKey(W) reached the script");
    check(!host.hudCmds.empty() && host.hudCmds.back().text == "frame 10", "Hud.DrawText queued");
    check(std::abs(entities[2].localCenter.x - 1.0f) < 1e-4f, "an exception stops that script after one frame");
    check(cs.lastError().find("InvalidOperationException") != std::string::npos &&
              cs.lastError().find("Bad.cs") != std::string::npos,
          "the error names the exception and the file:line", cs.lastError().substr(0, 160));

    cs.removeEntity(2);
    check(entities[0].name == "bye", "OnDestroy ran when the object went");

    // --- A new build at the next Play ---------------------------------------------
    {
        std::ifstream in(dir / "scripts" / "Rotator.cs", std::ios::binary);
        std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const std::string from = "new Vector3(0f, Speed * Time.DeltaTime, 0f)";
        const std::size_t at = src.find(from);
        if (at != std::string::npos) src.replace(at, from.size(), "new Vector3(Speed * Time.DeltaTime, 0f, 0f)");
        write(dir / "scripts" / "Rotator.cs", src);
    }
    b.start(D);
    b.wait();
    check(b.ok(), "rebuild while the old assembly is loaded (no lock)", b.ok() ? std::string() : b.log());
    entities[0].localRotation = glm::vec3(0.0f);
    cs.reset();
    frames(10);
    check(std::abs(entities[0].localRotation.x - 90.0f) < 1e-3f && std::abs(entities[0].localRotation.y) < 1e-3f,
          "the next Play runs the new code", std::to_string(entities[0].localRotation.x));

    // --- The rest of the API, through Lua's own functions ------------------------
    // One runtime per process: the first scene's scripts go before the second
    // scene's load would drop them (and run their OnDestroy against it).
    cs.reset();
    write(dir / "scripts" / "ApiProbe.cs", kApiProbe);
    write(dir / "scripts" / "KeyWatcher.cs", kKeyWatcher);
    b.start(D);
    b.wait();
    check(b.ok(), "build with the API probe", b.ok() ? std::string() : b.log());
    {
        std::vector<Entity> scene(4);
        const char* n2[4] = {"Spinner", "ApiProbe", "Watcher", "Empty"};
        for (int i = 0; i < 4; ++i) { scene[i].id = 10 + i; scene[i].name = n2[i]; }
        scene[1].parent = 10;
        const fs::path report = dir / "apiprobe.txt";
        auto addScript = [&](Entity& e, const char* file, const std::string& reportPath) {
            auto sc = std::make_unique<ScriptComponent>();
            sc->file = file;
            if (!reportPath.empty()) {
                ScriptParam p;
                p.name = "Report";
                p.type = ScriptParam::Type::String;
                p.str  = reportPath;
                sc->params.push_back(p);
            }
            e.components.items.push_back(std::move(sc));
        };
        addScript(scene[1], "ApiProbe.cs", report.string());
        addScript(scene[2], "KeyWatcher.cs", "");
        addScript(scene[0], "Rotator.cs", "");

        ScriptHost h2;
        auto find = [&](int id) -> Entity* { for (Entity& e : scene) if (e.id == id) return &e; return nullptr; };
        h2.findEntity   = [&](const std::string& n) { for (Entity& e : scene) if (e.name == n) return e.id; return -1; };
        h2.allEntities  = [&] { std::vector<int> v; for (Entity& e : scene) v.push_back(e.id); return v; };
        h2.children     = [&](int id) { std::vector<int> v; for (Entity& e : scene) if (e.parent == id) v.push_back(e.id); return v; };
        h2.setName      = [&](int id, const std::string& n) { if (Entity* e = find(id)) e->name = n; };
        h2.keyDown      = [](int k) { return k == 'W'; };
        h2.entityInfo   = [&](int id, ScriptEntityInfo& out) {
            Entity* e = find(id);
            if (!e) return false;
            out.id = e->id; out.name = e->name; out.parent = e->parent;
            for (const auto& c : e->components.items)
                if (auto* sc = dynamic_cast<ScriptComponent*>(c.get())) { out.script = sc->file; break; }
            return true;
        };
        ScriptLightEdit light;
        light.color = glm::vec3(1.0f); light.intensity = 1.0f; light.range = 10.0f; light.type = 0;
        light.spotAngle = 30.0f; light.spotBlend = 0.2f; light.shadows = false; light.baked = false;
        h2.setLight = [&](int, const ScriptLightEdit& ed) {
            if (ed.color) light.color = ed.color; if (ed.intensity) light.intensity = ed.intensity;
            if (ed.type) light.type = ed.type;
            return true;
        };
        h2.getLight = [&](int, ScriptLightEdit& out) { out = light; return true; };
        ScriptMaterialInfo made;
        h2.createMaterial = [&](const ScriptMaterialEdit& ed) {
            made.id = "guid-" + ed.name.value_or("?");
            made.name = ed.name.value_or("");
            made.emission = ed.emission.value_or(glm::vec3(0.0f));
            return made.id;
        };
        h2.materialInfo = [&](const std::string& id, ScriptMaterialInfo& out) {
            if (id != made.id) return false;
            out = made;
            return true;
        };
        h2.worldToHud = [](glm::vec3, glm::vec2& out) { out = glm::vec2(10.0f, 20.0f); return true; };
        h2.saveGame = "csharpcheck";

        ScriptSystem lua;
        lua.setHost(&h2);
        {
            nlohmann::json out;
            lua.callApi("@shared", "set", nlohmann::json::array({"fromLua", "hello"}), "", out);
        }
        CSharpScripts cs2;
        cs2.setEntities(&scene);
        cs2.setHost(&h2);
        cs2.inputBlocked = [&] { return lua.inputBlocked(); };
        cs2.enterScript  = [&](const std::string& who) { lua.setCurrentScript(who); };
        cs2.callLua = [&](const std::string& t, const std::string& f, const std::string& a, const std::string& who,
                          std::string& result) {
            nlohmann::json args = nlohmann::json::parse(a, nullptr, false);
            nlohmann::json out;
            const bool ok = lua.callApi(t, f, args, who, out);
            result = out.is_string() ? out.get<std::string>()
                                     : out.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            return ok;
        };
        cs2.setAssembly(dll);
        cs2.reset();
        for (int f = 0; f < 2; ++f) {
            lua.beginFrame();
            for (Entity& e : scene)
                for (const auto& c : e.components.items)
                    if (auto* sc = dynamic_cast<ScriptComponent*>(c.get()))
                        cs2.update(e, *sc, 0.1f, 0.0f);
        }
        std::ifstream in(report);
        int lines = 0;
        for (std::string line; std::getline(in, line);) {
            ++lines;
            const bool ok = line.size() >= 3 && line.compare(line.size() - 3, 3, " ok") == 0;
            check(ok, ("api: " + line).c_str());
        }
        check(lines >= 15, "the API probe reported", std::to_string(lines) + " lines");
        check(scene[2].name == "blind", "a C# script holding the keys hides them from another", scene[2].name);
        if (!cs2.lastError().empty()) std::printf("      last C# error: %s\n", cs2.lastError().c_str());
        cs2.reset();
    }

    // --- A compile error -------------------------------------------------------------
    write(dir / "scripts" / "Broken.cs", "public class Broken : Fitzel.Behaviour\n{\n    void Update() { undefinedThing = 1; }\n}\n");
    b.start(D);
    b.wait();
    const auto msgs = b.messages();
    bool found = false;
    for (const auto& m : msgs)
        if (m.error && fs::path(m.file).filename() == "Broken.cs" && m.line == 3 && m.text.rfind("CS0103", 0) == 0)
            found = true;
    check(!b.ok() && found, "a compile error comes back as Broken.cs(3): CS0103",
          msgs.empty() ? b.log().substr(0, 300) : msgs[0].file + "(" + std::to_string(msgs[0].line) + "): " + msgs[0].text);

    cs.reset();
    std::printf("%s\n", g_failed ? "csharpcheck: FAILED" : "csharpcheck: all passed");
    return g_failed ? 1 : 0;
}
