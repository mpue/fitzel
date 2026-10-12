#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

// The C# editor's line to its language service: Fitzel.LanguageServer.dll
// (sandbox/dotnet/Fitzel.LanguageServer), started as `dotnet <dll>` and fed
// JSON lines on stdin, answering JSON lines on stdout -- Roslyn's completion,
// quick info, diagnostics and so on for the open project's scripts. A process
// of its own, so Roslyn's memory and any trouble it has stay out of the
// editor, and the game's scripts keep the in-process runtime to themselves.
//
// Requests are fire-and-poll: request() returns an id, poll() hands back the
// replies that have arrived. A request in the same "slot" as an earlier one
// cancels it on the service's side (a completion asked for while the last is
// still being worked out). Editor only.
namespace cslang {

struct Reply {
    std::int64_t   id = 0;
    std::string    op;
    bool           ok = false;
    nlohmann::json result;   // ok: what the op returned
    std::string    error;    // !ok: why not ("cancelled" when superseded)
};

class Client {
public:
    Client() = default;
    ~Client();
    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    // The service for this project: started on first use, restarted for
    // another project or after it died. False (and problem()) when it cannot.
    bool ensure(const std::string& projectDir);
    void stop();

    bool               running() const { return m_running.load(); }
    // Warmed up: the first completion will not take seconds.
    bool               ready() const { return m_ready.load(); }
    const std::string& problem() const { return m_problem; }
    const std::string& project() const { return m_project; }

    // Send `args` (an object) as op. Returns the id, 0 when not running.
    std::int64_t request(const std::string& op, nlohmann::json args = nlohmann::json::object(),
                         const std::string& slot = {});
    // The replies that arrived since the last call, oldest first.
    std::vector<Reply> poll();
    // Block until the reply to `id` arrives (other replies stay queued for
    // poll), at most `ms` milliseconds. False on timeout.
    bool wait(std::int64_t id, int ms, Reply& out);

private:
    bool start(const std::string& projectDir);
    void readLoop();
    void errLoop();
    bool writeLine(const std::string& line);

    std::string       m_project;
    std::string       m_problem;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::int64_t      m_nextId = 1;
    std::int64_t      m_warmId = 0;
    double            m_lastAttempt = -1e9;

    std::mutex        m_lock;
    std::condition_variable m_arrived;
    std::deque<Reply> m_replies;
    std::mutex        m_writeLock;
    std::thread       m_reader, m_errReader;

#ifdef _WIN32
    void* m_process = nullptr;   // HANDLE
    void* m_in  = nullptr;       // our end of its stdin
    void* m_out = nullptr;       // our end of its stdout
    void* m_err = nullptr;       // our end of its stderr
#else
    int m_pid = -1, m_in = -1, m_out = -1, m_err = -1;
#endif
};

} // namespace cslang
