#include "DdsDecode.hpp"

#include <cmath>
#include <cstring>

namespace dds {

namespace {

std::uint32_t rd32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

enum class Kind { None, BC1, BC2, BC3, BC4, BC5, Masked, RGBA8, BGRA8, BGRX8 };

void rgb565(std::uint16_t c, std::uint8_t* out) {
    const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    out[0] = static_cast<std::uint8_t>((r << 3) | (r >> 2));
    out[1] = static_cast<std::uint8_t>((g << 2) | (g >> 4));
    out[2] = static_cast<std::uint8_t>((b << 3) | (b >> 2));
    out[3] = 255;
}

// A BC1 colour block into a 4x4 RGBA tile. `punchThrough`: BC1's three-colour
// mode with transparent black (BC2 and BC3 always interpolate four colours).
void colorBlock(const std::uint8_t* b, bool punchThrough, std::uint8_t tile[16][4]) {
    const std::uint16_t c0 = static_cast<std::uint16_t>(b[0] | (b[1] << 8));
    const std::uint16_t c1 = static_cast<std::uint16_t>(b[2] | (b[3] << 8));
    std::uint8_t pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    if (c0 > c1 || !punchThrough) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = static_cast<std::uint8_t>((2 * pal[0][i] + pal[1][i] + 1) / 3);
            pal[3][i] = static_cast<std::uint8_t>((pal[0][i] + 2 * pal[1][i] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; ++i) pal[2][i] = static_cast<std::uint8_t>((pal[0][i] + pal[1][i]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    const std::uint32_t idx = rd32(b + 4);
    for (int i = 0; i < 16; ++i) std::memcpy(tile[i], pal[(idx >> (2 * i)) & 3], 4);
}

// A BC3 alpha / BC4 block: two endpoints and sixteen three-bit indices.
void alphaBlock(const std::uint8_t* b, std::uint8_t out[16]) {
    const int a0 = b[0], a1 = b[1];
    int pal[8] = {a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; ++i) pal[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    } else {
        for (int i = 1; i < 5; ++i) pal[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        pal[6] = 0;
        pal[7] = 255;
    }
    std::uint64_t bits = 0;
    for (int i = 0; i < 6; ++i) bits |= static_cast<std::uint64_t>(b[2 + i]) << (8 * i);
    for (int i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(pal[(bits >> (3 * i)) & 7]);
}

// A channel by its mask: shifted down and stretched to eight bits.
std::uint8_t channel(std::uint32_t px, std::uint32_t mask, std::uint8_t absent) {
    if (!mask) return absent;
    int shift = 0;
    while (!((mask >> shift) & 1)) ++shift;
    const std::uint32_t m = mask >> shift;
    const std::uint32_t v = (px & mask) >> shift;
    return static_cast<std::uint8_t>((v * 255 + m / 2) / m);
}

} // namespace

bool decode(const std::uint8_t* data, std::size_t size, Image& out, std::string& error) {
    out = Image{};
    if (size < 128 || std::memcmp(data, "DDS ", 4) != 0) {
        error = "not a DDS file";
        return false;
    }
    const int h = static_cast<int>(rd32(data + 12));
    const int w = static_cast<int>(rd32(data + 16));
    const std::uint32_t pfFlags = rd32(data + 80);
    const std::uint32_t fourcc = rd32(data + 84);
    const std::uint32_t bits = rd32(data + 88);
    const std::uint32_t mR = rd32(data + 92), mG = rd32(data + 96), mB = rd32(data + 100),
                        mA = rd32(data + 104);
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        error = "bad size";
        return false;
    }
    std::size_t offset = 128;
    Kind kind = Kind::None;
    auto cc = [](const char* s) {
        return static_cast<std::uint32_t>(s[0]) | (static_cast<std::uint32_t>(s[1]) << 8) |
               (static_cast<std::uint32_t>(s[2]) << 16) | (static_cast<std::uint32_t>(s[3]) << 24);
    };
    if (pfFlags & 0x4) {   // DDPF_FOURCC
        if (fourcc == cc("DXT1")) kind = Kind::BC1;
        else if (fourcc == cc("DXT2") || fourcc == cc("DXT3")) kind = Kind::BC2;
        else if (fourcc == cc("DXT4") || fourcc == cc("DXT5")) kind = Kind::BC3;
        else if (fourcc == cc("ATI1") || fourcc == cc("BC4U")) kind = Kind::BC4;
        else if (fourcc == cc("ATI2") || fourcc == cc("BC5U")) kind = Kind::BC5;
        else if (fourcc == cc("DX10")) {
            if (size < 148) { error = "DX10 header cut short"; return false; }
            offset = 148;
            switch (rd32(data + 128)) {
                case 71: case 72: kind = Kind::BC1; break;
                case 74: case 75: kind = Kind::BC2; break;
                case 77: case 78: kind = Kind::BC3; break;
                case 80: case 81: kind = Kind::BC4; break;
                case 83: case 84: kind = Kind::BC5; break;
                case 28: case 29: kind = Kind::RGBA8; break;
                case 87: case 91: kind = Kind::BGRA8; break;
                case 88: case 93: kind = Kind::BGRX8; break;
                default:
                    error = "DXGI format " + std::to_string(rd32(data + 128)) + " not supported";
                    return false;
            }
        } else if (fourcc == 21) { kind = Kind::BGRA8;   // D3DFMT_A8R8G8B8
        } else if (fourcc == 22) { kind = Kind::BGRX8;   // D3DFMT_X8R8G8B8
        } else {
            char s[5] = {};
            std::memcpy(s, &fourcc, 4);
            error = std::string("FourCC '") + s + "' not supported";
            return false;
        }
    } else if (pfFlags & (0x40 | 0x20000 | 0x2)) {   // RGB, luminance, alpha-only
        if (bits != 8 && bits != 16 && bits != 24 && bits != 32) {
            error = std::to_string(bits) + "-bit pixels not supported";
            return false;
        }
        kind = Kind::Masked;
    } else {
        error = "unknown pixel format";
        return false;
    }

    out.width = w;
    out.height = h;
    out.rgba.assign(static_cast<std::size_t>(w) * h * 4, 255);
    const std::uint8_t* p = data + offset;
    const std::size_t avail = size - offset;

    if (kind == Kind::Masked || kind == Kind::RGBA8 || kind == Kind::BGRA8 || kind == Kind::BGRX8) {
        const int bpp = kind == Kind::Masked ? static_cast<int>(bits / 8) : 4;
        if (avail < static_cast<std::size_t>(w) * h * bpp) { error = "pixels cut short"; return false; }
        const bool lum = (pfFlags & 0x20000) != 0;
        const bool alphaOnly = (pfFlags & 0x2) && !(pfFlags & 0x40) && !lum;
        for (int i = 0; i < w * h; ++i) {
            const std::uint8_t* s = p + static_cast<std::size_t>(i) * bpp;
            std::uint8_t* d = &out.rgba[static_cast<std::size_t>(i) * 4];
            if (kind == Kind::RGBA8) { std::memcpy(d, s, 4); continue; }
            if (kind == Kind::BGRA8 || kind == Kind::BGRX8) {
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0];
                d[3] = kind == Kind::BGRA8 ? s[3] : 255;
                continue;
            }
            std::uint32_t px = 0;
            std::memcpy(&px, s, bpp);
            if (alphaOnly) {
                d[0] = d[1] = d[2] = 255;
                d[3] = channel(px, mA ? mA : 0xff, 255);
            } else if (lum) {
                d[0] = d[1] = d[2] = channel(px, mR ? mR : 0xff, 0);
                d[3] = (pfFlags & 0x1) ? channel(px, mA, 255) : 255;
            } else {
                d[0] = channel(px, mR, 0);
                d[1] = channel(px, mG, 0);
                d[2] = channel(px, mB, 0);
                d[3] = (pfFlags & 0x1) ? channel(px, mA, 255) : 255;
            }
        }
        return true;
    }

    const int bw = (w + 3) / 4, bh = (h + 3) / 4;
    const std::size_t blockBytes = (kind == Kind::BC1 || kind == Kind::BC4) ? 8 : 16;
    if (avail < blockBytes * bw * bh) { error = "blocks cut short"; return false; }
    std::uint8_t tile[16][4];
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const std::uint8_t* b = p + blockBytes * (static_cast<std::size_t>(by) * bw + bx);
            switch (kind) {
                case Kind::BC1: colorBlock(b, true, tile); break;
                case Kind::BC2:
                    colorBlock(b + 8, false, tile);
                    for (int i = 0; i < 16; ++i) {
                        const int a = (b[i / 2] >> (4 * (i & 1))) & 15;
                        tile[i][3] = static_cast<std::uint8_t>(a * 17);
                    }
                    break;
                case Kind::BC3: {
                    colorBlock(b + 8, false, tile);
                    std::uint8_t a[16];
                    alphaBlock(b, a);
                    for (int i = 0; i < 16; ++i) tile[i][3] = a[i];
                    break;
                }
                case Kind::BC4: {
                    std::uint8_t v[16];
                    alphaBlock(b, v);
                    for (int i = 0; i < 16; ++i) {
                        tile[i][0] = tile[i][1] = tile[i][2] = v[i];
                        tile[i][3] = 255;
                    }
                    break;
                }
                case Kind::BC5: {
                    std::uint8_t x[16], y[16];
                    alphaBlock(b, x);
                    alphaBlock(b + 8, y);
                    for (int i = 0; i < 16; ++i) {
                        const float nx = x[i] / 127.5f - 1.0f, ny = y[i] / 127.5f - 1.0f;
                        const float nz = std::sqrt(std::fmax(0.0f, 1.0f - nx * nx - ny * ny));
                        tile[i][0] = x[i];
                        tile[i][1] = y[i];
                        tile[i][2] = static_cast<std::uint8_t>(std::lround((nz + 1.0f) * 127.5f));
                        tile[i][3] = 255;
                    }
                    break;
                }
                default: break;
            }
            for (int ty = 0; ty < 4; ++ty) {
                const int y = by * 4 + ty;
                if (y >= h) break;
                for (int tx = 0; tx < 4; ++tx) {
                    const int x = bx * 4 + tx;
                    if (x >= w) break;
                    std::memcpy(&out.rgba[(static_cast<std::size_t>(y) * w + x) * 4], tile[ty * 4 + tx], 4);
                }
            }
        }
    return true;
}

} // namespace dds
