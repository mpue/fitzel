#include "ScriptSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <fitzel/asset/Vfs.hpp>

#include "SaveData.hpp"
#include "MusicSystem.hpp"
#include "ScriptNet.hpp"
#include "SynthSystem.hpp"

namespace {

// Load a Lua chunk through the VFS rather than the filesystem, so scripts inside
// an exported game's archive run like loose ones. The "@" prefix keeps Lua's
// error messages pointing at a file name instead of dumping the source.
int loadLuaChunk(lua_State* L, const std::string& path) {
    const std::string src = fitzel::vfs::readText(path);
    if (src.empty()) {
        lua_pushfstring(L, "cannot open %s", path.c_str());
        return LUA_ERRFILE;
    }
    const std::string chunk = "@" + path;
    return luaL_loadbuffer(L, src.data(), src.size(), chunk.c_str());
}

void setNum(lua_State* L, const char* k, float v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
}
void setInt(lua_State* L, const char* k, int v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
}
void setBool(lua_State* L, const char* k, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
}
void setStr(lua_State* L, const char* k, const std::string& v) {
    lua_pushlstring(L, v.c_str(), v.size());
    lua_setfield(L, -2, k);
}
// Push {x, y, z} under key `k` (also as [1..3], so both t.k.x and t.k[1] work).
void setVec(lua_State* L, const char* k, const glm::vec3& v) {
    lua_createtable(L, 3, 3);
    const float c[3] = {v.x, v.y, v.z};
    const char* n[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        lua_pushnumber(L, c[i]); lua_seti(L, -2, i + 1);
        lua_pushnumber(L, c[i]); lua_setfield(L, -2, n[i]);
    }
    lua_setfield(L, -2, k);
}
// Push a list of integers as a 1-based array.
void pushIntArray(lua_State* L, const std::vector<int>& v) {
    lua_createtable(L, static_cast<int>(v.size()), 0);
    for (std::size_t i = 0; i < v.size(); ++i) {
        lua_pushinteger(L, v[i]);
        lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
    }
}
float getNum(lua_State* L, const char* k, float fallback) {
    lua_getfield(L, -1, k);
    const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1))
                                        : fallback;
    lua_pop(L, 1);
    return v;
}

// --- `game` table C functions ----------------------------------------------
// Each is registered with the owning ScriptSystem* as upvalue 1, so it can reach
// the host bridge. Returns null host -> the call is a harmless no-op.

ScriptHost* hostOf(lua_State* L) {
    auto* self =
        static_cast<ScriptSystem*>(lua_touserdata(L, lua_upvalueindex(1)));
    return self ? self->host() : nullptr;
}
ScriptSystem* systemOf(lua_State* L) {
    return static_cast<ScriptSystem*>(lua_touserdata(L, lua_upvalueindex(1)));
}
// Another script holds the keys (game.captureInput): this one is told nothing
// is pressed. The host is still asked, so its press detection keeps track.
bool inputBlocked(lua_State* L) {
    const ScriptSystem* s = systemOf(L);
    return s && s->inputBlocked();
}

// Read table field `key` (a number) from the table at stack index `t`, or
// `fallback` when absent/non-numeric.
float field(lua_State* L, int t, const char* key, float fallback) {
    lua_getfield(L, t, key);
    const float v = lua_isnumber(L, -1) ? static_cast<float>(lua_tonumber(L, -1))
                                        : fallback;
    lua_pop(L, 1);
    return v;
}
std::string fieldStr(lua_State* L, int t, const char* key) {
    lua_getfield(L, t, key);
    std::string s;
    if (lua_isstring(L, -1)) s = lua_tostring(L, -1);
    lua_pop(L, 1);
    return s;
}

// --- Optional table fields (for the "edit" tables: only what's present is
// applied, so a script can tweak one value without restating the rest) --------
std::optional<float> optNum(lua_State* L, int t, const char* key) {
    lua_getfield(L, t, key);
    std::optional<float> v;
    if (lua_isnumber(L, -1)) v = static_cast<float>(lua_tonumber(L, -1));
    lua_pop(L, 1);
    return v;
}
std::optional<int> optInt(lua_State* L, int t, const char* key) {
    const auto v = optNum(L, t, key);
    return v ? std::optional<int>(static_cast<int>(*v)) : std::nullopt;
}
std::optional<bool> optBool(lua_State* L, int t, const char* key) {
    lua_getfield(L, t, key);
    std::optional<bool> v;
    if (lua_isboolean(L, -1)) v = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return v;
}
std::optional<std::string> optStr(lua_State* L, int t, const char* key) {
    lua_getfield(L, t, key);
    std::optional<std::string> v;
    if (lua_isstring(L, -1)) v = std::string(lua_tostring(L, -1));
    lua_pop(L, 1);
    return v;
}
// A colour/vector field, written either as {x, y, z} / {r, g, b} (array or named)
// or as a single number (a grey level / uniform vector).
std::optional<glm::vec3> optVec(lua_State* L, int t, const char* key) {
    lua_getfield(L, t, key);
    std::optional<glm::vec3> v;
    if (lua_isnumber(L, -1)) {
        v = glm::vec3(static_cast<float>(lua_tonumber(L, -1)));
    } else if (lua_istable(L, -1)) {
        const int tt = lua_gettop(L);
        glm::vec3 c{0.0f};
        const char* named[3] = {"x", "y", "z"};
        const char* rgb[3]   = {"r", "g", "b"};
        for (int i = 0; i < 3; ++i) {
            lua_geti(L, tt, i + 1);
            if (lua_isnumber(L, -1)) {
                c[i] = static_cast<float>(lua_tonumber(L, -1));
            } else {
                lua_pop(L, 1);
                lua_getfield(L, tt, named[i]);
                if (!lua_isnumber(L, -1)) {
                    lua_pop(L, 1);
                    lua_getfield(L, tt, rgb[i]);
                }
                if (lua_isnumber(L, -1)) c[i] = static_cast<float>(lua_tonumber(L, -1));
            }
            lua_pop(L, 1);
        }
        v = c;
    }
    lua_pop(L, 1);
    return v;
}

// Read a material edit table (see game.createMaterial / game.setMaterialProps).
ScriptMaterialEdit readMaterialEdit(lua_State* L, int t) {
    ScriptMaterialEdit ed;
    ed.name             = optStr(L, t, "name");
    ed.albedo           = optVec(L, t, "color");
    if (!ed.albedo)     ed.albedo = optVec(L, t, "albedo");
    if (!ed.albedo) { // flat r/g/b, like game.spawn
        const auto r = optNum(L, t, "r"), g = optNum(L, t, "g"), b = optNum(L, t, "b");
        if (r || g || b)
            ed.albedo = glm::vec3(r.value_or(0.8f), g.value_or(0.8f), b.value_or(0.8f));
    }
    ed.reflectivity     = optNum(L, t, "reflectivity");
    ed.roughness        = optNum(L, t, "roughness");
    ed.opacity          = optNum(L, t, "opacity");
    ed.glass            = optBool(L, t, "glass");
    ed.alphaMode        = optInt(L, t, "alphaMode");
    ed.alphaCutoff      = optNum(L, t, "cutoff");
    if (!ed.alphaCutoff) ed.alphaCutoff = optNum(L, t, "alphaCutoff");
    ed.emission         = optVec(L, t, "emission");
    ed.emissionStrength = optNum(L, t, "emissionStrength");
    ed.texture          = optStr(L, t, "texture");
    ed.normalMap        = optStr(L, t, "normalMap");
    ed.emissionMap      = optStr(L, t, "emissionMap");
    return ed;
}

void pushAssetInfo(lua_State* L, const ScriptAssetInfo& a) {
    lua_createtable(L, 0, 5);
    setStr(L, "id", a.id);      setStr(L, "name", a.name);
    setStr(L, "path", a.path);  setStr(L, "type", a.type);
    setStr(L, "source", a.source);
}
void pushMaterialInfo(lua_State* L, const ScriptMaterialInfo& m) {
    lua_createtable(L, 0, 15);
    setStr(L, "id", m.id);                setStr(L, "name", m.name);
    setVec(L, "color", m.albedo);
    setNum(L, "reflectivity", m.reflectivity);
    setNum(L, "roughness", m.roughness);  setNum(L, "opacity", m.opacity);
    setBool(L, "glass", m.glass);         setInt(L, "alphaMode", m.alphaMode);
    setNum(L, "cutoff", m.alphaCutoff);
    setVec(L, "emission", m.emission);
    setNum(L, "emissionStrength", m.emissionStrength);
    setStr(L, "texture", m.texture);      setStr(L, "normalMap", m.normalMap);
    setStr(L, "emissionMap", m.emissionMap);
    setBool(L, "fromModel", m.fromModel);
}

