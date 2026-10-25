#pragma once

// What every file of the Vulkan backend shares: the loader (volk -- nothing is
// linked against libvulkan, it is opened at run time), VMA, the error check,
// and the formats: which VkFormat stores an rhi::Format, and the conversions
// between the data a caller hands over and what the image really holds.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>

#include <volk.h>
#include <vk_mem_alloc.h>

#include "fitzel/rhi/Types.hpp"

namespace fitzel::rhi::vk {

// A failing call during set-up is fatal (the window falls back to OpenGL);
// later ones are logged once per call site and the frame goes on.
[[noreturn]] void fail(const char* what, VkResult r);
void report(const char* what, VkResult r);
const char* resultName(VkResult r);

#define FZ_VK_INIT(call)                                                    \
    do {                                                                     \
        const VkResult fz_r = (call);                                        \
        if (fz_r != VK_SUCCESS) ::fitzel::rhi::vk::fail(#call, fz_r);        \
    } while (0)
#define FZ_VK(call)                                                         \
    do {                                                                     \
        const VkResult fz_r = (call);                                        \
        if (fz_r != VK_SUCCESS) ::fitzel::rhi::vk::report(#call, fz_r);      \
    } while (0)

// --- Formats ----------------------------------------------------------------------

struct FormatInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    // The rhi format whose layout the image's texels have. Usually the format
    // itself; RGB8/RGB16F/RGB32F are stored with a fourth channel (three-channel
    // images are rarely sampleable), and Depth24 may become Depth32F.
    Format   stored = Format::Unknown;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

// Filled in by the device once it knows which depth formats the GPU has.
struct FormatTable {
    bool hasD32S8 = true;   // else D24S8 (whose depth does not read back as float)
    FormatInfo info(Format f) const;
};

bool isIntegerFormat(Format f);

// Convert `count` texels from `src` (laid out as `from`) to `dst` (as `to`).
// Any colour format to any colour format; depth to depth. Unknown pairs leave
// `dst` zeroed.
void convertPixels(Format from, const void* src, Format to, void* dst, std::size_t count);

} // namespace fitzel::rhi::vk
