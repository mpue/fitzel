#include "Ollama.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>

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

#include "NetSocket.hpp"
#include "UserDir.hpp"

using nlohmann::json;

namespace ollama {

namespace {

#ifdef _WIN32
using Sock = SOCKET;
const Sock kNoSock = INVALID_SOCKET;
void closeSock(Sock s) { closesocket(s); }
void cutSock(Sock s) { shutdown(s, SD_BOTH); }
bool setBlocking(Sock s, bool on) { u_long nb = on ? 0 : 1; return ioctlsocket(s, FIONBIO, &nb) == 0; }
#else
using Sock = int;
const Sock kNoSock = -1;
void closeSock(Sock s) { ::close(s); }
void cutSock(Sock s) { ::shutdown(s, SHUT_RDWR); }
bool setBlocking(Sock s, bool on) {
    const int f = fcntl(s, F_GETFL, 0);
    return f >= 0 && fcntl(s, F_SETFL, on ? (f & ~O_NONBLOCK) : (f | O_NONBLOCK)) == 0;
}
#endif

// A host that is not there answers a connect with silence, and the kernel's
// own patience is two minutes. A model server on this machine or the LAN
// either takes the connection at once or is not running.
constexpr int kConnectSec = 4;
// A big model loaded cold takes a while before its first token; one that has
// said nothing for this long is not going to.
constexpr int kReadSec = 300;

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// "http://host:port/" -> host, port. False (and why) for anything else.
bool parseUrl(const std::string& url, std::string& host, int& port, std::string& err) {
    std::string s = url;
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
    if (s.rfind("https://", 0) == 0) {
        err = "https is not supported -- Ollama speaks plain http (http://host:11434)";
        return false;
    }
    if (s.rfind("http://", 0) == 0) s = s.substr(7);
    const std::size_t slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);
    port = 11434;
    const std::size_t colon = s.rfind(':');
    if (colon != std::string::npos && s.find(']') == std::string::npos) {
        port = std::atoi(s.c_str() + colon + 1);
        s    = s.substr(0, colon);
    }
    if (!s.empty() && s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
    host = s;
    if (host.empty() || port <= 0 || port > 65535) {
        err = "not a server address: " + url;
        return false;
    }
    return true;
}

Sock connectTo(const std::string& host, int port, const Reply& r, std::string& err) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string ps = std::to_string(port);
    if (getaddrinfo(host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) {
        err = "cannot resolve " + host;
        return kNoSock;
    }
    Sock s = kNoSock;
    for (addrinfo* a = res; a && s == kNoSock && !r.cancelled; a = a->ai_next) {
        s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == kNoSock) continue;
        setBlocking(s, false);
        bool up = ::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0;
        // Waited for in short slices, so a cancel does not sit out the timeout.
        for (int slice = 0; !up && slice < kConnectSec * 10 && !r.cancelled; ++slice) {
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
    if (s == kNoSock && err.empty())
        err = "no model server at " + host + ":" + ps + " (is Ollama running?)";
    return s;
}

// Reading a chunked body as it arrives: bytes in, the decoded body out.
struct Dechunker {
    std::string in;
    std::size_t left = 0;     // of the current chunk (its CRLF not counted)
    bool        inData = false, finished = false;
    void feed(const char* p, std::size_t n, std::string& out) {
        in.append(p, n);
        std::size_t at = 0;
        while (!finished) {
            if (inData) {
                const std::size_t take = std::min(left, in.size() - at);
                out.append(in, at, take);
                at += take;
                left -= take;
                if (left > 0) break;
                if (in.size() - at < 2) break;   // the chunk's own CRLF
                at += 2;
                inData = false;
            } else {
                const std::size_t eol = in.find("\r\n", at);
                if (eol == std::string::npos) break;
                left = std::strtoul(in.substr(at, eol - at).c_str(), nullptr, 16);
                at = eol + 2;
                if (left == 0) finished = true;
                else inData = true;
            }
        }
        in.erase(0, at);
    }
};

// One streamed object (a line of NDJSON) into the reply.
void take(Reply& r, const json& j) {
    std::lock_guard<std::mutex> g(r.lock);
    if (j.contains("error")) {
        r.error = j["error"].is_string() ? j["error"].get<std::string>() : j["error"].dump();
        return;
    }
    if (j.contains("message") && j["message"].is_object()) {
        const json& m = j["message"];
        if (m.contains("content") && m["content"].is_string()) r.content += m["content"].get<std::string>();
        if (m.contains("thinking") && m["thinking"].is_string()) r.thinking += m["thinking"].get<std::string>();
        if (m.contains("tool_calls") && m["tool_calls"].is_array())
            for (const json& c : m["tool_calls"]) r.toolCalls.push_back(c);
    }
    if (j.contains("response") && j["response"].is_string()) r.content += j["response"].get<std::string>();
    if (!j.contains("message") && j.contains("thinking") && j["thinking"].is_string())
        r.thinking += j["thinking"].get<std::string>();
    if (j.value("done", false)) r.tokens = j.value("eval_count", 0LL);
    r.last = j;
}

void run(ReplyPtr rp, std::string host, int port, std::string method, std::string path,
         std::string body, bool streamed) {
    Reply& r = *rp;
    const double t0 = now();
    auto finish = [&](const std::string& err) {
        std::lock_guard<std::mutex> g(r.lock);
        if (!err.empty() && r.error.empty()) r.error = err;
        if (r.cancelled && r.error.empty()) r.error = "cancelled";
        r.ok      = r.error.empty() && r.status == 200;
        if (!r.ok && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
        r.seconds = now() - t0;
        r.done    = true;
    };
    netsock::init();
    std::string err;
    const Sock s = connectTo(host, port, r, err);
    if (s == kNoSock) { finish(err); return; }
    {
        std::lock_guard<std::mutex> g(r.lock);
        r.sock = static_cast<std::intptr_t>(s);
        if (r.cancelled) cutSock(s);
    }
#ifdef _WIN32
    const DWORD tv = kReadSec * 1000;
#else
    timeval tv{kReadSec, 0};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    std::string msg = method + " " + path + " HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                      "\r\nAccept: application/json\r\nConnection: close\r\n";
    if (!body.empty())
        msg += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
    msg += "\r\n" + body;
    bool ok = true;
    for (std::size_t sent = 0; sent < msg.size();) {
        const int n = ::send(s, msg.data() + sent, static_cast<int>(msg.size() - sent), 0);
        if (n <= 0) { ok = false; err = "send failed"; break; }
        sent += static_cast<std::size_t>(n);
    }

    // The head, then the body as it comes: decoded from chunks when it is
    // chunked, cut into lines when it streams.
    std::string head, bodyBytes, line, whole;
    bool        haveHead = false, chunked = false;
    Dechunker   dech;
    auto onBody = [&](const char* p, std::size_t n) {
        std::string dec;
        if (chunked) dech.feed(p, n, dec);
        else dec.assign(p, n);
        if (!streamed || r.status != 200) { whole += dec; return; }
        line += dec;
        std::size_t nl;
        while ((nl = line.find('\n')) != std::string::npos) {
            const std::string one = line.substr(0, nl);
            line.erase(0, nl + 1);
            if (one.find_first_not_of(" \r\t") == std::string::npos) continue;
            const json j = json::parse(one, nullptr, false);
            if (!j.is_discarded()) take(r, j);
        }
    };
    char buf[16384];
    while (ok) {
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            if (!r.cancelled) err = "no answer (timed out or cut off)";
            ok = false;
            break;
        }
        if (haveHead) { onBody(buf, static_cast<std::size_t>(n)); continue; }
        head.append(buf, static_cast<std::size_t>(n));
        const std::size_t end = head.find("\r\n\r\n");
        if (end == std::string::npos) continue;
        haveHead = true;
        if (head.compare(0, 5, "HTTP/") != 0) { err = "not an HTTP answer"; ok = false; break; }
        const std::size_t sp = head.find(' ');
        {
            std::lock_guard<std::mutex> g(r.lock);
            r.status = sp != std::string::npos ? std::atoi(head.c_str() + sp + 1) : 0;
        }
        std::string h = head.substr(0, end);
        std::transform(h.begin(), h.end(), h.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        chunked = h.find("transfer-encoding: chunked") != std::string::npos;
        const std::string rest = head.substr(end + 4);
        if (!rest.empty()) onBody(rest.data(), rest.size());
    }
    {
        std::lock_guard<std::mutex> g(r.lock);
        r.sock = -1;
    }
    closeSock(s);
    if (!line.empty() && streamed) {
        const json j = json::parse(line, nullptr, false);
        if (!j.is_discarded()) take(r, j);
    }
    if (!whole.empty()) {
        const json j = json::parse(whole, nullptr, false);
        if (j.is_discarded()) {
            if (err.empty() && r.status == 200) err = "the server's answer is not JSON";
            else if (err.empty()) err = "HTTP " + std::to_string(r.status) + ": " + whole.substr(0, 200);
        } else {
            take(r, j);
        }
    }
    finish(ok ? std::string() : err);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

Client& client() {
    static Client c;
    return c;
}

Client::~Client() {
    std::vector<std::pair<std::thread, ReplyPtr>> all;
    {
        std::lock_guard<std::mutex> g(m_lock);
        all.swap(m_threads);
    }
    for (auto& t : all) cancel(t.second);
    for (auto& t : all)
        if (t.first.joinable()) t.first.join();
}

void Client::load() {
    std::ifstream in(userdir::file("ollama.json"));
    if (!in) return;
    const json j = json::parse(in, nullptr, false);
    if (!j.is_object()) return;
    Settings& s = settings;
    s.url            = j.value("url", s.url);
    s.chatModel      = j.value("chatModel", s.chatModel);
    s.codeModel      = j.value("codeModel", s.codeModel);
    s.contextSize    = std::clamp(j.value("contextSize", s.contextSize), 2048, 262144);
    s.temperature    = j.value("temperature", s.temperature);
    s.inlineAuto     = j.value("inlineAuto", s.inlineAuto);
    s.inlineDelay    = std::clamp(j.value("inlineDelay", s.inlineDelay), 0.1f, 5.0f);
    s.inlineMaxLines = std::clamp(j.value("inlineMaxLines", s.inlineMaxLines), 1, 60);
    s.confirmChanges = j.value("confirmChanges", s.confirmChanges);
}

void Client::save() const {
    const Settings& s = settings;
    const json j = {{"url", s.url},
                    {"chatModel", s.chatModel},
                    {"codeModel", s.codeModel},
                    {"contextSize", s.contextSize},
                    {"temperature", s.temperature},
                    {"inlineAuto", s.inlineAuto},
                    {"inlineDelay", s.inlineDelay},
                    {"inlineMaxLines", s.inlineMaxLines},
                    {"confirmChanges", s.confirmChanges}};
    std::ofstream out(userdir::file("ollama.json"));
    if (out) out << j.dump(2);
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

ReplyPtr Client::start(const std::string& method, const std::string& path, std::string body,
                       bool streamed) {
    reap();
    auto r = std::make_shared<Reply>();
    std::string host, err;
    int port = 0;
    if (!parseUrl(settings.url, host, port, err)) {
        r->error = err;
        r->done  = true;
        return r;
    }
    std::thread t(run, r, host, port, method, path, std::move(body), streamed);
    std::lock_guard<std::mutex> g(m_lock);
    m_threads.emplace_back(std::move(t), r);
    return r;
}

void Client::cancel(const ReplyPtr& r) {
    if (!r) return;
    std::lock_guard<std::mutex> g(r->lock);
    r->cancelled = true;
    if (r->sock != -1) cutSock(static_cast<Sock>(r->sock));
}

namespace {
void fillOptions(json& body, const Settings& s) {
    if (!body.contains("options") || !body["options"].is_object()) body["options"] = json::object();
    json& o = body["options"];
    if (!o.contains("num_ctx")) o["num_ctx"] = s.contextSize;
    if (!o.contains("temperature")) o["temperature"] = s.temperature;
}
} // namespace

ReplyPtr Client::chat(json body) {
    if (!body.contains("model")) body["model"] = chatModel();
    if (!body.contains("stream")) body["stream"] = true;
    fillOptions(body, settings);
    const bool streamed = body.value("stream", true);
    return start("POST", "/api/chat", body.dump(-1, ' ', false, json::error_handler_t::replace), streamed);
}

ReplyPtr Client::generate(json body) {
    if (!body.contains("model")) body["model"] = codeModel();
    if (!body.contains("stream")) body["stream"] = true;
    fillOptions(body, settings);
    const bool streamed = body.value("stream", true);
    return start("POST", "/api/generate", body.dump(-1, ' ', false, json::error_handler_t::replace), streamed);
}

ReplyPtr Client::request(const std::string& method, const std::string& path, const json& body) {
    return start(method, path, body.is_null() ? std::string() : body.dump(), false);
}

void Client::refreshModels() {
    m_askedModels = true;
    m_modelsAt    = now();
    if (m_tags) return;
    m_tags = request("GET", "/api/tags");
}

const std::vector<std::string>& Client::models() {
    // Asked once; and again now and then while there are none -- Ollama may
    // be started after the editor.
    if (!m_askedModels || (m_models.empty() && !m_tags && now() - m_modelsAt > 20.0)) refreshModels();
    return m_models;
}

const std::vector<std::string>& Client::capabilities(const std::string& model) {
    static const std::vector<std::string> none;
    if (model.empty()) return none;
    const auto it = m_caps.find(model);
    if (it != m_caps.end()) return it->second;
    if (!m_showing.count(model)) m_showing[model] = request("POST", "/api/show", json{{"model", model}});
    return none;
}

Fim Client::fimFor(const std::string& model) {
    const std::string m = lower(model);
    // The families whose hole-filling tokens are known go raw: their Ollama
    // template often has no suffix slot (qwen3-coder's does not).
    if (m.find("qwen") != std::string::npos && m.find("coder") != std::string::npos) return Fim::Qwen;
    if (m.find("codegemma") != std::string::npos) return Fim::Qwen;   // the same tokens
    if (m.find("deepseek-coder") != std::string::npos) return Fim::DeepSeek;
    if (m.find("codellama") != std::string::npos) return Fim::CodeLlama;
    if (m.find("starcoder") != std::string::npos) return Fim::StarCoder;
    const auto& caps = capabilities(model);
    if (std::find(caps.begin(), caps.end(), "insert") != caps.end()) return Fim::Suffix;
    return Fim::Chat;
}

std::string Client::chatModel() {
    if (!settings.chatModel.empty()) return settings.chatModel;
    const auto& ms = models();
    // A coder that also calls tools does both jobs; else the first one.
    for (const std::string& m : ms)
        if (lower(m).find("coder") != std::string::npos) return m;
    return ms.empty() ? std::string() : ms.front();
}

std::string Client::codeModel() {
    return settings.codeModel.empty() ? chatModel() : settings.codeModel;
}

void Client::poll() {
    if (m_tags && m_tags->finished()) {
        std::lock_guard<std::mutex> g(m_tags->lock);
        m_models.clear();
        m_modelsError.clear();
        if (!m_tags->ok) {
            m_modelsError = m_tags->error;
        } else if (m_tags->last.contains("models") && m_tags->last["models"].is_array()) {
            for (const json& m : m_tags->last["models"])
                if (m.contains("name") && m["name"].is_string()) m_models.push_back(m["name"].get<std::string>());
            std::sort(m_models.begin(), m_models.end());
            if (m_models.empty()) m_modelsError = "Ollama has no models (ollama pull qwen2.5-coder)";
        }
        m_tags.reset();
    }
    for (auto it = m_showing.begin(); it != m_showing.end();) {
        if (!it->second->finished()) { ++it; continue; }
        std::vector<std::string> caps;
        {
            std::lock_guard<std::mutex> g(it->second->lock);
            const json& j = it->second->last;
            if (j.contains("capabilities") && j["capabilities"].is_array())
                for (const json& c : j["capabilities"])
                    if (c.is_string()) caps.push_back(c.get<std::string>());
        }
        m_caps[it->first] = std::move(caps);
        it = m_showing.erase(it);
    }
}

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

std::string firstCodeBlock(const std::string& s) {
    const std::size_t a = s.find("```");
    if (a == std::string::npos) return s;
    std::size_t body = s.find('\n', a);
    if (body == std::string::npos) return std::string();
    ++body;
    const std::size_t b = s.find("\n```", body - 1);
    if (b == std::string::npos) return s.substr(body);
    return b < body ? std::string() : s.substr(body, b - body + 1);
}

} // namespace ollama