int l_keyDown(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int k = static_cast<int>(luaL_checkinteger(L, 1));
    const bool v = h && h->keyDown && h->keyDown(k);
    lua_pushboolean(L, v && !inputBlocked(L));
    return 1;
}
int l_keyPressed(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int k = static_cast<int>(luaL_checkinteger(L, 1));
    const bool v = h && h->keyPressed && h->keyPressed(k);
    lua_pushboolean(L, v && !inputBlocked(L));
    return 1;
}
int l_mouseDown(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int b = static_cast<int>(luaL_optinteger(L, 1, 0));
    const bool v = h && h->mouseDown && h->mouseDown(b);
    lua_pushboolean(L, v && !inputBlocked(L));
    return 1;
}
int l_mousePressed(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int b = static_cast<int>(luaL_optinteger(L, 1, 0));
    const bool v = h && h->mousePressed && h->mousePressed(b);
    lua_pushboolean(L, v && !inputBlocked(L));
    return 1;
}
// game.waterAt(x, z) -> surface height, depth -- or nil where it is dry.
int l_waterAt(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const float x = static_cast<float>(luaL_checknumber(L, 1));
    const float z = static_cast<float>(luaL_checknumber(L, 2));
    float surf = 0.0f;
    if (h && h->waterAt && h->waterAt(x, z, surf)) {
        lua_pushnumber(L, surf);
        const float ground = h->terrainHeight ? h->terrainHeight(x, z) : surf;
        lua_pushnumber(L, surf - ground);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}
// game.trees(x0, z0, x1, z1) -> { x, z, scale, x, z, scale, ... }
int l_trees(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const glm::vec2 a{static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2))};
    const glm::vec2 b{static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4))};
    std::vector<glm::vec4> list;
    if (h && h->trees) h->trees(glm::min(a, b), glm::max(a, b), list);
    lua_createtable(L, static_cast<int>(list.size() * 3), 0);
    lua_Integer n = 0;
    for (const glm::vec4& t : list) {
        lua_pushnumber(L, t.x); lua_seti(L, -2, ++n);
        lua_pushnumber(L, t.z); lua_seti(L, -2, ++n);
        lua_pushnumber(L, t.w); lua_seti(L, -2, ++n);
    }
    return 1;
}
// game.clearTrees(x, z, r) -- the forest leaves this disc alone from now on.
int l_clearTrees(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (h && h->clearTrees)
        h->clearTrees(static_cast<float>(luaL_checknumber(L, 1)), static_cast<float>(luaL_checknumber(L, 2)),
                      static_cast<float>(luaL_checknumber(L, 3)));
    return 0;
}
int l_mouseWheel(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const float v = (h && h->mouseWheel && !inputBlocked(L)) ? h->mouseWheel() : 0.0f;
    lua_pushnumber(L, v);
    return 1;
}
// captureInput() -> true while this script holds the keys (see ScriptSystem).
int l_captureInput(lua_State* L) {
    ScriptSystem* s = systemOf(L);
    lua_pushboolean(L, s && s->claimInput());
    return 1;
}
// mousePos() -> x, y, over: the pointer in HUD canvas units (1080 high, origin top
// left) and whether it is over the view at all.
int l_mousePos(lua_State* L) {
    ScriptHost* h = hostOf(L);
    glm::vec2 p(0.0f);
    const bool over = h && h->mousePos && h->mousePos(p);
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushboolean(L, over);
    return 3;
}
// mouseRay() -> ox, oy, oz, dx, dy, dz: the world ray under the pointer (nil if
// there is no view to aim through).
int l_mouseRay(lua_State* L) {
    ScriptHost* h = hostOf(L);
    glm::vec3 o, d;
    if (!h || !h->mouseRay || !h->mouseRay(o, d)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, o.x); lua_pushnumber(L, o.y); lua_pushnumber(L, o.z);
    lua_pushnumber(L, d.x); lua_pushnumber(L, d.y); lua_pushnumber(L, d.z);
    return 6;
}
int l_showCursor(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (h && h->showCursor) h->showCursor(lua_isnone(L, 1) || lua_toboolean(L, 1) != 0);
    return 0;
}
int l_cameraPos(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const glm::vec3 p = h ? h->camPos : glm::vec3(0.0f);
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
    return 3;
}
int l_cameraDir(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const glm::vec3 d = h ? h->camDir : glm::vec3(0.0f, 0.0f, -1.0f);
    lua_pushnumber(L, d.x); lua_pushnumber(L, d.y); lua_pushnumber(L, d.z);
    return 3;
}
int l_spawn(lua_State* L) {
    ScriptHost* h = hostOf(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    ScriptSpawn s;
    s.type    = static_cast<int>(field(L, 1, "type", 3.0f));
    const float sz = field(L, 1, "size", 0.5f);
    s.pos     = {field(L, 1, "x", 0.0f),  field(L, 1, "y", 0.0f),  field(L, 1, "z", 0.0f)};
    s.half    = {field(L, 1, "sx", sz),   field(L, 1, "sy", sz),   field(L, 1, "sz", sz)};
    s.rot     = {field(L, 1, "rx", 0.0f), field(L, 1, "ry", 0.0f), field(L, 1, "rz", 0.0f)};
    s.color   = {field(L, 1, "r", 0.8f),  field(L, 1, "g", 0.8f),  field(L, 1, "b", 0.8f)};
    s.vel     = {field(L, 1, "vx", 0.0f), field(L, 1, "vy", 0.0f), field(L, 1, "vz", 0.0f)};
    s.mass    = field(L, 1, "mass", 1.0f);
    s.physics = static_cast<int>(field(L, 1, "physics", 2.0f));
    s.scale   = field(L, 1, "scale", 1.0f);
    s.parent  = static_cast<int>(field(L, 1, "parent", -1.0f));
    s.name     = fieldStr(L, 1, "name");
    s.script   = fieldStr(L, 1, "script");
    s.model    = fieldStr(L, 1, "model");
    s.material = fieldStr(L, 1, "material");
    const int id = (h && h->spawn) ? h->spawn(s) : 0;
    lua_pushinteger(L, id);
    return 1;
}
int l_spawnPrefab(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    const glm::vec3 pos{static_cast<float>(luaL_optnumber(L, 2, 0.0)),
                        static_cast<float>(luaL_optnumber(L, 3, 0.0)),
                        static_cast<float>(luaL_optnumber(L, 4, 0.0))};
    const float yaw = static_cast<float>(luaL_optnumber(L, 5, 0.0));
    const int id = (h && h->spawnPrefab) ? h->spawnPrefab(name, pos, yaw) : 0;
    lua_pushinteger(L, id);
    return 1;
}
int l_destroy(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    if (h && h->destroy) h->destroy(id);
    return 0;
}
int l_clone(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const char* name = luaL_optstring(L, 2, "");
    lua_pushinteger(L, (h && h->clone) ? h->clone(id, name) : 0);
    return 1;
}
int l_getPos(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    glm::vec3 p;
    if (h && h->getPos && h->getPos(id, p)) {
        lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}
int l_setPos(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const glm::vec3 p{static_cast<float>(luaL_checknumber(L, 2)),
                      static_cast<float>(luaL_checknumber(L, 3)),
                      static_cast<float>(luaL_checknumber(L, 4))};
    if (h && h->setPos) h->setPos(id, p);
    return 0;
}
int l_setVelocity(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const glm::vec3 v{static_cast<float>(luaL_checknumber(L, 2)),
                      static_cast<float>(luaL_checknumber(L, 3)),
                      static_cast<float>(luaL_checknumber(L, 4))};
    if (h && h->setVelocity) h->setVelocity(id, v);
    return 0;
}
int l_applyImpulse(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const glm::vec3 j{static_cast<float>(luaL_checknumber(L, 2)),
                      static_cast<float>(luaL_checknumber(L, 3)),
                      static_cast<float>(luaL_checknumber(L, 4))};
    // Optionally where it struck: x, y, z after the impulse.
    glm::vec3 at(0.0f);
    const bool hasAt = lua_isnumber(L, 5) && lua_isnumber(L, 6) && lua_isnumber(L, 7);
    if (hasAt)
        at = glm::vec3(static_cast<float>(lua_tonumber(L, 5)), static_cast<float>(lua_tonumber(L, 6)),
                       static_cast<float>(lua_tonumber(L, 7)));
    if (h && h->applyImpulse) h->applyImpulse(id, j, hasAt ? &at : nullptr);
    return 0;
}
int l_playSound(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    if (h && h->playSound) h->playSound(name);
    return 0;
}
// game.sound(name [, volume [, pitch [, x, y, z [, near [, far]]]]])
int l_sound(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name  = luaL_checkstring(L, 1);
    const float vol   = static_cast<float>(luaL_optnumber(L, 2, 1.0));
    const float pitch = static_cast<float>(luaL_optnumber(L, 3, 1.0));
    const bool  at    = lua_isnumber(L, 4) && lua_isnumber(L, 5) && lua_isnumber(L, 6);
    glm::vec3 p(0.0f);
    if (at)
        p = {static_cast<float>(lua_tonumber(L, 4)), static_cast<float>(lua_tonumber(L, 5)),
             static_cast<float>(lua_tonumber(L, 6))};
    const float nearM = static_cast<float>(luaL_optnumber(L, 7, 15.0));
    const float farM  = static_cast<float>(luaL_optnumber(L, 8, 400.0));
    if (h && h->playSoundEx) h->playSoundEx(name, vol, pitch, at ? &p : nullptr, nearM, farM);
    return 0;
}
// game.setLocal(id, x, y, z [, rx, ry, rz]) -- the LOCAL transform (relative to
// the parent; for a root object the same as the world one).
int l_setLocal(lua_State* L) {
    ScriptHost* h = hostOf(L);
    ScriptLocal l;
    l.id  = static_cast<int>(luaL_checkinteger(L, 1));
    l.pos = {static_cast<float>(luaL_checknumber(L, 2)), static_cast<float>(luaL_checknumber(L, 3)),
             static_cast<float>(luaL_checknumber(L, 4))};
    if (lua_isnumber(L, 5)) {
        l.rot    = {static_cast<float>(luaL_checknumber(L, 5)), static_cast<float>(luaL_checknumber(L, 6)),
                    static_cast<float>(luaL_checknumber(L, 7))};
        l.hasRot = true;
    }
    if (h && h->setLocals) h->setLocals({l});
    return 0;
}
// game.setLocals(list [, stride]) -- a flat list of id, x, y, z, rx, ry, rz, id,
// ... (stride 7, the default) or id, x, y, z, id, ... (stride 4: position only).
int l_setLocals(lua_State* L) {
    ScriptHost* h = hostOf(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    const int stride = static_cast<int>(luaL_optinteger(L, 2, 7));
    if (stride != 4 && stride != 7) return luaL_error(L, "game.setLocals: stride must be 4 or 7");
    const lua_Integer n = luaL_len(L, 1);
    std::vector<ScriptLocal> list;
    list.reserve(static_cast<std::size_t>(n / stride));
    auto num = [&](lua_Integer k) {
        lua_rawgeti(L, 1, k);
        const float f = static_cast<float>(lua_tonumber(L, -1));
        lua_pop(L, 1);
        return f;
    };
    for (lua_Integer i = 1; i + stride - 1 <= n; i += stride) {
        ScriptLocal l;
        lua_rawgeti(L, 1, i);
        l.id = static_cast<int>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        l.pos = {num(i + 1), num(i + 2), num(i + 3)};
        if (stride == 7) {
            l.rot    = {num(i + 4), num(i + 5), num(i + 6)};
            l.hasRot = true;
        }
        list.push_back(l);
    }
    if (h && h->setLocals && !list.empty()) h->setLocals(list);
    return 0;
}
// A package.searchers entry: `require "steelwars.units"` loads
// <scripts>/steelwars/units.lua -- through the VFS, so a module inside an
// exported game's archive is found like a loose one. It REPLACES Lua's own file
// searcher, which knows neither the project's folder nor the archive.
int l_searchScripts(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    std::string rel = name;
    std::replace(rel.begin(), rel.end(), '.', '/');
    const std::string dir  = (h && !h->scriptsDir.empty()) ? h->scriptsDir : std::string("scripts");
    const std::string path = dir + "/" + rel + ".lua";
    if (!fitzel::vfs::exists(path)) {
        lua_pushfstring(L, "no script '%s'", path.c_str());
        return 1;
    }
    if (loadLuaChunk(L, path) != LUA_OK) return lua_error(L);   // a syntax error is an error
    lua_pushstring(L, path.c_str());
    return 2;
}
int l_playAudio(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    if (h && h->playAudio) h->playAudio(id);
    return 0;
}
int l_stopAudio(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    if (h && h->stopAudio) h->stopAudio(id);
    return 0;
}
// --- `synth` table: the Synth components (see SynthSystem) -------------------

SynthSystem* synthsOf(lua_State* L) {
    ScriptHost* h = hostOf(L);
    return h ? h->synths : nullptr;
}

// A note name to its MIDI number: "C4" is middle C (60), "A4" is 69, "F#3",
// "Bb2", "C-1" (0). No octave means the fourth. -1 if it is not a note.
int parseNote(const char* s) {
    if (!s || !*s) return -1;
    static const int kFromA[7] = {9, 11, 0, 2, 4, 5, 7};   // A B C D E F G
    const int letter = std::toupper(static_cast<unsigned char>(*s));
    if (letter < 'A' || letter > 'G') return -1;
    int n = kFromA[letter - 'A'];
    ++s;
    for (; *s == '#' || *s == 'b'; ++s) n += (*s == '#') ? 1 : -1;
    int octave = 4;
    if (*s) {
        char* end = nullptr;
        octave = static_cast<int>(std::strtol(s, &end, 10));
        if (end == s || *end) return -1;
    }
    n += (octave + 1) * 12;
    return (n >= 0 && n <= 127) ? n : -1;
}

// A note argument: a number or a name.
int noteArg(lua_State* L, int i) {
    if (lua_type(L, i) == LUA_TSTRING) {
        const int n = parseNote(lua_tostring(L, i));
        if (n < 0) luaL_argerror(L, i, "not a note name (try \"C4\", \"F#3\")");
        return n;
    }
    return std::clamp(static_cast<int>(std::lround(luaL_checknumber(L, i))), 0, 127);
}

int l_synthPlay(lua_State* L) {
    SynthSystem* s = synthsOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, s && s->play(id));
    return 1;
}
int l_synthStop(lua_State* L) {
    SynthSystem* s = synthsOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    if (s) s->stop(id);
    return 0;
}
int l_synthNoteOn(lua_State* L) {
    SynthSystem* s = synthsOf(L);
    const int id   = static_cast<int>(luaL_checkinteger(L, 1));
    const int note = noteArg(L, 2);
    float vel = static_cast<float>(luaL_optnumber(L, 3, 0.8));
    if (vel > 1.0f) vel /= 127.0f;   // a MIDI velocity, 0..127
    lua_pushboolean(L, s && s->noteOn(id, note, vel));
    return 1;
}
int l_synthNoteOff(lua_State* L) {
    SynthSystem* s = synthsOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    if (lua_isnoneornil(L, 2)) {
        if (s) s->allNotesOff(id);
    } else {
        const int note = noteArg(L, 2);
        if (s) s->noteOff(id, note);
    }
    return 0;
}
int l_synthSet(lua_State* L) {
    SynthSystem* s = synthsOf(L);
    const int         id   = static_cast<int>(luaL_checkinteger(L, 1));
    const std::string dial = luaL_checkstring(L, 2);
    const float       v    = static_cast<float>(luaL_checknumber(L, 3));
    lua_pushboolean(L, s && s->setDial(id, dial, v));
    return 1;
}
int l_synthPlayMidi(lua_State* L) {
    SynthSystem* s = synthsOf(L);
    const int         id   = static_cast<int>(luaL_checkinteger(L, 1));
    const std::string file = luaL_optstring(L, 2, "");
    const int loop = lua_isnoneornil(L, 3) ? -1 : (lua_toboolean(L, 3) ? 1 : 0);
    lua_pushboolean(L, s && s->playMidi(id, file, loop));
    return 1;
}
int l_synthStopMidi(lua_State* L) {
    SynthSystem* s = synthsOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    if (s) s->stopMidi(id);
    return 0;
}
int l_synthIsPlaying(lua_State* L) {
    SynthSystem* s = synthsOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, s && s->midiPlaying(id));
    return 1;
}
int l_synthSetTempo(lua_State* L) {
    SynthSystem* s = synthsOf(L);
    const int   id    = static_cast<int>(luaL_checkinteger(L, 1));
    const float scale = static_cast<float>(luaL_checknumber(L, 2));
    lua_pushboolean(L, s && s->setTempo(id, scale));
    return 1;
}
int l_synthNote(lua_State* L) {
    const int n = parseNote(luaL_checkstring(L, 1));
    if (n < 0) lua_pushnil(L);
    else       lua_pushinteger(L, n);
    return 1;
}
int l_synthError(lua_State* L) {
    SynthSystem* s = synthsOf(L);
    lua_pushstring(L, s ? s->lastError().c_str() : "");
    return 1;
}

// --- `music` table: the game's song (see MusicSystem) ------------------------

MusicSystem* musicOf(lua_State* L) {
    ScriptHost* h = hostOf(L);
    return h ? h->music : nullptr;
}
fitzel::MusicPlayer* musicPlayer(lua_State* L) {
    MusicSystem* m = musicOf(L);
    return m ? m->player() : nullptr;
}

void pushFloats(lua_State* L, const std::vector<float>& v) {
    lua_createtable(L, static_cast<int>(v.size()), 0);
    for (std::size_t i = 0; i < v.size(); ++i) {
        lua_pushnumber(L, v[i]);
        lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
    }
}

// music.load(name) -> true, seconds | false, why
int l_musicLoad(lua_State* L) {
    MusicSystem* m = musicOf(L);
    const std::string name = luaL_checkstring(L, 1);
    fitzel::MusicPlayer* p = m ? m->player() : nullptr;
    std::string err = m && !p ? m->error() : std::string("no audio");
    if (p && p->load(m->resolve(name), &err)) {
        lua_pushboolean(L, 1);
        lua_pushnumber(L, p->duration());
        return 2;
    }
    lua_pushboolean(L, 0);
    lua_pushstring(L, err.c_str());
    return 2;
}
int l_musicPlay(lua_State* L) {
    if (auto* p = musicPlayer(L)) p->play(luaL_optnumber(L, 1, 0.0));
    return 0;
}
int l_musicStop(lua_State* L) {
    if (MusicSystem* m = musicOf(L)) if (auto* p = m->existing()) p->stop();
    return 0;
}
int l_musicPause(lua_State* L) {
    if (MusicSystem* m = musicOf(L)) if (auto* p = m->existing()) p->pause();
    return 0;
}
int l_musicResume(lua_State* L) {
    if (MusicSystem* m = musicOf(L)) if (auto* p = m->existing()) p->resume();
    return 0;
}
int l_musicFade(lua_State* L) {
    const double sec = luaL_optnumber(L, 1, 1.0);
    if (MusicSystem* m = musicOf(L)) if (auto* p = m->existing()) p->fadeOut(sec);
    return 0;
}
int l_musicTime(lua_State* L) {
    MusicSystem* m = musicOf(L);
    auto* p = m ? m->existing() : nullptr;
    lua_pushnumber(L, p ? p->time() : 0.0);
    return 1;
}
int l_musicDuration(lua_State* L) {
    MusicSystem* m = musicOf(L);
    auto* p = m ? m->existing() : nullptr;
    lua_pushnumber(L, p ? p->duration() : 0.0);
    return 1;
}
int l_musicIsPlaying(lua_State* L) {
    MusicSystem* m = musicOf(L);
    auto* p = m ? m->existing() : nullptr;
    lua_pushboolean(L, p && p->playing());
    return 1;
}
int l_musicIsPaused(lua_State* L) {
    MusicSystem* m = musicOf(L);
    auto* p = m ? m->existing() : nullptr;
    lua_pushboolean(L, p && p->paused());
    return 1;
}
// music.setFilter(cutoffHz, gain, shelfDb [, smoothSec=0.12])
int l_musicSetFilter(lua_State* L) {
    const float cut   = static_cast<float>(luaL_checknumber(L, 1));
    const float gain  = static_cast<float>(luaL_optnumber(L, 2, 1.0));
    const float shelf = static_cast<float>(luaL_optnumber(L, 3, 0.0));
    const float tc    = static_cast<float>(luaL_optnumber(L, 4, 0.12));
    if (auto* p = musicPlayer(L)) p->setFilter(cut, gain, shelf, tc);
    return 0;
}
int l_musicSetVolume(lua_State* L) {
    if (MusicSystem* m = musicOf(L)) m->setVolume(static_cast<float>(luaL_checknumber(L, 1)));
    return 0;
}
int l_musicSampleRate(lua_State* L) {
    auto* p = musicPlayer(L);
    lua_pushinteger(L, p ? p->sampleRate() : 48000);
    return 1;
}
// music.spectrum() -> 512 bins 0..1 (call once a frame, see MusicPlayer)
int l_musicSpectrum(lua_State* L) {
    std::vector<float> v;
    MusicSystem* m = musicOf(L);
    if (auto* p = m ? m->existing() : nullptr) p->spectrum(v);
    else v.assign(512, 0.0f);
    pushFloats(L, v);
    return 1;
}
int l_musicWaveform(lua_State* L) {
    const int n = static_cast<int>(luaL_optinteger(L, 1, 128));
    std::vector<float> v;
    MusicSystem* m = musicOf(L);
    if (auto* p = m ? m->existing() : nullptr) p->waveform(v, n);
    else v.assign(static_cast<std::size_t>(std::clamp(n, 1, 1024)), 0.0f);
    pushFloats(L, v);
    return 1;
}
// music.analyze(name) -> { duration, rate, hop, frames, lowFlux, midFlux,
// highFlux, lowRms, midRms, highRms } | nil, why. Blocks while it decodes.
int l_musicAnalyze(lua_State* L) {
    MusicSystem* m = musicOf(L);
    const std::string name = luaL_checkstring(L, 1);
    fitzel::music::Bands bands;
    std::string err = "no music system";
    if (!m || !fitzel::music::analyzeBands(m->resolve(name), bands, &err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    lua_newtable(L);
    lua_pushnumber(L, bands.duration);  lua_setfield(L, -2, "duration");
    lua_pushinteger(L, bands.rate);     lua_setfield(L, -2, "rate");
    lua_pushinteger(L, bands.hop);      lua_setfield(L, -2, "hop");
    lua_pushinteger(L, static_cast<lua_Integer>(bands.flux[0].size()));
    lua_setfield(L, -2, "frames");
    static const char* kFlux[3] = {"lowFlux", "midFlux", "highFlux"};
    static const char* kRms[3]  = {"lowRms", "midRms", "highRms"};
    for (int b = 0; b < 3; ++b) {
        pushFloats(L, bands.flux[b]); lua_setfield(L, -2, kFlux[b]);
        pushFloats(L, bands.rms[b]);  lua_setfield(L, -2, kRms[b]);
    }
    return 1;
}

int l_addScore(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int n = static_cast<int>(luaL_optinteger(L, 1, 1));
    if (h) h->score += n;
    return 0;
}
int l_getScore(lua_State* L) {
    ScriptHost* h = hostOf(L);
    lua_pushinteger(L, h ? h->score : 0);
    return 1;
}
int l_setHud(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* text = luaL_optstring(L, 1, "");
    if (h) h->hud = text;
    return 0;
}

// --- Script-drawn HUD ----------------------------------------------------------
// Every call queues one ScriptHudCmd; the host draws the queue after the frame.
// Colours are four numbers 0..1 (alpha optional, default 1) packed the way
// IM_COL32 packs them, so the host hands them to ImGui as they are.

unsigned hudColour(lua_State* L, int first) {
    auto ch = [&](int i, double dflt) {
        const double v = luaL_optnumber(L, i, dflt);
        return static_cast<unsigned>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5);
    };
    return ch(first, 1.0) | (ch(first + 1, 1.0) << 8) | (ch(first + 2, 1.0) << 16) |
           (ch(first + 3, 1.0) << 24);
}
float num(lua_State* L, int i) { return static_cast<float>(luaL_checknumber(L, i)); }
float optNum(lua_State* L, int i, float d) {
    return static_cast<float>(luaL_optnumber(L, i, d));
}
void queueHud(lua_State* L, ScriptHudCmd&& c) {
    if (ScriptHost* h = hostOf(L)) h->hudCmds.push_back(std::move(c));
}

// hudRect(x, y, w, h, r, g, b, a, rounding)
int l_hudRect(lua_State* L) {
    ScriptHudCmd c;
    c.kind = ScriptHudCmd::Kind::Rect;
    for (int i = 0; i < 4; ++i) c.a[i] = num(L, i + 1);
    c.col  = hudColour(L, 5);
    c.a[4] = optNum(L, 9, 0.0f);
    queueHud(L, std::move(c));
    return 0;
}
// hudGradient(x, y, w, h, r, g, b, a, r2, g2, b2, a2) -- top colour to bottom
int l_hudGradient(lua_State* L) {
    ScriptHudCmd c;
    c.kind = ScriptHudCmd::Kind::Gradient;
    for (int i = 0; i < 4; ++i) c.a[i] = num(L, i + 1);
    c.col  = hudColour(L, 5);
    c.col2 = hudColour(L, 9);
    queueHud(L, std::move(c));
    return 0;
}
// hudFrame(x, y, w, h, r, g, b, a, thickness, rounding)
int l_hudFrame(lua_State* L) {
    ScriptHudCmd c;
    c.kind = ScriptHudCmd::Kind::Frame;
    for (int i = 0; i < 4; ++i) c.a[i] = num(L, i + 1);
    c.col  = hudColour(L, 5);
    c.size = optNum(L, 9, 2.0f);
    c.a[4] = optNum(L, 10, 0.0f);
    queueHud(L, std::move(c));
    return 0;
}
// hudLine(x1, y1, x2, y2, r, g, b, a, thickness)
int l_hudLine(lua_State* L) {
    ScriptHudCmd c;
    c.kind = ScriptHudCmd::Kind::Line;
    for (int i = 0; i < 4; ++i) c.a[i] = num(L, i + 1);
    c.col  = hudColour(L, 5);
    c.size = optNum(L, 9, 2.0f);
    queueHud(L, std::move(c));
    return 0;
}
// hudCircle(x, y, radius, r, g, b, a, thickness) -- no thickness = filled
int l_hudCircle(lua_State* L) {
    ScriptHudCmd c;
    for (int i = 0; i < 3; ++i) c.a[i] = num(L, i + 1);
    c.col  = hudColour(L, 4);
    c.size = optNum(L, 8, 0.0f);
    c.kind = c.size > 0.0f ? ScriptHudCmd::Kind::Ring : ScriptHudCmd::Kind::Circle;
    queueHud(L, std::move(c));
    return 0;
}
// hudTri(x1, y1, x2, y2, x3, y3, r, g, b, a) -- filled
int l_hudTri(lua_State* L) {
    ScriptHudCmd c;
    c.kind = ScriptHudCmd::Kind::Tri;
    for (int i = 0; i < 6; ++i) c.a[i] = num(L, i + 1);
    c.col = hudColour(L, 7);
    queueHud(L, std::move(c));
    return 0;
}
// hudText(x, y, text, size, r, g, b, a, align, bold) -- y is the top of the line
int l_hudText(lua_State* L) {
    ScriptHudCmd c;
    c.kind  = ScriptHudCmd::Kind::Text;
    c.a[0]  = num(L, 1);
    c.a[1]  = num(L, 2);
    c.text  = luaL_checkstring(L, 3);
    c.size  = optNum(L, 4, 32.0f);
    c.col   = hudColour(L, 5);
    c.align = optNum(L, 9, 0.0f);
    c.bold  = lua_toboolean(L, 10) != 0;
    queueHud(L, std::move(c));
    return 0;
}
// hudTextSize(text, size, bold) -> w, h
int l_hudTextSize(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const std::string text = luaL_checkstring(L, 1);
    const float size = optNum(L, 2, 32.0f);
    glm::vec2 s(0.0f);
    if (h && h->measureText) s = h->measureText(text, size, lua_toboolean(L, 3) != 0);
    lua_pushnumber(L, s.x);
    lua_pushnumber(L, s.y);
    return 2;
}
// hudSize() -> w, h: the canvas is 1080 high and as wide as the view's aspect.
int l_hudSize(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const float aspect = (h && h->screen.y > 0.0f) ? h->screen.x / h->screen.y
                                                   : 16.0f / 9.0f;
    lua_pushnumber(L, 1080.0f * aspect);
    lua_pushnumber(L, 1080.0f);
    return 2;
}
int l_setCrosshair(lua_State* L) {
    if (ScriptHost* h = hostOf(L)) h->crosshair = lua_toboolean(L, 1) != 0;
    return 0;
}
// hudImage(tex, x, y, w, h, r, g, b, a, u0, v0, u1, v1) -- a Texture asset by
// file name or GUID, tinted (default white), cropped to the uv box (default the
// whole picture; v runs down it). An unknown picture draws nothing.
int l_hudImage(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    ScriptImage img;
    if (!h || !h->hudImage || !h->hudImage(name, img) || img.tex == 0) return 0;
    ScriptHudCmd c;
    c.kind = ScriptHudCmd::Kind::Image;
    for (int i = 0; i < 4; ++i) c.a[i] = num(L, i + 2);
    c.col   = hudColour(L, 6);
    c.tex   = img.tex;
    c.uv[0] = optNum(L, 10, 0.0f);
    c.uv[1] = optNum(L, 11, 0.0f);
    c.uv[2] = optNum(L, 12, 1.0f);
    c.uv[3] = optNum(L, 13, 1.0f);
    queueHud(L, std::move(c));
    return 0;
}
// imageSize(tex) -> w, h, u0, v0, u1, v1: the picture's size in pixels as the
// HUD holds it, and the box its visible part fills (0..1) | nil
int l_imageSize(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    ScriptImage img;
    if (!h || !h->hudImage || !h->hudImage(name, img) || img.tex == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, img.width);
    lua_pushinteger(L, img.height);
    for (int i = 0; i < 4; ++i) lua_pushnumber(L, img.content[i]);
    return 6;
}
// worldToHud(x, y, z) -> hx, hy on the HUD canvas | nil when behind the eye
int l_worldToHud(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const glm::vec3 p{num(L, 1), num(L, 2), num(L, 3)};
    glm::vec2 out(0.0f);
    if (!h || !h->worldToHud || !h->worldToHud(p, out)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, out.x);
    lua_pushnumber(L, out.y);
    return 2;
}
// game.rest([fps]): see ScriptHost::restFps. Two scripts asking: the faster
// rate wins, the one with something left to show.
int l_rest(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (!h) return 0;
    const float fps = std::clamp(static_cast<float>(luaL_optnumber(L, 1, 10.0)), 1.0f, 120.0f);
    h->restFps = std::max(h->restFps, fps);
    return 0;
}

// --- Assets ------------------------------------------------------------------

int l_assets(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* type = luaL_optstring(L, 1, "");
    std::vector<ScriptAssetInfo> list;
    if (h && h->assetList) list = h->assetList(type);
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        pushAssetInfo(L, list[i]);
        lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}
int l_findAsset(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    const char* type = luaL_optstring(L, 2, "");
    const std::string id = (h && h->findAsset) ? h->findAsset(name, type) : std::string();
    if (id.empty()) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, id.c_str(), id.size());
    return 1;
}
int l_assetInfo(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* id = luaL_checkstring(L, 1);
    ScriptAssetInfo a;
    if (!h || !h->assetInfo || !h->assetInfo(id, a)) { lua_pushnil(L); return 1; }
    pushAssetInfo(L, a);
    return 1;
}
int l_assetPath(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* id = luaL_checkstring(L, 1);
    const std::string p = (h && h->assetPath) ? h->assetPath(id) : std::string();
    if (p.empty()) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, p.c_str(), p.size());
    return 1;
}
int l_refreshAssets(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (h && h->refreshAssets) h->refreshAssets();
    return 0;
}

