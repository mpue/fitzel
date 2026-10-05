#pragma once

struct lua_State;

// The Lua `net` table: a client connection to a lockstep relay (NetRelay.hpp)
// and, for the player who hosts, the relay itself running inside the game.
//
// Everything is polled: net.poll() progresses the connect, ticks a hosted relay,
// reads what arrived and writes what is queued. A script that calls it once per
// frame is the whole network loop -- nothing runs behind the VM's back, and
// stopping Play (reset) closes every socket the game opened.
namespace scriptnet {

// Register the global `net` table in a fresh VM.
void install(lua_State* L);
// Close the connection and stop a hosted relay (Play stopped, or a new VM).
void reset();

} // namespace scriptnet
