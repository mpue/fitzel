#include "ComfyUi.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>

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
#include <fcntl.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <stb_image.h>

#include "NetSocket.hpp"
#include "UserDir.hpp"

using nlohmann::json;

namespace comfy {

namespace {

#ifdef _WIN32
using Sock = SOCKET;
const Sock kNoSock = INVALID_SOCKET;
void closeSock(Sock s) { closesocket(s); }
bool setBlocking(Sock s, bool on) { u_long nb = on ? 0 : 1; return ioctlsocket(s, FIONBIO, &nb) == 0; }
constexpr int kSendFlags = 0;
#else
using Sock = int;
const Sock kNoSock = -1;
void closeSock(Sock s) { ::close(s); }
bool setBlocking(Sock s, bool on) {
    const int f = fcntl(s, F_GETFL, 0);
    return f >= 0 && fcntl(s, F_SETFL, on ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK)) == 0;
}
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;   // a server that hung up is an error, not a SIGPIPE
#else
constexpr int kSendFlags = 0;
#endif
#endif

// A server on this machine or the LAN takes a connection at once or is not
// running; the kernel's own patience is two minutes.
constexpr int kConnectSec = 4;
// How long a queued prompt may take, all told, before we stop waiting for it.
constexpr double kJobSec = 30.0 * 60.0;
// After the start command: how long ComfyUI may take to come up.
constexpr double kStartSec = 120.0;

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct Target {
    std::string host;
    int         port = 8188;
    std::string url() const { return "http://" + host + ":" + std::to_string(port); }
};

// "http://host:port/" -> host, port. False (and why) for anything else.
bool parseUrl(const std::string& url, Target& t, std::string& err) {
    std::string s = url;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    if (s.rfind("https://", 0) == 0) {
        err = "https is not supported -- ComfyUI speaks plain http (http://host:8188)";
        return false;
    }
    if (s.rfind("http://", 0) == 0) s = s.substr(7);
    const std::size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    t.port = 8188;
    const std::size_t colon = s.rfind(':');
    if (colon != std::string::npos && s.find(']') == std::string::npos) {
        t.port = std::atoi(s.c_str() + colon + 1);
        s      = s.substr(0, colon);
    }
    if (!s.empty() && s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
    t.host = s;
    if (t.host.empty() || t.port <= 0 || t.port > 65535) {
        err = "not a server address: " + url;
        return false;
    }
    return true;
}

// Something to read (or the other side gone) within `ms`.
bool readable(Sock s, int ms) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(s, &r);
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    return ::select(static_cast<int>(s) + 1, &r, nullptr, nullptr, &tv) > 0;
}

Sock connectTo(const Target& t, const std::atomic<bool>& cancel, std::string& err) {
    netsock::init();
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string ps = std::to_string(t.port);
    if (getaddrinfo(t.host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) {
        err = "cannot resolve " + t.host;
        return kNoSock;
    }
    Sock s = kNoSock;
    for (addrinfo* a = res; a && s == kNoSock && !cancel; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kNoSock) continue;
        setBlocking(s, false);
        bool up = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0;
        // Waited for in short slices, so a cancel does not sit out the timeout.
        for (int slice = 0; !up && slice < kConnectSec * 10 && !cancel; ++slice) {
            fd_set w, x;
            FD_ZERO(&w);
            FD_ZERO(&x);
            FD_SET(s, &w);
            FD_SET(s, &x);
            timeval tv{0, 100000};
            const int n = ::select(static_cast<int>(s) + 1, nullptr, &w, &x, &tv);
            if (n < 0) break;
            if (n == 0) continue;
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
            up = soerr == 0 && FD_ISSET(s, &w);
            break;
        }
        if (up) {
            setBlocking(s, true);
            break;
        }
        closeSock(s);
        s = kNoSock;
    }
    freeaddrinfo(res);
    if (s == kNoSock && err.empty()) err = "no ComfyUI answers at " + t.url();
    return s;
}

bool sendAll(Sock s, const std::string& data) {
    for (std::size_t sent = 0; sent < data.size();) {
        const int n = ::send(s, data.data() + sent, static_cast<int>(data.size() - sent), kSendFlags);
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string dechunk(const std::string& in) {
    std::string out;
    std::size_t at = 0;
    while (at < in.size()) {
        const std::size_t eol = in.find("\r\n", at);
        if (eol == std::string::npos) break;
        const std::size_t n = std::strtoul(in.substr(at, eol - at).c_str(), nullptr, 16);
        if (n == 0) break;
        at = eol + 2;
        out.append(in, at, std::min(n, in.size() - at));
        at += n + 2;
    }
    return out;
}

struct Http {
    int         status = 0;
    std::string body, err;
    bool        reached = false;   // the server took the connection
    bool ok() const { return err.empty() && status == 200; }
    json parsed() const { return json::parse(body, nullptr, false); }
};

// One request, the whole answer (Connection: close). Gives up after `sec`
// seconds, or as soon as `cancel` is set.
Http request(const Target& t, const std::string& method, const std::string& path, const std::string& body,
             const std::atomic<bool>& cancel, double sec = 30.0) {
    Http h;
    const Sock s = connectTo(t, cancel, h.err);
    if (s == kNoSock) return h;
    h.reached = true;
    std::string msg = method + " " + path + " HTTP/1.1\r\nHost: " + t.host + ":" + std::to_string(t.port) +
                      "\r\nAccept: */*\r\nConnection: close\r\n";
    if (!body.empty())
        msg += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    msg += "\r\n" + body;
    std::string all;
    if (!sendAll(s, msg)) {
        h.err = "send failed";
    } else {
        const double until = now() + sec;
        char buf[65536];
        for (;;) {
            if (cancel) { h.err = "cancelled"; break; }
            if (now() > until) { h.err = "no answer from " + t.url() + " (timed out)"; break; }
            if (!readable(s, 200)) continue;
            const int n = ::recv(s, buf, sizeof(buf), 0);
            if (n == 0) break;
            if (n < 0) { h.err = "the connection was cut off"; break; }
            all.append(buf, static_cast<std::size_t>(n));
        }
    }
    closeSock(s);
    if (!h.err.empty()) return h;
    const std::size_t end = all.find("\r\n\r\n");
    if (all.compare(0, 5, "HTTP/") != 0 || end == std::string::npos) {
        h.err = "not an HTTP answer from " + t.url();
        return h;
    }
    const std::size_t sp = all.find(' ');
    h.status = std::atoi(all.c_str() + sp + 1);
    const std::string head = lower(all.substr(0, end));
    h.body = all.substr(end + 4);
    if (head.find("transfer-encoding: chunked") != std::string::npos) h.body = dechunk(h.body);
    return h;
}

std::string base64(const unsigned char* p, std::size_t n) {
    static const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < n; i += 3) {
        const unsigned v = (unsigned(p[i]) << 16) | (i + 1 < n ? unsigned(p[i + 1]) << 8 : 0u) | (i + 2 < n ? p[i + 2] : 0u);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < n ? k[(v >> 6) & 63] : '=';
        out += i + 2 < n ? k[v & 63] : '=';
    }
    return out;
}

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::uint32_t random32() {
    static std::mt19937 rng{std::random_device{}()};
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    return rng();
}

// ComfyUI's WebSocket, the receiving side: what it says about the prompts
// this client queued (its clientId) -- progress, previews, the end.
class WebSocket {
public:
    ~WebSocket() { close(); }

    bool open(const Target& t, const std::string& path, const std::atomic<bool>& cancel, std::string& err) {
        m_s = connectTo(t, cancel, err);
        if (m_s == kNoSock) return false;
        unsigned char key[16];
        for (unsigned char& b : key) b = static_cast<unsigned char>(random32());
        const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + t.host + ":" + std::to_string(t.port) +
                                "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
                                base64(key, 16) + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!sendAll(m_s, req)) { err = "send failed"; close(); return false; }
        const double until = now() + 5.0;
        char buf[4096];
        while (m_in.find("\r\n\r\n") == std::string::npos) {
            if (cancel || now() > until) { err = "no WebSocket answer"; close(); return false; }
            if (!readable(m_s, 100)) continue;
            const int n = ::recv(m_s, buf, sizeof(buf), 0);
            if (n <= 0) { err = "the WebSocket was refused"; close(); return false; }
            m_in.append(buf, static_cast<std::size_t>(n));
        }
        const std::size_t end = m_in.find("\r\n\r\n");
        if (m_in.compare(0, 12, "HTTP/1.1 101") != 0) { err = "the server did not switch to a WebSocket"; close(); return false; }
        m_in.erase(0, end + 4);
        return true;
    }

    // The next whole message, waiting up to `ms` for it: 1 text, 2 binary,
    // 0 nothing yet, -1 the socket is gone.
    int next(std::string& msg, int ms) {
        if (m_s == kNoSock) return -1;
        const double until = now() + ms / 1000.0;
        for (;;) {
            int op = 0;
            std::string payload;
            bool fin = false;
            if (frame(op, fin, payload)) {
                if (op == 8) { close(); return -1; }
                if (op == 9) { pong(payload); continue; }
                if (op == 10) continue;
                if (op == 0) m_msg += payload;
                else { m_msg = std::move(payload); m_op = op; }
                if (!fin) continue;
                msg = std::move(m_msg);
                m_msg.clear();
                return m_op;
            }
            const double left = until - now();
            if (left <= 0.0) return 0;
            if (!readable(m_s, std::max(1, int(left * 1000.0)))) return 0;
            char buf[65536];
            const int n = ::recv(m_s, buf, sizeof(buf), 0);
            if (n <= 0) { close(); return -1; }
            m_in.append(buf, static_cast<std::size_t>(n));
        }
    }

    void close() {
        if (m_s != kNoSock) closeSock(m_s);
        m_s = kNoSock;
    }

private:
    // One frame off the front of what has arrived, if it is all there.
    bool frame(int& op, bool& fin, std::string& payload) {
        if (m_in.size() < 2) return false;
        const auto b = [&](std::size_t i) { return static_cast<unsigned char>(m_in[i]); };
        fin = (b(0) & 0x80) != 0;
        op  = b(0) & 0x0F;
        const bool masked = (b(1) & 0x80) != 0;
        std::uint64_t len = b(1) & 0x7F;
        std::size_t at = 2;
        if (len == 126) {
            if (m_in.size() < 4) return false;
            len = (std::uint64_t(b(2)) << 8) | b(3);
            at = 4;
        } else if (len == 127) {
            if (m_in.size() < 10) return false;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | b(2 + std::size_t(i));
            at = 10;
        }
        const std::size_t maskAt = at;
        if (masked) at += 4;
        if (m_in.size() < at + len) return false;
        payload = m_in.substr(at, std::size_t(len));
        if (masked)
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = char(payload[i] ^ m_in[maskAt + i % 4]);
        m_in.erase(0, at + std::size_t(len));
        return true;
    }

    // A client's frames are masked; a pong carries the ping's bytes back.
    void pong(const std::string& data) {
        std::string f;
        f += char(0x8A);
        f += char(0x80 | std::min<std::size_t>(data.size(), 125));
        unsigned char mask[4];
        for (unsigned char& c : mask) c = static_cast<unsigned char>(random32());
        f.append(reinterpret_cast<const char*>(mask), 4);
        for (std::size_t i = 0; i < std::min<std::size_t>(data.size(), 125); ++i) f += char(data[i] ^ mask[i % 4]);
        sendAll(m_s, f);
    }

    Sock        m_s = kNoSock;
    std::string m_in, m_msg;   // bytes not yet framed; a message in pieces so far
    int         m_op = 0;
};

bool decode(const unsigned char* p, std::size_t n, Image& out) {
    int w = 0, h = 0, c = 0;
    stbi_set_flip_vertically_on_load_thread(0);
    unsigned char* data = stbi_load_from_memory(p, int(n), &w, &h, &c, 4);
    if (!data) return false;
    out.w = w;
    out.h = h;
    out.px.assign(data, data + std::size_t(w) * h * 4);
    stbi_image_free(data);
    return true;
}

// What ComfyUI said when it would not take the prompt, in one line: the first
// node it found fault with, and for a missing file which file.
std::string refusal(const Http& h) {
    const json j = h.parsed();
    if (!j.is_object()) return "ComfyUI refused the prompt (HTTP " + std::to_string(h.status) + "): " + h.body.substr(0, 300);
    if (j.contains("node_errors") && j["node_errors"].is_object())
        for (const auto& [id, ne] : j["node_errors"].items()) {
            if (!ne.is_object() || !ne.contains("errors") || !ne["errors"].is_array() || ne["errors"].empty()) continue;
            const json& e = ne["errors"][0];
            const std::string cls = ne.value("class_type", "node " + id);
            const std::string type = e.value("type", ""), details = e.value("details", "");
            if (type == "value_not_in_list") {
                // "unet_name: 'x.safetensors' not in ['a', 'b']"
                const std::size_t a = details.find('\''), b = a == std::string::npos ? a : details.find('\'', a + 1);
                const std::string file = b == std::string::npos ? details : details.substr(a + 1, b - a - 1);
                return "This ComfyUI has no " + file + " (" + cls + "). Put it into its models folder, or pick a workflow "
                       "that uses what it has.";
            }
            return cls + ": " + e.value("message", type) + (details.empty() ? "" : " -- " + details.substr(0, 300));
        }
    if (j.contains("error")) {
        const json& e = j["error"];
        if (e.is_string()) return "ComfyUI: " + e.get<std::string>();
        if (e.is_object()) {
            const std::string d = e.value("details", "");
            return "ComfyUI: " + e.value("message", std::string("refused")) + (d.empty() ? "" : " -- " + d.substr(0, 300));
        }
    }
    return "ComfyUI refused the prompt (HTTP " + std::to_string(h.status) + ")";
}

// What a node is doing, for the progress line.
std::string doing(const std::string& cls) {
    const std::string c = lower(cls);
    if (c.find("loader") != std::string::npos) return "Loading the model";
    if (c.find("textencode") != std::string::npos || c.find("clip") != std::string::npos) return "Reading the prompt";
    if (c.find("sampler") != std::string::npos) return "Painting";
    if (c.find("vaedecode") != std::string::npos) return "Decoding the picture";
    if (c.find("save") != std::string::npos || c.find("preview") != std::string::npos) return "Saving";
    return cls;
}

// The pictures a finished prompt left, from its history entry: the saved ones
// (a workflow that only previews: those).
std::vector<json> outputsOf(const json& entry) {
    std::vector<json> saved, temp;
    if (!entry.contains("outputs") || !entry["outputs"].is_object()) return saved;
    for (const auto& [id, out] : entry["outputs"].items()) {
        if (!out.is_object() || !out.contains("images") || !out["images"].is_array()) continue;
        for (const json& im : out["images"]) {
            if (!im.is_object() || !im.contains("filename")) continue;
            (im.value("type", "output") == "output" ? saved : temp).push_back(im);
        }
    }
    return saved.empty() ? temp : saved;
}

// The failure a history entry records, if it records one.
std::string historyError(const json& entry) {
    if (!entry.contains("status") || !entry["status"].is_object()) return {};
    const json& st = entry["status"];
    if (st.value("status_str", "") != "error") return {};
    if (st.contains("messages") && st["messages"].is_array())
        for (const json& m : st["messages"])
            if (m.is_array() && m.size() == 2 && m[0] == "execution_error" && m[1].is_object())
                return m[1].value("node_type", std::string("ComfyUI")) + ": " + m[1].value("exception_message", std::string("failed"));
    return "the prompt failed on the server";
}

void generateJob(Job& job, const Target& t, const json& graph, const std::string& startCmd) {
    auto stage = [&](const std::string& s) {
        std::lock_guard<std::mutex> g(job.lock);
        job.stage = s;
    };
    auto fail = [&](const std::string& e) {
        std::lock_guard<std::mutex> g(job.lock);
        job.error = e;
    };
    const std::atomic<bool>& cancel = job.cancelled;

    stage("Connecting to " + t.url());
    Http h = request(t, "GET", "/system_stats", "", cancel, 10.0);
    if (!h.reached && !cancel) {
        if (startCmd.empty()) {
            fail("No ComfyUI answers at " + t.url() + ". Start it -- or give a start command under Server, and "
                 "this window starts it for you.");
            return;
        }
        stage("Starting ComfyUI (" + startCmd + ")");
        const int rc = std::system(startCmd.c_str());
        if (rc != 0) { fail("The start command failed (exit " + std::to_string(rc) + "): " + startCmd); return; }
        const double until = now() + kStartSec;
        for (;;) {
            if (cancel) return;
            h = request(t, "GET", "/system_stats", "", cancel, 5.0);
            if (h.ok()) break;
            if (now() > until) {
                fail("ComfyUI did not come up at " + t.url() + " within two minutes of: " + startCmd);
                return;
            }
            for (int i = 0; i < 10 && !cancel; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    if (cancel) return;
    if (!h.ok()) {
        fail(h.err.empty() ? "ComfyUI at " + t.url() + " answered HTTP " + std::to_string(h.status) : h.err);
        return;
    }

    // The socket first, so no word about the prompt is missed.
    char cid[40];
    std::snprintf(cid, sizeof cid, "fitzel-%08x%08x", random32(), random32());
    const std::string clientId = cid;
    WebSocket ws;
    std::string wsErr;
    bool hasWs = ws.open(t, "/ws?clientId=" + clientId, cancel, wsErr);

    stage("Queueing the prompt");
    const json body = {{"prompt", graph},
                       {"client_id", clientId},
                       // Previews while it samples, whatever the server was started with.
                       {"extra_data", {{"preview_method", "auto"}}}};
    h = request(t, "POST", "/prompt", body.dump(-1, ' ', false, json::error_handler_t::replace), cancel, 30.0);
    if (cancel) return;
    if (!h.ok()) { fail(h.err.empty() ? refusal(h) : h.err); return; }
    const json queued = h.parsed();
    const std::string promptId = queued.is_object() ? queued.value("prompt_id", "") : "";
    if (promptId.empty()) { fail("ComfyUI took the prompt but named no prompt_id"); return; }
    stage(hasWs ? "Waiting in ComfyUI's queue" : "Working (this server sends no progress)");

    // Until it is done: the socket's word, and the history now and then in case
    // a word went missing (or there is no socket).
    const std::atomic<bool> never{false};
    const double until = now() + kJobSec;
    double nextLook = now() + (hasWs ? 5.0 : 0.5);
    bool running = false, finishing = false;
    json entry;
    for (;;) {
        if (cancel) {
            // Off the queue if it waits there; stopped if it is the one running.
            const json id = {{"prompt_id", promptId}};
            if (running) request(t, "POST", "/interrupt", id.dump(), never, 3.0);
            request(t, "POST", "/queue", json{{"delete", json::array({promptId})}}.dump(), never, 3.0);
            return;
        }
        if (now() > until) { fail("No picture after 30 minutes -- given up waiting."); return; }
        std::string msg;
        const int kind = hasWs ? ws.next(msg, 250) : 0;
        if (kind < 0) {
            hasWs = false;
            nextLook = now();
        } else if (kind == 1) {
            const json m = json::parse(msg, nullptr, false);
            const std::string type = m.is_object() ? m.value("type", "") : "";
            const json data = m.is_object() && m.contains("data") && m["data"].is_object() ? m["data"] : json::object();
            // Status goes to everyone; the rest names its prompt.
            const bool ours = data.value("prompt_id", promptId) == promptId;
            if (type == "status") {
                const int left = !running && data.contains("status")
                               ? data["status"].value("exec_info", json::object()).value("queue_remaining", 0) : 0;
                if (left > 1) stage("Waiting in ComfyUI's queue (" + std::to_string(left - 1) + " ahead)");
            } else if (!ours) {
            } else if (type == "execution_start") {
                running = true;
                stage("Starting");
            } else if (type == "executing" && data.contains("node")) {
                running = true;
                if (data["node"].is_null()) {
                    finishing = true;
                    nextLook = now();
                } else {
                    const std::string node = data["node"].is_string() ? data["node"].get<std::string>() : data["node"].dump();
                    const json* n = graph.contains(node) ? &graph[node] : nullptr;
                    std::lock_guard<std::mutex> g(job.lock);
                    job.stage = n ? doing(n->value("class_type", node)) : node;
                    job.step = job.steps = 0;
                }
            } else if (type == "progress") {
                std::lock_guard<std::mutex> g(job.lock);
                job.step  = data.value("value", 0);
                job.steps = data.value("max", 0);
            } else if (type == "execution_success") {
                finishing = true;
                nextLook = now();
            } else if (type == "execution_error") {
                fail(data.value("node_type", std::string("ComfyUI")) + ": " +
                     data.value("exception_message", std::string("the prompt failed")));
                return;
            } else if (type == "execution_interrupted") {
                fail("Interrupted on the server.");
                return;
            }
        } else if (kind == 2 && msg.size() > 8) {
            // A preview: event 1 (big-endian), then the picture's type, then the picture.
            const auto* p = reinterpret_cast<const unsigned char*>(msg.data());
            const std::uint32_t event = (std::uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
            Image pv;
            if (event == 1 && decode(p + 8, msg.size() - 8, pv)) {
                std::lock_guard<std::mutex> g(job.lock);
                job.preview = std::move(pv);
                ++job.previewVersion;
            }
        } else if (!hasWs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (now() < nextLook) continue;
        h = request(t, "GET", "/history/" + promptId, "", cancel, 10.0);
        const json hist = h.ok() ? h.parsed() : json();
        if (hist.is_object() && hist.contains(promptId)) {
            entry = hist[promptId];
            const std::string e = historyError(entry);
            if (!e.empty()) { fail(e); return; }
            break;
        }
        // Done but not in the history yet: it is written just after the last word.
        nextLook = now() + (finishing || !hasWs ? 0.3 : 5.0);
    }

    const std::vector<json> files = outputsOf(entry);
    if (files.empty()) { fail("The workflow finished without a picture -- does it end in a Save Image node?"); return; }
    stage(files.size() == 1 ? "Fetching the picture" : "Fetching the pictures");
    std::vector<Image> images;
    for (const json& f : files) {
        const std::string name = f.value("filename", "");
        h = request(t, "GET", "/view?filename=" + urlEncode(name) + "&subfolder=" + urlEncode(f.value("subfolder", "")) +
                    "&type=" + urlEncode(f.value("type", "output")), "", cancel, 60.0);
        if (cancel) return;
        Image im;
        if (!h.ok() || !decode(reinterpret_cast<const unsigned char*>(h.body.data()), h.body.size(), im)) {
            fail("Could not fetch " + name + (h.err.empty() ? "" : ": " + h.err));
            return;
        }
        im.file = name;
        images.push_back(std::move(im));
    }
    std::lock_guard<std::mutex> g(job.lock);
    job.images = std::move(images);
}

// The model files a workflow loads, with the loader that loads each.
std::vector<std::pair<std::string, Slot>> modelFiles(const json& graph) {
    std::vector<std::pair<std::string, Slot>> out;
    if (!graph.is_object()) return out;
    for (const auto& [id, n] : graph.items()) {
        if (!n.is_object() || !n.contains("inputs") || !n["inputs"].is_object()) continue;
        for (const auto& [k, v] : n["inputs"].items()) {
            if (!v.is_string()) continue;
            const std::string e = lower(std::filesystem::path(v.get<std::string>()).extension().string());
            if (e == ".safetensors" || e == ".ckpt" || e == ".pt" || e == ".pth" || e == ".bin" || e == ".gguf" || e == ".sft")
                out.push_back({n.value("class_type", ""), Slot{id, k}});
        }
    }
    return out;
}

void probeJob(Job& job, const Target& t, const json& graph) {
    const std::atomic<bool>& cancel = job.cancelled;
    Http h = request(t, "GET", "/system_stats", "", cancel, 10.0);
    if (!h.ok()) {
        std::lock_guard<std::mutex> g(job.lock);
        job.error = !h.reached ? "No ComfyUI answers at " + t.url() + "."
                  : h.err.empty() ? "HTTP " + std::to_string(h.status) : h.err;
        return;
    }
    const json st = h.parsed();
    std::string info = "ComfyUI";
    if (st.is_object() && st.contains("system")) info += " " + st["system"].value("comfyui_version", std::string());
    if (st.is_object() && st.contains("devices") && st["devices"].is_array() && !st["devices"].empty()) {
        const json& d = st["devices"][0];
        // "cuda:0 NVIDIA GeForce RTX 4090 : cudaMallocAsync" -> the card's name.
        std::string name = d.value("name", std::string());
        if (const std::size_t sp = name.find(' '); name.rfind("cuda:", 0) == 0 && sp != std::string::npos) name = name.substr(sp + 1);
        if (const std::size_t c = name.find(" : "); c != std::string::npos) name = name.substr(0, c);
        char vram[64];
        std::snprintf(vram, sizeof vram, ", %.0f GB (%.1f free)", d.value("vram_total", 0.0) / 1073741824.0,
                      d.value("vram_free", 0.0) / 1073741824.0);
        info += " on " + name + vram;
    }
    // The files the workflow loads, against the lists the loaders offer.
    std::map<std::string, std::set<std::string>> offered;
    std::vector<std::string> missing;
    for (const auto& [cls, slot] : modelFiles(graph)) {
        if (cancel) return;
        if (!offered.count(cls)) {
            std::set<std::string>& opts = offered[cls];
            h = request(t, "GET", "/object_info/" + urlEncode(cls), "", cancel, 10.0);
            const json oi = h.ok() ? h.parsed() : json();
            if (!oi.is_object() || !oi.contains(cls)) continue;
            const json& in = oi[cls].value("input", json::object());
            for (const char* part : {"required", "optional"}) {
                if (!in.contains(part) || !in[part].contains(slot.input)) continue;
                const json& spec = in[part][slot.input];
                // [["a", "b"], {...}] -- or, newer, ["COMBO", {"options": [...]}].
                const json* list = spec.is_array() && !spec.empty() && spec[0].is_array() ? &spec[0]
                                 : spec.is_array() && spec.size() > 1 && spec[1].is_object() && spec[1].contains("options") ? &spec[1]["options"]
                                 : nullptr;
                if (list)
                    for (const json& o : *list)
                        if (o.is_string()) opts.insert(o.get<std::string>());
            }
        }
        const std::string file = graph[slot.node]["inputs"][slot.input].get<std::string>();
        const auto& opts = offered[cls];
        if (!opts.empty() && !opts.count(file)) missing.push_back(file);
    }
    if (!missing.empty()) {
        info += "\nLacks what the workflow loads:";
        for (const std::string& m : missing) info += "\n  " + m;
    } else if (!modelFiles(graph).empty()) {
        info += "\nHas every model file the workflow loads.";
    }
    std::lock_guard<std::mutex> g(job.lock);
    job.info = info;
}

// --- Workflows ------------------------------------------------------------------------------

const json* node(const json& graph, const std::string& id) {
    const auto it = graph.find(id);
    return it != graph.end() && it->is_object() ? &*it : nullptr;
}

const json& inputsOf(const json& n) {
    static const json none = json::object();
    return n.contains("inputs") && n["inputs"].is_object() ? n["inputs"] : none;
}

// An input that is a link: ["node id", output index].
bool isLink(const json& v) { return v.is_array() && v.size() == 2 && v[0].is_string() && v[1].is_number(); }

void addSlot(std::vector<Slot>& v, const Slot& s) {
    for (const Slot& o : v)
        if (o.node == s.node && o.input == s.input) return;
    v.push_back(s);
}

// From a conditioning link back to the text it is made of -- through the
// nodes between (guidance, zero-out, a string node feeding the text).
void textBehind(const json& graph, const json& link, std::vector<Slot>& out, int depth = 0) {
    if (depth > 8 || !isLink(link)) return;
    const std::string id = link[0].get<std::string>();
    const json* n = node(graph, id);
    if (!n) return;
    const json& in = inputsOf(*n);
    bool found = false;
    for (const char* k : {"text", "prompt", "string", "value", "t5xxl", "clip_l", "text_g", "text_l"}) {
        if (!in.contains(k)) continue;
        if (in[k].is_string()) { addSlot(out, Slot{id, k}); found = true; }
        else if (isLink(in[k])) { textBehind(graph, in[k], out, depth + 1); found = true; }
    }
    if (found) return;
    for (const auto& [k, v] : in.items())
        if (isLink(v) && (k.find("conditioning") != std::string::npos || k == "positive" || k == "cond"))
            textBehind(graph, v, out, depth + 1);
}

int intAt(const json& graph, const std::vector<Slot>& slots, int def) {
    for (const Slot& s : slots) {
        const json* n = node(graph, s.node);
        if (n && inputsOf(*n).contains(s.input) && inputsOf(*n)[s.input].is_number()) return inputsOf(*n)[s.input].get<int>();
    }
    return def;
}

// The slots of an exported graph, found by what its nodes are.
bool findSlots(Workflow& wf, std::string& err) {
    const json& g = wf.graph;
    // The samplers' (or guiders') positive and negative sides.
    for (const auto& [id, n] : g.items()) {
        if (!n.is_object()) continue;
        const json& in = inputsOf(n);
        if (in.contains("positive") && isLink(in["positive"])) {
            textBehind(g, in["positive"], wf.prompt);
            if (in.contains("negative")) textBehind(g, in["negative"], wf.negative);
        } else if (lower(n.value("class_type", "")).find("guider") != std::string::npos && in.contains("conditioning")) {
            textBehind(g, in["conditioning"], wf.prompt);
        }
    }
    // A negative made by zeroing the positive leads back to the positive's text.
    std::erase_if(wf.negative, [&](const Slot& s) {
        return std::any_of(wf.prompt.begin(), wf.prompt.end(), [&](const Slot& p) { return p.node == s.node && p.input == s.input; });
    });
    if (wf.prompt.empty()) {
        err = "Found no prompt text in this workflow (no text node feeds a sampler).";
        return false;
    }
    for (const auto& [id, n] : g.items()) {
        if (!n.is_object()) continue;
        const json& in = inputsOf(n);
        const std::string cls = lower(n.value("class_type", ""));
        for (const char* k : {"seed", "noise_seed"})
            if (in.contains(k) && in[k].is_number()) addSlot(wf.seed, Slot{id, k});
        if (in.contains("steps") && in["steps"].is_number() &&
            (cls.find("sampler") != std::string::npos || cls.find("scheduler") != std::string::npos))
            addSlot(wf.steps, Slot{id, "steps"});
        if (cls.find("latent") != std::string::npos && in.contains("width") && in["width"].is_number() &&
            in.contains("height") && in["height"].is_number()) {
            addSlot(wf.width, Slot{id, "width"});
            addSlot(wf.height, Slot{id, "height"});
            if (in.contains("batch_size") && in["batch_size"].is_number()) addSlot(wf.count, Slot{id, "batch_size"});
        }
    }
    return true;
}

} // namespace

// --- Workflows ------------------------------------------------------------------------------

Workflow builtinWorkflow() {
    // ComfyUI's template "Text to Image (Z-Image-Turbo)", in API form.
    static const char* kGraph = R"({
      "1": {"class_type": "UNETLoader", "inputs": {"unet_name": "z_image_turbo_bf16.safetensors", "weight_dtype": "default"}},
      "2": {"class_type": "CLIPLoader", "inputs": {"clip_name": "qwen_3_4b.safetensors", "type": "lumina2", "device": "default"}},
      "3": {"class_type": "VAELoader", "inputs": {"vae_name": "ae.safetensors"}},
      "4": {"class_type": "ModelSamplingAuraFlow", "inputs": {"model": ["1", 0], "shift": 3}},
      "5": {"class_type": "CLIPTextEncode", "inputs": {"text": "", "clip": ["2", 0]}},
      "6": {"class_type": "ConditioningZeroOut", "inputs": {"conditioning": ["5", 0]}},
      "7": {"class_type": "EmptySD3LatentImage", "inputs": {"width": 1024, "height": 1024, "batch_size": 1}},
      "8": {"class_type": "KSampler", "inputs": {"model": ["4", 0], "seed": 0, "steps": 8, "cfg": 1,
                                                 "sampler_name": "res_multistep", "scheduler": "simple",
                                                 "positive": ["5", 0], "negative": ["6", 0],
                                                 "latent_image": ["7", 0], "denoise": 1}},
      "9": {"class_type": "VAEDecode", "inputs": {"samples": ["8", 0], "vae": ["3", 0]}},
      "10": {"class_type": "SaveImage", "inputs": {"images": ["9", 0], "filename_prefix": "fitzel/image"}}
    })";
    Workflow wf;
    std::string err;
    parseWorkflow(json::parse(kGraph), wf, err);
    wf.name = "Z-Image Turbo (built in)";
    return wf;
}

bool parseWorkflow(const json& j, Workflow& wf, std::string& err) {
    wf = Workflow{};
    if (!j.is_object()) { err = "Not a workflow (no JSON object)."; return false; }
    if (j.contains("nodes") && j.contains("links")) {
        err = "This is ComfyUI's UI format. Save the workflow with File > Export (API) instead.";
        return false;
    }
    if (j.contains("graph") && j["graph"].is_object()) {
        // Slots named by hand: "set": {"prompt": ["5", "text"], ...}.
        wf.graph = j["graph"];
        wf.name  = j.value("name", std::string());
        if (j.contains("set") && j["set"].is_object()) {
            const std::pair<const char*, std::vector<Slot>*> keys[] = {
                {"prompt", &wf.prompt}, {"negative", &wf.negative}, {"width", &wf.width}, {"height", &wf.height},
                {"seed", &wf.seed}, {"steps", &wf.steps}, {"count", &wf.count}, {"batch_size", &wf.count}};
            for (const auto& [k, slots] : keys) {
                const json& s = j["set"].value(k, json());
                if (s.is_array() && s.size() == 2 && s[0].is_string() && s[1].is_string())
                    addSlot(*slots, Slot{s[0].get<std::string>(), s[1].get<std::string>()});
            }
            // The batch beside the size, when it was not named.
            if (wf.count.empty() && !wf.width.empty())
                if (const json* n = node(wf.graph, wf.width[0].node); n && inputsOf(*n).contains("batch_size"))
                    addSlot(wf.count, Slot{wf.width[0].node, "batch_size"});
            if (wf.prompt.empty()) { err = "The workflow's \"set\" names no prompt."; return false; }
        } else if (!findSlots(wf, err)) {
            return false;
        }
    } else {
        for (const auto& [id, n] : j.items())
            if (!n.is_object() || !n.contains("class_type")) {
                err = "Not a ComfyUI workflow in API format (node \"" + id + "\" has no class_type).";
                return false;
            }
        wf.graph = j;
        if (!findSlots(wf, err)) return false;
    }
    const std::pair<const char*, const std::vector<Slot>*> all[] = {
        {"prompt", &wf.prompt}, {"negative", &wf.negative}, {"width", &wf.width}, {"height", &wf.height},
        {"seed", &wf.seed}, {"steps", &wf.steps}, {"count", &wf.count}};
    for (const auto& [k, slots] : all)
        for (const Slot& s : *slots)
            if (!node(wf.graph, s.node)) { err = std::string("The ") + k + " goes to node " + s.node + ", which is not there."; return false; }
    wf.defWidth  = intAt(wf.graph, wf.width, 1024);
    wf.defHeight = intAt(wf.graph, wf.height, 1024);
    wf.defSteps  = intAt(wf.graph, wf.steps, 0);
    return true;
}

bool loadWorkflow(const std::string& path, Workflow& wf, std::string& err) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) { err = "Could not read " + path; return false; }
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded()) { err = std::filesystem::path(path).filename().string() + " is not JSON."; return false; }
    if (!parseWorkflow(j, wf, err)) return false;
    if (wf.name.empty()) wf.name = std::filesystem::path(path).stem().string();
    return true;
}