// --- Models ------------------------------------------------------------------

int l_loadModel(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* asset = luaL_checkstring(L, 1);
    const int id = (h && h->loadModel) ? h->loadModel(asset) : -1;
    if (id < 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, id);
    return 1;
}
int l_modelInfo(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    ScriptModelInfo m;
    if (!h || !h->modelInfo || !h->modelInfo(id, m)) { lua_pushnil(L); return 1; }
    lua_createtable(L, 0, 7);
    setStr(L, "name", m.name);   setStr(L, "path", m.path);
    setVec(L, "min", m.boundsMin);
    setVec(L, "max", m.boundsMax);
    setVec(L, "size", m.boundsMax - m.boundsMin);
    setInt(L, "meshes", m.meshes);
    setBool(L, "animated", m.animated);
    return 1;
}

// --- Materials ---------------------------------------------------------------

int l_materials(lua_State* L) {
    ScriptHost* h = hostOf(L);
    std::vector<ScriptMaterialInfo> list;
    if (h && h->materialList) list = h->materialList();
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        pushMaterialInfo(L, list[i]);
        lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}
int l_findMaterial(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    const std::string id = (h && h->findMaterial) ? h->findMaterial(name) : std::string();
    if (id.empty()) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, id.c_str(), id.size());
    return 1;
}
int l_materialInfo(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* id = luaL_checkstring(L, 1);
    ScriptMaterialInfo m;
    if (!h || !h->materialInfo || !h->materialInfo(id, m)) { lua_pushnil(L); return 1; }
    pushMaterialInfo(L, m);
    return 1;
}
int l_createMaterial(lua_State* L) {
    ScriptHost* h = hostOf(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    const ScriptMaterialEdit ed = readMaterialEdit(L, 1);
    const std::string id = (h && h->createMaterial) ? h->createMaterial(ed) : std::string();
    if (id.empty()) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, id.c_str(), id.size());
    return 1;
}
int l_setMaterialProps(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const ScriptMaterialEdit ed = readMaterialEdit(L, 2);
    lua_pushboolean(L, h && h->setMaterialProps && h->setMaterialProps(id, ed));
    return 1;
}
int l_setMaterial(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int   e  = static_cast<int>(luaL_checkinteger(L, 1));
    const char* id = luaL_checkstring(L, 2);
    lua_pushboolean(L, h && h->setEntityMaterial && h->setEntityMaterial(e, id));
    return 1;
}
int l_getMaterial(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int e = static_cast<int>(luaL_checkinteger(L, 1));
    const std::string id = (h && h->entityMaterial) ? h->entityMaterial(e) : std::string();
    if (id.empty()) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, id.c_str(), id.size());
    return 1;
}
int l_setColor(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int e = static_cast<int>(luaL_checkinteger(L, 1));
    const glm::vec3 c{static_cast<float>(luaL_checknumber(L, 2)),
                      static_cast<float>(luaL_checknumber(L, 3)),
                      static_cast<float>(luaL_checknumber(L, 4))};
    lua_pushboolean(L, h && h->setEntityColor && h->setEntityColor(e, c));
    return 1;
}

