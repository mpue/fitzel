#include "AssistantTools.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fitzel/core/LogCapture.hpp>

#include "CodeAssist.hpp"
#include "Command.hpp"
#include "Component.hpp"
#include "EditorContext.hpp"
#include "LuaCompletion.hpp"
#include "PropertyMeta.hpp"
#include "SceneGraph.hpp"
#include "SceneOps.hpp"

namespace fs = std::filesystem;
using nlohmann::json;

namespace assistant {

namespace {

// Numbers rounded for the model: it reads 1.25, not 1.2500000476837158.
double r3(float v) { return std::round(static_cast<double>(v) * 1000.0) / 1000.0; }
json vec(const glm::vec3& v) { return json::array({r3(v.x), r3(v.y), r3(v.z)}); }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool readVec(const json& args, const char* key, glm::vec3& out) {
    if (!args.contains(key)) return false;
    const json& v = args[key];
    if (v.is_array() && v.size() >= 3 && v[0].is_number() && v[1].is_number() && v[2].is_number()) {
        out = glm::vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
        return true;
    }
    if (v.is_object() && v.contains("x")) {
        out = glm::vec3(v.value("x", 0.0f), v.value("y", 0.0f), v.value("z", 0.0f));
        return true;
    }
    if (v.is_number()) {   // one number: the same on every axis (a size)
        out = glm::vec3(v.get<float>());
        return true;
    }
    return false;
}

// [r, g, b] in 0..1 (or 0..255), "#rrggbb", or a colour's name.
bool readColor(const json& v, glm::vec3& out) {
    if (v.is_array() && v.size() >= 3 && v[0].is_number()) {
        out = glm::vec3(v[0].get<float>(), v[1].get<float>(), v[2].get<float>());
        if (out.x > 1.0f || out.y > 1.0f || out.z > 1.0f) out /= 255.0f;
        out = glm::clamp(out, glm::vec3(0.0f), glm::vec3(1.0f));
        return true;
    }
    if (!v.is_string()) return false;
    std::string s = lower(v.get<std::string>());
    if (!s.empty() && s[0] == '#' && (s.size() == 7 || s.size() == 4)) {
        if (s.size() == 4) s = std::string("#") + s[1] + s[1] + s[2] + s[2] + s[3] + s[3];
        const unsigned long n = std::strtoul(s.c_str() + 1, nullptr, 16);
        out = glm::vec3((n >> 16) & 255, (n >> 8) & 255, n & 255) / 255.0f;
        return true;
    }
    static const std::pair<const char*, glm::vec3> named[] = {
        {"red", {0.8f, 0.1f, 0.08f}},   {"green", {0.15f, 0.6f, 0.2f}}, {"blue", {0.1f, 0.25f, 0.8f}},
        {"yellow", {0.95f, 0.8f, 0.1f}}, {"orange", {0.95f, 0.45f, 0.08f}}, {"purple", {0.45f, 0.15f, 0.6f}},
        {"pink", {0.95f, 0.5f, 0.65f}}, {"white", {0.92f, 0.92f, 0.92f}}, {"black", {0.03f, 0.03f, 0.03f}},
        {"grey", {0.5f, 0.5f, 0.5f}},   {"gray", {0.5f, 0.5f, 0.5f}},   {"brown", {0.4f, 0.24f, 0.12f}},
        {"cyan", {0.1f, 0.75f, 0.8f}},  {"magenta", {0.8f, 0.1f, 0.7f}}, {"gold", {1.0f, 0.77f, 0.34f}},
        {"silver", {0.78f, 0.78f, 0.8f}},
    };
    for (const auto& [name, c] : named)
        if (s == name) { out = c; return true; }
    return false;
}

// The object an argument names: an id, or a name -- exact, then without case,
// then the one name it is part of.
int resolve(EditorContext& ed, const json& v, std::string& err) {
    if (v.is_number_integer()) {
        const int id = v.get<int>();
        if (ed.document.find(id)) return id;
        err = "no object with id " + std::to_string(id);
        return -1;
    }
    if (v.is_string()) {
        const std::string s = v.get<std::string>();
        // "12" is an id too.
        if (!s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); }))
            if (ed.document.find(std::atoi(s.c_str()))) return std::atoi(s.c_str());
        for (const Entity& e : ed.entities)
            if (e.name == s) return e.id;
        const std::string ls = lower(s);
        for (const Entity& e : ed.entities)
            if (lower(e.name) == ls) return e.id;
        int found = -1, n = 0;
        for (const Entity& e : ed.entities)
            if (lower(e.name).find(ls) != std::string::npos) { found = e.id; ++n; }
        if (n == 1) return found;
        err = n > 1 ? "several objects are called something like \"" + s + "\" -- give the id"
                    : "no object called \"" + s + "\"";
        return -1;
    }
    err = "name the object by id or by name";
    return -1;
}

