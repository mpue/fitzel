#include "GpuTimer.hpp"

#include <cstdint>
#include <vector>

#include <glad/gl.h>

#include "Profiler.hpp"

namespace gputime {
namespace {

// One measured pass: the two timestamps, and the frame it was issued in.
struct Pending {
    const char*   name  = nullptr;
    std::uint32_t begin = 0;
    std::uint32_t end   = 0;
    int           frame = 0;
    bool          closed = false;   // the end timestamp has been issued
};

std::vector<Pending>       g_pending;
std::vector<std::uint32_t> g_pool;    // recycled query objects
int  g_frame     = 0;
bool g_available = false;
bool g_probed    = false;

// A query object, from the pool or fresh. 0 when the driver has none to give.
std::uint32_t take() {
    if (!g_pool.empty()) {
        const std::uint32_t q = g_pool.back();
        g_pool.pop_back();
        return q;
    }
    std::uint32_t q = 0;
    glGenQueries(1, &q);
    return q;
}

// Is there a live GL context that answers timestamp queries? Asked once: the
// answer cannot change within a run, and asking per zone would cost a driver
// round trip for something already known.
bool probe() {
    if (g_probed) return g_available;
    g_probed = true;
    // A context is required for any of this. glGetIntegerv on a bit count is the
    // cheapest question that tells us both that we have one and that the driver
    // implements timers (0 bits = it does not).
    GLint bits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
    // Clear whatever complaint a context-less call left behind, so the caller's
    // next glGetError() is about the caller.
    while (glGetError() != GL_NO_ERROR) {}
    g_available = bits > 0;
    return g_available;
}

} // namespace

Scope::Scope(const char* name) : m_slot(-1) {
    if (!probe()) return;
    const std::uint32_t q = take();
    if (!q) return;
    glQueryCounter(q, GL_TIMESTAMP);
    m_slot = static_cast<int>(g_pending.size());
    g_pending.push_back(Pending{name, q, 0, g_frame, false});
}

Scope::~Scope() {
    if (m_slot < 0 || m_slot >= static_cast<int>(g_pending.size())) return;
    Pending& p = g_pending[static_cast<std::size_t>(m_slot)];
    p.end = take();
    if (!p.end) { // out of query objects: drop the pair rather than half-report
        g_pool.push_back(p.begin);
        p.name = nullptr;
        return;
    }
    glQueryCounter(p.end, GL_TIMESTAMP);
    p.closed = true;
}

// Compacts the pending list, so it must never run while a zone is open: an open
// Scope holds an index into it. Every zone closes inside the frame that opened
// it, and this is called at the end of one, so that holds by construction.
void collect() {
    ++g_frame;
    if (g_pending.empty()) return;

    std::vector<Pending> keep;
    keep.reserve(g_pending.size());
    for (Pending& p : g_pending) {
        // A pair whose end never came (an early return between the two, or an
        // exhausted pool): give the objects back and forget it.
        if (!p.name || !p.closed) {
            if (p.begin) g_pool.push_back(p.begin);
            if (p.end)   g_pool.push_back(p.end);
            continue;
        }
        // Never in the frame that issued it -- see the header. Two frames of
        // slack is what keeps this from turning into a glFinish.
        if (g_frame - p.frame < 2) { keep.push_back(p); continue; }
        GLint done = 0;
        glGetQueryObjectiv(p.end, GL_QUERY_RESULT_AVAILABLE, &done);
        if (!done) { keep.push_back(p); continue; }
        GLuint64 t0 = 0, t1 = 0;
        glGetQueryObjectui64v(p.begin, GL_QUERY_RESULT, &t0);
        glGetQueryObjectui64v(p.end,   GL_QUERY_RESULT, &t1);
        if (t1 > t0)
            prof::add(p.name, static_cast<double>(t1 - t0) / 1.0e6); // ns -> ms
        g_pool.push_back(p.begin);
        g_pool.push_back(p.end);
    }
    g_pending.swap(keep);
}

bool available() { return g_available; }

void reset() {
    for (Pending& p : g_pending) {
        if (p.begin) g_pool.push_back(p.begin);
        if (p.end)   g_pool.push_back(p.end);
    }
    g_pending.clear();
}

} // namespace gputime
