// Formats and the conversions between a caller's pixels and an image's texels.
//
// OpenGL converts an upload's data to the texture's format in the driver
// (RGB float data uploaded into an RGBA16F texture, say); Vulkan
// copies bytes. So the conversion happens here, on the CPU, on the way into
// the staging buffer -- and the other way for read-backs.

#include "VkCommon.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace fitzel::rhi::vk {

const char* resultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return "VkResult";
    }
}

void fail(const char* what, VkResult r) {
    throw std::runtime_error(std::string("Vulkan: ") + what + " failed: " + resultName(r));
}

void report(const char* what, VkResult r) {
    std::fprintf(stderr, "[Fitzel] Vulkan: %s failed: %s (%d)\n", what, resultName(r),
                 static_cast<int>(r));
}

FormatInfo FormatTable::info(Format f) const {
    switch (f) {
        case Format::R8:      return {VK_FORMAT_R8_UNORM, f};
        case Format::RG8:     return {VK_FORMAT_R8G8_UNORM, f};
        // Three channels are kept in four: an R8G8B8 image is rarely
        // something a GPU can sample, never one it can render into.
        case Format::RGB8:    return {VK_FORMAT_R8G8B8A8_UNORM, Format::RGBA8};
        case Format::RGBA8:   return {VK_FORMAT_R8G8B8A8_UNORM, f};
        case Format::R16F:    return {VK_FORMAT_R16_SFLOAT, f};
        case Format::RG16F:   return {VK_FORMAT_R16G16_SFLOAT, f};
        case Format::RGB16F:  return {VK_FORMAT_R16G16B16A16_SFLOAT, Format::RGBA16F};
        case Format::RGBA16F: return {VK_FORMAT_R16G16B16A16_SFLOAT, f};
        case Format::R32F:    return {VK_FORMAT_R32_SFLOAT, f};
        case Format::RG32F:   return {VK_FORMAT_R32G32_SFLOAT, f};
        case Format::RGB32F:  return {VK_FORMAT_R32G32B32A32_SFLOAT, Format::RGBA32F};
        case Format::RGBA32F: return {VK_FORMAT_R32G32B32A32_SFLOAT, f};
        case Format::R32UI:   return {VK_FORMAT_R32_UINT, f};
        // Depth is float everywhere: one layout for every copy in and out, and
        // at least the precision OpenGL's fixed-point formats had.
        case Format::Depth16:
        case Format::Depth24:
        case Format::Depth32F:
            return {VK_FORMAT_D32_SFLOAT, Format::Depth32F, VK_IMAGE_ASPECT_DEPTH_BIT};
        case Format::Depth24Stencil8:
        case Format::Depth32FStencil8:
            return {hasD32S8 ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT,
                    Format::Depth32F,
                    VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
        case Format::Unknown: break;
    }
    return {};
}

bool isIntegerFormat(Format f) { return f == Format::R32UI; }

namespace {

float halfToFloat(std::uint16_t h) {
    const std::uint32_t sign = (h >> 15) & 1u, exp = (h >> 10) & 0x1Fu, man = h & 0x3FFu;
    float v;
    if (exp == 0)       v = std::ldexp(static_cast<float>(man), -24);
    else if (exp == 31) v = man ? NAN : INFINITY;
    else                v = std::ldexp(static_cast<float>(man | 0x400u), static_cast<int>(exp) - 25);
    return sign ? -v : v;
}

std::uint16_t floatToHalf(float f) {
    std::uint32_t x;
    std::memcpy(&x, &f, 4);
    const std::uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = static_cast<int>((x >> 23) & 0xFFu) - 127 + 15;
    std::uint32_t man = x & 0x7FFFFFu;
    if (((x >> 23) & 0xFFu) == 0xFFu)   // inf / nan
        return static_cast<std::uint16_t>(sign | 0x7C00u | (man ? 0x200u : 0u));
    if (exp >= 31) return static_cast<std::uint16_t>(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return static_cast<std::uint16_t>(sign);
        man |= 0x800000u;
        const int shift = 14 - exp;
        std::uint32_t h = man >> shift;
        if ((man >> (shift - 1)) & 1u) ++h;   // round half up
        return static_cast<std::uint16_t>(sign | h);
    }
    std::uint32_t h = (static_cast<std::uint32_t>(exp) << 10) | (man >> 13);
    if (man & 0x1000u) ++h;   // round half up (carry into the exponent is right)
    return static_cast<std::uint16_t>(sign | h);
}

struct Layout {
    int  channels = 0;
    int  bytes = 0;   // per channel
    enum Kind { UNorm, Float, Half, UInt, DepthU32, DepthU16 } kind = UNorm;
};

Layout layoutOf(Format f) {
    switch (f) {
        case Format::R8:      return {1, 1, Layout::UNorm};
        case Format::RG8:     return {2, 1, Layout::UNorm};
        case Format::RGB8:    return {3, 1, Layout::UNorm};
        case Format::RGBA8:   return {4, 1, Layout::UNorm};
        case Format::R16F:    return {1, 2, Layout::Half};
        case Format::RG16F:   return {2, 2, Layout::Half};
        case Format::RGB16F:  return {3, 2, Layout::Half};
        case Format::RGBA16F: return {4, 2, Layout::Half};
        case Format::R32F:    return {1, 4, Layout::Float};
        case Format::RG32F:   return {2, 4, Layout::Float};
        case Format::RGB32F:  return {3, 4, Layout::Float};
        case Format::RGBA32F: return {4, 4, Layout::Float};
        case Format::R32UI:   return {1, 4, Layout::UInt};
        case Format::Depth16: return {1, 2, Layout::DepthU16};
        case Format::Depth24: return {1, 4, Layout::DepthU32};
        case Format::Depth32F: return {1, 4, Layout::Float};
        default: break;
    }
    return {};
}

void decode(const Layout& l, const unsigned char* p, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    for (int c = 0; c < l.channels; ++c) {
        const unsigned char* q = p + c * l.bytes;
        switch (l.kind) {
            case Layout::UNorm: out[c] = q[0] / 255.0f; break;
            case Layout::Half: { std::uint16_t h; std::memcpy(&h, q, 2); out[c] = halfToFloat(h); break; }
            case Layout::Float: std::memcpy(&out[c], q, 4); break;
            case Layout::UInt: { std::uint32_t u; std::memcpy(&u, q, 4); out[c] = static_cast<float>(u); break; }
            case Layout::DepthU32: { std::uint32_t u; std::memcpy(&u, q, 4); out[c] = static_cast<float>(u / 4294967295.0); break; }
            case Layout::DepthU16: { std::uint16_t u; std::memcpy(&u, q, 2); out[c] = u / 65535.0f; break; }
        }
    }
}

void encode(const Layout& l, const float in[4], unsigned char* p) {
    for (int c = 0; c < l.channels; ++c) {
        unsigned char* q = p + c * l.bytes;
        const float v = in[c];
        switch (l.kind) {
            case Layout::UNorm:
                q[0] = static_cast<unsigned char>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                break;
            case Layout::Half: { const std::uint16_t h = floatToHalf(v); std::memcpy(q, &h, 2); break; }
            case Layout::Float: std::memcpy(q, &v, 4); break;
            case Layout::UInt: { const auto u = static_cast<std::uint32_t>(std::max(v, 0.0f)); std::memcpy(q, &u, 4); break; }
            case Layout::DepthU32: {
                const auto u = static_cast<std::uint32_t>(std::clamp(static_cast<double>(v), 0.0, 1.0) * 4294967295.0);
                std::memcpy(q, &u, 4);
                break;
            }
            case Layout::DepthU16: {
                const auto u = static_cast<std::uint16_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 65535.0f));
                std::memcpy(q, &u, 2);
                break;
            }
        }
    }
}

} // namespace