json brief(const Entity& e) {
    json j = {{"id", e.id}, {"name", e.name}, {"type", entityTypeName(e.type)}, {"position", vec(e.center)}};
    if (e.parent >= 0) j["parent"] = e.parent;
    if (!e.active) j["active"] = false;
    return j;
}

json describe(const Entity& e) {
    json j = {{"id", e.id},
              {"name", e.name},
              {"type", entityTypeName(e.type)},
              {"parent", e.parent},
              {"active", e.active},
              {"position", vec(e.localCenter)},
              {"world_position", vec(e.center)},
              {"rotation", vec(e.localRotation)},
              {"half_size", vec(e.half)}};
    json comps = json::array();
    for (const auto& c : e.components.items) {
        json props;
        c->save(props);
        comps.push_back({{"type", c->typeId()}, {"name", c->displayName()}, {"properties", props}});
    }
    j["components"] = comps;
    return j;
}

json fail(const std::string& why) { return json{{"error", why}}; }

EntityType typeFrom(const std::string& s, bool& ok) {
    const std::string t = lower(s);
    ok = true;
    if (t == "box" || t == "cube") return EntityType::Box;
    if (t == "ramp" || t == "wedge") return EntityType::Ramp;
    if (t == "cylinder") return EntityType::Cylinder;
    if (t == "sphere" || t == "ball") return EntityType::Sphere;
    if (t == "plane" || t == "floor") return EntityType::Plane;
    if (t == "light" || t == "lamp") return EntityType::Light;
    if (t == "empty" || t == "group") return EntityType::Empty;
    ok = false;
    return EntityType::Box;
}

// A material of that colour: the one already made for it, or a new one.
fitzel::AssetId colourMaterial(EditorContext& ed, const glm::vec3& c, float rough, float metal, float glow) {
    char name[96];
    std::snprintf(name, sizeof name, "#%02X%02X%02X", (int)std::lround(c.x * 255.0f),
                  (int)std::lround(c.y * 255.0f), (int)std::lround(c.z * 255.0f));
    std::string n = name;
    if (std::abs(rough - 0.5f) > 1e-3f) n += " rough " + std::to_string((int)std::lround(rough * 100.0f));
    if (metal > 1e-3f) n += " metal " + std::to_string((int)std::lround(metal * 100.0f));
    if (glow > 1e-3f) n += " glow " + std::to_string((int)std::lround(glow * 100.0f));
    for (const MaterialDef& m : ed.materials)
        if (m.name == n) return m.assetId;
    const fitzel::AssetId id = ed.document.addMaterial(n, c, metal, rough);
    if (glow > 1e-3f)
        for (MaterialDef& m : ed.materials)
            if (m.assetId == id) { m.emission = c; m.emissionStrength = glow; }
    return id;
}

void wear(Entity& e, const fitzel::AssetId& mat) {
    if (auto* mc = e.components.get<MaterialComponent>()) {
        mc->material = mat;
        return;
    }
    auto mc = std::make_unique<MaterialComponent>();
    mc->material = mat;
    e.components.items.push_back(std::move(mc));
}

// A file under scripts/, never outside it.
bool scriptPath(const Host& host, const std::string& file, fs::path& out, std::string& err) {
    if (file.empty()) { err = "give the file name"; return false; }
    const fs::path rel = fs::path(file).lexically_normal();
    if (rel.is_absolute() || rel.empty() || rel.begin()->string() == "..") {
        err = "a script is a path inside scripts/, like door.lua";
        return false;
    }
    fs::path p = rel;
    if (!p.empty() && p.begin()->string() == "scripts") p = p.lexically_relative("scripts");
    const std::string ext = lower(p.extension().string());
    if (ext != ".lua" && ext != ".cs") { err = "a script ends in .lua or .cs"; return false; }
    out = fs::path(host.scriptsDir()) / p;
    return true;
}

