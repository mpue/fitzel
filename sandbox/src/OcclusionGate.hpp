#pragma once

#include <array>
#include <cstdint>

#include <fitzel/rhi/Device.hpp>

// Is a surface actually on screen? Asked of the GPU with an occlusion query
// around the surface's own draw, read back a frame or two later without ever
// waiting for it.
//
// For work that only a visible surface needs: the lake's reflection and
// refraction are two more renders of the whole scene, and "the water plane
// crosses the view" is true of every view that shows a horizon -- from a street
// in a valley the lake is behind three ridges and the passes were paid for a
// surface that drew no pixel. The surface itself is still drawn every frame the
// plane crosses the view (it is one cheap quad, and its query is what notices
// the lake coming back into sight); only the work that feeds it waits for a
// yes. The price is a frame or two of the previous reflection when the lake
// first appears.
class OcclusionGate {
public:
    OcclusionGate() = default;
    OcclusionGate(const OcclusionGate&) = delete;
    OcclusionGate& operator=(const OcclusionGate&) = delete;
    ~OcclusionGate() {
        for (Slot& s : m_ring)
            if (s.query) fitzel::rhi::device().destroy(s.query);
    }

    // Once a frame, before asking seen(): collects whatever answers are in.
    void poll() {
        ++m_frame;
        fitzel::rhi::Device& gpu = fitzel::rhi::device();
        for (Slot& s : m_ring) {
            if (!s.pending) continue;
            if (!gpu.queryReady(s.query)) continue;
            const std::uint64_t any = gpu.queryResult(s.query);
            if (any && s.frame > m_lastSeen) m_lastSeen = s.frame;
            s.pending = false;
        }
    }

    // Did the surface draw a pixel within the last `frames` answered frames?
    // True from the start, so the first frames do the work until told otherwise.
    bool seen(int frames = 3) const { return m_frame - m_lastSeen <= frames; }

    // Around the surface's draw.
    void begin() {
        m_active = nullptr;
        for (Slot& s : m_ring)
            if (!s.pending) { m_active = &s; break; }
        if (!m_active) { m_lastSeen = m_frame; return; }   // all in flight: assume seen
        fitzel::rhi::Device& gpu = fitzel::rhi::device();
        if (!m_active->query)
            m_active->query = gpu.createQuery(fitzel::rhi::QueryType::AnySamplesPassed);
        gpu.beginQuery(m_active->query);
    }
    void end() {
        if (!m_active) return;
        fitzel::rhi::device().endQuery(m_active->query);
        m_active->pending = true;
        m_active->frame   = m_frame;
        m_active = nullptr;
    }

private:
    struct Slot {
        fitzel::rhi::QueryHandle query;
        bool pending = false;
        std::int64_t frame = 0;
    };
    std::array<Slot, 4> m_ring{};
    Slot*        m_active   = nullptr;
    std::int64_t m_frame    = 0;
    std::int64_t m_lastSeen = 0;
};
