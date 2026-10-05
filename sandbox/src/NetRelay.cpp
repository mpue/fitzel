#include "NetRelay.hpp"

#include <algorithm>
#include <random>

namespace netrelay {

std::string clean(const std::string& s) {
    std::string o = s;
    for (char& ch : o)
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
    return o;
}

namespace {

// "VERB rest" -> verb, rest.
void splitVerb(const std::string& msg, std::string& verb, std::string& rest) {
    const std::size_t sp = msg.find(' ');
    verb = msg.substr(0, sp);
    rest = (sp == std::string::npos) ? std::string() : msg.substr(sp + 1);
}

std::vector<std::string> fields(const std::string& s) {
    std::vector<std::string> out;
    std::size_t at = 0;
    for (;;) {
        const std::size_t tab = s.find('\t', at);
        out.push_back(s.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
        if (tab == std::string::npos) break;
        at = tab + 1;
    }
    return out;
}

int toInt(const std::string& s, int fallback) {
    try { return std::stoi(s); } catch (...) { return fallback; }
}

} // namespace

Server::Server(Options opt) : m_opt(opt) {}
Server::~Server() { stop(); }

bool Server::listen(int port, std::string* err) {
    stop();
    m_listen = netsock::listenOn(port, err);
    if (m_listen == netsock::kInvalid) return false;
    m_port = netsock::boundPort(m_listen);
    say("listening on port " + std::to_string(m_port));
    return true;
}

void Server::stop() {
    for (auto& [id, c] : m_clients) c.conn.close();
    m_clients.clear();
    m_rooms.clear();
    if (m_listen != netsock::kInvalid) {
        netsock::closeHandle(m_listen);
        say("stopped");
    }
    m_listen = netsock::kInvalid;
    m_port   = 0;
}

void Server::say(const std::string& line) const {
    if (log) log(line);
}

Server::Client* Server::client(int id) {
    auto it = m_clients.find(id);
    return it == m_clients.end() ? nullptr : &it->second;
}

void Server::sendTo(int clientId, const std::string& msg) {
    if (Client* c = client(clientId)) c->conn.queue(msg);
}

void Server::broadcast(const Room& r, const std::string& msg) {
    if (r.started) {
        for (const Slot& s : r.slots)
            if (s.client) sendTo(s.client, msg);
    } else {
        for (int id : r.members) sendTo(id, msg);
    }
}

void Server::sendRoom(const Room& r) {
    std::string m = "ROOM " + r.name + "\t" + std::to_string(r.host) + "\t" + std::to_string(r.max) +
                    "\t" + (r.started ? "1" : "0") + "\t" + r.config;
    for (int id : r.members)
        if (const Client* c = client(id))
            m += "\n" + std::to_string(c->id) + "\t" + c->name + "\t" + c->data;
    broadcast(r, m);
}

void Server::leaveRoom(Client& c) {
    if (c.room.empty()) return;
    auto it = m_rooms.find(c.room);
    c.room.clear();
    const int slot = c.slot;
    c.slot = -1;
    if (it == m_rooms.end()) return;
    Room& r = it->second;
    r.members.erase(std::remove(r.members.begin(), r.members.end(), c.id), r.members.end());
    if (r.started && slot >= 0 && slot < static_cast<int>(r.slots.size())) {
        r.slots[slot].client = 0;
        // Leaving is giving up -- as an order of theirs, inside a turn, so every
        // machine applies it at the same step.
        r.pending.emplace_back(slot, "gg");
        broadcast(r, "LEFT " + std::to_string(slot) + "\t" + c.name);
        say(c.name + " left the game in '" + r.name + "'");
    }
    if (r.members.empty()) {
        say("room '" + r.name + "' closed");
        m_rooms.erase(it);
        return;
    }
    if (r.host == c.id) r.host = r.members.front();
    if (!r.started) sendRoom(r);
}

void Server::checkHashes(Room& r) {
    for (auto it = r.hashes.begin(); it != r.hashes.end();) {
        int alive = 0;
        for (const Slot& s : r.slots)
            if (s.client) ++alive;
        if (static_cast<int>(it->second.size()) < alive) { ++it; continue; }
        const std::string& first = it->second.begin()->second;
        bool same = true;
        for (const auto& [slot, value] : it->second)
            if (value != first) same = false;
        if (!same) {
            say("DESYNC in '" + r.name + "' at turn " + std::to_string(it->first));
            broadcast(r, "DESYNC " + std::to_string(it->first));
        }
        it = r.hashes.erase(it);
    }
    // A player who never reports must not grow the table forever.
    while (r.hashes.size() > 64) r.hashes.erase(r.hashes.begin());
}

void Server::handle(Client& c, const std::string& msg, double now) {
    std::string verb, rest;
    splitVerb(msg, verb, rest);
    Room* room = nullptr;
    if (!c.room.empty()) {
        auto it = m_rooms.find(c.room);
        if (it != m_rooms.end()) room = &it->second;
    }

    if (verb == "HELLO") {
        const auto f = fields(rest);
        c.name = clean(f.size() > 0 && !f[0].empty() ? f[0] : "player");
        c.game = clean(f.size() > 1 ? f[1] : "");
        c.conn.queue("WELCOME " + std::to_string(c.id));
        say("hello from " + c.name + " (" + c.game + ")");
    } else if (verb == "LIST") {
        std::string m = "ROOMS";
        for (const auto& [name, r] : m_rooms)
            if (r.game == c.game)
                m += "\n" + r.name + "\t" + std::to_string(r.members.size()) + "\t" +
                     std::to_string(r.max) + "\t" + (r.started ? "1" : "0") + "\t" + r.config;
        c.conn.queue(m);
    } else if (verb == "CREATE") {
        const auto f = fields(rest);
        const std::string name = clean(f.size() > 1 ? f[1] : "");
        if (name.empty()) { c.conn.queue("ERROR a room needs a name"); return; }
        if (m_rooms.count(name)) { c.conn.queue("ERROR room '" + name + "' exists"); return; }
        leaveRoom(c);
        Room r;
        r.name   = name;
        r.game   = c.game;
        r.max    = std::clamp(toInt(f.empty() ? "" : f[0], 2), 1, 8);
        r.config = clean(f.size() > 2 ? f[2] : "");
        r.host   = c.id;
        r.members.push_back(c.id);
        c.room = name;
        say(c.name + " opened room '" + name + "'");
        sendRoom(m_rooms.emplace(name, std::move(r)).first->second);
    } else if (verb == "JOIN") {
        auto it = m_rooms.find(clean(rest));
        if (it == m_rooms.end() || it->second.game != c.game) {
            c.conn.queue("ERROR no room '" + rest + "'");
            return;
        }
        Room& r = it->second;
        if (r.started) { c.conn.queue("ERROR the game in '" + r.name + "' is running"); return; }
        if (static_cast<int>(r.members.size()) >= r.max) { c.conn.queue("ERROR room is full"); return; }
        if (c.room == r.name) return;
        leaveRoom(c);
        r.members.push_back(c.id);
        c.room = r.name;
        say(c.name + " joined '" + r.name + "'");
        sendRoom(r);
    } else if (verb == "LEAVE") {
        leaveRoom(c);
    } else if (verb == "SET") {
        c.data = clean(rest);
        if (room && !room->started) sendRoom(*room);
    } else if (verb == "CONFIG") {
        if (room && room->host == c.id && !room->started) {
            room->config = clean(rest);
            sendRoom(*room);
        }
    } else if (verb == "START") {
        if (!room || room->host != c.id || room->started) return;
        Room& r = *room;
        r.started  = true;
        r.turn     = 0;
        r.nextTurn = now + 0.25;   // a breath for everyone to load before turn 0
        r.pending.clear();
        r.slots.clear();
        for (int id : r.members)
            if (const Client* m = client(id)) r.slots.push_back({m->id, m->name, m->data});
        std::random_device rd;
        const unsigned seed = rd() & 0x7fffffffu;
        std::string tail;
        for (std::size_t i = 0; i < r.slots.size(); ++i) {
            Client* m = client(r.slots[i].client);
            if (m) m->slot = static_cast<int>(i);
            tail += "\n" + std::to_string(i) + "\t" + std::to_string(r.slots[i].client) + "\t" +
                    r.slots[i].name + "\t" + r.slots[i].data;
        }
        for (std::size_t i = 0; i < r.slots.size(); ++i)
            sendTo(r.slots[i].client, "START " + std::to_string(seed) + "\t" + std::to_string(m_opt.turnMs) +
                                          "\t" + std::to_string(i) + "\t" + std::to_string(r.slots.size()) +
                                          "\t" + r.config + tail);
        say("game started in '" + r.name + "' with " + std::to_string(r.slots.size()) + " players");
    } else if (verb == "CMD") {
        if (room && room->started && c.slot >= 0) room->pending.emplace_back(c.slot, clean(rest));
    } else if (verb == "HASH") {
        if (!room || !room->started || c.slot < 0) return;
        const auto f = fields(rest);
        if (f.size() < 2) return;
        room->hashes[toInt(f[0], -1)][c.slot] = f[1];
        checkHashes(*room);
    } else if (verb == "CHAT") {
        if (room) broadcast(*room, "CHAT " + c.name + "\t" + clean(rest));
    } else if (verb == "PING") {
        c.conn.queue("PONG " + rest);
    } else {
        c.conn.queue("ERROR unknown " + verb);
    }
}

void Server::tick(double now) {
    if (!running()) return;
    // New players.
    for (;;) {
        const netsock::Handle h = netsock::acceptOne(m_listen);
        if (h == netsock::kInvalid) break;
        Client c;
        c.id        = m_nextId++;
        c.conn.sock = h;
        c.conn.open = true;
        m_clients.emplace(c.id, std::move(c));
    }
    // What they said.
    std::vector<std::string> msgs;
    for (auto& [id, c] : m_clients) {
        msgs.clear();
        if (!c.conn.read(msgs)) c.gone = true;
        for (const std::string& m : msgs) handle(c, m, now);
    }
    // Who went away.
    for (auto it = m_clients.begin(); it != m_clients.end();) {
        if (!it->second.gone) { ++it; continue; }
        say(it->second.name + " disconnected");
        leaveRoom(it->second);
        it->second.conn.close();
        it = m_clients.erase(it);
    }
    // The clock: every running room gets its turns, empty or not -- an empty
    // turn is what lets the clients step at all.
    for (auto& [name, r] : m_rooms) {
        if (!r.started) continue;
        const double step = m_opt.turnMs / 1000.0;
        if (now - r.nextTurn > 2.0) r.nextTurn = now;   // a stall: do not burst
        while (now >= r.nextTurn) {
            std::string m = "TURN " + std::to_string(r.turn);
            for (const auto& [slot, payload] : r.pending)
                m += "\n" + std::to_string(slot) + "\t" + payload;
            r.pending.clear();
            broadcast(r, m);
            ++r.turn;
            r.nextTurn += step;
        }
    }
    for (auto& [id, c] : m_clients)
        if (!c.conn.flush()) c.gone = true;
}

} // namespace netrelay
