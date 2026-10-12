#include "CSharpLanguage.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>

#include "CSharpHost.hpp"
#include "CSharpProject.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace cslang {
namespace {

double now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
#endif

} // namespace

Client::~Client() { stop(); }

bool Client::ensure(const std::string& projectDir) {
    if (projectDir.empty()) {
        if (m_running.load()) stop();
        return false;
    }
    if (!m_running.load() && m_reader.joinable()) stop();   // it died: clean up first
    if (m_running.load() && m_project == projectDir) return true;
    if (m_running.load()) stop();
    // Not every frame: a service that will not start is tried again in a while.
    if (m_project == projectDir && now() - m_lastAttempt < 10.0) return false;
    m_lastAttempt = now();
    m_project     = projectDir;
    return start(projectDir);
}

bool Client::start(const std::string& projectDir) {
    m_problem.clear();
    m_ready.store(false);
    const fs::path dll = CSharpScripts::engineDir() / "Fitzel.LanguageServer.dll";
    std::error_code ec;
    if (!fs::exists(dll, ec)) {
        m_problem = "the C# language service is missing (" + dll.generic_string() + ")";
        return false;
    }
    const fs::path dotnet = csproject::dotnetPath();
    if (dotnet.empty()) {
        m_problem = "dotnet was not found: the C# editor needs the .NET 10 SDK";
        return false;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inRd = nullptr, inWr = nullptr, outRd = nullptr, outWr = nullptr, errRd = nullptr, errWr = nullptr;
    if (!CreatePipe(&inRd, &inWr, &sa, 0) || !CreatePipe(&outRd, &outWr, &sa, 0) || !CreatePipe(&errRd, &errWr, &sa, 0)) {
        m_problem = "no pipes for the C# language service";
        return false;
    }
    SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = inRd;
    si.hStdOutput = outWr;
    si.hStdError  = errWr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + dotnet.wstring() + L"\" \"" + dll.wstring() + L"\"";
    SetEnvironmentVariableW(L"DOTNET_CLI_TELEMETRY_OPTOUT", L"1");
    SetEnvironmentVariableW(L"DOTNET_NOLOGO", L"1");
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   nullptr, &si, &pi);
    CloseHandle(inRd);
    CloseHandle(outWr);
    CloseHandle(errWr);
    if (!ok) {
        CloseHandle(inWr); CloseHandle(outRd); CloseHandle(errRd);
        m_problem = "the C# language service would not start";
        return false;
    }
    CloseHandle(pi.hThread);
    m_process = pi.hProcess;
    m_in = inWr; m_out = outRd; m_err = errRd;
#else
    int in[2], out[2], err[2];
    if (pipe(in) != 0 || pipe(out) != 0 || pipe(err) != 0) {
        m_problem = "no pipes for the C# language service";
        return false;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(in[0], 0); dup2(out[1], 1); dup2(err[1], 2);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]); close(err[0]); close(err[1]);
        setenv("DOTNET_CLI_TELEMETRY_OPTOUT", "1", 1);
        setenv("DOTNET_NOLOGO", "1", 1);
        const std::string exe = dotnet.string(), arg = dll.string();
        execl(exe.c_str(), exe.c_str(), arg.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(in[0]); close(out[1]); close(err[1]);
    if (pid < 0) {
        close(in[1]); close(out[0]); close(err[0]);
        m_problem = "the C# language service would not start";
        return false;
    }
    std::signal(SIGPIPE, SIG_IGN);   // a dead service must not take the editor along
    m_pid = pid; m_in = in[1]; m_out = out[0]; m_err = err[0];
#endif

    m_running.store(true);
    m_reader    = std::thread([this] { readLoop(); });
    m_errReader = std::thread([this] { errLoop(); });
    request("project", {{"dir", projectDir}});
    m_warmId = m_nextId;   // before the request: its reply may come back at once
    request("warm", nlohmann::json::object(), "warm");
    std::fprintf(stderr, "[C# editor] language service started for %s\n", projectDir.c_str());
    return true;
}

