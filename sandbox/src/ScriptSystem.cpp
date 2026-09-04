#include "ScriptSystem.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <fitzel/asset/Vfs.hpp>

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
    lua_pushboolean(L, h && h->keyDown && h->keyDown(k));
    return 1;
}
int l_keyPressed(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int k = static_cast<int>(luaL_checkinteger(L, 1));
    lua_pushboolean(L, h && h->keyPressed && h->keyPressed(k));
    return 1;
}
int l_mouseDown(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int b = static_cast<int>(luaL_optinteger(L, 1, 0));
    lua_pushboolean(L, h && h->mouseDown && h->mouseDown(b));
    return 1;
}
int l_mousePressed(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const int b = static_cast<int>(luaL_optinteger(L, 1, 0));
    lua_pushboolean(L, h && h->mousePressed && h->mousePressed(b));
    return 1;
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
    if (h && h->applyImpulse) h->applyImpulse(id, j);
    return 0;
}
int l_playSound(lua_State* L) {
    ScriptHost* h = hostOf(L);
    const char* name = luaL_checkstring(L, 1);
    if (h && h->playSound) h->playSound(name);
    return 0;
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
    m_lua = luaL_newstate();
    luaL_openlibs(m_lua);
    installApi();
    m_env.clear();
    m_failed.clear();
    m_lastError.clear();
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
    fn("cameraPos", l_cameraPos);     fn("cameraDir", l_cameraDir);
    fn("spawn", l_spawn);             fn("destroy", l_destroy);
    fn("spawnPrefab", l_spawnPrefab);
    fn("getPos", l_getPos);           fn("setPos", l_setPos);
    fn("setVelocity", l_setVelocity); fn("applyImpulse", l_applyImpulse);
    fn("playSound", l_playSound);
    fn("playAudio", l_playAudio);     fn("stopAudio", l_stopAudio);
    fn("addScore", l_addScore);       fn("getScore", l_getScore);
    fn("setHud", l_setHud);
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
    fn("raycast", l_raycast);         fn("log", l_log);
    fn("loadScene", l_loadScene);
    fn("setCameraPos", l_setCameraPos); fn("setCameraDir", l_setCameraDir);
    fn("setCameraFov", l_setCameraFov); fn("setCamera", l_setCamera);
    fn("screenSize", l_screenSize);

    auto k = [&](const char* name, int v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, name);
    };
    // Entity types (match EntityType in SceneTypes.hpp).
    k("BOX", 0); k("RAMP", 1); k("CYLINDER", 2); k("SPHERE", 3);
    k("LIGHT", 4); k("SUN", 5); k("MODEL", 6); k("EMPTY", 7);
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
    std::fprintf(stderr, "[Fitzel] script error (entity %d, %s): %s' + chr(92) + 'n", id,
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
    if (lua_pcall(L, 3, 0, 0) != LUA_OK) {
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
