#pragma once

// What the GPU spends the frame on, pass by pass.
//
// The profiler next door (Profiler.hpp) times the CPU, and for a renderer that
// is half an answer: every draw call returns long before the work happens, so a
// frame that is entirely GPU-bound shows up there as "present (swap)" and
// nothing else. Which pass filled those milliseconds -- the shadow cascades, the
// grass, the trees, the fog -- is exactly what that one number cannot say, and
// it is the only question worth asking when a scene runs at 15 fps.
//
// So: a pair of GL timestamp queries around each pass. Timestamps rather than
// GL_TIME_ELAPSED because only one elapsed query may be active at a time, which
// would forbid nesting a pass inside another -- and the passes here do nest.
//
// The results are read back two frames late, never in the frame that issued
// them: asking for a result while the GPU is still working on it blocks the CPU
// until it finishes, which would fabricate exactly the stall this is meant to
// find. They are then reported into prof:: under the same name, so the
// Performance window shows them beside the CPU zones with no extra plumbing. A
// number therefore describes a frame or two ago -- fine for a steady-state cost,
// and worth knowing before reading a single spike off it.
//
// Costs nothing measurable when nobody looks: a zone is two query objects out of
// a pool and two driver calls.
namespace gputime {

// Time the GPU work issued in this scope, reported to prof:: as `name`.
// `name` must outlive the program (a string literal), like prof::Scope.
//
// Safe before the GL context exists and safe on a driver without timer queries:
// it simply records nothing.
struct Scope {
    explicit Scope(const char* name);
    ~Scope();
    Scope(const Scope&)            = delete;
    Scope& operator=(const Scope&) = delete;

private:
    int m_slot;   // index into the pending list, -1 when not measuring
};

#define FZ_GPU_CAT2(a, b) a##b
#define FZ_GPU_CAT(a, b)  FZ_GPU_CAT2(a, b)
// Time the enclosing scope's GPU work under `name`.
#define FZ_GPU_ZONE(name) ::gputime::Scope FZ_GPU_CAT(fzGpuZone_, __LINE__)(name)

// Drain whatever finished into prof::. Call once per frame, anywhere -- the
// natural place is right after the swap, where the oldest queries are done.
void collect();

// Is the driver answering at all? False on a context without timer queries (and
// before the first collect()), which is worth showing rather than displaying a
// column of zeros as if the passes were free.
bool available();

// Drop everything in flight (a context teardown, a long stall). Not needed in
// normal use.
void reset();

} // namespace gputime
