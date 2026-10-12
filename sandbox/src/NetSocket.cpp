#include "NetSocket.hpp"

#include <cctype>
#include <cstdint>
#include <cstring>

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
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace netsock {

namespace {

#ifdef _WIN32
SOCKET raw(Handle h) { return static_cast<SOCKET>(h); }
bool wouldBlock() {
    const int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS || e == WSAEALREADY;
}
std::string lastError() { return "socket error " + std::to_string(WSAGetLastError()); }
void setNonBlocking(Handle h) {
    u_long on = 1;
    ioctlsocket(raw(h), FIONBIO, &on);
}
#else
int raw(Handle h) { return h; }
bool wouldBlock() { return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS; }
std::string lastError() { return std::string("socket error ") + std::strerror(errno); }
void setNonBlocking(Handle h) {
    const int fl = fcntl(h, F_GETFL, 0);
    fcntl(h, F_SETFL, fl | O_NONBLOCK);
}
#endif

// Lockstep traffic is many small messages: Nagle would hold each one back
// waiting for the next, which is a turn's worth of latency for nothing.
void noDelay(Handle h) {
    int on = 1;
    setsockopt(raw(h), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

} // namespace

bool init() {
#ifdef _WIN32
    static bool done = false;
    static bool ok   = false;
    if (!done) {
        WSADATA d;
        ok   = WSAStartup(MAKEWORD(2, 2), &d) == 0;
        done = true;
    }
    return ok;
#else
    return true;
#endif
}

// --- WebSocket (RFC 6455), server side --------------------------------------------
namespace {

// SHA-1, for the one thing a WebSocket handshake needs it for.
std::string sha1(const std::string& msg) {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    std::string m = msg;
    const std::uint64_t bits = static_cast<std::uint64_t>(msg.size()) * 8u;
    m.push_back(static_cast<char>(0x80));
    while (m.size() % 64 != 56) m.push_back('\0');
    for (int i = 7; i >= 0; --i) m.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));
    const auto rol = [](std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); };
    for (std::size_t c = 0; c < m.size(); c += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(static_cast<unsigned char>(m[c + i * 4])) << 24) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(m[c + i * 4 + 1])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(m[c + i * 4 + 2])) << 8) |
                   static_cast<std::uint32_t>(static_cast<unsigned char>(m[c + i * 4 + 3]));
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], cc = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20)      { f = (b & cc) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ cc ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ cc ^ d;                    k = 0xCA62C1D6u; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = cc; cc = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += cc; h[3] += d; h[4] += e;
    }
    std::string out;
    for (std::uint32_t v : h)
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
    return out;
}

std::string base64(const std::string& s) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t i = 0;
    for (; i + 2 < s.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(s[i]) << 16) |
                           (static_cast<unsigned char>(s[i + 1]) << 8) | static_cast<unsigned char>(s[i + 2]);
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += t[v & 63];
    }
    if (i + 1 == s.size()) {
        const unsigned v = static_cast<unsigned char>(s[i]) << 16;
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == s.size()) {
        const unsigned v = (static_cast<unsigned char>(s[i]) << 16) | (static_cast<unsigned char>(s[i + 1]) << 8);
        out += t[(v >> 18) & 63]; out += t[(v >> 12) & 63]; out += t[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// The value of one header of an HTTP request head (names compare without case).
std::string header(const std::string& head, const std::string& name) {
    std::size_t at = head.find("\r\n");
    while (at != std::string::npos && at + 2 < head.size()) {
        const std::size_t end = head.find("\r\n", at + 2);
        const std::string line = head.substr(at + 2, end == std::string::npos ? std::string::npos : end - at - 2);
        const std::size_t colon = line.find(':');
        if (colon != std::string::npos && colon == name.size()) {
            bool same = true;
            for (std::size_t i = 0; i < colon; ++i)
                if (std::tolower(static_cast<unsigned char>(line[i])) != std::tolower(static_cast<unsigned char>(name[i])))
                    same = false;
            if (same) {
                std::size_t v = colon + 1;
                while (v < line.size() && (line[v] == ' ' || line[v] == '\t')) ++v;
                std::size_t e = line.size();
                while (e > v && (line[e - 1] == ' ' || line[e - 1] == '\t')) --e;
                return line.substr(v, e - v);
            }
        }
        at = end;
    }
    return {};
}

// One unmasked frame, server to client.
std::string wsFrame(int opcode, const std::string& payload) {
    std::string f;
    f.push_back(static_cast<char>(0x80 | opcode));
    const std::uint64_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(n));
    } else if (n < 65536) {
        f.push_back(static_cast<char>(126));
        f.push_back(static_cast<char>((n >> 8) & 0xff));
        f.push_back(static_cast<char>(n & 0xff));
    } else {
        f.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) f.push_back(static_cast<char>((n >> (i * 8)) & 0xff));
    }
    return f + payload;
}

} // namespace

