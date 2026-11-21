#pragma once

#include <map>
#include <string>
#include <vector>

// The plumbing between the editor and fitzelgeo, the local geometry cache: a
// plain HTTP/1.1 GET over a socket, the server's side of one, and starting a
// program. Blocking on purpose -- both callers run it on worker threads of their
// own -- and small on purpose: the editor talks to exactly one server on its own
// machine (or, later, on the LAN), never to the internet. The server reaches the
// internet through curl, run as a child process, which is how this project uses
// every program it does not link (ffmpeg, Blender, dotnet): no TLS library in the
// build, and curl is on every Linux and on Windows since 10.
namespace geonet {

bool init();   // sockets (WSAStartup on Windows); idempotent

struct Response {
    int status = 0;                          // 0 = no answer at all
    std::string body;
    std::map<std::string, std::string> headers;   // lower-case names
};

// GET http://host:port/path. False when nothing answered (connect refused,
// timed out, cut off); `r.status` then 0 and `err` says why.
bool get(const std::string& host, int port, const std::string& path, int timeoutSec,
         Response& r, std::string* err = nullptr);

// --- Serving ---------------------------------------------------------------------

using Socket = long long;   // SOCKET or fd; -1 = none
Socket listenOn(const std::string& bindAddr, int port, std::string* err);
Socket acceptOne(Socket listener);   // blocks; -1 on error
void   closeSocket(Socket s);

struct Request {
    std::string method, path;                  // path without the query
    std::map<std::string, std::string> query;  // decoded
};
// Read one request's head (a GET carries nothing else). False on garbage or a
// peer that went away.
bool readRequest(Socket s, Request& req, int timeoutSec = 15);
void respond(Socket s, int status, const std::string& contentType, const std::string& body,
             const std::map<std::string, std::string>& extra = {});

// --- Programs -----------------------------------------------------------------------

// Run argv[0] (looked up on the PATH) and wait for it, at most `timeoutSec`
// (then it is killed). Its stdout and stderr go to `log` (a file path) or
// nowhere. The exit code, or -1 if it would not start / was killed.
int run(const std::vector<std::string>& argv, int timeoutSec, const std::string& log = {});
// Start argv[0] and do not wait: it lives on after the caller (a server).
bool spawn(const std::vector<std::string>& argv, const std::string& log = {});

} // namespace geonet
