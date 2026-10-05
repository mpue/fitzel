#include "ScriptNet.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include "NetRelay.hpp"
#include "NetSocket.hpp"

namespace scriptnet {

namespace {

enum class State { Idle, Connecting, Connected, Closed };

struct Link {
    netsock::Conn conn;
    State         state = State::Idle;
    std::string   error;
    std::vector<std::string> inbox;   // arrived, not yet handed to the script
};

Link&                             link() { static Link l; return l; }
std::unique_ptr<netrelay::Server>& hosted() { static std::unique_ptr<netrelay::Server> s; return s; }

double now() {
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return duration<double>(steady_clock::now() - t0).count();
}

void closeLink(const char* why) {
    Link& l = link();
    l.conn.close();
    if (l.state == State::Connecting || l.state == State::Connected) {
        l.state = State::Closed;
        if (why && l.error.empty()) l.error = why;
    }
}

// One step of everything: the hosted relay, the connect, the reads and writes.
void pump() {
    if (auto& s = hosted(); s) s->tick(now());
    Link& l = link();
    if (l.state == State::Connecting) {
        std::string err;
        const int r = netsock::connectDone(l.conn.sock, &err);
        if (r > 0) { l.conn.open = true; l.state = State::Connected; }
        else if (r < 0) { l.error = err; closeLink("connection failed"); }
    }
    if (l.state == State::Connected) {
        if (!l.conn.read(l.inbox)) closeLink("the server closed the connection");
        else if (!l.conn.flush()) closeLink("send failed");
    }
    if (auto& s = hosted(); s) s->tick(now());   // answer our own writes this frame
}

const char* stateName(State s) {
    switch (s) {
        case State::Idle:       return "idle";
        case State::Connecting: return "connecting";
        case State::Connected:  return "connected";
        case State::Closed:     return "closed";
    }
    return "idle";
}

int l_connect(lua_State* L) {
    const char* host = luaL_checkstring(L, 1);
    const int   port = static_cast<int>(luaL_optinteger(L, 2, 27960));
    Link& l = link();
    l.conn.close();
    l.inbox.clear();
    l.error.clear();
    std::string err;
    const netsock::Handle h = netsock::connectTo(host, port, &err);
    if (h == netsock::kInvalid) {
        l.state = State::Closed;
        l.error = err;
        lua_pushboolean(L, 0);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    l.conn.sock = h;
    l.state     = State::Connecting;
    lua_pushboolean(L, 1);
    return 1;
}

int l_status(lua_State* L) {
    const Link& l = link();
    lua_pushstring(L, stateName(l.state));
    lua_pushstring(L, l.error.c_str());
    return 2;
}

int l_send(lua_State* L) {
    std::size_t n = 0;
    const char* msg = luaL_checklstring(L, 1, &n);
    Link& l = link();
    if (l.state != State::Connected && l.state != State::Connecting) {
        lua_pushboolean(L, 0);
        return 1;
    }
    l.conn.queue(std::string(msg, n));
    lua_pushboolean(L, 1);
    return 1;
}

int l_poll(lua_State* L) {
    pump();
    Link& l = link();
    lua_createtable(L, static_cast<int>(l.inbox.size()), 0);
    for (std::size_t i = 0; i < l.inbox.size(); ++i) {
        lua_pushlstring(L, l.inbox[i].data(), l.inbox[i].size());
        lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    l.inbox.clear();
    return 1;
}

int l_close(lua_State*) {
    Link& l = link();
    l.conn.close();
    l.inbox.clear();
    l.state = State::Idle;
    l.error.clear();
    return 0;
}

int l_serve(lua_State* L) {
    const int port   = static_cast<int>(luaL_optinteger(L, 1, 27960));
    const int turnMs = static_cast<int>(luaL_optinteger(L, 2, 100));
    netrelay::Options o;
    o.turnMs = turnMs < 20 ? 20 : turnMs;
    auto s = std::make_unique<netrelay::Server>(o);
    s->log = [](const std::string& line) { std::fprintf(stderr, "[net] %s\n", line.c_str()); };
    std::string err;
    if (!s->listen(port, &err)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    const int bound = s->port();
    hosted() = std::move(s);
    lua_pushboolean(L, 1);
    lua_pushinteger(L, bound);
    return 2;
}

int l_stopServer(lua_State*) {
    hosted().reset();
    return 0;
}

int l_serverInfo(lua_State* L) {
    const auto& s = hosted();
    lua_pushboolean(L, s && s->running());
    lua_pushinteger(L, s ? s->port() : 0);
    lua_pushinteger(L, s ? s->clientCount() : 0);
    lua_pushinteger(L, s ? s->roomCount() : 0);
    return 4;
}

int l_addresses(lua_State* L) {
    const std::vector<std::string> a = netsock::localAddresses();
    lua_createtable(L, static_cast<int>(a.size()), 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        lua_pushstring(L, a[i].c_str());
        lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

int l_clock(lua_State* L) {
    lua_pushnumber(L, now());
    return 1;
}

} // namespace

void install(lua_State* L) {
    lua_newtable(L);
    const luaL_Reg fns[] = {
        {"connect", l_connect},       {"status", l_status},     {"send", l_send},
        {"poll", l_poll},             {"close", l_close},       {"serve", l_serve},
        {"stopServer", l_stopServer}, {"serverInfo", l_serverInfo},
        {"addresses", l_addresses},   {"clock", l_clock},       {nullptr, nullptr}};
    luaL_setfuncs(L, fns, 0);
    lua_setglobal(L, "net");
}

void reset() {
    Link& l = link();
    l.conn.close();
    l.inbox.clear();
    l.state = State::Idle;
    l.error.clear();
    hosted().reset();
}

} // namespace scriptnet
