// fitzelserver: the dedicated lockstep relay for multiplayer Fitzel games
// (Steelwars and anything else whose rules run in the clients' scripts).
//
// It is the same relay a player starts from inside the game with net.serve --
// see NetRelay.hpp for the protocol -- run on its own, so a game does not end
// when the player who hosted it leaves.
//
//   fitzelserver [--port 27960] [--turn-ms 100] [--quiet]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "NetRelay.hpp"

int main(int argc, char** argv) {
    int  port   = 27960;
    int  turnMs = 100;
    bool quiet  = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--port" && i + 1 < argc)         port = std::atoi(argv[++i]);
        else if (a == "--turn-ms" && i + 1 < argc) turnMs = std::atoi(argv[++i]);
        else if (a == "--quiet")                   quiet = true;
        else if (a == "--help" || a == "-h") {
            std::printf("fitzelserver [--port 27960] [--turn-ms 100] [--quiet]\n");
            return 0;
        }
    }
    netrelay::Options opt;
    opt.turnMs = turnMs < 20 ? 20 : turnMs;
    netrelay::Server server(opt);
    if (!quiet)
        server.log = [](const std::string& line) {
            const auto t  = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            char stamp[32] = {};
            std::strftime(stamp, sizeof(stamp), "%H:%M:%S", std::localtime(&t));
            std::printf("[%s] %s\n", stamp, line.c_str());
            std::fflush(stdout);
        };
    std::string err;
    if (!server.listen(port, &err)) {
        std::fprintf(stderr, "fitzelserver: %s\n", err.c_str());
        return 1;
    }
    std::printf("fitzelserver: lockstep relay on port %d, %d ms turns. Ctrl+C stops it.\n",
                server.port(), opt.turnMs);
    std::fflush(stdout);
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const double now =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        server.tick(now);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
