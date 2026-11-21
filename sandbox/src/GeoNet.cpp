#include "GeoNet.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <csignal>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace geonet {

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
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

Sock toSock(Socket s) { return static_cast<Sock>(s); }

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Wait until `s` can be read (or written), at most `sec`. 1 ready, 0 timeout, -1 error.
int waitFor(Sock s, bool write, double sec) {
    if (sec <= 0.0) return 0;
    fd_set fs, ex;
    FD_ZERO(&fs);
    FD_ZERO(&ex);
    FD_SET(s, &fs);
    FD_SET(s, &ex);
    timeval tv;
    tv.tv_sec  = static_cast<long>(sec);
    tv.tv_usec = static_cast<long>((sec - static_cast<double>(tv.tv_sec)) * 1e6);
    const int n = ::select(static_cast<int>(s) + 1, write ? nullptr : &fs, write ? &fs : nullptr, &ex, &tv);
    if (n < 0) return -1;
    if (n == 0) return 0;
    return FD_ISSET(s, &ex) && !FD_ISSET(s, &fs) ? -1 : 1;
}

bool sendAll(Sock s, const char* data, std::size_t n, double until) {
    while (n > 0) {
        if (waitFor(s, true, until - now()) <= 0) return false;
        const auto k = ::send(s, data, static_cast<int>(std::min<std::size_t>(n, 1 << 20)), kSendFlags);
        if (k <= 0) return false;
        data += k;
        n -= static_cast<std::size_t>(k);
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

std::string decode(const std::string& s) {
    std::string o;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                 std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

const char* reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default:  return "Status";
    }
}

} // namespace

bool init() {
#ifdef _WIN32
    static bool done = [] {
        WSADATA d;
        return WSAStartup(MAKEWORD(2, 2), &d) == 0;
    }();
    return done;
#else
    static bool done = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    return done;
#endif
}

bool get(const std::string& host, int port, const std::string& path, int timeoutSec, Response& r,
         std::string* err) {
    init();
    r = Response{};
    const double until = now() + std::max(timeoutSec, 1);
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string ps = std::to_string(port);
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) {
        if (err) *err = "cannot resolve " + host;
        return false;
    }
    Sock s = kNoSock;
    for (addrinfo* a = res; a && s == kNoSock; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kNoSock) continue;
        setBlocking(s, false);
        bool up = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0;
        if (!up && waitFor(s, true, std::min(3.0, until - now())) > 0) {
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soerr), &len);
            up = soerr == 0;
        }
        if (!up) { closeSock(s); s = kNoSock; }
    }
    freeaddrinfo(res);
    if (s == kNoSock) {
        if (err) *err = "nothing answers at " + host + ":" + ps;
        return false;
    }
    const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + ps +
                            "\r\nUser-Agent: Fitzel\r\nAccept: */*\r\nConnection: close\r\n\r\n";
    if (!sendAll(s, req.data(), req.size(), until)) {
        closeSock(s);
        if (err) *err = "the server went away";
        return false;
    }
    std::string in;
    char buf[65536];
    bool closed = false;
    while (!closed) {
        const int w = waitFor(s, false, until - now());
        if (w <= 0) break;
        const auto k = ::recv(s, buf, sizeof(buf), 0);
        if (k <= 0) { closed = true; break; }
        in.append(buf, static_cast<std::size_t>(k));
        // Done as soon as a Content-Length body is whole.
        const std::size_t he = in.find("\r\n\r\n");
        if (he != std::string::npos) {
            const std::string head = lower(in.substr(0, he));
            const std::size_t cl = head.find("content-length:");
            if (cl != std::string::npos) {
                const std::size_t want = static_cast<std::size_t>(std::strtoull(head.c_str() + cl + 15, nullptr, 10));
                if (in.size() >= he + 4 + want) { closed = true; break; }
            }
        }
    }
    closeSock(s);
    const std::size_t he = in.find("\r\n\r\n");
    if (he == std::string::npos || in.compare(0, 5, "HTTP/") != 0) {
        if (err) *err = closed ? "the server answered nothing" : "the server took too long";
        return false;
    }
    const std::size_t sp = in.find(' ');
    r.status = sp != std::string::npos ? std::atoi(in.c_str() + sp + 1) : 0;
    std::size_t pos = in.find("\r\n") + 2;
    while (pos < he) {
        const std::size_t e = in.find("\r\n", pos);
        const std::string line = in.substr(pos, e - pos);
        const std::size_t c = line.find(':');
        if (c != std::string::npos) r.headers[lower(trim(line.substr(0, c)))] = trim(line.substr(c + 1));
        pos = e + 2;
    }
    std::string body = in.substr(he + 4);
    const auto te = r.headers.find("transfer-encoding");
    if (te != r.headers.end() && lower(te->second).find("chunked") != std::string::npos) {
        std::string out;
        std::size_t p = 0;
        while (p < body.size()) {
            const std::size_t e = body.find("\r\n", p);
            if (e == std::string::npos) break;
            const std::size_t n = static_cast<std::size_t>(std::strtoull(body.substr(p, e - p).c_str(), nullptr, 16));
            if (n == 0) break;
            out.append(body, e + 2, n);
            p = e + 2 + n + 2;
        }
        body = std::move(out);
    } else {
        const auto cl = r.headers.find("content-length");
        if (cl != r.headers.end()) {
            const std::size_t want = static_cast<std::size_t>(std::strtoull(cl->second.c_str(), nullptr, 10));
            if (body.size() < want) {
                if (err) *err = "the answer was cut short";
                r.body = std::move(body);
                return false;
            }
            body.resize(want);
        }
    }
    r.body = std::move(body);
    return true;
}