std::string webSocketAccept(const std::string& key) {
    return base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

// The handshake, once its head is whole: answered, or false (not one).
static bool wsHandshake(Conn& c) {
    const std::size_t end = c.ws.find("\r\n\r\n");
    if (end == std::string::npos) return c.ws.size() <= 8192;   // more to come -- within reason
    const std::string head = c.ws.substr(0, end + 2);
    c.ws.erase(0, end + 4);
    const std::string key = header(head, "Sec-WebSocket-Key");
    if (key.empty()) {
        c.out += "HTTP/1.1 426 Upgrade Required\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
        c.flush();
        return false;
    }
    std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " + webSocketAccept(key) + "\r\n";
    // A browser that asks for subprotocols drops a connection that picks none:
    // "binary" (what emscripten asks for) when offered, else the first offered.
    const std::string protos = header(head, "Sec-WebSocket-Protocol");
    if (!protos.empty()) {
        std::string pick = protos.substr(0, protos.find(','));
        if (protos.find("binary") != std::string::npos) pick = "binary";
        while (!pick.empty() && pick.back() == ' ') pick.pop_back();
        reply += "Sec-WebSocket-Protocol: " + pick + "\r\n";
    }
    c.out += reply + "\r\n";
    c.wire = Conn::Wire::WebSocket;
    return true;
}

// Whole frames out of c.ws: data into the stream (c.in), a ping answered, a
// close the end. False when the peer is gone or broke the framing.
static bool wsFrames(Conn& c) {
    std::size_t at = 0;
    for (;;) {
        const std::size_t have = c.ws.size() - at;
        if (have < 2) break;
        const auto* p = reinterpret_cast<const unsigned char*>(c.ws.data() + at);
        const int opcode = p[0] & 0x0f;
        const bool masked = (p[1] & 0x80) != 0;
        std::uint64_t len = p[1] & 0x7f;
        std::size_t hdr = 2;
        if (len == 126) {
            if (have < 4) break;
            len = (static_cast<std::uint64_t>(p[2]) << 8) | p[3];
            hdr = 4;
        } else if (len == 127) {
            if (have < 10) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i];
            hdr = 10;
        }
        if (len > kMaxMessage + 16) return false;
        const std::size_t maskAt = hdr;
        if (masked) hdr += 4;
        if (have < hdr + len) break;
        std::string payload(c.ws.data() + at + hdr, static_cast<std::size_t>(len));
        if (masked)
            for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ p[maskAt + (i & 3)]);
        at += hdr + static_cast<std::size_t>(len);
        switch (opcode) {
            case 0x0: case 0x1: case 0x2: c.in += payload; break;   // continuation, text, binary
            case 0x8: return false;                                  // close
            case 0x9: c.out += wsFrame(0xA, payload); break;         // ping -> pong
            case 0xA: break;                                         // pong
            default:  return false;
        }
    }
    if (at) c.ws.erase(0, at);
    return true;
}

