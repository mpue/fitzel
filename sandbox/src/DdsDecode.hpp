#pragma once

// DirectDraw Surface files to plain RGBA8: the top mip of the first face, which
// is all a glTF export wants. Block-compressed BC1 (DXT1), BC2 (DXT2/3), BC3
// (DXT4/5), BC4 (ATI1) and BC5 (ATI2, a two-channel normal map) -- legacy
// FourCC or DX10 header -- and uncompressed 16/24/32-bit RGB(A) and luminance
// by their channel masks. BC6H/BC7 are refused with a reason (no X-Ray level
// uses them; the Enhanced Editions keep them for a few skyboxes).

#include <cstdint>
#include <string>
#include <vector>

namespace dds {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;   // width * height * 4, top row first
};

bool decode(const std::uint8_t* data, std::size_t size, Image& out, std::string& error);

} // namespace dds
