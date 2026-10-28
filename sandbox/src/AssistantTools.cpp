#include "AssistantTools.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/core/LogCapture.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include "CodeAssist.hpp"
#include "Command.hpp"
#include "Component.hpp"
#include "EditorContext.hpp"
#include "LuaCompletion.hpp"
#include "ModelLibrary.hpp"
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
    json v = args[key];
    if (v.is_string()) {   // "[1, 2, 3]": the array a model wrote as text
        const json p = json::parse(v.get<std::string>(), nullptr, false);
        if (!p.is_discarded()) v = p;
    }
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

// [r, g, b] in 0..1 (or 0..255), "#rrggbb", or a colour's name -- also when a
// model wrapped any of them in a string ("[0.5,0.5,0.5]", "\"#808080\"").
bool readColor(const json& v, glm::vec3& out) {
    if (v.is_string()) {
        std::string t = v.get<std::string>();
        while (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') || (t.front() == '\'' && t.back() == '\'')))
            t = t.substr(1, t.size() - 2);
        if (!t.empty() && t.front() == '[') {
            const json arr = json::parse(t, nullptr, false);
            if (arr.is_array()) return readColor(arr, out);
        }
        if (t != v.get<std::string>()) return readColor(json(t), out);
    }
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

// The chapters (heading to heading) of the manuals that best fit a question:
// the most of its words first, then the most mentions. Words of three letters
// and fewer are left out -- "die", "the", "wie" match everything.
// The words of a question as search terms: split at anything that is not a
// letter or a digit, short and common words left out, the rest cut to a stem
// ("exportiere" finds "Export"), and the German words the manuals' English
// half calls something else given their English name too.
std::vector<std::vector<std::string>> searchTerms(const std::string& query) {
    static const char* stop[] = {
        "fitzel", "eine", "einen", "einem", "einer", "mein", "meine", "meinem", "meinen", "mich", "dass",
        "wenn", "will", "muss", "kann", "soll", "wird", "werden", "habe", "haben", "sein", "sind", "auch",
        "noch", "oder", "aber", "alle", "alles", "diese", "dieser", "dieses", "nach", "durch", "bitte",
        "mache", "machen", "möchte", "with", "that", "this", "what", "have", "from", "into", "your", "does",
        "there", "which", "where", "when", "make", "want", "need"};
    static const std::pair<const char*, const char*> english[] = {
        {"stra\xC3\x9F", "road"}, {"br\xC3\xBC" "ck", "bridge"}, {"baum", "tree"}, {"b\xC3\xA4um", "tree"},
        {"wetter", "weather"}, {"regen", "rain"}, {"figur", "character"}, {"spieler", "player"},
        {"gel\xC3\xA4nd", "terrain"}, {"stadt", "town"}, {"st\xC3\xA4" "dt", "town"}, {"wasser", "water"},
        {"fluss", "river"}, {"fl\xC3\xBCss", "river"}, {"licht", "light"}, {"lampe", "light"},
        {"kamera", "camera"}, {"fahrz", "vehicle"}, {"t\xC3\xBCr", "door"}, {"himmel", "sky"},
        {"nebel", "fog"}, {"musik", "music"}, {"skript", "script"}, {"laufen", "walk"}, {"textur", "texture"},
        {"wolke", "cloud"}, {"schatten", "shadow"}, {"partikel", "particle"}, {"physik", "physics"},
        {"modell", "model"}, {"wald", "forest"}, {"meer", "ocean"}, {"boot", "boat"}, {"verkehr", "traffic"},
        {"stra\xC3\x9F" "enbahn", "tram"}, {"dritt", "third"}, {"exporti", "export"}, {"spiel", "game"},
        {"tunnel", "tunnel"}, {"gras", "grass"}, {"ger\xC3\xA4usch", "sound"}, {"klang", "sound"},
    };
    // One group a word: its stem and its English name -- either found is the
    // word found, so "Stadt" counts once whether a chapter says Stadt or town.
    std::vector<std::vector<std::string>> out;
    std::string w;
    auto flush = [&] {
        if (w.size() > 3 && std::find(std::begin(stop), std::end(stop), w) == std::end(stop)) {
            std::vector<std::string> g;
            // A stem: six bytes, not ending inside a character.
            std::string stem = w;
            if (stem.size() > 6) {
                std::size_t cut = 6;
                while (cut < stem.size() && (static_cast<unsigned char>(stem[cut]) & 0xC0) == 0x80) ++cut;
                stem = stem.substr(0, cut);
            }
            g.push_back(stem);
            for (const auto& [de, en] : english)
                if (w.rfind(de, 0) == 0 && std::find(g.begin(), g.end(), en) == g.end()) g.push_back(en);
            bool dup = false;
            for (const auto& o : out) dup |= o.front() == g.front();
            if (!dup) out.push_back(std::move(g));
        }
        w.clear();
    };
    for (char c : lower(query)) {
        if (std::isalnum(static_cast<unsigned char>(c)) || static_cast<unsigned char>(c) >= 0x80) w += c;
        else flush();
    }
    flush();
    return out;
}