bool Conn::read(std::vector<std::string>& msgs) {
    if (!open) return false;
    char buf[16384];
    std::string got;
    for (;;) {
        const int n = static_cast<int>(recv(raw(sock), buf, sizeof(buf), 0));
        if (n > 0) { got.append(buf, static_cast<std::size_t>(n)); continue; }
        if (n == 0) return false;            // orderly close
        if (wouldBlock()) break;
        return false;
    }
    if (!accepted) wire = Wire::Stream;
    if (wire == Wire::Undecided) {
        ws += got;
        if (ws.size() < 4) return true;
        if (ws.compare(0, 4, "GET ") != 0) {
            wire = Wire::Stream;
            in += ws;
            ws.clear();
        } else if (!wsHandshake(*this)) {
            return false;
        }
        if (wire == Wire::WebSocket && !wsFrames(*this)) return false;
    } else if (wire == Wire::WebSocket) {
        ws += got;
        if (!wsFrames(*this)) return false;
    } else {
        in += got;
    }
    std::size_t at = 0;
    while (in.size() - at >= 4) {
        const auto* p = reinterpret_cast<const unsigned char*>(in.data() + at);
        const std::size_t len = static_cast<std::size_t>(p[0]) | (static_cast<std::size_t>(p[1]) << 8) |
                                (static_cast<std::size_t>(p[2]) << 16) |
                                (static_cast<std::size_t>(p[3]) << 24);
        if (len > kMaxMessage) return false; // not our protocol
        if (in.size() - at - 4 < len) break;
        msgs.emplace_back(in.data() + at + 4, len);
        at += 4 + len;
    }
    if (at) in = in.substr(at);
    return true;
}

void Conn::queue(const std::string& msg) {
    const std::size_t len = msg.size();
    const char hdr[4] = {static_cast<char>(len & 0xff), static_cast<char>((len >> 8) & 0xff),
                         static_cast<char>((len >> 16) & 0xff), static_cast<char>((len >> 24) & 0xff)};
    if (wire == Wire::WebSocket) {
        out += wsFrame(0x2, std::string(hdr, 4) + msg);
        return;
    }
    out.append(hdr, 4);
    out.append(msg);
}

bool Conn::flush() {
    if (!open) return false;
    // A peer that has gone is an error code here, not a SIGPIPE that ends the
    // whole process (the game hosting, or the dedicated server).
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    std::size_t sent = 0;
    while (sent < out.size()) {
        const int n = static_cast<int>(send(raw(sock), out.data() + sent,
                                            static_cast<int>(out.size() - sent), kFlags));
        if (n > 0) { sent += static_cast<std::size_t>(n); continue; }
        if (n < 0 && wouldBlock()) break;
        return false;
    }
    if (sent) out = out.substr(sent);
    return true;
}

void Conn::close() {
    if (sock != kInvalid) closeHandle(sock);
    sock = kInvalid;
    open = false;
    in.clear();
    out.clear();
    ws.clear();
    wire = accepted ? Wire::Undecided : Wire::Stream;
}

Handle listenOn(int port, std::string* err) {
    if (!init()) { if (err) *err = "network unavailable"; return kInvalid; }
    const auto s = static_cast<Handle>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s == kInvalid) { if (err) *err = lastError(); return kInvalid; }