// The Lua manual's chapters that mention every word of the question.
std::string searchManual(const std::string& name, const std::string& query) {
    const std::string text = CodeAssist::manual(name);
    if (text.empty()) return std::string();
    std::vector<std::string> words;
    {
        std::istringstream ss(lower(query));
        std::string w;
        while (ss >> w) words.push_back(w);
    }
    // Chapters: from one heading to the next.
    std::vector<std::string> chapters;
    std::istringstream in(text);
    std::string line, cur;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '#' && !cur.empty()) { chapters.push_back(cur); cur.clear(); }
        cur += line + "\n";
    }
    if (!cur.empty()) chapters.push_back(cur);
    std::vector<std::pair<int, std::size_t>> hits;
    for (std::size_t i = 0; i < chapters.size(); ++i) {
        const std::string lc = lower(chapters[i]);
        int score = 0;
        bool all = true;
        for (const std::string& w : words) {
            std::size_t at = 0;
            int n = 0;
            while ((at = lc.find(w, at)) != std::string::npos) { ++n; at += w.size(); }
            if (!n) all = false;
            score += n;
        }
        if (all && score > 0) hits.push_back({score, i});
    }
    std::sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::string out;
    for (std::size_t k = 0; k < hits.size() && out.size() < 9000; ++k) {
        std::string ch = chapters[hits[k].second];
        if (ch.size() > 5000) ch = ch.substr(0, 5000) + "\n[...]\n";
        out += ch + "\n";
    }
    return out;
}

json objArg() { return {{"type", {"integer", "string"}}, {"description", "the object's id, or its name"}}; }
json vec3Arg(const char* what) {
    return {{"type", "array"}, {"items", {{"type", "number"}}}, {"minItems", 3}, {"maxItems", 3},
            {"description", what}};
}
json schema(json props, std::vector<std::string> required = {}) {
    json s = {{"type", "object"}, {"properties", std::move(props)}};
    if (!required.empty()) s["required"] = required;
    return s;
}

} // namespace

std::string brief(EditorContext& ed, const Host& host) {
    std::string s;
    const std::string file = host.projectFile ? host.projectFile() : std::string();
    s += "Scene: " + (file.empty() ? std::string("(not saved yet)") : fs::path(file).filename().string()) + ", " +
         std::to_string(ed.entities.size()) + " objects. ";
    if (ed.sel.valid()) {
        const Entity& e = ed.entities[ed.sel.index()];
        s += "Selected: \"" + e.name + "\" (id " + std::to_string(e.id) + ", " + entityTypeName(e.type) + "). ";
    } else {
        s += "Nothing selected. ";
    }
    s += std::string("Scripts are written in ") + (host.csharp && host.csharp() ? "C#" : "Lua") + ". ";
    s += host.playing && host.playing() ? "The game is running (Play)." : "The editor is not playing.";
    return s;
}

std::string reference(const Host& host) {
    std::string s = host.csharp && host.csharp() ? CodeAssist::manual("csharp-scripting.md", "## 3.", "## 5.")
                                                 : luacomplete::apiSummary();
    // What add_component takes: a built-in one often does what a script would.
    s += "\nComponents (add_component type: name): ";
    bool first = true;
    for (const components::TypeInfo& ti : components::registry()) {
        if (!ti.addable) continue;
        s += (first ? "" : ", ") + ti.typeId + ": " + ti.displayName;
        first = false;
    }
    return s + "\n";
}