void Client::stop() {
    if (!m_reader.joinable() && !m_errReader.joinable()) {
        m_running.store(false);
        return;
    }
    writeLine("{\"op\":\"exit\"}");
#ifdef _WIN32
    if (m_in) { CloseHandle(static_cast<HANDLE>(m_in)); m_in = nullptr; }
    if (m_process) {
        if (WaitForSingleObject(static_cast<HANDLE>(m_process), 1500) == WAIT_TIMEOUT)
            TerminateProcess(static_cast<HANDLE>(m_process), 1);
    }
#else
    if (m_in >= 0) { close(m_in); m_in = -1; }
    if (m_pid > 0) {
        for (int i = 0; i < 30; ++i) {
            if (waitpid(m_pid, nullptr, WNOHANG) == m_pid) { m_pid = -1; break; }
            usleep(50000);
        }
        if (m_pid > 0) { kill(m_pid, SIGKILL); waitpid(m_pid, nullptr, 0); m_pid = -1; }
    }
#endif
    if (m_reader.joinable()) m_reader.join();
    if (m_errReader.joinable()) m_errReader.join();
#ifdef _WIN32
    if (m_out) { CloseHandle(static_cast<HANDLE>(m_out)); m_out = nullptr; }
    if (m_err) { CloseHandle(static_cast<HANDLE>(m_err)); m_err = nullptr; }
    if (m_process) { CloseHandle(static_cast<HANDLE>(m_process)); m_process = nullptr; }
#else
    if (m_out >= 0) { close(m_out); m_out = -1; }
    if (m_err >= 0) { close(m_err); m_err = -1; }
#endif
    m_running.store(false);
    m_ready.store(false);
    std::lock_guard<std::mutex> g(m_lock);
    m_replies.clear();
}

bool Client::writeLine(const std::string& line) {
    std::lock_guard<std::mutex> g(m_writeLock);
    const std::string data = line + "\n";
#ifdef _WIN32
    if (!m_in) return false;
    DWORD n = 0;
    return WriteFile(static_cast<HANDLE>(m_in), data.data(), static_cast<DWORD>(data.size()), &n, nullptr) &&
           n == data.size();
#else
    if (m_in < 0) return false;
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = write(m_in, data.data() + done, data.size() - done);
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
#endif
}

std::int64_t Client::request(const std::string& op, nlohmann::json args, const std::string& slot) {
    if (!m_running.load()) return 0;
    if (!args.is_object()) args = nlohmann::json::object();
    const std::int64_t id = m_nextId++;
    args["id"] = id;
    args["op"] = op;
    if (!slot.empty()) args["slot"] = slot;
    // Invalid UTF-8 in a buffer must not stop the request (or the editor).
    if (!writeLine(args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))) return 0;
    return id;
}

std::vector<Reply> Client::poll() {
    std::lock_guard<std::mutex> g(m_lock);
    std::vector<Reply> out(m_replies.begin(), m_replies.end());
    m_replies.clear();
    return out;
}

bool Client::wait(std::int64_t id, int ms, Reply& out) {
    std::unique_lock<std::mutex> g(m_lock);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        for (auto it = m_replies.begin(); it != m_replies.end(); ++it)
            if (it->id == id) {
                out = std::move(*it);
                m_replies.erase(it);
                return true;
            }
        if (!m_running.load()) return false;
        if (m_arrived.wait_until(g, until) == std::cv_status::timeout) return false;
    }
}

void Client::readLoop() {
    std::string buf;
    char        chunk[16384];
    for (;;) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(static_cast<HANDLE>(m_out), chunk, sizeof chunk, &n, nullptr) || n == 0) break;
#else
        const ssize_t n = read(m_out, chunk, sizeof chunk);
        if (n <= 0) break;
#endif
        buf.append(chunk, static_cast<std::size_t>(n));
        std::size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            const std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
            if (j.is_discarded() || !j.is_object() || !j.contains("id")) continue;
            Reply r;
            r.id = j.value("id", std::int64_t(0));
            r.op = j.value("op", std::string());
            if (j.contains("error")) r.error = j["error"].is_string() ? j["error"].get<std::string>() : "error";
            else { r.ok = true; r.result = j.contains("result") ? j["result"] : nlohmann::json::object(); }
            if (r.id == m_warmId) {
                m_ready.store(true);
                std::fprintf(stderr, "[C# editor] language service ready\n");
            }
            {
                std::lock_guard<std::mutex> g(m_lock);
                m_replies.push_back(std::move(r));
                // Nobody polling (window closed): keep the queue bounded.
                while (m_replies.size() > 512) m_replies.pop_front();
            }
            m_arrived.notify_all();
        }
    }
    m_running.store(false);
    m_arrived.notify_all();
}

// The service's stderr into the editor's (the Log window shows it).
void Client::errLoop() {
    std::string buf;
    char        chunk[4096];
    for (;;) {
#ifdef _WIN32
        DWORD n = 0;
        if (!ReadFile(static_cast<HANDLE>(m_err), chunk, sizeof chunk, &n, nullptr) || n == 0) break;
#else
        const ssize_t n = read(m_err, chunk, sizeof chunk);
        if (n <= 0) break;
#endif
        buf.append(chunk, static_cast<std::size_t>(n));
        std::size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) std::fprintf(stderr, "%s\n", line.c_str());
        }
    }
}

} // namespace cslang