// The chapters (heading to heading) of the manuals that best fit a question:
// the most of its words first, then the densest -- a long chapter that names
// everything once is not the answer to anything.
std::string searchDocs(const std::vector<std::string>& files, const std::string& query, std::size_t budget,
                       int minDistinct = 1, std::size_t maxChapters = 5) {
    const std::vector<std::vector<std::string>> words = searchTerms(query);
    if (words.empty()) return std::string();
    struct Hit { int distinct, count; std::string text, file; };
    std::vector<Hit> hits;
    for (const std::string& f : files) {
        const std::string text = readManual(f);
        std::istringstream in(text);
        std::string line, cur;
        auto chapter = [&] {
            if (cur.empty()) return;
            const std::string lc = lower(cur);
            int distinct = 0, count = 0;
            for (const std::vector<std::string>& g : words) {
                int n = 0;
                for (const std::string& w : g)
                    for (std::size_t at = 0; (at = lc.find(w, at)) != std::string::npos; at += w.size()) ++n;
                if (n) ++distinct;
                count += std::min(n, 6);
            }
            if (distinct >= std::min<int>(minDistinct, static_cast<int>(words.size())))
                hits.push_back({distinct, count, cur, f});
            cur.clear();
        };
        bool fenced = false;
        while (std::getline(in, line)) {
            if (line.rfind("```", 0) == 0) fenced = !fenced;
            if (!fenced && !line.empty() && line[0] == '#') chapter();
            cur += line + "\n";
        }
        chapter();
    }
    auto density = [](const Hit& h) { return h.count / (1.0 + h.text.size() / 1500.0); };
    std::sort(hits.begin(), hits.end(), [&](const Hit& a, const Hit& b) {
        return a.distinct != b.distinct ? a.distinct > b.distinct : density(a) > density(b);
    });
    std::string out;
    for (std::size_t k = 0; k < hits.size() && k < maxChapters && out.size() < budget; ++k) {
        std::string ch = hits[k].text;
        if (ch.size() > 3500) ch = ch.substr(0, 3500) + "\n[...]\n";
        if (out.size() + ch.size() > budget && !out.empty()) break;
        out += "[" + hits[k].file + "]\n" + ch + "\n";
    }
    return out;
}

