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
//   7. What a server open to the internet needs: connections past the limit
//      are turned away, one that never says HELLO is dropped, a player cannot
//      put more than its share of orders into one turn, and one that stops
//      reading is dropped before its backlog grows without end.
//   8. A browser's WebSocket on the same port: the handshake (RFC 6455's own
//      example key), masked frames cut and bundled any way, a ping answered,
//      a plain HTTP request turned away -- and a WebSocket player and a desktop
//      player in one game, getting the same turns.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

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

// --- A browser, by hand: a WebSocket client as the RFC has it ---------------------
struct WsPeer {
    netsock::Handle sock = netsock::kInvalid;
    std::string raw;        // bytes from the server not yet taken
    std::string stream;     // unwrapped payload: the length-framed messages
    std::vector<std::string> got;
    bool handshaken = false;
    std::string head;
    int pongs = 0;
    bool closed = false;

    void sendRaw(const std::string& b) {
        ::send(static_cast<decltype(::socket(0, 0, 0))>(sock), b.data(), static_cast<int>(b.size()), 0);
    }
    // One masked client frame.
    static std::string frame(int opcode, const std::string& payload) {
        std::string f;
        f.push_back(static_cast<char>(0x80 | opcode));
        const std::size_t n = payload.size();
        if (n < 126) f.push_back(static_cast<char>(0x80 | n));
        else { f.push_back(static_cast<char>(0x80 | 126)); f.push_back(static_cast<char>(n >> 8)); f.push_back(static_cast<char>(n & 0xff)); }
        const unsigned char mask[4] = {0x37, 0xfa, 0x21, 0x3d};
        for (unsigned char m : mask) f.push_back(static_cast<char>(m));
        for (std::size_t i = 0; i < n; ++i) f.push_back(static_cast<char>(payload[i] ^ mask[i & 3]));
        return f;
    }
    static std::string msg(const std::string& m) {
        const std::size_t n = m.size();
        const char h[4] = {static_cast<char>(n & 0xff), static_cast<char>((n >> 8) & 0xff),
                           static_cast<char>((n >> 16) & 0xff), static_cast<char>((n >> 24) & 0xff)};
        return std::string(h, 4) + m;
    }
    void poll() {
        char buf[16384];
        for (;;) {
            const int n = static_cast<int>(::recv(static_cast<decltype(::socket(0, 0, 0))>(sock), buf, sizeof(buf), 0));
            if (n > 0) { raw.append(buf, static_cast<std::size_t>(n)); continue; }
            if (n == 0) closed = true;
            break;
        }
        if (!handshaken) {
            const std::size_t e = raw.find("\r\n\r\n");
            if (e == std::string::npos) return;
            head = raw.substr(0, e + 4);
            raw.erase(0, e + 4);
            handshaken = true;
        }
        for (;;) {   // server frames: unmasked
            if (raw.size() < 2) break;
            const auto* p = reinterpret_cast<const unsigned char*>(raw.data());
            std::size_t len = p[1] & 0x7f, hdr = 2;
            if (len == 126) { if (raw.size() < 4) break; len = (p[2] << 8) | p[3]; hdr = 4; }
            else if (len == 127) { if (raw.size() < 10) break; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | p[2 + i]; hdr = 10; }
            if (raw.size() < hdr + len) break;
            const int op = p[0] & 0x0f;
            if (op == 0x2) stream += raw.substr(hdr, len);
            if (op == 0xA) ++pongs;
            raw.erase(0, hdr + len);
        }
        while (stream.size() >= 4) {
            const auto* p = reinterpret_cast<const unsigned char*>(stream.data());
            const std::size_t len = p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::size_t>(p[3]) << 24);
            if (stream.size() < 4 + len) break;
            got.push_back(stream.substr(4, len));
            stream.erase(0, 4 + len);
        }
    }
    bool has(const std::string& prefix) const {
        for (const std::string& m : got) if (m.rfind(prefix, 0) == 0) return true;
        return false;
    }
};

bool connectWs(WsPeer& w, int port) {
    Peer tmp;   // connected as any peer, then spoken to by hand
    if (!connectPeer(tmp, port)) return false;
    w.sock = tmp.conn.sock;
    tmp.conn.sock = netsock::kInvalid;
    return true;
}