Socket listenOn(const std::string& bindAddr, int port, std::string* err) {
    init();
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;
    addrinfo* res = nullptr;
    const std::string ps = std::to_string(port);
    if (getaddrinfo(bindAddr.empty() ? nullptr : bindAddr.c_str(), ps.c_str(), &hints, &res) != 0 || !res) {
        if (err) *err = "cannot resolve " + bindAddr;
        return -1;
    }
    Sock s = kNoSock;
    for (addrinfo* a = res; a && s == kNoSock; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kNoSock) continue;
        int yes = 1;
#ifndef _WIN32
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
        (void)yes;
#endif
        if (::bind(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) != 0 || ::listen(s, 64) != 0) {
            closeSock(s);
            s = kNoSock;
        }
    }
    freeaddrinfo(res);
    if (s == kNoSock) {
        if (err) *err = "cannot listen on " + bindAddr + ":" + ps + " (in use?)";
        return -1;
    }
    return static_cast<Socket>(s);
}

Socket acceptOne(Socket listener) {
    const Sock c = ::accept(toSock(listener), nullptr, nullptr);
    if (c == kNoSock) return -1;
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    return static_cast<Socket>(c);
}

void closeSocket(Socket s) {
    if (s >= 0) closeSock(toSock(s));
}

bool readRequest(Socket sock, Request& req, int timeoutSec) {
    const Sock s = toSock(sock);
    setBlocking(s, false);
    const double until = now() + std::max(timeoutSec, 1);
    std::string in;
    char buf[4096];
    while (in.find("\r\n\r\n") == std::string::npos) {
        if (in.size() > 64 * 1024) return false;
        if (waitFor(s, false, until - now()) <= 0) return false;
        const auto k = ::recv(s, buf, sizeof(buf), 0);
        if (k <= 0) return false;
        in.append(buf, static_cast<std::size_t>(k));
    }
    const std::size_t e = in.find("\r\n");
    const std::string line = in.substr(0, e);
    const std::size_t a = line.find(' '), b = line.rfind(' ');
    if (a == std::string::npos || b == a) return false;
    req.method = line.substr(0, a);
    std::string target = line.substr(a + 1, b - a - 1);
    const std::size_t q = target.find('?');
    req.path = decode(target.substr(0, q));
    req.query.clear();
    if (q != std::string::npos) {
        std::string qs = target.substr(q + 1);
        std::size_t p = 0;
        while (p <= qs.size()) {
            const std::size_t amp = qs.find('&', p);
            const std::string kv = qs.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
            const std::size_t eq = kv.find('=');
            if (!kv.empty()) req.query[decode(kv.substr(0, eq))] = eq == std::string::npos ? "" : decode(kv.substr(eq + 1));
            if (amp == std::string::npos) break;
            p = amp + 1;
        }
    }
    return true;
}