// What a component is, from the header that defines it: the comment above its
// class and the members with theirs -- written for people, and exactly right.
std::string componentSource(const std::string& typeId) {
    static const char* headers[] = {"Component.hpp", "HouseGen.hpp", "Modifiers.hpp", "ProcGraph.hpp",
                                    "StairGen.hpp", "StreetSign.hpp", "TownTraffic.hpp"};
    const std::string needle = "return \"" + typeId + "\";";
    for (const char* h : headers) {
        const std::string text = readManual(h);
        if (text.empty()) continue;
        std::vector<std::string> lines;
        {
            std::istringstream in(text);
            std::string l;
            while (std::getline(in, l)) lines.push_back(l);
        }
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find("typeId()") == std::string::npos || lines[i].find(needle) == std::string::npos) continue;
            std::size_t cls = i;
            while (cls > 0 && lines[cls].rfind("class ", 0) != 0 && lines[cls].rfind("struct ", 0) != 0) --cls;
            std::size_t top = cls;
            while (top > 0 && lines[top - 1].rfind("//", 0) == 0) --top;
            std::string out;
            for (std::size_t k = top; k < i && out.size() < 6000; ++k) {
                // The members and their comments; the plumbing below them is not the model's.
                if (lines[k].find("clone()") != std::string::npos) break;
                out += lines[k] + "\n";
            }
            return out;
        }
    }
    return std::string();
}

// The game.* names a script may call: what completion offers and what the
// manual names (luadoccheck keeps the manual complete).
const std::vector<std::string>& gameNames() {
    static const std::vector<std::string> names = [] {
        std::vector<std::string> n;
        auto scan = [&](const std::string& text) {
            for (std::size_t at = text.find("game."); at != std::string::npos; at = text.find("game.", at + 5)) {
                std::size_t k = at + 5;
                while (k < text.size() && (std::isalnum(static_cast<unsigned char>(text[k])) || text[k] == '_')) ++k;
                if (k > at + 5) n.push_back(text.substr(at + 5, k - at - 5));
            }
        };
        scan(luacomplete::apiSummary());
        scan(readManual("lua-scripting.md"));
        std::sort(n.begin(), n.end());
        n.erase(std::unique(n.begin(), n.end()), n.end());
        return n;
    }();
    return names;
}

