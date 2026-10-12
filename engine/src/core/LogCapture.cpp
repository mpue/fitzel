#include "fitzel/core/LogCapture.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#  include <fcntl.h>
#  include <io.h>
#elif !defined(__EMSCRIPTEN__)
#  include <cerrno>
#  include <unistd.h>
#endif

namespace fitzel::logcap {
namespace {

#if !defined(__EMSCRIPTEN__)
#  if defined(_WIN32)
int  sysPipe(int fds[2])                      { return _pipe(fds, 65536, _O_BINARY); }
int  sysDup(int fd)                           { return _dup(fd); }
int  sysDup2(int from, int to)                { return _dup2(from, to); }
int  sysClose(int fd)                         { return _close(fd); }
long sysRead(int fd, char* buf, unsigned n)   { return _read(fd, buf, n); }
long sysWrite(int fd, const char* buf, unsigned n) { return _write(fd, buf, n); }
#  else
int  sysPipe(int fds[2])                      { return ::pipe(fds); }
int  sysDup(int fd)                           { return ::dup(fd); }
int  sysDup2(int from, int to)                { return ::dup2(from, to); }
int  sysClose(int fd)                         { return ::close(fd); }
long sysRead(int fd, char* buf, unsigned n) {
    ssize_t r;
    do r = ::read(fd, buf, n); while (r < 0 && errno == EINTR);
    return static_cast<long>(r);
}
long sysWrite(int fd, const char* buf, unsigned n) {
    ssize_t r;
    do r = ::write(fd, buf, n); while (r < 0 && errno == EINTR);
    return static_cast<long>(r);
}
#  endif
#endif

// One redirected descriptor.
struct Tap {
    int  target   = -1;   // 1 or 2
    int  saved    = -1;   // the original, which the terminal still hangs off
    int  readEnd  = -1;
    bool running  = false;
    std::string partial;  // bytes after the last newline, owned by the reader
};

// Never destroyed: the reader threads are detached and may still be draining
// while static destructors run at exit.
struct State {
    std::mutex              mtx;
    std::condition_variable drained;
    std::deque<Line>        ring;
    std::size_t             capacity = 10000;
    std::uint64_t           nextSeq  = 1;
    bool                    active   = false;
    Tap                     taps[2];
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
};

State& state() {
    static State* s = new State();
    return *s;
}

// Terminal colour and cursor codes (ESC [ ... final-byte) are for the terminal;
// in the window they would only be noise.
std::string stripAnsi(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c == '\x1b' && i + 1 < in.size() && in[i + 1] == '[') {
            i += 2;
            while (i < in.size() && !(in[i] >= 0x40 && in[i] <= 0x7e)) ++i;
            continue;
        }
        if (c == '\r') continue;
        if (c == '\t') { out += "    "; continue; }
        out += c;
    }
    return out;
}

void pushLine(State& s, Stream stream, const std::string& raw) {
    Line l;
    l.text   = stripAnsi(raw);
    l.stream = stream;
    l.time   = std::chrono::duration<double>(std::chrono::steady_clock::now() - s.t0).count();
    std::lock_guard<std::mutex> lk(s.mtx);
    l.seq = s.nextSeq++;
    s.ring.push_back(std::move(l));
    while (s.ring.size() > s.capacity) s.ring.pop_front();
}

#if !defined(__EMSCRIPTEN__)
void readerLoop(Tap* tap, Stream stream) {
    State& s = state();
    char   buf[4096];
    for (;;) {
        const long n = sysRead(tap->readEnd, buf, sizeof(buf));
        if (n <= 0) break;   // EOF: every write end is closed (stop())
        // Tee first: the terminal must not wait on the window's bookkeeping.
        for (long off = 0; off < n;) {
            const long w = sysWrite(tap->saved, buf + off, static_cast<unsigned>(n - off));
            if (w <= 0) break;
            off += w;
        }
        for (long i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                pushLine(s, stream, tap->partial);
                tap->partial.clear();
            } else {
                tap->partial += buf[i];
            }
        }
    }
    if (!tap->partial.empty()) pushLine(s, stream, tap->partial);
    tap->partial.clear();
    sysClose(tap->readEnd);
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        tap->readEnd = -1;
        tap->running = false;
    }
    s.drained.notify_all();
}

bool startTap(Tap& tap, FILE* file, Stream stream) {
    const int fd = fileno(file);
    if (fd < 0) return false;   // a GUI process with no console has no fd to take over
    int fds[2];
    if (sysPipe(fds) != 0) return false;
    tap.target = fd;
    tap.saved  = sysDup(fd);
    if (tap.saved < 0) {
        sysClose(fds[0]);
        sysClose(fds[1]);
        return false;
    }
    std::fflush(file);
    if (sysDup2(fds[1], fd) < 0) {
        sysClose(fds[0]);
        sysClose(fds[1]);
        sysClose(tap.saved);
        tap.saved = -1;
        return false;
    }
    // The pipe's write end now lives on as fd 1/2 only, so restoring the
    // descriptor in stop() is what closes it and ends the reader.
    sysClose(fds[1]);
    tap.readEnd = fds[0];
    tap.running = true;
    std::thread(readerLoop, &tap, stream).detach();
    return true;
}
#endif

} // namespace

bool start(std::size_t capacity) {
#if defined(__EMSCRIPTEN__)
    (void)capacity;
    return false;
#else
    State& s = state();
    if (s.active) return true;
    s.capacity = capacity ? capacity : 1;
    s.t0       = std::chrono::steady_clock::now();
    // Into a pipe stdout would be fully buffered; stderr stays unbuffered.
    std::setvbuf(stdout, nullptr, _IOLBF, 4096);
    const bool out = startTap(s.taps[0], stdout, Stream::Out);
    const bool err = startTap(s.taps[1], stderr, Stream::Err);
    s.active = out || err;
    return s.active;
#endif
}

void stop() {
#if !defined(__EMSCRIPTEN__)
    State& s = state();
    if (!s.active) return;
    std::fflush(stdout);
    std::fflush(stderr);
    for (Tap& t : s.taps) {
        if (t.saved < 0) continue;
        sysDup2(t.saved, t.target);   // closes the pipe's write end
    }
    // Give the readers a moment to pass the tail on to the terminal. A child
    // process that inherited the pipe keeps it open; then we do not wait for it.
    {
        std::unique_lock<std::mutex> lk(s.mtx);
        s.drained.wait_for(lk, std::chrono::milliseconds(250), [&] {
            return !s.taps[0].running && !s.taps[1].running;
        });
    }
    s.active = false;
#endif
}

bool active() { return state().active; }

std::uint64_t linesSince(std::uint64_t afterSeq, std::vector<Line>& out) {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (!s.ring.empty() && s.ring.back().seq > afterSeq) {
        // Seqs in the ring are consecutive, so the first new one is an offset.
        const std::uint64_t front = s.ring.front().seq;
        std::size_t i = afterSeq >= front ? static_cast<std::size_t>(afterSeq - front + 1) : 0;
        for (; i < s.ring.size(); ++i) out.push_back(s.ring[i]);
    }
    return s.nextSeq - 1;
}

void clear() {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.ring.clear();
}

} // namespace fitzel::logcap
