#include "ScriptLlm.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#include "LuaJson.hpp"
#include "NetSocket.hpp"

namespace scriptllm {

namespace {

#ifdef _WIN32
using Sock = SOCKET;
const Sock kNoSock = INVALID_SOCKET;
void closeSock(Sock s) { closesocket(s); }
#else
using Sock = int;
const Sock kNoSock = -1;
void closeSock(Sock s) { ::close(s); }
#endif

// A model that has to be loaded first takes half a minute on a cold start;
// one that has not answered in this long is not going to.
constexpr int kTimeoutSec = 180;

struct Request {
    long long   id = 0;
    unsigned    gen = 0;     // the VM generation that asked (see reset)
    std::string host;
    int         port = 0;
    std::string path;
    std::string body;
    bool        wantsJson = false;   // format given: decode the answer too
};

struct Answer {
    long long      id = 0;
    unsigned       gen = 0;
    bool           ok = false;
    std::string    text;     // what the model said, thinking taken out
    std::string    error;
    nlohmann::json data;     // `text` decoded, when JSON was asked for and came
    double         ms = 0.0;
    long long      tokens = 0;
};

// --- HTTP -------------------------------------------------------------------
// Just enough HTTP/1.1 for a server on this machine: one POST per connection
// (Connection: close), the reply read to the end, a chunked body put back
// together. No TLS -- this is for localhost (or the LAN), not the internet.

bool dechunk(const std::string& in, std::string& out) {
    out.clear();
    std::size_t p = 0;
    for (;;) {
        const std::size_t eol = in.find("\r\n", p);
        if (eol == std::string::npos) return false;
        const unsigned long n = std::strtoul(in.substr(p, eol - p).c_str(), nullptr, 16);
        p = eol + 2;
        if (n == 0) return true;
        if (p + n > in.size()) return false;
        out.append(in, p, n);
        p += n + 2;   // the chunk's own CRLF
    }
}

class Http {
public:
    // The connection being answered, so a shutdown can cut it (a blocked recv
    // returns once its socket is closed under it).
    std::mutex* lock = nullptr;
    Sock*       current = nullptr;

    bool post(const Request& r, int& status, std::string& reply, std::string& err) {
        netsock::init();
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const std::string port = std::to_string(r.port);
        if (getaddrinfo(r.host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
            err = "cannot resolve " + r.host;
            return false;
        }
        Sock s = kNoSock;
        for (addrinfo* a = res; a; a = a->ai_next) {
            s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (s == kNoSock) continue;
            if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0) break;
            closeSock(s);
            s = kNoSock;
        }
        freeaddrinfo(res);
        if (s == kNoSock) {
            err = "no model server at " + r.host + ":" + port + " (is Ollama running?)";
            return false;
        }
#ifdef _WIN32
        const DWORD tv = kTimeoutSec * 1000;
#else
        timeval tv{kTimeoutSec, 0};
#endif
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        {
            std::lock_guard<std::mutex> g(*lock);
            *current = s;
        }
        std::string msg = "POST " + r.path + " HTTP/1.1\r\nHost: " + r.host + ":" + port +
                          "\r\nContent-Type: application/json\r\nContent-Length: " +
                          std::to_string(r.body.size()) +
                          "\r\nConnection: close\r\n\r\n" + r.body;
        bool ok = true;
        for (std::size_t sent = 0; sent < msg.size();) {
            const int n = ::send(s, msg.data() + sent, static_cast<int>(msg.size() - sent), 0);
            if (n <= 0) { ok = false; err = "send failed"; break; }
            sent += static_cast<std::size_t>(n);
        }
        std::string in;
        if (ok) {
            char buf[16384];
            for (;;) {
                const int n = ::recv(s, buf, sizeof(buf), 0);
                if (n > 0) { in.append(buf, static_cast<std::size_t>(n)); continue; }
                if (n < 0) { ok = false; err = "no answer (timed out or cut off)"; }
                break;
            }
        }
        {
            std::lock_guard<std::mutex> g(*lock);
            if (*current == s) { *current = kNoSock; closeSock(s); }
        }
        if (!ok) return false;
        const std::size_t head = in.find("\r\n\r\n");
        if (head == std::string::npos || in.compare(0, 5, "HTTP/") != 0) {
            err = "not an HTTP answer";
            return false;
        }
        const std::size_t sp = in.find(' ');
        status = sp != std::string::npos ? std::atoi(in.c_str() + sp + 1) : 0;
        std::string headers = in.substr(0, head);
        std::transform(headers.begin(), headers.end(), headers.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string body = in.substr(head + 4);
        if (headers.find("transfer-encoding: chunked") != std::string::npos) {
            if (!dechunk(body, reply)) { err = "broken chunked answer"; return false; }
        } else {
            reply = std::move(body);
        }
        return true;
    }
};

// What the model said, without the reasoning a thinking model writes first.
std::string withoutThinking(std::string s) {
    for (;;) {
        const std::size_t a = s.find("<think>");
        if (a == std::string::npos) break;
        const std::size_t b = s.find("</think>", a);
        s = b == std::string::npos ? s.substr(0, a) : s.substr(0, a) + s.substr(b + 8);
    }
    const std::size_t first = s.find_first_not_of(" \t\r\n");
    const std::size_t last  = s.find_last_not_of(" \t\r\n");
    return first == std::string::npos ? std::string() : s.substr(first, last - first + 1);
}

// --- The worker ---------------------------------------------------------------

struct Worker {
    std::mutex              lock;
    std::condition_variable wake;
    std::deque<Request>     queue;
    std::vector<Answer>     done;
    unsigned                gen = 0;
    long long               nextId = 1;
    int                     busy = 0;        // requests being answered (0 or 1)
    bool                    stop = false;
    Sock                    current = kNoSock;
    std::thread             thread;