// What is wrong with a Lua script before it runs: what the compiler says, and
// the mistakes a model makes with this engine -- fields e does not have, state
// kept on e (a new table every call), game functions that do not exist.
std::vector<std::string> luaProblems(const std::string& src) {
    std::vector<std::string> out;
    lua_State* L = luaL_newstate();
    if (L) {
        if (luaL_loadbuffer(L, src.data(), src.size(), "=script") != LUA_OK)
            out.push_back(std::string("syntax: ") + lua_tostring(L, -1));
        lua_close(L);
    }
    static const char* fields[] = {"x", "y", "z", "rx", "ry", "rz", "sx", "sy", "sz",
                                   "name", "id", "type", "parent", "active"};
    auto ident = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    // What the script calls the object's table: start's or update's first
    // parameter (e by convention; self, entity ... by habit from elsewhere).
    std::string me = "e";
    for (const char* fn : {"function update(", "function start("}) {
        const std::size_t at = src.find(fn);
        if (at == std::string::npos) continue;
        std::size_t k = at + std::strlen(fn);
        while (k < src.size() && src[k] == ' ') ++k;
        std::size_t q = k;
        while (q < src.size() && ident(src[q])) ++q;
        if (q > k) { me = src.substr(k, q - k); break; }
    }
    const std::string meDot = me + ".";
    std::vector<std::string> seen;
    for (const char* owner : {meDot.c_str(), "game."}) {
        const std::size_t ol = std::strlen(owner);
        for (std::size_t at = src.find(owner); at != std::string::npos; at = src.find(owner, at + ol)) {
            if (at > 0 && (ident(src[at - 1]) || src[at - 1] == '.' || src[at - 1] == ':')) continue;
            // Not inside a comment: no "--" earlier on the line.
            const std::size_t bol = src.rfind('\n', at);
            const std::string head = src.substr(bol == std::string::npos ? 0 : bol + 1,
                                                at - (bol == std::string::npos ? 0 : bol + 1));
            if (head.find("--") != std::string::npos) continue;
            std::size_t k = at + ol;
            while (k < src.size() && ident(src[k])) ++k;
            const std::string f = src.substr(at + ol, k - at - ol);
            if (f.empty() || std::find(seen.begin(), seen.end(), owner + f) != seen.end()) continue;
            seen.push_back(owner + f);
            if (owner == meDot.c_str()) {
                bool known = false;
                for (const char* x : fields) known |= f == x;
                std::size_t q = k;
                while (q < src.size() && src[q] == ' ') ++q;
                const bool assigned = q < src.size() && src[q] == '=' && (q + 1 >= src.size() || src[q + 1] != '=');
                if (!known)
                    out.push_back(meDot + f + (assigned ? " = ...: the object's table is a new one on every call, so this "
                                                          "is lost by the next frame -- keep it in a local declared at "
                                                          "the top of the file"
                                                        : ": the object's table has no field " + f +
                                                              " (it has x y z, rx ry rz, sx sy sz, name, id, type, parent, active)"));
            } else if (!std::isupper(static_cast<unsigned char>(f[0])) &&
                       !std::binary_search(gameNames().begin(), gameNames().end(), f)) {
                out.push_back("game." + f + " does not exist (look it up with scripting_reference)");
            }
        }
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

std::string readManual(const std::string& name) {
    // Beside the exe (the build copies them there), or in a source tree.
    for (const std::string& p : {name, "docs/" + name, "docs/components/" + name, "sandbox/src/" + name}) {
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        return ss.str();
    }
    return std::string();
}

std::string lookup(const std::string& question, std::size_t budget) {
    // A component named in the question: its definition first -- the manuals
    // mention most of them in passing, the header says what they do.
    std::string out;
    const std::string q = " " + lower(question) + " ";
    int named = 0;
    for (const components::TypeInfo& ti : components::registry()) {
        if (!ti.addable || named >= 2) continue;
        bool hit = false;
        for (const std::string& n : {lower(ti.typeId), lower(ti.displayName)}) {
            const std::size_t at = q.find(n);
            hit |= n.size() > 3 && at != std::string::npos &&
                   !std::isalnum(static_cast<unsigned char>(q[at - 1])) &&
                   !std::isalnum(static_cast<unsigned char>(q[at + n.size()]));
        }
        if (!hit) continue;
        std::string src = componentSource(ti.typeId);
        if (src.empty()) continue;
        if (src.size() > 3000) src = src.substr(0, 3000) + "\n[...]\n";
        out += "[component " + ti.typeId + " (\"" + ti.displayName + "\"), its definition]\n" + src + "\n";
        ++named;
    }
    const std::size_t rest = budget > out.size() ? budget - out.size() : 0;
    if (rest > 1500)
        out += searchDocs({"FEATURES.md", "README.md", "lua-scripting.md", "csharp-scripting.md", "assistant.md"},
                          question, rest, 2, 3);
    return out;
}

std::string overview(const Host& host) {
    // A line a feature: its bold title and the start of what follows.
    std::string s;
    std::istringstream in(readManual("FEATURES.md"));
    std::string line;
    bool skip = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("## ", 0) == 0) {
            // What is not there yet is not the model's to offer.
            skip = line.find("Offen") != std::string::npos || line.find("Nice to have") != std::string::npos;
            if (!skip) s += "\n" + line.substr(3) + ":\n";
            continue;
        }
        if (skip || line.rfind("- **", 0) != 0) continue;
        const std::size_t end = line.find("**", 4);
        if (end == std::string::npos) continue;
        std::string title = line.substr(4, end - 4);
        while (!title.empty() && (title.back() == '.' || title.back() == ':')) title.pop_back();
        std::string rest;
        for (char c : line.substr(end + 2))
            if (c != '*' && c != '`') rest += c;
        const std::size_t a = rest.find_first_not_of(" :.,");
        rest = a == std::string::npos ? std::string() : rest.substr(a);
        if (rest.size() > 100) rest = rest.substr(0, rest.rfind(' ', 100)) + " ...";
        s += "- " + title + (rest.empty() ? "" : ": " + rest) + "\n";
    }
    if (host.panels) {
        s += "\nThe editor's panels (View menu, by group; open_panel opens one): ";
        std::string group;
        for (const std::string& p : host.panels()) {
            const std::size_t gt = p.find(" > ");
            const std::string g = gt == std::string::npos ? std::string() : p.substr(0, gt);
            const std::string n = gt == std::string::npos ? p : p.substr(gt + 3);
            if (g != group) { s += (group.empty() ? "" : "; ") + g + ": "; group = g; }
            else s += ", ";
            s += n;
        }
        s += ". Always open: Hierarchy, Inspector, the 3D view; File > Export for a game build.\n";
    }
    return s;
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
            const std::string content = a.value("content", std::string());
            out << content;
            out.close();
            const bool lua = lower(p.extension().string()) == ".lua";
            if (lua && host.reloadScripts) host.reloadScripts();
            if (host.openScript) host.openScript(p.lexically_relative(host.scriptsDir()).generic_string());
            json res = {{"ok", true}, {"file", file}, {"replaced", existed}};
            // Written either way; what would go wrong in Play comes back as an
            // error, so the model writes it again rather than reporting success.
            if (lua) {
                const std::vector<std::string> problems = luaProblems(content);
                if (!problems.empty()) {
                    std::string msg = "written, but it will not work -- fix and write it again: ";
                    for (std::size_t i = 0; i < problems.size(); ++i) msg += (i ? "; " : "") + problems[i];
                    res["error"] = msg;
                }
            }
            return res;
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
                                                : searchDocs({"csharp-scripting.md"}, q, 9000);
                return s.empty() ? fail("nothing found") : json{{"reference", s}};
            }
            if (q.empty()) return json{{"reference", luacomplete::apiSummary()}};
            const std::string s = searchDocs({"lua-scripting.md"}, q, 9000);
            return s.empty() ? fail("no chapter mentions: " + q) : json{{"reference", s}};
        });

    add("search_docs",
        "Look something up in Fitzel's manuals: how a feature works, where it is in the editor, how "
        "to export, what a panel does. The README (English), the feature list, the Lua and C# "
        "manuals and the assistant's (German). Give a few keywords in English and German "
        "(e.g. \"road bridge Straße Brücke\"). Use it before saying Fitzel cannot do something.",
        schema({{"query", {{"type", "string"}}}}, {"query"}), false,
        [](EditorContext&, const json& a) {
            const std::string s = searchDocs({"README.md", "FEATURES.md", "lua-scripting.md", "csharp-scripting.md",
                                              "assistant.md"},
                                             a.value("query", std::string()), 10000);
            return s.empty() ? fail("nothing in the manuals mentions that") : json{{"found", s}};
        });

    add("read_example",
        "Read one of the example scripts that come with Fitzel -- whole games and the usual pieces: "
        "walker.lua (a figure walked in third person), door.lua, inventory.lua, weapon.lua, "
        "orbit.lua, spin.lua, shooter.lua, scroller.lua, chess.lua, sokoban.lua, shmup.lua, "
        "arkanoid.lua, pinball.lua, invaders.lua ... The comments at their top say how to set the "
        "scene up for them. Without a name: the list. Copy one into the project with write_script.",
        schema({{"name", {{"type", "string"}}}}), false,
        [](EditorContext&, const json& a) {
            std::error_code ec;
            fs::path dir;
            for (const char* d : {"scripts", "sandbox/scripts"})
                if (fs::exists(fs::path(d) / "walker.lua", ec)) { dir = d; break; }
            if (dir.empty()) return fail("the examples are not beside the editor");
            std::string name = a.value("name", std::string());
            if (name.empty()) {
                json out = json::array();
                for (const auto& de : fs::directory_iterator(dir, ec))
                    if (de.path().extension() == ".lua") out.push_back(de.path().filename().string());
                std::sort(out.begin(), out.end());
                return json{{"examples", out}};
            }
            if (fs::path(name).extension().empty()) name += ".lua";
            name = fs::path(name).filename().string();   // never outside the folder
            std::ifstream in(dir / name, std::ios::binary);
            if (!in) return fail("no example called " + name);
            std::stringstream ss;
            ss << in.rdbuf();
            std::string src = ss.str();
            if (src.size() > 24000) src = src.substr(0, 24000) + "\n-- [... cut]";
            return json{{"name", name}, {"content", src}};
        });

    add("describe_component",
        "What a component does and what each of its properties means: its definition with the "
        "comments the engine's authors wrote, its properties' ranges and choices, and the values a "
        "new one starts with. Ask before adding one you are not sure about.",
        schema({{"type", {{"type", "string"}, {"description", "type id (\"physics\") or name (\"Physics\")"}}}},
               {"type"}),
        false,
        [](EditorContext&, const json& a) {
            const std::string type = a.value("type", std::string());
            const components::TypeInfo* info = nullptr;
            for (const components::TypeInfo& ti : components::registry())
                if (lower(ti.typeId) == lower(type) || lower(ti.displayName) == lower(type)) info = &ti;
            if (!info) return fail("no component type \"" + type + "\" (see list_component_types)");
            json out = {{"type", info->typeId}, {"name", info->displayName}};
            if (auto c = info->make()) {
                json defaults;
                c->save(defaults);
                out["new_one"] = defaults;
                json props = json::array();
                static const char* kinds[] = {"text", "number", "vec3", "color", "bool", "enum", "int"};
                for (const Property& p : c->props()) {
                    json pj = {{"key", p.key}, {"label", p.label}, {"kind", kinds[static_cast<int>(p.kind)]}};
                    if (p.max > p.min) pj["range"] = {p.min, p.max};
                    if (!p.enumLabels.empty()) pj["choices"] = p.enumLabels;   // set by index
                    props.push_back(pj);
                }
                out["properties"] = props;
            }
            const std::string src = componentSource(info->typeId);
            if (!src.empty()) out["definition"] = src;
            return out;
        });

    // --- Assets, the sky, the panels ---------------------------------------------------
    add("list_assets",
        "The models, textures, sounds and materials of the project and the engine, and the "
        "project's prefabs. type: model, texture, sound, material, prefab (default: models and "
        "prefabs). filter: part of the path. place_asset puts a model or prefab into the scene.",
        schema({{"type", {{"type", "string"}}}, {"filter", {{"type", "string"}}}, {"limit", {{"type", "integer"}}}}),
        false,
        [host](EditorContext& ed, const json& a) {
            const std::string type = lower(a.value("type", std::string()));
            const std::string f = lower(a.value("filter", std::string()));
            const int limit = std::clamp(a.value("limit", 80), 1, 500);
            json out = json::array();
            int total = 0;
            auto take = [&](const std::string& kind, const std::string& path) {
                if (!f.empty() && lower(path).find(f) == std::string::npos) return;
                ++total;
                if ((int)out.size() < limit) out.push_back({{"type", kind}, {"asset", path}});
            };
            if ((type.empty() || type == "prefab") && host.prefabs)
                for (const auto& [name, file] : host.prefabs()) take("prefab", name);
            if (type != "prefab") {
                for (const fitzel::AssetId& id : ed.assetDb.allAssets()) {
                    const fitzel::AssetDatabase::Entry* e = ed.assetDb.entry(id);
                    if (!e) continue;
                    const char* kind = e->type == fitzel::AssetType::Model    ? "model"
                                     : e->type == fitzel::AssetType::Texture  ? "texture"
                                     : e->type == fitzel::AssetType::Sound    ? "sound"
                                     : e->type == fitzel::AssetType::Material ? "material"
                                                                              : nullptr;
                    if (!kind || (type.empty() ? std::string(kind) != "model" : type != kind)) continue;
                    take(kind, e->relPath);
                }
            }
            return json{{"assets", out}, {"count", total}};
        });

    add("place_asset",
        "Put a model (a .glb/.gltf from list_assets) or a prefab (by name) into the scene, standing "
        "on the ground at position (world [x,y,z]; default: in front of the camera). Returns the "
        "new object's id.",
        schema({{"asset", {{"type", "string"}, {"description", "the model's path or the prefab's name"}}},
                {"position", vec3Arg("world position [x,y,z]: the ground under it")},
                {"name", {{"type", "string"}}}},
               {"asset"}),
        true,
        [host](EditorContext& ed, const json& a) {
            const std::string want = a.value("asset", std::string());
            glm::vec3 pos = host.spawnPoint ? host.spawnPoint() : glm::vec3(0.0f);
            readVec(a, "position", pos);
            int id = -1;
            // A prefab by name first, then a model by its path (or part of it).
            if (host.prefabs && host.placePrefab)
                for (const auto& [name, file] : host.prefabs())
                    if (lower(name) == lower(want)) { id = host.placePrefab(file, pos); break; }
            if (id < 0) {
                fitzel::AssetId found;
                int n = 0;
                for (const fitzel::AssetId& gid : ed.assetDb.allAssets()) {
                    const fitzel::AssetDatabase::Entry* e = ed.assetDb.entry(gid);
                    if (!e || e->type != fitzel::AssetType::Model) continue;
                    if (lower(e->relPath) == lower(want)) { found = gid; n = 1; break; }
                    if (lower(e->relPath).find(lower(want)) != std::string::npos) { found = gid; ++n; }
                }
                if (n == 0) return fail("no model or prefab called \"" + want + "\" (see list_assets)");
                if (n > 1) return fail("several models match \"" + want + "\" -- give the whole path");
                const std::string mp = ed.assetDb.pathForId(found).string();
                const int before = ed.sel.activeId();
                if (ed.isStructuredModel && ed.isStructuredModel(mp)) {
                    ed.addModelHierarchy(pos, mp);
                } else {
                    const int mid = ed.models.import(mp, ed.assetDb, ed.materials);
                    if (mid < 0) return fail("the model did not load");
                    ed.addModelEntity(pos, mid);
                }
                id = ed.sel.activeId() != before ? ed.sel.activeId() : -1;
            }
            if (id < 0) return fail("it could not be placed");
            if (a.contains("name") && a["name"].is_string() && !a["name"].get<std::string>().empty()) {
                const std::vector<int> ids{id};
                auto before = ed.snapshot(ids);
                ed.document.find(id)->name = a["name"].get<std::string>();
                ed.commitEdit(std::move(before), ids, "Rename");
            }
            scenegraph::resolve(ed.entities);
            const Entity* e = ed.document.find(id);
            return e ? json{{"created", brief(*e)}} : fail("it could not be placed");
        });

    add("create_road",
        "Lay a road (a spline, the Roads panel's kind) and build it into the terrain: points are "
        "control points [x,z] in world metres, at least 2, a few metres to some hundred apart -- the "
        "road runs smoothly through them. lifts: metres above the ground per point (an embankment; "
        "for a crossing over water or a valley use a bridge instead). bridges: [[a,b], ...] point "
        "index spans where the road flies on a deck and the ground below is left alone. closed: a "
        "circuit. Kerbs, rails and junctions with other roads come by themselves.",
        schema({{"points", {{"type", "array"}, {"items", {{"type", "array"}, {"items", {{"type", "number"}}}}}}},
                {"lifts", {{"type", "array"}, {"items", {{"type", "number"}}}}},
                {"bridges", {{"type", "array"}, {"items", {{"type", "array"}, {"items", {{"type", "integer"}}}}}}},
                {"closed", {{"type", "boolean"}}},
                {"name", {{"type", "string"}}}},
               {"points"}),
        true,
        [host](EditorContext&, const json& a) {
            if (!host.createRoad) return fail("roads cannot be laid from here");
            Host::Road r;
            r.name   = a.value("name", std::string());
            r.closed = a.value("closed", false);
            if (a.contains("points") && a["points"].is_array())
                for (const json& p : a["points"]) {
                    if (!p.is_array() || p.size() < 2 || !p[0].is_number() || !p[1].is_number()) continue;
                    // [x, z], or [x, y, z] by a model that thinks in 3D: y is ignored.
                    r.points.push_back(p.size() >= 3 && p[2].is_number()
                                           ? glm::vec2(p[0].get<float>(), p[2].get<float>())
                                           : glm::vec2(p[0].get<float>(), p[1].get<float>()));
                }
            if (r.points.size() < 2) return fail("a road needs at least two points [x,z]");
            r.lifts.assign(r.points.size(), 0.0f);
            if (a.contains("lifts") && a["lifts"].is_array())
                for (std::size_t i = 0; i < a["lifts"].size() && i < r.points.size(); ++i)
                    if (a["lifts"][i].is_number()) r.lifts[i] = a["lifts"][i].get<float>();
            if (a.contains("bridges") && a["bridges"].is_array())
                for (const json& b : a["bridges"]) {
                    if (!b.is_array() || b.size() < 2 || !b[0].is_number() || !b[1].is_number()) continue;
                    const int x = b[0].get<int>(), y = b[1].get<int>();
                    const int n = static_cast<int>(r.points.size());
                    if (x < 0 || y < 0 || x >= n || y >= n || x == y)
                        return fail("a bridge spans two point indices of this road (0.." + std::to_string(n - 1) + ")");
                    r.bridges.push_back({std::min(x, y), std::max(x, y)});
                }
            const int i = host.createRoad(r);
            if (i < 0) return fail("the road could not be laid");
            return json{{"road", i}, {"points", r.points.size()}, {"bridges", r.bridges.size()},
                        {"note", "built into the terrain; the Roads panel edits it further"}};
        });

    add("list_roads", "The scene's roads: name and how many points.", schema(json::object()), false,
        [host](EditorContext&, const json&) {
            return json{{"roads", host.roads ? host.roads() : std::vector<std::string>()}};
        });

    add("set_weather",
        "The sky and the clock, at once (in the editor; a script does it in Play). preset: one of "
        "the weather presets (\"Sunny day\", \"Morning dew\", \"Overcast\", \"Heavy rain\", \"Storm\", "
        "or the project's own -- list_weather). hour: time of day 0..24 (18.5 = evening). Either or both.",
        schema({{"preset", {{"type", "string"}}}, {"hour", {{"type", "number"}}}}), true,
        [host](EditorContext&, const json& a) {
            json out = json::object();
            if (a.contains("preset") && a["preset"].is_string()) {
                if (!host.setWeather || !host.setWeather(a["preset"].get<std::string>())) {
                    std::string names;
                    if (host.weatherPresets)
                        for (const std::string& n : host.weatherPresets()) names += (names.empty() ? "" : ", ") + n;
                    return fail("no weather preset \"" + a["preset"].get<std::string>() + "\" -- there are: " + names);
                }
                out["preset"] = a["preset"];
            }
            // After the preset: a preset may carry an hour of its own.
            if (a.contains("hour") && a["hour"].is_number() && host.setTimeOfDay) {
                host.setTimeOfDay(a["hour"].get<float>());
                out["hour"] = a["hour"];
            }
            if (out.empty()) return fail("give a preset, an hour, or both");
            return out;
        });

    add("list_weather", "The weather presets there are, and the time of day now.", schema(json::object()), false,
        [host](EditorContext&, const json&) {
            json out = {{"presets", host.weatherPresets ? host.weatherPresets() : std::vector<std::string>()}};
            if (host.timeOfDay) out["hour"] = r3(host.timeOfDay());
            return out;
        });

    add("open_panel",
        "Open one of the editor's panels for the user (by its name in the View menu: \"Roads\", "
        "\"Terrain sculpt\", \"Town generator\", \"Vegetation\", ...). For what no tool does: open the "
        "panel and say what to do in it.",
        schema({{"name", {{"type", "string"}}}}, {"name"}), false,
        [host](EditorContext&, const json& a) {
            const std::string n = a.value("name", std::string());
            if (host.openPanel && host.openPanel(n)) return json{{"opened", n}};
            std::string all;
            if (host.panels)
                for (const std::string& p : host.panels()) all += (all.empty() ? "" : ", ") + p;
            return fail("no panel \"" + n + "\" -- there are: " + all);
        });

    return t;
}

} // namespace assistant
