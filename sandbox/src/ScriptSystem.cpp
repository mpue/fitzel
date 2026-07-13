#include "ScriptSystem.hpp"

#include <cstdio>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

namespace {

void setNum(lua_State* L, const char* k, float v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
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
    lua_getfield(L, 1, "name");   if (lua_isstring(L, -1)) s.name   = lua_tostring(L, -1); lua_pop(L, 1);
    lua_getfield(L, 1, "script"); if (lua_isstring(L, -1)) s.script = lua_tostring(L, -1); lua_pop(L, 1);
    const int id = (h && h->spawn) ? h->spawn(s) : 0;
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
    fn("getPos", l_getPos);           fn("setPos", l_setPos);
    fn("setVelocity", l_setVelocity); fn("applyImpulse", l_applyImpulse);
    fn("playSound", l_playSound);
    fn("playAudio", l_playAudio);     fn("stopAudio", l_stopAudio);
    fn("addScore", l_addScore);       fn("getScore", l_getScore);
    fn("setHud", l_setHud);

    auto k = [&](const char* name, int v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, name);
    };
    // Entity types (match EntityType in SceneTypes.hpp).
    k("BOX", 0); k("RAMP", 1); k("CYLINDER", 2); k("SPHERE", 3);
    // Mouse buttons.
    k("MOUSE_LEFT", 0); k("MOUSE_RIGHT", 1); k("MOUSE_MIDDLE", 2);
    // Common GLFW key codes (stable values, so no GLFW header needed here).
    k("KEY_SPACE", 32); k("KEY_ENTER", 257); k("KEY_ESCAPE", 256);
    k("KEY_LSHIFT", 340); k("KEY_LCTRL", 341);
    k("KEY_LEFT", 263); k("KEY_RIGHT", 262); k("KEY_UP", 265); k("KEY_DOWN", 264);
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

void ScriptSystem::removeEntity(int id) {
    auto it = m_env.find(id);
    if (it != m_env.end()) {
        luaL_unref(m_lua, LUA_REGISTRYINDEX, it->second);
        m_env.erase(it);
    }
    m_failed.erase(id);
}

void ScriptSystem::fail(int id, const char* what) {
    m_failed.insert(id);
    m_lastError = what ? what : "unknown Lua error";
    std::fprintf(stderr, "[Fitzel] script error (entity %d): %s\n", id,
                 m_lastError.c_str());
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

bool ScriptSystem::loadFor(const Entity& e, const std::string& path) {
    lua_State* L = m_lua;
    if (luaL_loadfile(L, path.c_str()) != LUA_OK) {
        fail(e.id, lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    // Give the chunk its own environment (with the global table as a read
    // fallback), so each entity's script state is isolated.
    lua_newtable(L);                 // env
    lua_newtable(L);                 // metatable
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);            // keep a copy of env to register
    const int envRef = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_setupvalue(L, -2, 1);        // chunk's _ENV = env
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) { // run chunk (defines start/update)
        fail(e.id, lua_tostring(L, -1));
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, envRef);
        return false;
    }
    m_env[e.id] = envRef;
    return true;
}

bool ScriptSystem::callFunction(Entity& e, const char* fn, float dt, float time) {
    lua_State* L = m_lua;
    lua_rawgeti(L, LUA_REGISTRYINDEX, m_env[e.id]); // env
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
        fail(e.id, lua_tostring(L, -1));
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

void ScriptSystem::update(Entity& e, const std::string& scriptPath,
                          float dt, float time) {
    if (m_failed.count(e.id)) return;
    if (!m_env.count(e.id)) {
        if (!loadFor(e, scriptPath)) return;
        if (!callFunction(e, "start", dt, time)) return;
    }
    callFunction(e, "update", dt, time);
}