    std::string host  = "127.0.0.1";
    int         port  = 11434;
    std::string model = "qwen3:4b";

    ~Worker() {
        {
            std::lock_guard<std::mutex> g(lock);
            stop = true;
            queue.clear();
            if (current != kNoSock) { closeSock(current); current = kNoSock; }
        }
        wake.notify_all();
        if (thread.joinable()) thread.join();
    }

    void ensureRunning() {
        if (!thread.joinable()) thread = std::thread([this] { run(); });
    }

    void run() {
        Http http;
        http.lock    = &lock;
        http.current = &current;
        for (;;) {
            Request r;
            {
                std::unique_lock<std::mutex> g(lock);
                wake.wait(g, [this] { return stop || !queue.empty(); });
                if (stop) return;
                r = std::move(queue.front());
                queue.pop_front();
                busy = 1;
            }
            Answer a;
            a.id  = r.id;
            a.gen = r.gen;
            const auto t0 = std::chrono::steady_clock::now();
            int status = 0;
            std::string reply;
            if (http.post(r, status, reply, a.error)) {
                const nlohmann::json j = nlohmann::json::parse(reply, nullptr, false);
                if (j.is_discarded()) {
                    a.error = "the server's answer is not JSON (HTTP " + std::to_string(status) + ")";
                } else if (status != 200 || j.contains("error")) {
                    a.error = j.contains("error") && j["error"].is_string()
                                  ? j["error"].get<std::string>()
                                  : "HTTP " + std::to_string(status);
                } else {
                    std::string text;
                    if (j.contains("message") && j["message"].is_object())
                        text = j["message"].value("content", std::string());
                    else
                        text = j.value("response", std::string());
                    a.text   = withoutThinking(text);
                    a.tokens = j.value("eval_count", 0LL);
                    a.ok     = true;
                    if (r.wantsJson) {
                        nlohmann::json d = nlohmann::json::parse(a.text, nullptr, false);
                        if (!d.is_discarded()) a.data = std::move(d);
                    }
                }
            }
            a.ms = std::chrono::duration<double, std::milli>(
                       std::chrono::steady_clock::now() - t0).count();
            std::lock_guard<std::mutex> g(lock);
            busy = 0;
            if (stop) return;
            if (a.gen == gen) done.push_back(std::move(a));
        }
    }
};

Worker& worker() { static Worker w; return w; }

// --- llm.* ------------------------------------------------------------------------

// llm.chat(req) -> id
//   req.messages  {{role=, content=}, ...}   or req.prompt (one user message)
//   req.system    a system message put first
//   req.model     default llm.model()
//   req.format    "json" or a JSON schema table: the answer comes decoded as .data
//   req.priority  true: ahead of everything still waiting (a player is)
//   everything else (options, think, keep_alive, ...) goes to Ollama as it is.
int l_chat(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    nlohmann::json body = luajson::toJson(L, 1);
    if (!body.is_object()) body = nlohmann::json::object();
    Worker& w = worker();
    nlohmann::json msgs = body.contains("messages") && body["messages"].is_array()
                              ? body["messages"] : nlohmann::json::array();
    if (body.contains("prompt") && body["prompt"].is_string()) {
        msgs.push_back({{"role", "user"}, {"content", body["prompt"]}});
        body.erase("prompt");
    }
    if (body.contains("system") && body["system"].is_string()) {
        msgs.insert(msgs.begin(), nlohmann::json{{"role", "system"}, {"content", body["system"]}});
        body.erase("system");
    }
    if (msgs.empty()) return luaL_error(L, "llm.chat: give it messages or a prompt");
    body["messages"] = std::move(msgs);
    body["stream"]   = false;
    // A small thinking model spends seconds reasoning before every line; a
    // character in a street has to answer now. Ask for thinking explicitly.
    if (!body.contains("think")) body["think"] = false;
    Request r;
    {
        std::lock_guard<std::mutex> g(w.lock);
        if (!body.contains("model")) body["model"] = w.model;
        r.id   = w.nextId++;
        r.gen  = w.gen;
        r.host = w.host;
        r.port = w.port;
    }
    // A player waiting for an answer goes before the characters' own
    // planning: priority = true puts the request at the head of the queue.
    const bool first = body.value("priority", false);
    body.erase("priority");
    r.path      = "/api/chat";
    r.wantsJson = body.contains("format");
    r.body      = body.dump();
    const long long id = r.id;
    {
        std::lock_guard<std::mutex> g(w.lock);
        if (first) w.queue.push_front(std::move(r));
        else       w.queue.push_back(std::move(r));
        w.ensureRunning();
    }
    w.wake.notify_one();
    lua_pushinteger(L, id);
    return 1;
}

// llm.poll() -> { {id=, ok=, text=, data=, error=, ms=, tokens=}, ... }
int l_poll(lua_State* L) {
    std::vector<Answer> got;
    {
        Worker& w = worker();
        std::lock_guard<std::mutex> g(w.lock);
        got.swap(w.done);
    }
    lua_createtable(L, static_cast<int>(got.size()), 0);
    for (std::size_t i = 0; i < got.size(); ++i) {
        const Answer& a = got[i];
        lua_createtable(L, 0, 7);
        lua_pushinteger(L, a.id);                lua_setfield(L, -2, "id");
        lua_pushboolean(L, a.ok);                lua_setfield(L, -2, "ok");
        lua_pushlstring(L, a.text.data(), a.text.size()); lua_setfield(L, -2, "text");
        if (!a.error.empty()) { lua_pushstring(L, a.error.c_str()); lua_setfield(L, -2, "error"); }
        if (!a.data.is_null()) { luajson::push(L, a.data); lua_setfield(L, -2, "data"); }
        lua_pushnumber(L, a.ms);                 lua_setfield(L, -2, "ms");
        lua_pushinteger(L, a.tokens);            lua_setfield(L, -2, "tokens");
        lua_seti(L, -2, static_cast<lua_Integer>(i + 1));
    }
    return 1;
}

// llm.pending() -> how many requests are waiting or being answered
int l_pending(lua_State* L) {
    Worker& w = worker();
    std::lock_guard<std::mutex> g(w.lock);
    lua_pushinteger(L, static_cast<lua_Integer>(w.queue.size()) + w.busy);
    return 1;
}

// llm.clear() -- drop every request not yet being answered
int l_clear(lua_State*) {
    Worker& w = worker();
    std::lock_guard<std::mutex> g(w.lock);
    w.queue.clear();
    return 0;
}

int l_setHost(lua_State* L) {
    const char* host = luaL_checkstring(L, 1);
    const int   port = static_cast<int>(luaL_optinteger(L, 2, 11434));
    Worker& w = worker();
    std::lock_guard<std::mutex> g(w.lock);
    w.host = host;
    w.port = port;
    return 0;
}

int l_setModel(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    Worker& w = worker();
    std::lock_guard<std::mutex> g(w.lock);
    w.model = name;
    return 0;
}

int l_model(lua_State* L) {
    Worker& w = worker();
    std::lock_guard<std::mutex> g(w.lock);
    lua_pushstring(L, w.model.c_str());
    return 1;
}

// --- json.* -----------------------------------------------------------------------

int l_encode(lua_State* L) {
    luaL_checkany(L, 1);
    const bool pretty = lua_toboolean(L, 2) != 0;
    const std::string s = luajson::toJson(L, 1).dump(pretty ? 2 : -1, ' ', false,
                                                     nlohmann::json::error_handler_t::replace);
    lua_pushlstring(L, s.data(), s.size());
    return 1;
}

int l_decode(lua_State* L) {
    std::size_t n = 0;
    const char* s = luaL_checklstring(L, 1, &n);
    const nlohmann::json j = nlohmann::json::parse(s, s + n, nullptr, false);
    if (j.is_discarded()) {
        lua_pushnil(L);
        lua_pushstring(L, "not valid JSON");
        return 2;
    }
    luajson::push(L, j);
    return 1;
}

} // namespace

void install(lua_State* L) {
    lua_newtable(L);
    const luaL_Reg llm[] = {
        {"chat", l_chat},         {"poll", l_poll},         {"pending", l_pending},
        {"clear", l_clear},       {"setHost", l_setHost},   {"setModel", l_setModel},
        {"model", l_model},       {nullptr, nullptr}};
    luaL_setfuncs(L, llm, 0);
    lua_setglobal(L, "llm");

    lua_newtable(L);
    const luaL_Reg js[] = {{"encode", l_encode}, {"decode", l_decode}, {nullptr, nullptr}};
    luaL_setfuncs(L, js, 0);
    lua_setglobal(L, "json");
}

void reset() {
    Worker& w = worker();
    std::lock_guard<std::mutex> g(w.lock);
    ++w.gen;
    w.queue.clear();
    w.done.clear();
}

} // namespace scriptllm
