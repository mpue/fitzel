#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

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

// The same, but on a layer whose transparency is locked: the colour moves
// towards (r, g, b) by a, the alpha stays what it was.
inline void paint(std::uint8_t* d, float r, float g, float b, float a, bool lockAlpha) {
    if (!lockAlpha) { over(d, r, g, b, a); return; }
    if (a <= 0.0f) return;
    a = std::min(a, 1.0f);
    d[0] = to8(d[0] * k1_255 * (1.0f - a) + r * a);
    d[1] = to8(d[1] * k1_255 * (1.0f - a) + g * a);
    d[2] = to8(d[2] * k1_255 * (1.0f - a) + b * a);
}

// A layer mask's value at one of its RGBA pixels, 0..255: luminance times alpha,
// so erasing a mask hides as painting it black does.
inline int maskValue(const std::uint8_t* q) {
    return ((q[0] * 77 + q[1] * 150 + q[2] * 29 + 128) >> 8) * q[3] / 255;
}

// Gaussian blur of `ch` floats per pixel, edges clamped (ImageOps.cpp).
void gaussBlur(std::vector<float>& v, int w, int h, int ch, float sigma);

// Squared distance, in pixels, from every pixel to the nearest one where
// `feature` is true; 1e20 with none (ImageDoc.cpp).
std::vector<float> distance2(const std::vector<std::uint8_t>& feature, int w, int h);

} // namespace img::px