void runWs(std::vector<Peer*> peers, std::vector<WsPeer*> ws, double seconds) {
    const double end = g_now + seconds;
    while (g_now < end) {
        g_server->tick(g_now);
        for (Peer* p : peers) { p->conn.flush(); p->conn.read(p->got); }
        for (WsPeer* w : ws) w->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        g_now += 0.005;
    }
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

    std::printf("7. limits\n");
    {
        netrelay::Options lo;
        lo.turnMs         = 100;
        lo.maxClients     = 3;
        lo.helloTimeout   = 0.3;
        lo.maxCmdsPerTurn = 5;
        lo.maxBacklog     = 4096;
        netrelay::Server small(lo);
        g_server = &small;
        if (!small.listen(0, &err)) { check(false, "second server listens"); return 1; }
        const int p2 = small.port();
        Peer h, j, silent, extra;
        check(connectPeer(h, p2) && connectPeer(j, p2) && connectPeer(silent, p2), "three connect");
        h.conn.queue("HELLO Host\tsteelwars\t1");
        j.conn.queue("HELLO Joe\tsteelwars\t1");
        run({&h, &j, &silent}, 0.05);
        connectPeer(extra, p2);   // the kernel accepts; the relay closes it
        run({&h, &j, &silent, &extra}, 0.05);
        std::vector<std::string> drop;
        check(!extra.conn.read(drop), "a fourth connection is turned away");
        check(small.clientCount() == 3, "three stay connected");
        run({&h, &j, &silent}, 0.4);
        check(small.clientCount() == 2, "the one that never said HELLO is dropped");
        h.conn.queue("CREATE 2\tLimits\t");
        run({&h, &j}, 0.05);
        j.conn.queue("JOIN Limits");
        run({&h, &j}, 0.05);
        h.conn.queue("START");
        run({&h, &j}, 0.4);
        h.got.clear();
        for (int k = 0; k < 20; ++k) h.conn.queue("CMD spam " + std::to_string(k));
        run({&h, &j}, 0.15);
        int spam = 0;
        for (const std::string& t : all(h, "TURN"))
            for (std::size_t at = t.find("spam"); at != std::string::npos; at = t.find("spam", at + 1)) ++spam;
        check(spam == 5, "five orders of twenty make it into the turn");
        // Joe stops reading: what the room says piles up behind him -- past
        // what the kernel's own buffers hold -- until he is dropped.
        for (int k = 0; k < 2000 && small.clientCount() == 2; ++k) {
            h.conn.queue("CHAT " + std::string(64 * 1024, 'x'));
            run({&h}, 0.01);
            h.got.clear();
        }
        check(small.clientCount() == 1, "a peer that stopped reading is dropped");
        g_server = &server;
    }

    std::printf("8. browsers (WebSocket)\n");
    {
        check(netsock::webSocketAccept("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
              "the accept key of RFC 6455's example");
        netrelay::Server mixed(opt);
        g_server = &mixed;
        if (!mixed.listen(0, &err)) { check(false, "third server listens"); return 1; }
        const int p3 = mixed.port();
        Peer desk;
        WsPeer web, http;
        check(connectPeer(desk, p3) && connectWs(web, p3) && connectWs(http, p3), "a desktop and two browsers connect");
        // the handshake in two pieces, the head split mid-line
        web.sendRaw("GET /fitzel-relay/27960 HTTP/1.1\r\nHost: example.org\r\nUpgrade: websocket\r\n"
                    "Connection: Upgrade\r\nSec-WebSocket-Ke");
        runWs({&desk}, {&web}, 0.02);
        web.sendRaw("y: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                    "Sec-WebSocket-Protocol: binary, base64\r\n\r\n");
        http.sendRaw("GET / HTTP/1.1\r\nHost: example.org\r\n\r\n");
        runWs({&desk}, {&web, &http}, 0.05);
        check(web.head.rfind("HTTP/1.1 101", 0) == 0 &&
                  web.head.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos,
              "101 Switching Protocols with the right accept key");
        check(web.head.find("Sec-WebSocket-Protocol: binary") != std::string::npos, "subprotocol 'binary' chosen");
        check(http.head.rfind("HTTP/1.1 426", 0) == 0 && http.closed, "a plain HTTP request: 426, closed");
        // HELLO cut across two frames, CREATE/JOIN-less LIST bundled with it
        const std::string hello = WsPeer::msg("HELLO Wendy\tsteelwars\t1");
        web.sendRaw(WsPeer::frame(0x2, hello.substr(0, 7)));
        runWs({&desk}, {&web}, 0.02);
        web.sendRaw(WsPeer::frame(0x0, hello.substr(7) + WsPeer::msg("LIST")) + WsPeer::frame(0x9, "hi"));
        desk.conn.queue("HELLO Dirk\tsteelwars\t1");
        desk.conn.queue("CREATE 2\tMixed\t");
        runWs({&desk}, {&web}, 0.05);
        check(web.has("WELCOME") && web.has("ROOMS"), "the browser is greeted and lists the rooms");
        check(web.pongs == 1, "a ping is answered with a pong");
        web.sendRaw(WsPeer::frame(0x2, WsPeer::msg("JOIN Mixed")));
        runWs({&desk}, {&web}, 0.05);
        desk.conn.queue("START");
        runWs({&desk}, {&web}, 0.4);
        web.sendRaw(WsPeer::frame(0x2, WsPeer::msg("CMD from the browser")));
        desk.conn.queue("CMD from the desktop");
        runWs({&desk}, {&web}, 0.4);
        std::vector<std::string> tw;
        for (const std::string& m : web.got) if (m.rfind("TURN ", 0) == 0) tw.push_back(m);
        const auto td = all(desk, "TURN");
        bool both = false;
        for (const std::string& t : td)
            if (t.find("\n1\tfrom the browser") != std::string::npos && t.find("\n0\tfrom the desktop") != std::string::npos)
                both = true;
        check(web.has("START") && !td.empty() && tw == td, "browser and desktop get the same turns");
        check(both, "each one's orders reach the other");
        web.sendRaw(WsPeer::frame(0x8, ""));
        runWs({&desk}, {&web}, 0.05);
        check(first(desk, "LEFT").rfind("LEFT 1\tWendy", 0) == 0, "a WebSocket close leaves the game");
        netsock::closeHandle(web.sock);
        netsock::closeHandle(http.sock);
        g_server = &server;
    }

    std::printf("%s\n", g_fail ? "netcheck: FAILED" : "netcheck: all good");
    return g_fail ? 1 : 0;
}
