#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Everything the process writes to stdout and stderr, kept for a log window.
//
// The engine, the scripts, Blender's bridge and every library under them talk
// through printf, std::cout and fprintf(stderr) -- there is no logger to hook,
// and there should not have to be one. So the capture sits under all of them, at
// the file descriptors: fd 1 and fd 2 are pointed into a pipe each, a reader
// thread per pipe splits what arrives into lines, and every byte is also written
// on to the ORIGINAL descriptor. The terminal sees exactly what it saw before
// (colour codes and all); the window gets a copy, the codes stripped.
//
// The copy is a ring: the oldest lines fall off once `capacity` is reached, so a
// script that prints every frame costs a fixed amount of memory, not an hour's.
//
// Not on the web build (there are no descriptors to redirect): start() returns
// false there and nothing else changes.
namespace fitzel::logcap {

enum class Stream : std::uint8_t { Out, Err };

struct Line {
    std::string   text;     // one line, without its newline or escape codes
    Stream        stream;
    double        time;     // seconds since start()
    std::uint64_t seq;      // 1, 2, 3 ... across the whole run; never reused
};

// Redirect fd 1 and fd 2. Call once, early -- after anything that reopens
// stdout/stderr (a console attach), before anything worth reading is printed.
// Also makes stdout line-buffered: into a pipe it would otherwise sit in a 4 KB
// buffer and arrive in the window (and the terminal) only when that fills.
// True when at least one stream is captured.
bool start(std::size_t capacity = 10000);

// Put the descriptors back and let the readers drain what is left into the
// terminal. Safe to call when start() failed or never ran.
void stop();

bool active();

// Append to `out` every line newer than `afterSeq` that is still in the ring,
// and return the newest seq handed out (pass it back next time). A reader that
// fell further behind than the ring is long simply misses the lines in between.
std::uint64_t linesSince(std::uint64_t afterSeq, std::vector<Line>& out);

// Drop every line kept so far. Seqs keep counting.
void clear();

} // namespace fitzel::logcap
