#include "NetSocket.hpp"

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

bool Conn::read(std::vector<std::string>& msgs) {
    if (!open) return false;
    char buf[16384];
    for (;;) {
        const int n = static_cast<int>(recv(raw(sock), buf, sizeof(buf), 0));
        if (n > 0) { in.append(buf, static_cast<std::size_t>(n)); continue; }
        if (n == 0) return false;            // orderly close
        if (wouldBlock()) break;
        return false;
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
    out.append(hdr, 4);
    out.append(msg);
}

bool Conn::flush() {
    if (!open) return false;
    std::size_t sent = 0;
    while (sent < out.size()) {
        const int n = static_cast<int>(send(raw(sock), out.data() + sent,
                                            static_cast<int>(out.size() - sent), 0));
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
    addrinfo hints{};
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) {
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
