#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

#include "NetSocket.hpp"

// A lockstep relay: the server half of a multiplayer game whose rules run in
// the clients (a Lua script), identically on every machine.
//
// --- Why a relay and not an authority -----------------------------------------
// An RTS has hundreds of units and a handful of orders a second. Sending the
// world is expensive; sending the ORDERS is nearly free, provided every client
// runs the same simulation from the same seed and applies the same orders at the
// same step. The server therefore knows nothing about the game: it keeps rooms,
// collects what each player ordered, and every `turnMs` broadcasts one numbered
// TURN holding all orders that arrived since the last one. A client executes
// turn n only once it has it, so all clients execute the same orders at the same
// simulation step -- the server's clock is the only clock that matters.
//
// The same class runs inside the game (net.serve -- a player hosts) and as the
// dedicated `fitzelserver` program; it is polled, never threaded.
//
// --- The protocol (text; one framed message each; tabs separate fields) ------
//   client -> server
//     HELLO name\tgame\tversion         introduce yourself (game = which game)
//     LIST                              rooms of the same game
//     CREATE max\tname\tconfig          open a room and become its host
//     JOIN name                         enter a room in the lobby
//     LEAVE                             leave the room
//     SET data                          your lobby entry (team, colour, ready...)
//     CONFIG text                       host only: the room's settings (map...)
//     START                             host only: begin the game
//     CMD payload                       an order, for the next turn
//     HASH turn\tvalue                  state checksum after `turn` (desync check)
//     CHAT text                         to everyone in the room
//     PING token                        answered with PONG token
//   server -> client
//     WELCOME id
//     ROOMS  [\nname\tplayers\tmax\tstarted\tconfig]...
//     ROOM   name\thostId\tmax\tstarted\tconfig[\nid\tname\tdata]...
//     START  seed\tturnMs\tyourSlot\tcount\tconfig[\nslot\tid\tname\tdata]...
//     TURN   n[\nslot\tpayload]...
//     LEFT   slot\tname                 a player dropped out of a running game
//     DESYNC turn                       the clients' states differ
//     CHAT   name\ttext
//     ERROR  text
// Names, data and payloads must not contain tabs or newlines; the server turns
// any it finds into spaces rather than let one player break everyone's parsing.
namespace netrelay {

struct Options {
    int turnMs = 100;   // lockstep turn length
};

class Server {
public:
    explicit Server(Options opt = {});
    ~Server();
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    bool listen(int port, std::string* err);
    // Accept, read, answer, pace the turns, write. `now` in seconds on any
    // monotonic clock.
    void tick(double now);
    void stop();

    bool running() const { return m_listen != netsock::kInvalid; }
    int  port() const { return m_port; }
    int  clientCount() const { return static_cast<int>(m_clients.size()); }
    int  roomCount() const { return static_cast<int>(m_rooms.size()); }

    // Where the server tells what it does (connects, rooms, desyncs). Optional.
    std::function<void(const std::string&)> log;

private:
    struct Client {
        int            id = 0;
        netsock::Conn  conn;
        std::string    name = "player";
        std::string    game;
        std::string    data;
        std::string    room;   // "" = in no room
        int            slot = -1;
        bool           gone = false;
    };
    struct Slot {
        int         client = 0;   // 0 = left the game
        std::string name, data;
    };
    struct Room {
        std::string name, game, config;
        int         host = 0;
        int         max = 2;
        bool        started = false;
        std::vector<int> members;               // client ids, join order
        std::vector<Slot> slots;                // fixed at START
        int         turn = 0;
        double      nextTurn = 0.0;
        std::vector<std::pair<int, std::string>> pending;   // slot, payload
        std::map<int, std::map<int, std::string>> hashes;   // turn -> slot -> value
    };

    Client* client(int id);
    void handle(Client& c, const std::string& msg, double now);
    void leaveRoom(Client& c);
    void sendRoom(const Room& r);
    void sendTo(int clientId, const std::string& msg);
    void broadcast(const Room& r, const std::string& msg);
    void say(const std::string& line) const;
    void checkHashes(Room& r);

    Options                       m_opt;
    netsock::Handle               m_listen = netsock::kInvalid;
    int                           m_port = 0;
    int                           m_nextId = 1;
    std::map<int, Client>         m_clients;
    std::map<std::string, Room>   m_rooms;
};

// Tabs and newlines out of a field: the protocol's separators stay separators.
std::string clean(const std::string& s);

} // namespace netrelay