void convertPixels(Format from, const void* srcV, Format to, void* dstV, std::size_t count) {
    const auto* src = static_cast<const unsigned char*>(srcV);
    auto* dst = static_cast<unsigned char*>(dstV);
    if (from == to) {
        std::memcpy(dst, src, count * bytesPerPixel(from));
        return;
    }
    const Layout a = layoutOf(from), b = layoutOf(to);
    if (!a.channels || !b.channels) {
        std::memset(dst, 0, count * bytesPerPixel(to));
        return;
    }
    // The ones that move megabytes at load time, without the float detour.
    if (a.kind == Layout::UNorm && b.kind == Layout::UNorm && a.channels == 3 && b.channels == 4) {
        for (std::size_t i = 0; i < count; ++i) {
            dst[i * 4 + 0] = src[i * 3 + 0];
            dst[i * 4 + 1] = src[i * 3 + 1];
            dst[i * 4 + 2] = src[i * 3 + 2];
            dst[i * 4 + 3] = 255;
        }
        return;
    }
    if (a.kind == Layout::Float && b.kind == Layout::Float && a.channels == 3 && b.channels == 4) {
        const auto* s = reinterpret_cast<const float*>(src);
        auto* d = reinterpret_cast<float*>(dst);
        for (std::size_t i = 0; i < count; ++i) {
            d[i * 4 + 0] = s[i * 3 + 0];
            d[i * 4 + 1] = s[i * 3 + 1];
            d[i * 4 + 2] = s[i * 3 + 2];
            d[i * 4 + 3] = 1.0f;
        }
        return;
    }
    const std::size_t sa = static_cast<std::size_t>(a.channels) * a.bytes;
    const std::size_t sb = static_cast<std::size_t>(b.channels) * b.bytes;
    float px[4];
    for (std::size_t i = 0; i < count; ++i) {
        decode(a, src + i * sa, px);
        encode(b, px, dst + i * sb);
    }
}

} // namespace fitzel::rhi::vk