json fill(const Workflow& wf, const Request& r) {
    json g = wf.graph;
    auto set = [&](const std::vector<Slot>& slots, const json& v) {
        for (const Slot& s : slots)
            if (g.contains(s.node) && g[s.node].is_object()) g[s.node]["inputs"][s.input] = v;
    };
    auto to16 = [](int v) { return std::clamp((v + 8) / 16 * 16, 64, 8192); };
    set(wf.prompt, r.prompt);
    if (!r.negative.empty()) set(wf.negative, r.negative);
    set(wf.width, to16(r.width));
    set(wf.height, to16(r.height));
    set(wf.seed, r.seed);
    if (r.steps > 0) set(wf.steps, r.steps);
    set(wf.count, std::clamp(r.count, 1, 16));
    return g;
}

long long randomSeed() {
    // ComfyUI's widgets take up to 2^53 - 1; this stays well inside.
    return (static_cast<long long>(random32()) << 18 ^ random32()) & ((1LL << 48) - 1);
}

// --- The client -----------------------------------------------------------------------------

Client& client() {
    static Client c;
    static const bool loaded = (c.load(), true);
    (void)loaded;
    return c;
}

Client::~Client() {
    std::vector<std::pair<std::thread, JobPtr>> all;
    {
        std::lock_guard<std::mutex> g(m_lock);
        all.swap(m_threads);
    }
    for (auto& t : all) cancel(t.second);
    for (auto& t : all)
        if (t.first.joinable()) t.first.join();
}

