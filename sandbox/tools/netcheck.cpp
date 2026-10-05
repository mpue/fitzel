// netcheck: the lockstep relay (NetRelay.hpp) end to end over loopback.
//
// What has to hold for a multiplayer game to stay in step:
//   1. HELLO/CREATE/JOIN/SET put both players in one room, and both see it.
//   2. START gives both the same seed and turn length, and each its own slot.
//   3. Orders sent before a turn closes arrive in that TURN, for everybody, in
//      the same order -- and turns are numbered without gaps.
//   4. Different HASH values for one turn produce DESYNC; equal ones do not.
//   5. A player who disconnects mid-game is announced with LEFT.
//   6. A room of another game is invisible in LIST.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "NetRelay.hpp"
#include "NetSocket.hpp"

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

struct Peer {
    netsock::Conn conn;
    std::vector<std::string> got;
};

double g_now = 0.0;
netrelay::Server* g_server = nullptr;

// Step the server and both peers for `seconds` of server clock.
void run(std::vector<Peer*> peers, double seconds) {
    const double end = g_now + seconds;
    while (g_now < end) {
        g_server->tick(g_now);
        for (Peer* p : peers) {
            p->conn.flush();
            p->conn.read(p->got);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        g_now += 0.005;
    }
}

bool connectPeer(Peer& p, int port) {
    std::string err;
    p.conn.sock = netsock::connectTo("127.0.0.1", port, &err);
    if (p.conn.sock == netsock::kInvalid) return false;
    for (int i = 0; i < 2000; ++i) {
        g_server->tick(g_now);
        const int r = netsock::connectDone(p.conn.sock, &err);
        if (r > 0) { p.conn.open = true; return true; }
        if (r < 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

std::string first(const Peer& p, const std::string& verb) {
    for (const std::string& m : p.got)
        if (m.rfind(verb + " ", 0) == 0 || m.rfind(verb + "\n", 0) == 0 || m == verb) return m;
    return {};
}

std::vector<std::string> all(const Peer& p, const std::string& verb) {
    std::vector<std::string> out;
    for (const std::string& m : p.got)
        if (m.rfind(verb + " ", 0) == 0) out.push_back(m);
    return out;
}

} // namespace

int main() {
    std::printf("netcheck: lockstep relay over loopback\n");
    netrelay::Options opt;
    opt.turnMs = 100;
    netrelay::Server server(opt);
    g_server = &server;
    std::string err;
    if (!server.listen(0, &err)) {
        std::printf("cannot listen: %s\n", err.c_str());
        return 1;
    }
    const int port = server.port();

    Peer a, b, c;
    check(connectPeer(a, port) && connectPeer(b, port) && connectPeer(c, port), "three peers connect");

    std::printf("1. lobby\n");
    a.conn.queue("HELLO Alice\tsteelwars\t1");
    b.conn.queue("HELLO Bob\tsteelwars\t1");
    c.conn.queue("HELLO Carol\totherGame\t1");
    run({&a, &b, &c}, 0.05);
    check(!first(a, "WELCOME").empty() && !first(b, "WELCOME").empty(), "WELCOME for each");
    a.conn.queue("CREATE 2\tArena\tmap=grenzland");
    run({&a, &b, &c}, 0.05);
    b.conn.queue("LIST");
    c.conn.queue("LIST");
    run({&a, &b, &c}, 0.05);
    check(first(b, "ROOMS").find("Arena\t1\t2\t0\tmap=grenzland") != std::string::npos,
          "the other player lists the room");
    check(first(c, "ROOMS").find("Arena") == std::string::npos, "another game does not see it");
    b.conn.queue("JOIN Arena");
    b.conn.queue("SET team=1\tready");   // the tab must not break the protocol
    run({&a, &b, &c}, 0.05);
    const auto roomsA = all(a, "ROOM");
    check(!roomsA.empty() && roomsA.back().find("Bob\tteam=1 ready") != std::string::npos,
          "the host sees the joined player's lobby entry (tab cleaned)");

    std::printf("2. start\n");
    a.got.clear();
    b.got.clear();
    b.conn.queue("START");   // not the host: ignored
    run({&a, &b}, 0.05);
    check(first(a, "START").empty(), "only the host can start");
    a.conn.queue("START");
    run({&a, &b}, 0.02);
    const std::string sa = first(a, "START"), sb = first(b, "START");
    auto seedOf = [](const std::string& s) { return s.substr(6, s.find('\t') - 6); };
    check(!sa.empty() && !sb.empty() && seedOf(sa) == seedOf(sb), "both get START with one seed");
    check(sa.find("\t100\t0\t2\t") != std::string::npos && sb.find("\t100\t1\t2\t") != std::string::npos,
          "slots 0 and 1, turn length 100 ms");

    std::printf("3. turns\n");
    run({&a, &b}, 0.3);   // the start breath, then turns flow
    a.conn.queue("CMD move 1 2");
    b.conn.queue("CMD attack 7");
    a.conn.queue("CMD stop");
    run({&a, &b}, 0.5);
    const auto ta = all(a, "TURN"), tb = all(b, "TURN");
    check(ta.size() >= 4 && ta == tb, "both get the same turns");
    bool gapless = true;
    for (std::size_t i = 0; i < ta.size(); ++i) {
        const std::string head = ta[i].substr(0, ta[i].find('\n'));
        if (head != "TURN " + std::to_string(i)) gapless = false;
    }
    check(gapless, "turns are numbered 0, 1, 2 ... without gaps");
    std::string withOrders;
    for (const std::string& t : ta)
        if (t.find('\n') != std::string::npos) withOrders = t;
    check(withOrders.find("\n0\tmove 1 2") != std::string::npos &&
              withOrders.find("\n1\tattack 7") != std::string::npos &&
              withOrders.find("\n0\tstop") != std::string::npos,
          "every order arrives, tagged with its player's slot");

    std::printf("4. desync\n");
    a.got.clear();
    b.got.clear();
    a.conn.queue("HASH 3\tabc");
    b.conn.queue("HASH 3\tabc");
    run({&a, &b}, 0.05);
    check(first(a, "DESYNC").empty(), "equal hashes: no DESYNC");
    a.conn.queue("HASH 4\tabc");
    b.conn.queue("HASH 4\tabd");
    run({&a, &b}, 0.05);
    check(first(a, "DESYNC") == "DESYNC 4" && first(b, "DESYNC") == "DESYNC 4", "different hashes: DESYNC 4");

    std::printf("5. a player leaves\n");
    b.conn.close();
    run({&a}, 0.05);
    check(first(a, "LEFT").rfind("LEFT 1\tBob", 0) == 0, "LEFT 1 Bob");
    a.got.clear();
    run({&a}, 0.25);
    check(all(a, "TURN").size() >= 2, "the game goes on for who is left");

    std::printf("%s\n", g_fail ? "netcheck: FAILED" : "netcheck: all good");
    return g_fail ? 1 : 0;
}