// --- Entities: query, transform, hierarchy -----------------------------------

int l_entities(lua_State* L) {
    ScriptHost* h = hostOf(L);
    pushIntArray(L, (h && h->allEntities) ? h->allEntities() : std::vector<int>{});
    return 1;
}
int l_find(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    const int id = (h && h->findEntity) ? h->findEntity(name) : -1;
    if (id < 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, id);
    return 1;
}
int l_findAll(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    pushIntArray(L, (h && h->findEntities) ? h->findEntities(name) : std::vector<int>{});
    return 1;
}
int l_entityInfo(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    ScriptEntityInfo e;
    if (!h || !h->entityInfo || !h->entityInfo(id, e)) { lua_pushnil(L); return 1; }
    lua_createtable(L, 0, 11);
    setInt(L, "id", e.id);          setInt(L, "type", e.type);
    setInt(L, "parent", e.parent);  setStr(L, "name", e.name);
    setStr(L, "script", e.script);  setStr(L, "material", e.material);
    setStr(L, "model", e.model);
    setBool(L, "active", e.active);
    setBool(L, "activeInHierarchy", e.activeInHierarchy);
    setBool(L, "physics", e.hasPhysics);
    setBool(L, "dynamic", e.dynamic);
    return 1;
}
int l_getRot(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    glm::vec3 r;
    if (h && h->getRot && h->getRot(id, r)) {
        lua_pushnumber(L, r.x); lua_pushnumber(L, r.y); lua_pushnumber(L, r.z);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}
int l_setRot(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const glm::vec3 r{static_cast<float>(luaL_checknumber(L, 2)),
                      static_cast<float>(luaL_checknumber(L, 3)),
                      static_cast<float>(luaL_checknumber(L, 4))};
    if (h && h->setRot) h->setRot(id, r);
    return 0;
}
int l_getScale(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    glm::vec3 s;
    if (h && h->getScale && h->getScale(id, s)) {
        lua_pushnumber(L, s.x); lua_pushnumber(L, s.y); lua_pushnumber(L, s.z);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}
int l_setScale(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const float sx = static_cast<float>(luaL_checknumber(L, 2));
    // One number scales uniformly: game.setScale(id, 0.5).
    const glm::vec3 s{sx, static_cast<float>(luaL_optnumber(L, 3, sx)),
                          static_cast<float>(luaL_optnumber(L, 4, sx))};
    if (h && h->setScale) h->setScale(id, s);
    return 0;
}
int l_getName(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    ScriptEntityInfo e;
    if (!h || !h->entityInfo || !h->entityInfo(id, e)) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, e.name.c_str(), e.name.size());
    return 1;
}
int l_setName(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    if (h && h->setName) h->setName(id, luaL_checkstring(L, 2));
    return 0;
}
int l_setActive(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const bool on = lua_isnone(L, 2) ? true : lua_toboolean(L, 2) != 0;
    if (h && h->setActive) h->setActive(id, on);
    return 0;
}
// game.animTrigger(id, "open") / animBool(id, "isOpen", true) /
// animNumber(id, "speed", 2.5) / animState(id) -> "Opening"
int l_animTrigger(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const char* p = luaL_checkstring(L, 2);
    if (h && h->animTrigger) h->animTrigger(id, p);
    return 0;
}
int l_animBool(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const char* p = luaL_checkstring(L, 2);
    const bool v = lua_isnone(L, 3) ? true : lua_toboolean(L, 3) != 0;
    if (h && h->animSetBool) h->animSetBool(id, p, v);
    return 0;
}
int l_animNumber(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const char* p = luaL_checkstring(L, 2);
    const float v = static_cast<float>(luaL_checknumber(L, 3));
    if (h && h->animSetNumber) h->animSetNumber(id, p, v);
    return 0;
}
int l_animState(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    if (!h || !h->animState) { lua_pushnil(L); return 1; }
    const std::string n = h->animState(id);
    if (n.empty()) lua_pushnil(L); else lua_pushstring(L, n.c_str());
    return 1;
}
int l_isActive(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    ScriptEntityInfo e;
    if (!h || !h->entityInfo || !h->entityInfo(id, e)) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, e.activeInHierarchy);
    return 1;
}
int l_setParent(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const int p  = static_cast<int>(luaL_optinteger(L, 2, -1));
    if (h && h->setParent) h->setParent(id, p);
    return 0;
}
int l_getParent(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    ScriptEntityInfo e;
    if (!h || !h->entityInfo || !h->entityInfo(id, e) || e.parent < 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, e.parent);
    return 1;
}
int l_children(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    pushIntArray(L, (h && h->children) ? h->children(id) : std::vector<int>{});
    return 1;
}