void Client::load() {
    std::ifstream in(userdir::file("comfyui.json"));
    if (!in) return;
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object()) return;
    Settings& s = settings;
    s.url          = j.value("url", s.url);
    s.startCommand = j.value("startCommand", s.startCommand);
    s.workflow     = j.value("workflow", s.workflow);
    s.prompt       = j.value("prompt", s.prompt);
    s.width        = std::clamp(j.value("width", s.width), 64, 8192);
    s.height       = std::clamp(j.value("height", s.height), 64, 8192);
    s.steps        = std::clamp(j.value("steps", s.steps), 0, 200);
    s.count        = std::clamp(j.value("count", s.count), 1, 16);
    s.randomSeed   = j.value("randomSeed", s.randomSeed);
    s.seed         = j.value("seed", s.seed);
    if (j.contains("recent") && j["recent"].is_array())
        for (const json& r : j["recent"])
            if (r.is_string()) s.recent.push_back(r.get<std::string>());
}

void Client::save() const {
    if (!persist) return;
    const Settings& s = settings;
    const json j = {{"url", s.url},
                    {"recent", s.recent},
                    {"startCommand", s.startCommand},
                    {"workflow", s.workflow},
                    {"prompt", s.prompt},
                    {"width", s.width},
                    {"height", s.height},
                    {"steps", s.steps},
                    {"count", s.count},
                    {"randomSeed", s.randomSeed},
                    {"seed", s.seed}};
    std::ofstream out(userdir::file("comfyui.json"));
    if (out) out << j.dump(2);
}