#ifdef _WIN32
    // Windows' SO_REUSEADDR lets a second server steal a live port; exclusive
    // use is what "the port is taken" should mean there.
    int excl = 1;
    setsockopt(raw(s), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
#else
    int on = 1;
    setsockopt(raw(s), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
#endif
    sockaddr_in a{};
    a.sin_family      = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port        = htons(static_cast<unsigned short>(port));
    if (bind(raw(s), reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(raw(s), 16) != 0) {
        if (err) *err = "port " + std::to_string(port) + ": " + lastError();
        closeHandle(s);
        return kInvalid;
    }
    setNonBlocking(s);
    return s;
}

int boundPort(Handle s) {
    sockaddr_in a{};
#ifdef _WIN32
    int len = sizeof(a);
#else
    socklen_t len = sizeof(a);
#endif
    if (getsockname(raw(s), reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return ntohs(a.sin_port);
}

Handle acceptOne(Handle listener) {
    const auto c = static_cast<Handle>(accept(raw(listener), nullptr, nullptr));
    if (c == kInvalid) return kInvalid;
    setNonBlocking(c);
    noDelay(c);
    return c;
}

Handle connectTo(const std::string& host, int port, std::string* err) {
    if (!init()) { if (err) *err = "network unavailable"; return kInvalid; }
#ifdef __EMSCRIPTEN__
    // The browser has no TCP: emscripten's sockets are WebSockets, opened to
    // the URL its SOCKFS reads at connect time. A page served over https may
    // open only wss://, which the relay leaves to the web server in front of it
    // (Caddy, nginx): wss://host/fitzel-relay/<port>, passed on to the relay's
    // own port, which takes WebSockets beside desktop games (see the header).
    // Over plain http -- a test on localhost -- straight to ws://host:port. A
    // host given as a whole ws:// or wss:// URL is taken as it is. (Inside the
    // macro below: no two slashes in a row, they would start a comment, and no
    // comma outside brackets, it would end the argument.)
    EM_ASM({
        var host = UTF8ToString($0);
        var port = $1;
        var url;
        if (host.indexOf('ws:' + '/' + '/') === 0 || host.indexOf('wss:' + '/' + '/') === 0) url = host;
        else if (location.protocol === 'https:') url = 'wss:' + '/' + '/' + host + '/fitzel-relay/' + port;
        else url = 'ws:' + '/' + '/' + host + ':' + port + '/';
        if (typeof SOCKFS !== 'undefined' && SOCKFS.websocketArgs) SOCKFS.websocketArgs['url'] = url;
        (Module['websocket'] = Module['websocket'] || {})['url'] = url;
    }, host.c_str(), port);
    // The address itself is only a name to emscripten (the URL decides); a
    // URL is no name it can look up.
    const std::string lookup = (host.rfind("ws://", 0) == 0 || host.rfind("wss://", 0) == 0) ? "localhost" : host;
#else
    const std::string& lookup = host;
#endif
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(lookup.c_str(), service.c_str(), &hints, &res) != 0 || !res) {
        if (err) *err = "unknown host '" + host + "'";
        return kInvalid;
    }
    const auto s = static_cast<Handle>(socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (s == kInvalid) { freeaddrinfo(res); if (err) *err = lastError(); return kInvalid; }
    setNonBlocking(s);
    noDelay(s);
    const int r = connect(raw(s), res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);
    if (r != 0 && !wouldBlock()) {
        if (err) *err = lastError();
        closeHandle(s);
        return kInvalid;
    }
    return s;
}

int connectDone(Handle s, std::string* err) {
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(raw(s), &w);
    FD_SET(raw(s), &e);
    timeval tv{0, 0};
    const int n = select(static_cast<int>(raw(s) + 1), nullptr, &w, &e, &tv);
    if (n < 0) { if (err) *err = lastError(); return -1; }
    if (n == 0) return 0;
    int soErr = 0;
#ifdef _WIN32
    int len = sizeof(soErr);
#else
    socklen_t len = sizeof(soErr);
#endif
    getsockopt(raw(s), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &len);
    if (FD_ISSET(raw(s), &e) || soErr != 0) {
        if (err) *err = soErr ? "connection refused (" + std::to_string(soErr) + ")" : "connection failed";
        return -1;
    }
    return 1;
}

void closeHandle(Handle s) {
    if (s == kInvalid) return;
#ifdef _WIN32
    closesocket(raw(s));
#else
    ::close(s);
#endif
}

std::vector<std::string> localAddresses() {
    std::vector<std::string> out;
    if (!init()) return out;
#ifdef _WIN32
    char name[256] = {};
    if (gethostname(name, sizeof(name)) != 0) return out;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* res = nullptr;
    if (getaddrinfo(name, nullptr, &hints, &res) != 0) return out;
    for (addrinfo* p = res; p; p = p->ai_next) {
        char ip[64] = {};
        const auto* a = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        if (std::strncmp(ip, "127.", 4) != 0) out.emplace_back(ip);
    }
    freeaddrinfo(res);
#else
    ifaddrs* ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (ifaddrs* p = ifs; p; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
        char ip[64] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, ip, sizeof(ip));
        if (std::strncmp(ip, "127.", 4) != 0) out.emplace_back(ip);
    }
    freeifaddrs(ifs);
#endif
    return out;
}

} // namespace netsock