// --- Physics reads / extra writes --------------------------------------------

int l_getVelocity(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    glm::vec3 v;
    if (h && h->getVelocity && h->getVelocity(id, v)) {
        lua_pushnumber(L, v.x); lua_pushnumber(L, v.y); lua_pushnumber(L, v.z);
        return 3;
    }
    lua_pushnil(L);
    return 1;
}
int l_setAngularVelocity(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const glm::vec3 w{static_cast<float>(luaL_checknumber(L, 2)),
                      static_cast<float>(luaL_checknumber(L, 3)),
                      static_cast<float>(luaL_checknumber(L, 4))};
    if (h && h->setAngularVelocity) h->setAngularVelocity(id, w);
    return 0;
}

// --- Lights ------------------------------------------------------------------

int l_setLight(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    ScriptLightEdit ed;
    ed.color     = optVec(L, 2, "color");
    ed.intensity = optNum(L, 2, "intensity");
    ed.range     = optNum(L, 2, "range");
    ed.type      = optInt(L, 2, "type");
    ed.spotAngle = optNum(L, 2, "spotAngle");
    ed.spotBlend = optNum(L, 2, "spotBlend");
    lua_pushboolean(L, h && h->setLight && h->setLight(id, ed));
    return 1;
}

// --- World / camera / misc ----------------------------------------------------

