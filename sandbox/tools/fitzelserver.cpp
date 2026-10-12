// fitzelserver: the dedicated lockstep relay for multiplayer Fitzel games
// (Steelwars and anything else whose rules run in the clients' scripts).
//
// It is the same relay a player starts from inside the game with net.serve --
// see NetRelay.hpp for the protocol -- run on its own, so a game does not end
// when the player who hosted it leaves. Headless: sockets and nothing else, so
// it runs on any server (as a systemd service, see steelwars/server/setup.sh).
//
//   fitzelserver [--port 27960] [--turn-ms 100] [--max-clients 64] [--quiet]
//   fitzelserver --probe host[:port]     is a relay answering there? (exit 0/1)
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

#include "NetRelay.hpp"
#include "NetSocket.hpp"

namespace {

std::atomic<bool> g_stop{false};
void onSignal(int) { g_stop = true; }

// Is stdout the journal (a systemd service)? Then it stamps every line itself.
// JOURNAL_STREAM is inherited by everything a service starts, so it counts only
// when it names the stream stdout really is.
bool stdoutIsJournal() {
#ifdef _WIN32
    return false;
#else
    const char* js = std::getenv("JOURNAL_STREAM");
    if (!js) return false;
    unsigned long long dev = 0, ino = 0;
    if (std::sscanf(js, "%llu:%llu", &dev, &ino) != 2) return false;
    struct stat st {};
    if (fstat(fileno(stdout), &st) != 0) return false;
    return static_cast<unsigned long long>(st.st_dev) == dev &&
           static_cast<unsigned long long>(st.st_ino) == ino;
#endif
}

double seconds() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Connect, say HELLO, ask for the rooms: a relay answers WELCOME and ROOMS.
int probe(const std::string& target, int defaultPort) {
    std::string host = target;
    int port = defaultPort;
    const std::size_t colon = target.rfind(':');
    if (colon != std::string::npos) {
        host = target.substr(0, colon);
        port = std::atoi(target.c_str() + colon + 1);
    }
    netsock::init();
    std::string err;
    netsock::Conn c;
    c.sock = netsock::connectTo(host, port, &err);
    if (c.sock == netsock::kInvalid) {
        std::fprintf(stderr, "probe %s:%d: %s\n", host.c_str(), port, err.c_str());
        return 1;
    }
    const double until = seconds() + 5.0;
    while (seconds() < until) {
        const int r = netsock::connectDone(c.sock, &err);
        if (r < 0) {
            std::fprintf(stderr, "probe %s:%d: %s\n", host.c_str(), port, err.c_str());
            return 1;
        }
        if (r > 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    c.open = true;
    c.queue("HELLO probe\tsteelwars\t0");
    c.queue("LIST");
    bool welcome = false;
    std::vector<std::string> msgs;
    while (seconds() < until) {
        c.flush();
        msgs.clear();
        if (!c.read(msgs)) break;
        for (const std::string& m : msgs) {
            if (m.rfind("WELCOME", 0) == 0) welcome = true;
            if (welcome && m.rfind("ROOMS", 0) == 0) {
                int rooms = 0;
                for (char ch : m) rooms += ch == '\n';
                std::printf("probe %s:%d: relay answers, %d Steelwars room(s) open\n", host.c_str(), port,
                            rooms);
                c.close();
                return 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    c.close();
    std::fprintf(stderr, "probe %s:%d: no relay answered\n", host.c_str(), port);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    int  port       = 27960;
    int  turnMs     = 100;
    int  maxClients = 64;
    bool quiet      = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc)             port = std::atoi(argv[++i]);
        else if (a == "--turn-ms" && i + 1 < argc)     turnMs = std::atoi(argv[++i]);
        else if (a == "--max-clients" && i + 1 < argc) maxClients = std::atoi(argv[++i]);
        else if (a == "--quiet")                       quiet = true;
        else if (a == "--probe" && i + 1 < argc)       return probe(argv[++i], port);
        else if (a == "--help" || a == "-h") {
            std::printf("fitzelserver [--port 27960] [--turn-ms 100] [--max-clients 64] [--quiet]\n"
                        "fitzelserver --probe host[:port]\n");
            return 0;
        } else {
            std::fprintf(stderr, "fitzelserver: unknown option '%s' (--help)\n", a.c_str());
            return 2;
        }
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);   // a peer gone mid-write is an error code, not our death
#endif
    netrelay::Options opt;
    opt.turnMs     = turnMs < 20 ? 20 : turnMs;
    opt.maxClients = maxClients < 2 ? 2 : maxClients;
    netrelay::Server server(opt);
    const bool stamp = !stdoutIsJournal();
    if (!quiet)
        server.log = [stamp](const std::string& line) {
            if (stamp) {
                const auto t  = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                char s[32] = {};
                std::strftime(s, sizeof(s), "%H:%M:%S", std::localtime(&t));
                std::printf("[%s] %s\n", s, line.c_str());
            } else {
                std::printf("%s\n", line.c_str());
            }
            std::fflush(stdout);
        };
    std::string err;
    if (!server.listen(port, &err)) {
        std::fprintf(stderr, "fitzelserver: %s\n", err.c_str());
        return 1;
    }
    std::printf("fitzelserver: lockstep relay on port %d, %d ms turns, up to %d connections.\n",
                server.port(), opt.turnMs, opt.maxClients);
    std::fflush(stdout);
    while (!g_stop) {
        server.tick(seconds());
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    server.stop();
    return 0;
}