void Client::rememberServer() {
    std::vector<std::string>& r = settings.recent;
    r.erase(std::remove(r.begin(), r.end(), settings.url), r.end());
    r.insert(r.begin(), settings.url);
    if (r.size() > 8) r.resize(8);
}

void Client::reap() {
    std::vector<std::thread> ended;
    {
        std::lock_guard<std::mutex> g(m_lock);
        for (std::size_t i = 0; i < m_threads.size();) {
            if (m_threads[i].second->finished()) {
                ended.push_back(std::move(m_threads[i].first));
                m_threads.erase(m_threads.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }
    // Done is the thread's last act: these return at once.
    for (std::thread& t : ended)
        if (t.joinable()) t.join();
}

JobPtr Client::start(std::function<void(Job&)> work) {
    reap();
    auto j = std::make_shared<Job>();
    std::thread t([j, work = std::move(work)] {
        const double t0 = now();
        work(*j);
        std::lock_guard<std::mutex> g(j->lock);
        if (j->cancelled && j->error.empty()) j->error = "cancelled";
        j->ok      = j->error.empty();
        j->seconds = now() - t0;
        j->done    = true;
    });
    std::lock_guard<std::mutex> g(m_lock);
    m_threads.emplace_back(std::move(t), j);
    return j;
}

JobPtr Client::generate(const Workflow& wf, const Request& r) {
    Target t;
    std::string err;
    if (!parseUrl(settings.url, t, err)) {
        auto j = std::make_shared<Job>();
        j->error = err;
        j->done  = true;
        return j;
    }
    json graph = fill(wf, r);
    JobPtr j = start([t, graph = std::move(graph), cmd = settings.startCommand](Job& job) { generateJob(job, t, graph, cmd); });
    std::lock_guard<std::mutex> g(j->lock);
    j->seed = r.seed;
    if (j->stage.empty()) j->stage = "Connecting";
    return j;
}

JobPtr Client::probe(const Workflow& wf) {
    Target t;
    std::string err;
    if (!parseUrl(settings.url, t, err)) {
        auto j = std::make_shared<Job>();
        j->error = err;
        j->done  = true;
        return j;
    }
    return start([t, graph = wf.graph](Job& job) { probeJob(job, t, graph); });
}

void Client::cancel(const JobPtr& j) {
    if (j) j->cancelled = true;
}

} // namespace comfy