int l_terrainHeight(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const float x = static_cast<float>(luaL_checknumber(L, 1));
    const float z = static_cast<float>(luaL_checknumber(L, 2));
    lua_pushnumber(L, (h && h->terrainHeight) ? h->terrainHeight(x, z) : 0.0f);
    return 1;
}
int l_raycast(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const glm::vec3 o{static_cast<float>(luaL_checknumber(L, 1)),
                      static_cast<float>(luaL_checknumber(L, 2)),
                      static_cast<float>(luaL_checknumber(L, 3))};
    const glm::vec3 d{static_cast<float>(luaL_checknumber(L, 4)),
                      static_cast<float>(luaL_checknumber(L, 5)),
                      static_cast<float>(luaL_checknumber(L, 6))};
    const float maxDist = static_cast<float>(luaL_optnumber(L, 7, 1000.0));
    glm::vec3 hit{0.0f};
    float     dist = 0.0f;
    const int id = (h && h->raycast) ? h->raycast(o, d, maxDist, hit, dist) : -1;
    if (id < 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, id);
    lua_pushnumber(L, hit.x); lua_pushnumber(L, hit.y); lua_pushnumber(L, hit.z);
    lua_pushnumber(L, dist);
    return 5;
}
// game.moveCharacter(id, vx, vz, dt) -> x, y, z, onGround, onTerrain | nil
int l_moveCharacter(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const glm::vec2 vel{static_cast<float>(luaL_checknumber(L, 2)),
                        static_cast<float>(luaL_checknumber(L, 3))};
    const float dt = static_cast<float>(luaL_optnumber(L, 4, 1.0 / 60.0));
    glm::vec3 foot{0.0f};
    bool onGround = false, onTerrain = false;
    if (!h || !h->moveCharacter || !h->moveCharacter(id, vel, dt, foot, onGround, onTerrain)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, foot.x); lua_pushnumber(L, foot.y); lua_pushnumber(L, foot.z);
    lua_pushboolean(L, onGround);
    lua_pushboolean(L, onTerrain);
    return 5;
}
int l_removeCharacter(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    if (h && h->removeCharacter) h->removeCharacter(id);
    return 0;
}
// game.spawnVehicle(id) -> bool   (parked in the physics world, not driven)
int l_spawnVehicle(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, h && h->spawnVehicle && h->spawnVehicle(id));
    return 1;
}
// game.driveVehicle(id) -> bool   (the player's controls go to that car)
int l_driveVehicle(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, h && h->driveVehicle && h->driveVehicle(id));
    return 1;
}
int l_leaveVehicle(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (h && h->leaveVehicle) h->leaveVehicle();
    return 0;
}
// game.drivenVehicle() -> id, speed, steer, throttle | nil
int l_drivenVehicle(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const ScriptHost::DrivenVehicle v =
        (h && h->drivenVehicle) ? h->drivenVehicle() : ScriptHost::DrivenVehicle{};
    if (v.id < 0) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, v.id);
    lua_pushnumber(L, v.speed);
    lua_pushnumber(L, v.steer);
    lua_pushnumber(L, v.throttle);
    return 4;
}
// castRay(ox, oy, oz, dx, dy, dz [, maxDist]) -> x, y, z, nx, ny, nz, id, dist | nil
int l_castRay(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const glm::vec3 o{num(L, 1), num(L, 2), num(L, 3)};
    const glm::vec3 d{num(L, 4), num(L, 5), num(L, 6)};
    const float maxDist = optNum(L, 7, 200.0f);
    ScriptRayHit hit;
    if (!h || !h->castRay || !h->castRay(o, d, maxDist, hit)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, hit.pos.x);    lua_pushnumber(L, hit.pos.y);    lua_pushnumber(L, hit.pos.z);
    lua_pushnumber(L, hit.normal.x); lua_pushnumber(L, hit.normal.y); lua_pushnumber(L, hit.normal.z);
    lua_pushinteger(L, hit.id);
    lua_pushnumber(L, hit.dist);
    return 8;
}
// orbitFrame(weight, dist, side, up, fov) -- this frame only; 0 keeps a value
int l_orbitFrame(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (h && h->orbitFrame)
        h->orbitFrame(num(L, 1), optNum(L, 2, 0.0f), optNum(L, 3, 0.0f),
                      optNum(L, 4, 0.0f), optNum(L, 5, 0.0f));
    return 0;
}
// reach(id, side, x, y, z [, weight]) -- a hand or a foot to a point, this
// frame; side is "left" / "right" / "leftFoot" / "rightFoot" (or 0 .. 3)
int l_reach(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    int side = -1;
    if (lua_type(L, 2) == LUA_TSTRING) {
        const std::string s = lua_tostring(L, 2);
        side = (s == "left" || s == "l")   ? 0
             : (s == "right" || s == "r")  ? 1
             : (s == "leftFoot")           ? 2
             : (s == "rightFoot")          ? 3 : -1;
    } else {
        side = static_cast<int>(luaL_checkinteger(L, 2));
    }
    const glm::vec3 target(num(L, 3), num(L, 4), num(L, 5));
    if (h && h->reach && side >= 0) h->reach(id, side, target, optNum(L, 6, 1.0f));
    return 0;
}
// decal(x, y, z, nx, ny, nz [, size [, material [, spin]]]) -> bool -- an image
// laid where something hit: a bullet hole by default
int l_decal(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const glm::vec3 p(num(L, 1), num(L, 2), num(L, 3));
    const glm::vec3 n(num(L, 4), num(L, 5), num(L, 6));
    const float size = optNum(L, 7, 0.12f);
    const std::string mat = lua_type(L, 8) == LUA_TSTRING ? lua_tostring(L, 8) : "";
    const float spin = optNum(L, 9, 0.0f);
    lua_pushboolean(L, h && h->decal && h->decal(p, n, size, mat, spin));
    return 1;
}
// shatter(id, x, y, z, dx, dy, dz [, strength]) -> bool -- glass breaks where struck
int l_shatter(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const glm::vec3 p(num(L, 2), num(L, 3), num(L, 4));
    const glm::vec3 d(num(L, 5), num(L, 6), num(L, 7));
    const float strength = optNum(L, 8, 1.0f);
    lua_pushboolean(L, h && h->shatter && h->shatter(id, p, d, strength));
    return 1;
}
// emit(id) -- replay the object's Particle burst
int l_emit(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    if (h && h->emit) h->emit(id);
    return 0;
}
// toWorld(id, x, y, z) -> wx, wy, wz | nil
int l_toWorld(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const glm::vec3 p{num(L, 2), num(L, 3), num(L, 4)};
    glm::vec3 w(0.0f);
    if (!h || !h->toWorld || !h->toWorld(id, p, w)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, w.x); lua_pushnumber(L, w.y); lua_pushnumber(L, w.z);
    return 3;
}
// groundHeight(x, y, z [, maxDist]) -> y of what lies below | nil
int l_groundHeight(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const glm::vec3 from{num(L, 1), num(L, 2), num(L, 3)};
    const float maxDist = optNum(L, 4, 50.0f);
    float y = 0.0f;
    if (!h || !h->groundHeight || !h->groundHeight(from, maxDist, y)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, y);
    return 1;
}
// --- Pickups -------------------------------------------------------------------
int l_collectibles(lua_State* L) {
    ScriptHost* h = hostOf(L);
    pushIntArray(L, (h && h->collectibles) ? h->collectibles() : std::vector<int>{});
    return 1;
}
// collectible(id) -> {item, icon, category, description, count, inventory,
// radius, points, sound} | nil
int l_collectible(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    ScriptCollectible c;
    if (!h || !h->collectible || !h->collectible(id, c)) { lua_pushnil(L); return 1; }
    lua_createtable(L, 0, 9);
    setStr(L, "item", c.item);            setStr(L, "icon", c.icon);
    setStr(L, "category", c.category);    setStr(L, "description", c.description);
    setInt(L, "count", c.count);          setBool(L, "inventory", c.inventory);
    setNum(L, "radius", c.radius);        setNum(L, "points", c.points);
    setStr(L, "sound", c.sound);
    return 1;
}
// --- Bones ---------------------------------------------------------------------
// bonePos(id, bone) -> x, y, z, rx, ry, rz (world, as last drawn) | nil
int l_bonePos(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const char* bone = luaL_checkstring(L, 2);
    glm::vec3 p(0.0f), r(0.0f);
    if (!h || !h->boneWorld || !h->boneWorld(id, bone, p, r)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, p.x); lua_pushnumber(L, p.y); lua_pushnumber(L, p.z);
    lua_pushnumber(L, r.x); lua_pushnumber(L, r.y); lua_pushnumber(L, r.z);
    return 6;
}
int l_bones(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    const std::vector<std::string> names =
        (h && h->boneNames) ? h->boneNames(id) : std::vector<std::string>{};
    lua_createtable(L, static_cast<int>(names.size()), 0);
    for (std::size_t i = 0; i < names.size(); ++i) {
        lua_pushlstring(L, names[i].c_str(), names[i].size());
        lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}
// attach(child, figure, bone [, x, y, z [, rx, ry, rz [, blend]]]) -> ok. Without
// the numbers the child stays where it is now, relative to the bone; `blend`
// seconds move it there from where it is instead of jumping.
int l_attach(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int child  = static_cast<int>(luaL_checkinteger(L, 1));
    const int figure = static_cast<int>(luaL_checkinteger(L, 2));
    const char* bone = luaL_checkstring(L, 3);
    const bool placed = !lua_isnoneornil(L, 4);
    const glm::vec3 pos{optNum(L, 4, 0.0f), optNum(L, 5, 0.0f), optNum(L, 6, 0.0f)};
    const glm::vec3 rot{optNum(L, 7, 0.0f), optNum(L, 8, 0.0f), optNum(L, 9, 0.0f)};
    const float blend = optNum(L, 10, 0.0f);
    const bool ok = h && h->attach &&
                    h->attach(child, figure, bone, placed ? &pos : nullptr,
                              placed ? &rot : nullptr, blend);
    lua_pushboolean(L, ok);
    return 1;
}
int l_detach(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int child = static_cast<int>(luaL_checkinteger(L, 1));
    if (h && h->detach) h->detach(child);
    return 0;
}
int l_log(lua_State* L) {
    ScriptHost* h = hostOf(L);
    std::string line;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) line += '\t';
        if (const char* s = luaL_tolstring(L, i, nullptr)) line += s;
        lua_pop(L, 1); // luaL_tolstring pushes the converted string
    }
    if (h && h->log) h->log(line);
    else             std::fprintf(stderr, "[Lua] %s\n", line.c_str());
    return 0;
}
// --- Saves: a Lua value <-> JSON -----------------------------------------------
// Tables with keys 1..n and nothing else become arrays, every other table an
// object with its keys as strings; numbers, strings and booleans go as they
// are. Functions and the like are not data and are left out. Nesting is capped:
// a table that contains itself must not take the game down with it.
nlohmann::json luaToJson(lua_State* L, int idx, int depth) {
    idx = lua_absindex(L, idx);
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: return lua_toboolean(L, idx) != 0;
    case LUA_TNUMBER:
        if (lua_isinteger(L, idx)) return static_cast<long long>(lua_tointeger(L, idx));
        return lua_tonumber(L, idx);
    case LUA_TSTRING: return std::string(lua_tostring(L, idx));
    case LUA_TTABLE: {
        if (depth > 32) return nullptr;
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
        lua_Integer keys = 0;
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) { ++keys; lua_pop(L, 1); }
        if (n > 0 && keys == n) {
            nlohmann::json arr = nlohmann::json::array();
            for (lua_Integer i = 1; i <= n; ++i) {
                lua_rawgeti(L, idx, i);
                arr.push_back(luaToJson(L, -1, depth + 1));
                lua_pop(L, 1);
            }
            return arr;
        }
        nlohmann::json obj = nlohmann::json::object();
        lua_pushnil(L);
        while (lua_next(L, idx) != 0) {
            const int kt = lua_type(L, -2);
            const int vt = lua_type(L, -1);
            if ((kt == LUA_TSTRING || kt == LUA_TNUMBER) &&
                (vt == LUA_TBOOLEAN || vt == LUA_TNUMBER || vt == LUA_TSTRING || vt == LUA_TTABLE)) {
                lua_pushvalue(L, -2);   // tostring on a copy: lua_next needs the key as it was
                const std::string key = lua_tostring(L, -1);
                lua_pop(L, 1);
                obj[key] = luaToJson(L, -1, depth + 1);
            }
            lua_pop(L, 1);
        }
        return obj;
    }
    default: return nullptr;
    }
}