void respond(Socket sock, int status, const std::string& contentType, const std::string& body,
             const std::map<std::string, std::string>& extra) {
    const Sock s = toSock(sock);
    std::string head = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
    head += "Content-Type: " + contentType + "\r\n";
    head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    head += "Access-Control-Allow-Origin: *\r\nConnection: close\r\n";
    for (const auto& [k, v] : extra) head += k + ": " + v + "\r\n";
    head += "\r\n";
    const double until = now() + 60.0;
    if (sendAll(s, head.data(), head.size(), until)) sendAll(s, body.data(), body.size(), until);
}

// --- Programs -----------------------------------------------------------------------

#ifdef _WIN32
namespace {
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
// CommandLineToArgvW's rules, backwards.
std::wstring quoted(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring o = L"\"";
    int slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') o.append(static_cast<std::size_t>(slashes * 2 + 1), L'\\');
        else o.append(static_cast<std::size_t>(slashes), L'\\');
        slashes = 0;
        o += c;
    }
    o.append(static_cast<std::size_t>(slashes * 2), L'\\');
    return o + L"\"";
}
bool start(const std::vector<std::string>& argv, const std::string& log, bool detached, PROCESS_INFORMATION& pi) {
    std::wstring cmd;
    for (const std::string& a : argv) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quoted(widen(a));
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    HANDLE out = INVALID_HANDLE_VALUE;
    if (!log.empty()) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        out = CreateFileW(widen(log).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = si.hStdError = out;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }
    }
    const DWORD flags = CREATE_NO_WINDOW | (detached ? (DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP) : 0);
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, out != INVALID_HANDLE_VALUE, flags,
                                   nullptr, nullptr, &si, &pi);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    return ok != 0;
}
} // namespace

int run(const std::vector<std::string>& argv, int timeoutSec, const std::string& log) {
    if (argv.empty()) return -1;
    PROCESS_INFORMATION pi{};
    if (!start(argv, log, false, pi)) return -1;
    const DWORD w = WaitForSingleObject(pi.hProcess, static_cast<DWORD>(std::max(timeoutSec, 1)) * 1000u);
    DWORD code = static_cast<DWORD>(-1);
    if (w == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    else GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return w == WAIT_TIMEOUT ? -1 : static_cast<int>(code);
}

bool spawn(const std::vector<std::string>& argv, const std::string& log) {
    if (argv.empty()) return false;
    PROCESS_INFORMATION pi{};
    if (!start(argv, log, true, pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}
#else
namespace {
// argv for execvp, built BEFORE fork: the child may only call what is safe there.
std::vector<char*> cargs(const std::vector<std::string>& argv) {
    std::vector<char*> a;
    for (const std::string& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    return a;
}
void redirect(const char* log) {
    const int in = ::open("/dev/null", O_RDONLY);
    if (in >= 0) { ::dup2(in, 0); ::close(in); }
    const int out = log && *log ? ::open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644) : ::open("/dev/null", O_WRONLY);
    if (out >= 0) { ::dup2(out, 1); ::dup2(out, 2); ::close(out); }
}
} // namespace

int run(const std::vector<std::string>& argv, int timeoutSec, const std::string& log) {
    if (argv.empty()) return -1;
    std::vector<char*> a = cargs(argv);
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        redirect(log.c_str());
        ::execvp(a[0], a.data());
        ::_exit(127);
    }
    const double until = now() + std::max(timeoutSec, 1);
    int status = 0;
    for (;;) {
        const pid_t w = ::waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (w < 0) return -1;
        if (now() > until) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

bool spawn(const std::vector<std::string>& argv, const std::string& log) {
    if (argv.empty()) return false;
    std::vector<char*> a = cargs(argv);
    // Twice, so the server is nobody's child: it outlives the editor and never
    // stays behind as a zombie of it.
    const pid_t pid = ::fork();
    if (pid < 0) return false;
    if (pid == 0) {
        ::setsid();
        const pid_t inner = ::fork();
        if (inner != 0) ::_exit(inner < 0 ? 1 : 0);
        redirect(log.c_str());
        ::execvp(a[0], a.data());
        ::_exit(127);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}
#endif

} // namespace geonet
