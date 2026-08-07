#include "Profiler.hpp"

#include <algorithm>
#include <chrono>

namespace prof {
namespace {

long long nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Zone {
    const char* name  = nullptr;
    double      accum = 0.0;   // this frame, summed over repeated opens
    double      last  = 0.0;
    double      avg   = 0.0;
    double      worst = 0.0;   // over the current window
};

std::vector<Zone>     g_zones;
std::vector<ZoneStat> g_stats;      // rebuilt on demand for callers
std::vector<float>    g_history;
int                   g_head       = 0;  // next write position once filled
int                   g_windowTick = 0;  // frames since the last worst-reset

Zone& zoneFor(const char* name) {
    for (Zone& z : g_zones)
        if (z.name == name) return z;   // literals: pointer identity is enough
    g_zones.push_back(Zone{name, 0.0, 0.0, 0.0, 0.0});
    return g_zones.back();
}

} // namespace

Scope::Scope(const char* name) : m_name(name), m_start(nowNs()) {}

Scope::~Scope() {
    add(m_name, static_cast<double>(nowNs() - m_start) / 1e6);
}

void add(const char* name, double ms) { zoneFor(name).accum += ms; }

long long mark() { return nowNs(); }

void addSince(const char* name, long long from) {
    add(name, static_cast<double>(nowNs() - from) / 1e6);
}

Frame::Frame() : m_start(nowNs()) {}

Frame::~Frame() {
    endFrame(static_cast<double>(nowNs() - m_start) / 1e6);
}

void endFrame(double frameMs) {
    // The worst-in-window figures reset once per full history sweep. Without
    // that a single load spike would sit in the display forever and every later
    // reading would be measured against it.
    const bool rollWindow = (++g_windowTick >= kHistory);
    if (rollWindow) g_windowTick = 0;

    for (Zone& z : g_zones) {
        z.last = z.accum;
        // Exponential average: no per-zone ring buffer, and it settles fast
        // enough to follow a change while still reading as a stable number.
        z.avg   = (z.avg == 0.0) ? z.accum : z.avg * 0.95 + z.accum * 0.05;
        z.worst = rollWindow ? z.accum : std::max(z.worst, z.accum);
        z.accum = 0.0;
    }

    const float ms = static_cast<float>(frameMs);
    if (static_cast<int>(g_history.size()) < kHistory) {
        g_history.push_back(ms);
    } else {
        g_history[static_cast<std::size_t>(g_head)] = ms;
        g_head = (g_head + 1) % kHistory;
    }
}

const std::vector<ZoneStat>& zones() {
    g_stats.clear();
    g_stats.reserve(g_zones.size());
    for (const Zone& z : g_zones)
        g_stats.push_back(ZoneStat{z.name, z.last, z.avg, z.worst});
    return g_stats;
}

const std::vector<float>& history() {
    // Callers plot this left-to-right as oldest-to-newest, so a filled ring has
    // to be rotated back into chronological order first.
    static std::vector<float> ordered;
    ordered.clear();
    if (static_cast<int>(g_history.size()) < kHistory) {
        ordered = g_history;
    } else {
        ordered.reserve(kHistory);
        ordered.insert(ordered.end(), g_history.begin() + g_head, g_history.end());
        ordered.insert(ordered.end(), g_history.begin(), g_history.begin() + g_head);
    }
    return ordered;
}

FrameStats frameStats() {
    FrameStats s;
    const std::vector<float>& h = history();
    if (h.empty()) return s;

    s.last = h.back();
    double sum = 0.0;
    for (float v : h) { sum += v; s.worst = std::max(s.worst, v); }
    s.avg = static_cast<float>(sum / h.size());

    std::vector<float> sorted = h;
    std::sort(sorted.begin(), sorted.end());
    // 99th percentile frame *time* -> the fps the worst 1% of frames feel like.
    const std::size_t p99 =
        std::min(sorted.size() - 1,
                 static_cast<std::size_t>(sorted.size() * 99 / 100));
    s.low1 = sorted[p99] > 0.0f ? 1000.0f / sorted[p99] : 0.0f;

    // A "spike" is measured against the median, not the mean: the mean is itself
    // dragged up by the spikes we're trying to count.
    const float median = sorted[sorted.size() / 2];
    for (float v : h)
        if (v > median * 2.0f) ++s.spikes;
    return s;
}

void reset() {
    g_history.clear();
    g_head       = 0;
    g_windowTick = 0;
    for (Zone& z : g_zones) { z.accum = z.last = z.avg = z.worst = 0.0; }
}

} // namespace prof