void jsonToLua(lua_State* L, const nlohmann::json& j) {
    if (j.is_boolean())             lua_pushboolean(L, j.get<bool>());
    else if (j.is_number_integer()) lua_pushinteger(L, j.get<long long>());
    else if (j.is_number())         lua_pushnumber(L, j.get<double>());
    else if (j.is_string())         lua_pushstring(L, j.get<std::string>().c_str());
    else if (j.is_array()) {
        lua_createtable(L, static_cast<int>(j.size()), 0);
        for (std::size_t i = 0; i < j.size(); ++i) {
            jsonToLua(L, j[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
    } else if (j.is_object()) {
        lua_createtable(L, 0, static_cast<int>(j.size()));
        for (auto it = j.begin(); it != j.end(); ++it) {
            jsonToLua(L, it.value());
            lua_setfield(L, -2, it.key().c_str());
        }
    } else {
        lua_pushnil(L);
    }
}

// game.saveData(slot, value) -> true when written
int l_saveData(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* slot = luaL_checkstring(L, 1);
    luaL_checkany(L, 2);
    const bool ok = savedata::write(h ? h->saveGame : std::string(), slot, luaToJson(L, 2, 0));
    lua_pushboolean(L, ok);
    return 1;
}
// game.loadData(slot) -> the value saved there, or nil
int l_loadData(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* slot = luaL_checkstring(L, 1);
    const auto j = savedata::read(h ? h->saveGame : std::string(), slot);
    if (!j) { lua_pushnil(L); return 1; }
    jsonToLua(L, *j);
    return 1;
}

int l_restart(lua_State* L) {
    ScriptHost* h = hostOf(L);
    if (h && h->restart) h->restart();
    return 0;
}
int l_loadScene(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    if (h && h->loadScene) h->loadScene(name);
    return 0;
}
int l_setCameraPos(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const glm::vec3 p{static_cast<float>(luaL_checknumber(L, 1)),
                      static_cast<float>(luaL_checknumber(L, 2)),
                      static_cast<float>(luaL_checknumber(L, 3))};
    if (h && h->setCamPos) h->setCamPos(p);
    return 0;
}
int l_setCameraDir(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const glm::vec3 d{static_cast<float>(luaL_checknumber(L, 1)),
                      static_cast<float>(luaL_checknumber(L, 2)),
                      static_cast<float>(luaL_checknumber(L, 3))};
    if (h && h->setCamDir) h->setCamDir(d);
    return 0;
}
int l_setCameraFov(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const float f = static_cast<float>(luaL_checknumber(L, 1));
    if (h && h->setCamFov) h->setCamFov(f);
    return 0;
}
// game.setFocus(near, far) -- or game.setFocus() for the view's own focus again.
int l_setFocus(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const float nearM = static_cast<float>(luaL_optnumber(L, 1, 0.0));
    const float farM  = static_cast<float>(luaL_optnumber(L, 2, 0.0));
    if (h && h->setFocus) h->setFocus(nearM, farM);
    return 0;
}
int l_setCamera(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int id = static_cast<int>(luaL_optinteger(L, 1, -1));
    if (h && h->setActiveCamera) h->setActiveCamera(id);
    return 0;
}
int l_screenSize(lua_State* L) {
    ScriptHost* h = hostOf(L);
    lua_pushnumber(L, h ? h->screen.x : 0.0f);
    lua_pushnumber(L, h ? h->screen.y : 0.0f);
    return 2;
}

} // namespace

ScriptSystem::ScriptSystem() { reset(); }

ScriptSystem::~ScriptSystem() {
    if (m_lua) lua_close(m_lua);
}

void ScriptSystem::reset() {
    if (m_lua) lua_close(m_lua);
    // A fresh game is a fresh network: whatever the last one connected to or
    // hosted is closed with its VM.
    scriptnet::reset();
    m_lua = luaL_newstate();
    luaL_openlibs(m_lua);
    installApi();
    m_env.clear();
    m_failed.clear();
    m_lastError.clear();
    m_current.clear();
    m_captureKey.clear();
    m_captureAsked = false;
}

void ScriptSystem::beginFrame() {
    // Held for as long as it is asked for: a holder that did not ask during the
    // last frame has let go.
    if (!m_captureAsked) m_captureKey.clear();
    m_captureAsked = false;
}

bool ScriptSystem::claimInput() {
    if (m_current.empty()) return false;
    if (m_captureKey.empty()) m_captureKey = m_current;
    if (m_captureKey != m_current) return false;
    m_captureAsked = true;
    return true;
}

bool ScriptSystem::inputBlocked() const {
    return !m_captureKey.empty() && m_captureKey != m_current;
}

void ScriptSystem::installApi() {
    lua_State* L = m_lua;
    lua_newtable(L); // the `game` table

    auto fn = [&](const char* name, lua_CFunction f) {
        lua_pushlightuserdata(L, this);   // upvalue 1: owning ScriptSystem
        lua_pushcclosure(L, f, 1);
        lua_setfield(L, -2, name);
    };
    fn("keyDown", l_keyDown);         fn("keyPressed", l_keyPressed);
    fn("mouseDown", l_mouseDown);     fn("mousePressed", l_mousePressed);
    fn("mousePos", l_mousePos);       fn("mouseRay", l_mouseRay);
    fn("mouseWheel", l_mouseWheel);
    fn("showCursor", l_showCursor);
    fn("cameraPos", l_cameraPos);     fn("cameraDir", l_cameraDir);
    fn("spawn", l_spawn);             fn("destroy", l_destroy);
    fn("spawnPrefab", l_spawnPrefab); fn("clone", l_clone);
    fn("getPos", l_getPos);           fn("setPos", l_setPos);
    fn("setLocal", l_setLocal);       fn("setLocals", l_setLocals);
    fn("sound", l_sound);
    fn("setVelocity", l_setVelocity); fn("applyImpulse", l_applyImpulse);
    fn("playSound", l_playSound);
    fn("playAudio", l_playAudio);     fn("stopAudio", l_stopAudio);
    fn("addScore", l_addScore);       fn("getScore", l_getScore);
    fn("setHud", l_setHud);
    // Script-drawn HUD
    fn("hudRect", l_hudRect);         fn("hudGradient", l_hudGradient);
    fn("hudFrame", l_hudFrame);       fn("hudLine", l_hudLine);
    fn("hudCircle", l_hudCircle);     fn("hudTri", l_hudTri);
    fn("hudText", l_hudText);         fn("hudTextSize", l_hudTextSize);
    fn("hudSize", l_hudSize);         fn("setCrosshair", l_setCrosshair);
    fn("hudImage", l_hudImage);       fn("imageSize", l_imageSize);
    fn("worldToHud", l_worldToHud);   fn("captureInput", l_captureInput);
    fn("rest", l_rest);
    // Assets
    fn("assets", l_assets);           fn("findAsset", l_findAsset);
    fn("assetInfo", l_assetInfo);     fn("assetPath", l_assetPath);
    fn("refreshAssets", l_refreshAssets);
    // Models
    fn("loadModel", l_loadModel);     fn("modelInfo", l_modelInfo);
    // Materials
    fn("materials", l_materials);     fn("findMaterial", l_findMaterial);
    fn("materialInfo", l_materialInfo);
    fn("createMaterial", l_createMaterial);
    fn("setMaterialProps", l_setMaterialProps);
    fn("setMaterial", l_setMaterial); fn("getMaterial", l_getMaterial);
    fn("setColor", l_setColor);
    // Entities
    fn("entities", l_entities);       fn("find", l_find);
    fn("findAll", l_findAll);         fn("entityInfo", l_entityInfo);
    fn("getRot", l_getRot);           fn("setRot", l_setRot);
    fn("getScale", l_getScale);       fn("setScale", l_setScale);
    fn("getName", l_getName);         fn("setName", l_setName);
    fn("setActive", l_setActive);     fn("isActive", l_isActive);
    // Animation state machines
    fn("animTrigger", l_animTrigger); fn("animBool", l_animBool);
    fn("animNumber", l_animNumber);   fn("animState", l_animState);
    fn("setParent", l_setParent);     fn("getParent", l_getParent);
    fn("children", l_children);
    // Physics
    fn("getVelocity", l_getVelocity);
    fn("setAngularVelocity", l_setAngularVelocity);
    // Lights
    fn("setLight", l_setLight);
    // World / camera / misc
    fn("terrainHeight", l_terrainHeight);
    fn("waterAt", l_waterAt);
    fn("trees", l_trees);             fn("clearTrees", l_clearTrees);
    fn("raycast", l_raycast);         fn("log", l_log);
    fn("moveCharacter", l_moveCharacter); fn("removeCharacter", l_removeCharacter);
    fn("spawnVehicle", l_spawnVehicle); fn("driveVehicle", l_driveVehicle);
    fn("leaveVehicle", l_leaveVehicle); fn("drivenVehicle", l_drivenVehicle);
    fn("groundHeight", l_groundHeight); fn("castRay", l_castRay);
    fn("orbitFrame", l_orbitFrame);   fn("emit", l_emit);
    fn("toWorld", l_toWorld);
    fn("reach", l_reach);
    fn("decal", l_decal);
    fn("shatter", l_shatter);
    // Pickups and the bones of a figure
    fn("collectibles", l_collectibles); fn("collectible", l_collectible);
    fn("bonePos", l_bonePos);         fn("bones", l_bones);
    fn("attach", l_attach);           fn("detach", l_detach);
    fn("loadScene", l_loadScene);     fn("restart", l_restart);
    fn("saveData", l_saveData);       fn("loadData", l_loadData);
    fn("setCameraPos", l_setCameraPos); fn("setCameraDir", l_setCameraDir);
    fn("setCameraFov", l_setCameraFov); fn("setCamera", l_setCamera);
    fn("setFocus", l_setFocus);
    fn("screenSize", l_screenSize);

    auto k = [&](const char* name, int v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, name);
    };
    // Entity types (match EntityType in SceneTypes.hpp).
    k("BOX", 0); k("RAMP", 1); k("CYLINDER", 2); k("SPHERE", 3);
    k("LIGHT", 4); k("SUN", 5); k("MODEL", 6); k("EMPTY", 7); k("PLANE", 8);
    // Physics modes (game.spawn's `physics`).
    k("PHYSICS_NONE", 0); k("PHYSICS_STATIC", 1); k("PHYSICS_DYNAMIC", 2);
    // Material alpha modes (see game.createMaterial).
    k("ALPHA_OPAQUE", 0); k("ALPHA_CUTOUT", 1); k("ALPHA_BLEND", 2);
    // Light types (game.setLight).
    k("LIGHT_POINT", 0); k("LIGHT_SPOT", 1);
    // Mouse buttons.
    k("MOUSE_LEFT", 0); k("MOUSE_RIGHT", 1); k("MOUSE_MIDDLE", 2);
    // Common GLFW key codes (stable values, so no GLFW header needed here).
    k("KEY_SPACE", 32); k("KEY_ENTER", 257); k("KEY_ESCAPE", 256);
    k("KEY_LSHIFT", 340); k("KEY_LCTRL", 341); k("KEY_LALT", 342);
    k("KEY_RSHIFT", 344); k("KEY_RCTRL", 345); k("KEY_RALT", 346);
    k("KEY_TAB", 258); k("KEY_BACKSPACE", 259); k("KEY_DELETE", 261);
    k("KEY_LEFT", 263); k("KEY_RIGHT", 262); k("KEY_UP", 265); k("KEY_DOWN", 264);
    for (int f = 1; f <= 12; ++f) { // KEY_F1 .. KEY_F12 (GLFW: F1 == 290)
        char name[8];
        std::snprintf(name, sizeof(name), "KEY_F%d", f);
        k(name, 289 + f);
    }
    for (int c = 'A'; c <= 'Z'; ++c) { // KEY_A .. KEY_Z (GLFW uses ASCII uppercase)
        char name[6] = {'K', 'E', 'Y', '_', static_cast<char>(c), '\0'};
        k(name, c);
    }
    for (int d = 0; d <= 9; ++d) {     // KEY_0 .. KEY_9
        char name[7] = {'K', 'E', 'Y', '_', static_cast<char>('0' + d), '\0'};
        k(name, '0' + d);
    }

    lua_setglobal(L, "game");

    // Modules: `require` finds them in the scripts folder (l_searchScripts),
    // so a game too big for one file can be several.
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "searchers");
    lua_pushlightuserdata(L, this);
    lua_pushcclosure(L, l_searchScripts, 1);
    lua_rawseti(L, -2, 2);
    lua_pop(L, 2);

    // The `net` table: lockstep multiplayer (ScriptNet.hpp).
    scriptnet::install(L);

    // `shared`: one plain table every script sees, for scripts that work
    // together -- a figure's controller and its inventory, say. Scripts are
    // otherwise sealed off from each other (each has its own environment);
    // this is the one door, and what goes through it is up to them. Fresh on
    // every Play, like everything else in the VM.
    lua_newtable(L);
    lua_setglobal(L, "shared");

    // The `synth` table: the Synth components, addressed by object id like
    // game.playAudio. Its own table rather than more game.* names, because it
    // is a small instrument API of its own (notes, dials, songs).
    lua_newtable(L);
    fn("play", l_synthPlay);          fn("stop", l_synthStop);
    fn("noteOn", l_synthNoteOn);      fn("noteOff", l_synthNoteOff);
    fn("set", l_synthSet);
    fn("playMidi", l_synthPlayMidi);  fn("stopMidi", l_synthStopMidi);
    fn("isPlaying", l_synthIsPlaying);
    fn("setTempo", l_synthSetTempo);
    fn("note", l_synthNote);          fn("lastError", l_synthError);
    lua_setglobal(L, "synth");

    // The `music` table: one song with a clock, for rhythm games (MusicSystem).
    lua_newtable(L);
    fn("load", l_musicLoad);          fn("play", l_musicPlay);
    fn("stop", l_musicStop);          fn("pause", l_musicPause);
    fn("resume", l_musicResume);      fn("fade", l_musicFade);
    fn("time", l_musicTime);          fn("duration", l_musicDuration);
    fn("isPlaying", l_musicIsPlaying); fn("isPaused", l_musicIsPaused);
    fn("setFilter", l_musicSetFilter); fn("setVolume", l_musicSetVolume);
    fn("sampleRate", l_musicSampleRate);
    fn("spectrum", l_musicSpectrum);  fn("waveform", l_musicWaveform);
    fn("analyze", l_musicAnalyze);
    lua_setglobal(L, "music");
}

std::string ScriptSystem::keyOf(int id, const std::string& file) {
    return std::to_string(id) + "|" + file;
}

void ScriptSystem::removeEntity(int id) {
    // Every script on it, not one: an object may carry several, and a reused id
    // that inherited a leftover environment would wake up mid-behaviour.
    const std::string prefix = std::to_string(id) + "|";
    for (auto it = m_env.begin(); it != m_env.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            luaL_unref(m_lua, LUA_REGISTRYINDEX, it->second);
            it = m_env.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_failed.begin(); it != m_failed.end();)
        it = (it->rfind(prefix, 0) == 0) ? m_failed.erase(it) : std::next(it);
}

void ScriptSystem::fail(const std::string& key, int id, const char* what) {
    m_failed.insert(key);
    m_lastError = what ? what : "unknown Lua error";
    std::fprintf(stderr, "[Fitzel] script error (entity %d, %s): %s\n", id,
                 key.c_str(), m_lastError.c_str());
}