std::vector<Tool> makeTools(EditorContext& edRef, const Host& host) {
    EditorContext* edp = &edRef;
    std::vector<Tool> t;
    auto add = [&](const char* name, const char* desc, json params, bool changes,
                   std::function<json(EditorContext&, const json&)> fn) {
        t.push_back({name, desc, std::move(params), changes,
                     [edp, fn](const json& a) { return fn(*edp, a.is_object() ? a : json::object()); }});
    };

    // --- Looking ---------------------------------------------------------------------
    add("list_objects",
        "List the scene's objects: id, name, type, world position (metres, Y up), parent. "
        "Optional filter: part of a name or a type.",
        schema({{"filter", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}}), false,
        [](EditorContext& ed, const json& a) {
            const std::string f = lower(a.value("filter", std::string()));
            const int limit = std::clamp(a.value("limit", 150), 1, 1000);
            json out = json::array();
            int total = 0;
            for (const Entity& e : ed.entities) {
                if (!f.empty() && lower(e.name).find(f) == std::string::npos &&
                    lower(entityTypeName(e.type)) != f)
                    continue;
                ++total;
                if ((int)out.size() < limit) out.push_back(brief(e));
            }
            return json{{"objects", out}, {"count", total}};
        });

    add("get_object",
        "Everything about one object: transform (position and rotation are relative to the parent; "
        "rotation in degrees; half_size is half the extent on each axis), and every component with "
        "its properties.",
        schema({{"object", objArg()}}, {"object"}), false,
        [](EditorContext& ed, const json& a) {
            std::string err;
            const int id = resolve(ed, a.value("object", json()), err);
            if (id < 0) return fail(err);
            return describe(*ed.document.find(id));
        });

    add("get_selection", "The objects selected in the editor right now.", schema(json::object()), false,
        [](EditorContext& ed, const json&) {
            json out = json::array();
            for (int id : ed.sel.ids())
                if (const Entity* e = ed.document.find(id)) out.push_back(brief(*e));
            return json{{"selected", out}};
        });

    // --- Making and changing ---------------------------------------------------------
    add("create_object",
        "Make a new object. type: Box, Sphere, Cylinder, Ramp, Plane, Light (a point light) or Empty "
        "(a group to parent things under). position: world [x,y,z] in metres, Y up (default: in front "
        "of the camera). half_size: HALF the extent per axis, so [0.5,0.5,0.5] is a 1 m cube "
        "(default [1,1,1]). rotation: degrees [x,y,z]. color: [r,g,b] 0..1, \"#rrggbb\" or a name. "
        "Returns the new object's id.",
        schema({{"type", {{"type", "string"},
                          {"enum", {"Box", "Sphere", "Cylinder", "Ramp", "Plane", "Light", "Empty"}}}},
                {"name", {{"type", "string"}}},
                {"position", vec3Arg("world position [x,y,z]")},
                {"rotation", vec3Arg("degrees [x,y,z]")},
                {"half_size", vec3Arg("half extents [x,y,z]")},
                {"color", {{"description", "[r,g,b] 0..1, \"#rrggbb\" or a colour name"}}},
                {"parent", objArg()}},
               {"type"}),
        true,
        [host](EditorContext& ed, const json& a) {
            bool ok = false;
            const EntityType type = typeFrom(a.value("type", std::string()), ok);
            if (!ok) return fail("type must be Box, Sphere, Cylinder, Ramp, Plane, Light or Empty");
            int parent = -1;
            if (a.contains("parent") && !a["parent"].is_null()) {
                std::string err;
                parent = resolve(ed, a["parent"], err);
                if (parent < 0) return fail(err);
            }
            glm::vec3 pos = host.spawnPoint ? host.spawnPoint() : glm::vec3(0.0f);
            glm::vec3 rot(0.0f), half(1.0f);
            readVec(a, "position", pos);
            readVec(a, "rotation", rot);
            const bool sized = readVec(a, "half_size", half);
            half = glm::max(glm::abs(half), glm::vec3(0.01f));
            // A primitive given no position sits ON the ground, not half in it.
            if (!a.contains("position") && isSolidPrimitive(type) && type != EntityType::Plane) pos.y += half.y;
            const int id = sceneops::spawnChild(ed, parent, type, pos, rot, half);
            // The name and colour: a second step, so undo can take them alone.
            const std::vector<int> ids{id};
            auto before = ed.snapshot(ids);
            Entity* e = ed.document.find(id);
            if (!e) return fail("the object was not made");
            if (a.contains("name") && a["name"].is_string() && !a["name"].get<std::string>().empty())
                e->name = a["name"].get<std::string>();
            glm::vec3 col;
            if (a.contains("color") && readColor(a["color"], col)) {
                if (auto* lc = e->components.get<LightComponent>()) lc->color = col;
                else if (isSolidPrimitive(type)) wear(*e, colourMaterial(ed, col, 0.5f, 0.0f, 0.0f));
            }
            (void)sized;
            ed.commitEdit(std::move(before), ids, "Name and colour");
            ed.sel.select(id);
            scenegraph::resolve(ed.entities);
            return json{{"created", brief(*ed.document.find(id))}};
        });

    add("modify_object",
        "Change an object: name, position and rotation (relative to its parent -- for an object at "
        "the root that is the world), half_size, active (shown and running or not), parent (an "
        "object, or -1 for the root; the object keeps its place in the world). Only what is given "
        "changes. offset adds to the position instead of setting it.",
        schema({{"object", objArg()},
                {"name", {{"type", "string"}}},
                {"position", vec3Arg("[x,y,z]")},
                {"offset", vec3Arg("[dx,dy,dz] added to the position")},
                {"rotation", vec3Arg("degrees [x,y,z]")},
                {"half_size", vec3Arg("half extents [x,y,z]")},
                {"active", {{"type", "boolean"}}},
                {"parent", objArg()}},
               {"object"}),
        true,
        [](EditorContext& ed, const json& a) {
            std::string err;
            const int id = resolve(ed, a.value("object", json()), err);
            if (id < 0) return fail(err);
            int newParent = -2;
            if (a.contains("parent")) {
                const json& p = a["parent"];
                if (p.is_null() || (p.is_number() && p.get<int>() < 0)) newParent = -1;
                else {
                    newParent = resolve(ed, p, err);
                    if (newParent < 0) return fail(err);
                    if (sceneops::isUnder(ed.entities, newParent, id))
                        return fail("that would put the object under itself");
                }
            }
            const std::vector<int> ids{id};
            auto before = ed.snapshot(ids);
            Entity& e = *ed.document.find(id);
            if (a.contains("name") && a["name"].is_string()) e.name = a["name"].get<std::string>();
            glm::vec3 v;
            if (readVec(a, "position", v)) e.localCenter = v;
            if (readVec(a, "offset", v)) e.localCenter += v;
            if (readVec(a, "rotation", v)) e.localRotation = v;
            if (readVec(a, "half_size", v)) e.half = glm::max(glm::abs(v), glm::vec3(0.01f));
            if (a.contains("active") && a["active"].is_boolean()) e.active = a["active"].get<bool>();
            if (newParent != -2 && newParent != e.parent) {
                scenegraph::resolve(ed.entities);
                Entity& me = *ed.document.find(id);
                const glm::vec3 wp = me.center, wr = me.rotation;
                me.parent = newParent;
                const glm::mat4 pw = scenegraph::parentWorld(ed.entities, me);
                scenegraph::setWorld(me, wp, wr, newParent >= 0 ? &pw : nullptr);
            }
            ed.commitEdit(std::move(before), ids, "Assistant edit");
            scenegraph::resolve(ed.entities);
            return json{{"object", describe(*ed.document.find(id))}};
        });

    add("delete_object", "Delete an object and everything under it (undo brings it back).",
        schema({{"object", objArg()}}, {"object"}), true,
        [](EditorContext& ed, const json& a) {
            std::string err;
            const int id = resolve(ed, a.value("object", json()), err);
            if (id < 0) return fail(err);
            const Entity* e = ed.document.find(id);
            if (e->type == EntityType::Sun) return fail("the sun cannot be deleted");
            const std::string name = e->name;
            sceneops::deleteEntity(ed, ed.document.indexOf(id));
            scenegraph::resolve(ed.entities);
            return json{{"deleted", name}};
        });

    add("duplicate_object", "Copy an object (with its children) beside it. Returns the copy.",
        schema({{"object", objArg()}}, {"object"}), true,
        [](EditorContext& ed, const json& a) {
            std::string err;
            const int id = resolve(ed, a.value("object", json()), err);
            if (id < 0) return fail(err);
            sceneops::duplicateEntity(ed, ed.document.indexOf(id));
            scenegraph::resolve(ed.entities);
            const int copy = ed.sel.activeId();
            const Entity* c = ed.document.find(copy);
            return c && copy != id ? json{{"copy", brief(*c)}} : fail("not copied");
        });

    add("set_color",
        "Give a solid object (Box, Sphere, Cylinder, Ramp, Plane) a colour -- a material of that "
        "colour, shared by everything given the same one. For a Light: its light colour. roughness "
        "0 polished .. 1 matt (default 0.5), metallic 0..1, glow: emission strength (0 = none).",
        schema({{"object", objArg()},
                {"color", {{"description", "[r,g,b] 0..1, \"#rrggbb\" or a colour name"}}},
                {"roughness", {{"type", "number"}}},
                {"metallic", {{"type", "number"}}},
                {"glow", {{"type", "number"}}}},
               {"object", "color"}),
        true,
        [](EditorContext& ed, const json& a) {
            std::string err;
            const int id = resolve(ed, a.value("object", json()), err);
            if (id < 0) return fail(err);
            glm::vec3 col;
            if (!a.contains("color") || !readColor(a["color"], col)) return fail("color: [r,g,b] 0..1 or \"#rrggbb\"");
            const std::vector<int> ids{id};
            auto before = ed.snapshot(ids);
            Entity& e = *ed.document.find(id);
            if (auto* lc = e.components.get<LightComponent>()) {
                lc->color = col;
            } else if (isSolidPrimitive(e.type)) {
                const fitzel::AssetId m =
                    colourMaterial(ed, col, std::clamp(a.value("roughness", 0.5f), 0.0f, 1.0f),
                                   std::clamp(a.value("metallic", 0.0f), 0.0f, 1.0f),
                                   std::max(a.value("glow", 0.0f), 0.0f));
                wear(*ed.document.find(id), m);
            } else {
                return fail(std::string("a ") + entityTypeName(e.type) + " has no surface to colour");
            }
            ed.commitEdit(std::move(before), ids, "Colour");
            return json{{"ok", true}};
        });

    // --- Components --------------------------------------------------------------------
    add("list_component_types",
        "The components an object can be given (physics, scripts, lights, sounds, ...): their type "
        "ids and properties.",
        schema(json::object()), false, [](EditorContext&, const json&) {
            json out = json::array();
            for (const components::TypeInfo& ti : components::registry()) {
                if (!ti.addable) continue;
                json props = json::array();
                if (auto c = ti.make()) {
                    for (const Property& p : c->props()) {
                        static const char* kinds[] = {"text", "number", "vec3", "color", "bool", "enum", "int"};
                        json pj = {{"key", p.key}, {"kind", kinds[static_cast<int>(p.kind)]}};
                        if (!p.enumLabels.empty()) pj["choices"] = p.enumLabels;
                        props.push_back(pj);
                    }
                }
                out.push_back({{"type", ti.typeId}, {"name", ti.displayName}, {"properties", props}});
            }
            return json{{"component_types", out}};
        });

    add("add_component",
        "Give an object a component (see list_component_types), with properties to set on it. "
        "An enum property takes the index of its choice. For a script use attach_script.",
        schema({{"object", objArg()}, {"type", {{"type", "string"}}}, {"properties", {{"type", "object"}}}},
               {"object", "type"}),
        true,
        [](EditorContext& ed, const json& a) {
            std::string err;
            const int id = resolve(ed, a.value("object", json()), err);
            if (id < 0) return fail(err);
            std::string type = a.value("type", std::string());
            // The type id, or the name the Inspector shows ("Spin" for spin).
            for (const components::TypeInfo& ti : components::registry())
                if (lower(ti.typeId) == lower(type) || lower(ti.displayName) == lower(type)) type = ti.typeId;
            auto c = components::create(type);
            if (!c) return fail("no component type \"" + type + "\" (see list_component_types)");
            if (a.contains("properties") && a["properties"].is_object()) {
                json cur;
                c->save(cur);
                cur.merge_patch(a["properties"]);
                c->load(cur);
            }
            const std::vector<int> ids{id};
            auto before = ed.snapshot(ids);
            ed.document.find(id)->components.items.push_back(std::move(c));
            ed.commitEdit(std::move(before), ids, "Add component");
            return json{{"object", describe(*ed.document.find(id))}};
        });

    add("set_component",
        "Change properties of a component an object has (type as in get_object). Only the given "
        "properties change.",
        schema({{"object", objArg()}, {"type", {{"type", "string"}}}, {"properties", {{"type", "object"}}}},
               {"object", "type", "properties"}),
        true,
        [](EditorContext& ed, const json& a) {
            std::string err;
            const int id = resolve(ed, a.value("object", json()), err);
            if (id < 0) return fail(err);
            const std::string type = a.value("type", std::string());
            const std::vector<int> ids{id};
            auto before = ed.snapshot(ids);
            Entity& e = *ed.document.find(id);
            for (auto& c : e.components.items) {
                if (c->typeId() != type) continue;
                json cur;
                c->save(cur);
                if (a.contains("properties") && a["properties"].is_object()) cur.merge_patch(a["properties"]);
                c->load(cur);
                ed.commitEdit(std::move(before), ids, "Component");
                json now;
                c->save(now);
                return json{{"properties", now}};
            }
            return fail("the object has no \"" + type + "\" component");
        });

    add("remove_component", "Take a component off an object (the first of that type).",
        schema({{"object", objArg()}, {"type", {{"type", "string"}}}}, {"object", "type"}), true,
        [](EditorContext& ed, const json& a) {
            std::string err;
            const int id = resolve(ed, a.value("object", json()), err);
            if (id < 0) return fail(err);
            const std::string type = a.value("type", std::string());
            const std::vector<int> ids{id};
            auto before = ed.snapshot(ids);
            auto& items = ed.document.find(id)->components.items;
            for (auto it = items.begin(); it != items.end(); ++it) {
                if ((*it)->typeId() != type) continue;
                items.erase(it);
                ed.commitEdit(std::move(before), ids, "Remove component");
                return json{{"ok", true}};
            }
            return fail("the object has no \"" + type + "\" component");
        });

    // --- The editor ------------------------------------------------------------------
    add("select", "Select an object in the editor and turn the view to it.",
        schema({{"object", objArg()}}, {"object"}), false,
        [host](EditorContext& ed, const json& a) {
            std::string err;
            const int id = resolve(ed, a.value("object", json()), err);
            if (id < 0) return fail(err);
            ed.sel.select(id);
            if (host.frame) host.frame(id);
            return json{{"selected", id}};
        });

    add("undo", "Take back the last change(s) in the scene (steps, default 1).",
        schema({{"steps", {{"type", "integer"}}}}), true, [](EditorContext& ed, const json& a) {
            const int n = std::clamp(a.value("steps", 1), 1, 50);
            for (int i = 0; i < n; ++i) ed.history.undo(ed.document);
            ed.sel.normalize();
            scenegraph::resolve(ed.entities);
            return json{{"undone", n}};
        });

    add("redo", "Do again what undo took back (steps, default 1).", schema({{"steps", {{"type", "integer"}}}}),
        true, [](EditorContext& ed, const json& a) {
            const int n = std::clamp(a.value("steps", 1), 1, 50);
            for (int i = 0; i < n; ++i) ed.history.redo(ed.document);
            ed.sel.normalize();
            scenegraph::resolve(ed.entities);
            return json{{"redone", n}};
        });

    add("play", "Start the game (Play) to try the scene and its scripts.", schema(json::object()), false,
        [host](EditorContext&, const json&) {
            if (host.playing && host.playing()) return json{{"ok", true}, {"note", "already playing"}};
            if (host.play) host.play();
            return json{{"ok", true}, {"note", "Play starts on the next frame; read_log shows what the scripts print"}};
        });

    add("stop", "Stop the game and go back to editing (the scene is as before Play).", schema(json::object()),
        false, [host](EditorContext&, const json&) {
            if (host.stop) host.stop();
            return json{{"ok", true}};
        });

    add("save_scene", "Save the scene to its file.", schema(json::object()), true,
        [host](EditorContext&, const json&) {
            if (!host.saveScene || !host.saveScene()) return fail(host.playing && host.playing() ? "the game is running -- stop it first"
                                                           : "the scene has no file yet -- save it once from the File menu");
            return json{{"ok", true}};
        });

    // --- Scripts -----------------------------------------------------------------------
    add("list_scripts", "The project's scripts (.lua and .cs under scripts/).", schema(json::object()), false,
        [host](EditorContext&, const json&) {
            json out = json::array();
            std::error_code ec;
            const fs::path root(host.scriptsDir());
            for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                const std::string ext = lower(it->path().extension().string());
                if (ext == ".lua" || ext == ".cs") out.push_back(it->path().lexically_relative(root).generic_string());
            }
            std::sort(out.begin(), out.end());
            return json{{"scripts", out}};
        });

    add("read_script", "Read a script file (a path under scripts/, like door.lua).",
        schema({{"file", {{"type", "string"}}}}, {"file"}), false,
        [host](EditorContext&, const json& a) {
            fs::path p;
            std::string err;
            if (!scriptPath(host, a.value("file", std::string()), p, err)) return fail(err);
            std::ifstream in(p, std::ios::binary);
            if (!in) return fail("no such script: " + a.value("file", std::string()));
            std::stringstream ss;
            ss << in.rdbuf();
            std::string s = ss.str();
            if (s.size() > 40000) s = s.substr(0, 40000) + "\n[... cut]";
            return json{{"file", a.value("file", std::string())}, {"content", s}};
        });

    add("write_script",
        "Write a script file under scripts/ (a new one, or the whole of an existing one replaced). "
        "Lua: define start(e) and/or update(e, dt, t); C#: a class deriving from Behaviour. Read "
        "scripting_reference first if unsure of the API. Then attach_script hangs it on an object.",
        schema({{"file", {{"type", "string"}, {"description", "e.g. spinner.lua"}}},
                {"content", {{"type", "string"}, {"description", "the whole file"}}}},
               {"file", "content"}),
        true,
        [host](EditorContext&, const json& a) {
            fs::path p;
            std::string err;
            const std::string file = a.value("file", std::string());
            if (!scriptPath(host, file, p, err)) return fail(err);
            std::error_code ec;
            const bool existed = fs::exists(p, ec);
            fs::create_directories(p.parent_path(), ec);
            std::ofstream out(p, std::ios::binary);
            if (!out) return fail("cannot write " + p.generic_string());
            out << a.value("content", std::string());
            out.close();
            if (lower(p.extension().string()) == ".lua" && host.reloadScripts) host.reloadScripts();
            if (host.openScript) host.openScript(p.lexically_relative(host.scriptsDir()).generic_string());
            return json{{"ok", true}, {"file", file}, {"replaced", existed}};
        });

    add("attach_script",
        "Hang a script (a file under scripts/) on an object: it runs on that object in Play. "
        "params sets the script's parameters (its module-level globals) for this object.",
        schema({{"object", objArg()}, {"file", {{"type", "string"}}}, {"params", {{"type", "object"}}}},
               {"object", "file"}),
        true,
        [host](EditorContext& ed, const json& a) {
            std::string err;
            const int id = resolve(ed, a.value("object", json()), err);
            if (id < 0) return fail(err);
            fs::path p;
            if (!scriptPath(host, a.value("file", std::string()), p, err)) return fail(err);
            std::error_code ec;
            if (!fs::exists(p, ec)) return fail("no such script -- write_script it first");
            const std::string rel = p.lexically_relative(host.scriptsDir()).generic_string();
            const std::vector<int> ids{id};
            auto before = ed.snapshot(ids);
            Entity& e = *ed.document.find(id);
            ScriptComponent* sc = nullptr;
            for (auto& c : e.components.items)
                if (auto* s = dynamic_cast<ScriptComponent*>(c.get()); s && s->file == rel) sc = s;
            if (!sc) {
                auto c = std::make_unique<ScriptComponent>();
                c->file = rel;
                sc = c.get();
                e.components.items.push_back(std::move(c));
            }
            if (a.contains("params") && a["params"].is_object()) {
                for (auto it = a["params"].begin(); it != a["params"].end(); ++it) {
                    ScriptParam sp;
                    sp.name = it.key();
                    const json& v = it.value();
                    glm::vec3 v3;
                    if (v.is_boolean()) { sp.type = ScriptParam::Type::Bool; sp.b = v.get<bool>(); }
                    else if (v.is_number()) { sp.type = ScriptParam::Type::Number; sp.num = v.get<double>(); }
                    else if (v.is_string()) { sp.type = ScriptParam::Type::String; sp.str = v.get<std::string>(); }
                    else if (readVec(json{{"v", v}}, "v", v3)) { sp.type = ScriptParam::Type::Vec3; sp.vec = v3; }
                    else continue;
                    auto same = std::find_if(sc->params.begin(), sc->params.end(),
                                             [&](const ScriptParam& q) { return q.name == sp.name; });
                    if (same != sc->params.end()) *same = sp;
                    else sc->params.push_back(sp);
                }
            }
            ed.commitEdit(std::move(before), ids, "Attach script");
            return json{{"ok", true}, {"object", id}, {"file", rel}};
        });

    add("script_errors", "The last error a Lua script raised (in Play), if any.", schema(json::object()), false,
        [host](EditorContext&, const json&) {
            const std::string e = host.scriptError ? host.scriptError() : std::string();
            return json{{"error", e.empty() ? json() : json(e)}};
        });

    add("read_log",
        "The editor's log (what scripts print, errors, warnings): the newest lines. Optional "
        "filter: only lines containing that text.",
        schema({{"lines", {{"type", "integer"}}}, {"filter", {{"type", "string"}}}}), false,
        [](EditorContext&, const json& a) {
            std::vector<fitzel::logcap::Line> got;
            fitzel::logcap::linesSince(0, got);
            const int n = std::clamp(a.value("lines", 40), 1, 400);
            const std::string f = lower(a.value("filter", std::string()));
            std::vector<std::string> keep;
            for (auto it = got.rbegin(); it != got.rend() && (int)keep.size() < n; ++it)
                if (f.empty() || lower(it->text).find(f) != std::string::npos) keep.push_back(it->text);
            std::reverse(keep.begin(), keep.end());
            return json{{"lines", keep}};
        });

    add("scripting_reference",
        "The scripting API. Without a query: the summary of every Lua function (game.*, synth.*, "
        "the entity table e). With a query (words): the chapters of the Lua manual (German) that "
        "mention them -- or, for C#, of the C# manual.",
        schema({{"query", {{"type", "string"}}}, {"language", {{"type", "string"}, {"enum", {"lua", "csharp"}}}}}),
        false,
        [host](EditorContext&, const json& a) {
            const bool cs = a.contains("language") ? a.value("language", std::string()) == "csharp"
                                                   : (host.csharp && host.csharp());
            const std::string q = a.value("query", std::string());
            if (cs) {
                const std::string s = q.empty() ? CodeAssist::manual("csharp-scripting.md", "## 3.", "## 5.")
                                                : searchManual("csharp-scripting.md", q);
                return s.empty() ? fail("nothing found") : json{{"reference", s}};
            }
            if (q.empty()) return json{{"reference", luacomplete::apiSummary()}};
            const std::string s = searchManual("lua-scripting.md", q);
            return s.empty() ? fail("no chapter mentions all of: " + q) : json{{"reference", s}};
        });

    return t;
}

} // namespace assistant
