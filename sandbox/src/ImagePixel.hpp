#pragma once

#include <algorithm>
#include <cstdint>

// The few per-pixel helpers ImageDoc.cpp and ImageOps.cpp share. Internal to the
// image editor's core; nothing outside it includes this.
namespace img::px {

constexpr float k1_255 = 1.0f / 255.0f;

inline std::uint8_t to8(float v) {
    return std::uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// Colour (r, g, b) at alpha a laid over the straight-alpha pixel d.
inline void over(std::uint8_t* d, float r, float g, float b, float a) {
    if (a <= 0.0f) return;
    a = std::min(a, 1.0f);
    const float da = d[3] * k1_255;
    const float oa = a + da * (1.0f - a);
    const float kd = da * (1.0f - a);
    d[0] = to8((r * a + d[0] * k1_255 * kd) / oa);
    d[1] = to8((g * a + d[1] * k1_255 * kd) / oa);
    d[2] = to8((b * a + d[2] * k1_255 * kd) / oa);
    d[3] = to8(oa);
}

} // namespace img::px
