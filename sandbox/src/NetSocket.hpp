#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The smallest socket layer a lockstep game needs: TCP, non-blocking, polled
// from the caller's own loop -- no threads, so nothing here ever touches the
// script VM or the scene behind the frame's back.
//
// --- Framing ------------------------------------------------------------------
// TCP is a byte stream; a game wants messages. Every message on the wire is a
// four-byte little-endian length and then that many bytes. A peer that announces
// more than kMaxMessage is not a peer that speaks this protocol, and the
// connection is dropped rather than the buffer grown without end.
namespace netsock {

#ifdef _WIN32
using Handle = std::uintptr_t;   // SOCKET, without dragging <winsock2.h> in here
inline constexpr Handle kInvalid = ~static_cast<Handle>(0);
#else
using Handle = int;
inline constexpr Handle kInvalid = -1;
#endif

inline constexpr std::size_t kMaxMessage = 1u << 20;   // 1 MiB

// WSAStartup on Windows, once per process; harmless to call again.
bool init();

// One connected stream: what has arrived but is not a whole message yet, and
// what is queued but not yet taken by the kernel.
struct Conn {
    Handle      sock = kInvalid;
    std::string in;
    std::string out;
    bool        open = false;

    // Read what the kernel has, append whole messages to `msgs`. False when the
    // peer has gone (closed, reset, or broke the framing) -- close() it then.
    bool read(std::vector<std::string>& msgs);
    // Queue one message (framed).
    void queue(const std::string& msg);
    // Hand queued bytes to the kernel as far as it takes them. False on error.
    bool flush();
    void close();
};

// Listen on all interfaces at `port` (0 = any; see boundPort). Non-blocking.
Handle listenOn(int port, std::string* err);
int    boundPort(Handle s);
// Accept one pending connection, or kInvalid when none waits.
Handle acceptOne(Handle listener);
// Start a non-blocking connect; the connection is usable once connectDone
// reports 1. `host` is a name or a dotted address.
Handle connectTo(const std::string& host, int port, std::string* err);
// 1 = connected, 0 = still trying, -1 = failed (err filled).
int    connectDone(Handle s, std::string* err);
void   closeHandle(Handle s);

// This machine's IPv4 addresses (not loopback) -- what a host tells a friend
// to type in.
std::vector<std::string> localAddresses();

} // namespace netsock