void ScriptSystem::pushEntityTable(const Entity& e) {
    lua_State* L = m_lua;
    lua_createtable(L, 0, 12);
    // x/y/z and rx/ry/rz are the entity's LOCAL transform (relative to its
    // parent); the scene-graph derives world. For a root object local == world.
    setNum(L, "x", e.localCenter.x);   setNum(L, "y", e.localCenter.y);   setNum(L, "z", e.localCenter.z);
    setNum(L, "rx", e.localRotation.x); setNum(L, "ry", e.localRotation.y); setNum(L, "rz", e.localRotation.z);
    setNum(L, "sx", e.half.x);    setNum(L, "sy", e.half.y);    setNum(L, "sz", e.half.z);
    lua_pushstring(L, e.name.c_str()); lua_setfield(L, -2, "name");
    lua_pushinteger(L, e.id);          lua_setfield(L, -2, "id");
    // Read-only context, so a script can branch on what it is attached to.
    setInt(L, "type", static_cast<int>(e.type));
    setInt(L, "parent", e.parent);
    setBool(L, "active", e.active);
}

void ScriptSystem::readEntityTable(Entity& e) {
    lua_State* L = m_lua;
    e.localCenter.x   = getNum(L, "x", e.localCenter.x);
    e.localCenter.y   = getNum(L, "y", e.localCenter.y);
    e.localCenter.z   = getNum(L, "z", e.localCenter.z);
    e.localRotation.x = getNum(L, "rx", e.localRotation.x);
    e.localRotation.y = getNum(L, "ry", e.localRotation.y);
    e.localRotation.z = getNum(L, "rz", e.localRotation.z);
    e.half.x     = getNum(L, "sx", e.half.x);
    e.half.y     = getNum(L, "sy", e.half.y);
    e.half.z     = getNum(L, "sz", e.half.z);
}

bool ScriptSystem::loadFor(const Entity& e, const ScriptComponent& sc,
                           const std::string& key, const std::string& path) {
    lua_State* L = m_lua;
    if (loadLuaChunk(L, path) != LUA_OK) {
        fail(key, e.id, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    // Give the chunk its own environment (with the global table as a read
    // fallback), so each script's state is isolated -- from other entities, and
    // from the object's own other scripts.
    lua_newtable(L);                 // env
    lua_newtable(L);                 // metatable
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);            // keep a copy of env to register
    const int envRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_setupvalue(L, -2, 1);        // chunk's _ENV = env
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) { // run chunk (defines start/update)
        fail(key, e.id, lua_tostring(L, -1));
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, envRef);
        return false;
    }
    m_env[key] = envRef;
    applyParams(envRef, sc); // inspector overrides win over the module defaults
    return true;
}

// Push each ScriptComponent override into the entity's environment as a global,
// after the chunk (which set the defaults) has run and before start() sees them.
void ScriptSystem::applyParams(int envRef, const ScriptComponent& sc) {
    if (sc.params.empty()) return;
    lua_State* L = m_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, envRef); // env at -1
    const int env = lua_gettop(L);
    for (const ScriptParam& p : sc.params) {
        switch (p.type) {
            case ScriptParam::Type::Number: lua_pushnumber(L, p.num); break;
            case ScriptParam::Type::Bool:   lua_pushboolean(L, p.b);  break;
            case ScriptParam::Type::String: lua_pushlstring(L, p.str.c_str(), p.str.size()); break;
            case ScriptParam::Type::Vec3:
            case ScriptParam::Type::Color: {
                // Same layout as setVec: readable as t.x/y/z, t.r/g/b or t[1..3].
                lua_createtable(L, 3, 6);
                const float c[3] = {p.vec.x, p.vec.y, p.vec.z};
                const char* xyz[3] = {"x", "y", "z"};
                const char* rgb[3] = {"r", "g", "b"};
                for (int i = 0; i < 3; ++i) {
                    lua_pushnumber(L, c[i]); lua_seti(L, -2, i + 1);
                    lua_pushnumber(L, c[i]); lua_setfield(L, -2, xyz[i]);
                    lua_pushnumber(L, c[i]); lua_setfield(L, -2, rgb[i]);
                }
                break;
            }
        }
        lua_setfield(L, env, p.name.c_str());
    }
    lua_pop(L, 1); // env
}

bool ScriptSystem::callFunction(Entity& e, const std::string& key, const char* fn,
                               float dt, float time) {
    lua_State* L = m_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, m_env[key]); // env
    lua_getfield(L, -1, fn);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 2);
        return true; // optional function is simply absent
    }
    // Keep a handle on the entity table so we can read mutations back after
    // the call (pcall pops its arguments).
    pushEntityTable(e);
    const int argRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_rawgeti(L, LUA_REGISTRYINDEX, argRef);
    lua_pushnumber(L, dt);
    lua_pushnumber(L, time);
    m_current = key;   // whose game.captureInput / game.keyDown this is
    const int status = lua_pcall(L, 3, 0, 0);
    m_current.clear();
    if (status != LUA_OK) {
        fail(key, e.id, lua_tostring(L, -1));
        lua_pop(L, 2); // error message + env
        luaL_unref(L, LUA_REGISTRYINDEX, argRef);
        return false;
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, argRef);
    readEntityTable(e);
    lua_pop(L, 2); // entity table + env
    luaL_unref(L, LUA_REGISTRYINDEX, argRef);
    return true;
}

void ScriptSystem::update(Entity& e, const ScriptComponent& sc,
                          const std::string& scriptPath, float dt, float time) {
    // The running script is the (entity, file) pair, so an object's second and
    // third scripts are their own machines with their own environments -- and a
    // broken one disables itself rather than its neighbours.
    const std::string key = keyOf(e.id, sc.file);
    if (m_failed.count(key)) return;
    if (!m_env.count(key)) {
        if (!loadFor(e, sc, key, scriptPath)) return;
        if (!callFunction(e, key, "start", dt, time)) return;
    }
    callFunction(e, key, "update", dt, time);
}

namespace {

// A "colour" if the field's name reads like one -- so a 3-number table gets a
// colour swatch rather than a bare xyz drag.
bool looksLikeColor(const std::string& name) {
    std::string n;
    for (char c : name) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    auto has = [&](const char* s) { return n.find(s) != std::string::npos; };
    return has("color") || has("colour") || has("tint") || has("rgb");
}

// Pull three floats from the table at stack index `t`, trying array [1..3], then
// x/y/z, then r/g/b. Sets `hasRgb` if it read via r/g/b keys. Returns whether all
// three came back as numbers (i.e. this really is a 3-vector).
bool readVec3(lua_State* L, int t, glm::vec3& out, bool& hasRgb) {
    hasRgb = false;
    int got = 0;
    for (int i = 0; i < 3; ++i) {
        lua_geti(L, t, i + 1);
        if (lua_isnumber(L, -1)) { out[i] = static_cast<float>(lua_tonumber(L, -1)); ++got; }
        lua_pop(L, 1);
    }
    if (got == 3) return true;
    const char* xyz[3] = {"x", "y", "z"};
    got = 0;
    for (int i = 0; i < 3; ++i) {
        lua_getfield(L, t, xyz[i]);
        if (lua_isnumber(L, -1)) { out[i] = static_cast<float>(lua_tonumber(L, -1)); ++got; }
        lua_pop(L, 1);
    }
    if (got == 3) return true;
    const char* rgb[3] = {"r", "g", "b"};
    got = 0;
    for (int i = 0; i < 3; ++i) {
        lua_getfield(L, t, rgb[i]);
        if (lua_isnumber(L, -1)) { out[i] = static_cast<float>(lua_tonumber(L, -1)); ++got; }
        lua_pop(L, 1);
    }
    if (got == 3) { hasRgb = true; return true; }
    return false;
}

} // namespace

std::vector<ScriptParam> ScriptSystem::scanParams(const std::string& path,
                                                  std::string* err) {
    std::vector<ScriptParam> out;
    // A throwaway VM: running an arbitrary module body must not touch the play VM.
    lua_State* L = luaL_newstate();
    if (!L) { if (err) *err = "out of memory"; return out; }
    luaL_openlibs(L);

    // Stub `game`: any access yields a no-op function, so module-level code that
    // pokes the API while we scan just gets nil back instead of faulting.
    lua_newtable(L);                       // game
    lua_newtable(L);                       // metatable
    lua_pushcclosure(L,
        [](lua_State* s) -> int {          // __index(table, key) -> a no-op fn
            lua_pushcfunction(s, [](lua_State*) { return 0; });
            return 1;
        }, 0);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_setglobal(L, "game");

    if (loadLuaChunk(L, path) != LUA_OK) {
        if (err) *err = lua_tostring(L, -1);
        lua_close(L);
        return out;
    }
    // Own environment (globals as read fallback), exactly like loadFor -- so the
    // keys we enumerate are only what THIS chunk assigned. The env is held by a
    // registry ref so the chunk can be pcall'd (it must sit alone on the stack top).
    lua_newtable(L);                 // env
    lua_newtable(L);                 // metatable
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);            // copy of env to register
    const int envRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_setupvalue(L, -2, 1);        // chunk's _ENV = env (pops env)
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
        if (err) *err = lua_tostring(L, -1);
        luaL_unref(L, LUA_REGISTRYINDEX, envRef);
        lua_close(L);
        return out;
    }

    // Enumerate the environment: every string key of a supported value type
    // becomes a parameter. Functions (start/update/helpers) and private "_name"
    // globals are skipped.
    lua_rawgeti(L, LUA_REGISTRYINDEX, envRef); // env at the top
    const int env = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, env) != 0) {
        // key at -2, value at -1
        if (lua_type(L, -2) == LUA_TSTRING) {
            const std::string name = lua_tostring(L, -2);
            if (!name.empty() && name[0] != '_') {
                ScriptParam p;
                p.name = name;
                bool keep = true;
                switch (lua_type(L, -1)) {
                    case LUA_TNUMBER:
                        p.type = ScriptParam::Type::Number;
                        p.num  = lua_tonumber(L, -1);
                        break;
                    case LUA_TBOOLEAN:
                        p.type = ScriptParam::Type::Bool;
                        p.b    = lua_toboolean(L, -1) != 0;
                        break;
                    case LUA_TSTRING:
                        p.type = ScriptParam::Type::String;
                        p.str  = lua_tostring(L, -1);
                        break;
                    case LUA_TTABLE: {
                        glm::vec3 v{0.0f};
                        bool hasRgb = false;
                        if (readVec3(L, lua_gettop(L), v, hasRgb)) {
                            p.type = (hasRgb || looksLikeColor(name))
                                         ? ScriptParam::Type::Color
                                         : ScriptParam::Type::Vec3;
                            p.vec  = v;
                        } else {
                            keep = false; // a table that isn't a 3-vector
                        }
                        break;
                    }
                    default:
                        keep = false; // function, userdata, nil, ...
                        break;
                }
                if (keep) out.push_back(std::move(p));
            }
        }
        lua_pop(L, 1); // value; keep key for the next lua_next
    }
    lua_pop(L, 1); // env
    luaL_unref(L, LUA_REGISTRYINDEX, envRef);
    lua_close(L);

    // Stable, predictable order (Lua table iteration order is unspecified).
    std::sort(out.begin(), out.end(),
              [](const ScriptParam& a, const ScriptParam& b) { return a.name < b.name; });
    return out;
}
