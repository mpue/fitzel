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

} // namespace

ScriptSystem::ScriptSystem() { reset(); }

ScriptSystem::~ScriptSystem() {
    if (m_lua) lua_close(m_lua);
}

void ScriptSystem::reset() {
    if (m_lua) lua_close(m_lua);
    m_lua = luaL_newstate();
    luaL_openlibs(m_lua);
    m_env.clear();
    m_failed.clear();
    m_lastError.clear();
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
    setNum(L, "x", e.center.x);   setNum(L, "y", e.center.y);   setNum(L, "z", e.center.z);
    setNum(L, "rx", e.rotation.x); setNum(L, "ry", e.rotation.y); setNum(L, "rz", e.rotation.z);
    setNum(L, "sx", e.half.x);    setNum(L, "sy", e.half.y);    setNum(L, "sz", e.half.z);
    lua_pushstring(L, e.name.c_str()); lua_setfield(L, -2, "name");
    lua_pushinteger(L, e.id);          lua_setfield(L, -2, "id");
}

void ScriptSystem::readEntityTable(Entity& e) {
    lua_State* L = m_lua;
    e.center.x   = getNum(L, "x", e.center.x);
    e.center.y   = getNum(L, "y", e.center.y);
    e.center.z   = getNum(L, "z", e.center.z);
    e.rotation.x = getNum(L, "rx", e.rotation.x);
    e.rotation.y = getNum(L, "ry", e.rotation.y);
    e.rotation.z = getNum(L, "rz", e.rotation.z);
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
