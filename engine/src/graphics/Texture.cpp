#include "fitzel/graphics/Texture.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include <glad/gl.h>
#include <stb_image.h>
#include <tinyexr.h>

namespace fitzel {

namespace {

GLenum formatForChannels(int channels) {
    switch (channels) {
        case 1:  return GL_RED;
        case 2:  return GL_RG;
        case 3:  return GL_RGB;
        case 4:  return GL_RGBA;
        default: return GL_RGB;
    }
}

bool endsWithExr(const std::string& p) {
    if (p.size() < 4) return false;
    std::string ext = p.substr(p.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(c));
    return ext == ".exr";
}

} // namespace

Texture::~Texture() {
    if (m_id) {
        glDeleteTextures(1, &m_id);
    }
}

Texture::Texture(Texture&& other) noexcept
    : m_id(std::exchange(other.m_id, 0)),
      m_width(std::exchange(other.m_width, 0)),
      m_height(std::exchange(other.m_height, 0)) {}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (m_id) {
            glDeleteTextures(1, &m_id);
        }
        m_id     = std::exchange(other.m_id, 0);
        m_width  = std::exchange(other.m_width, 0);
        m_height = std::exchange(other.m_height, 0);
    }
    return *this;
}

Texture Texture::fromPixels(const unsigned char* pixels, int width, int height,
                            int channels) {
    // Grey+alpha sources (2-channel PNGs, e.g. the *_disp displacement maps):
    // expand to RGBA on the CPU. These used to fall through to GL_RGB, telling
    // the driver to read width*height*3 bytes from a width*height*2 buffer --
    // an out-of-bounds read that crashed inside the driver whenever the
    // allocation ended near a page boundary (the frequent Assets-browser
    // crash: its 128px thumbnails hit that layout almost every time).
    if (channels == 2 && pixels && width > 0 && height > 0) {
        const std::size_t n = static_cast<std::size_t>(width) * height;
        std::vector<unsigned char> rgba(n * 4);
        for (std::size_t i = 0; i < n; ++i) {
            const unsigned char l = pixels[i * 2 + 0];
            rgba[i * 4 + 0] = l;
            rgba[i * 4 + 1] = l;
            rgba[i * 4 + 2] = l;
            rgba[i * 4 + 3] = pixels[i * 2 + 1];
        }
        return fromPixels(rgba.data(), width, height, 4);
    }
    Texture tex;
    tex.m_width  = width;
    tex.m_height = height;

    glGenTextures(1, &tex.m_id);
    glBindTexture(GL_TEXTURE_2D, tex.m_id);

    // Tightly-packed rows (handles widths that aren't multiples of 4).
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const GLenum format = formatForChannels(channels);
    glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(format), width, height, 0,
                 format, GL_UNSIGNED_BYTE, pixels);
    glGenerateMipmap(GL_TEXTURE_2D);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

Texture Texture::fromFile(const std::string& path, bool flipVertically) {
    // OpenEXR (e.g. PBR normal maps): load via tinyexr, convert to 8-bit RGB.
    if (endsWithExr(path)) {
        float* rgba = nullptr;
        int w = 0, h = 0;
        const char* err = nullptr;
        if (LoadEXR(&rgba, &w, &h, path.c_str(), &err) != TINYEXR_SUCCESS) {
            std::fprintf(stderr, "[Fitzel] failed to load EXR '%s': %s\n",
                         path.c_str(), err ? err : "unknown error");
            if (err) FreeEXRErrorMessage(err);
            return Texture{};
        }
        std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y) {
            const int sy = flipVertically ? (h - 1 - y) : y;
            for (int x = 0; x < w; ++x) {
                const float* s = &rgba[(static_cast<std::size_t>(sy) * w + x) * 4];
                unsigned char* d = &px[(static_cast<std::size_t>(y) * w + x) * 3];
                for (int c = 0; c < 3; ++c) {
                    d[c] = static_cast<unsigned char>(
                        std::clamp(s[c], 0.0f, 1.0f) * 255.0f + 0.5f);
                }
            }
        }
        std::free(rgba);
        return fromPixels(px.data(), w, h, 3);
    }

    stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

    int width = 0, height = 0, channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &width, &height, &channels, 0);
    if (!data) {
        std::fprintf(stderr, "[Fitzel] failed to load texture '%s': %s\n",
                     path.c_str(), stbi_failure_reason());
        return Texture{};
    }

    Texture tex = fromPixels(data, width, height, channels);
    stbi_image_free(data);
    return tex;
}

Texture Texture::checkerboard(int size, int cells) {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(size) * size * 3);
    const int cellSize = (cells > 0) ? size / cells : size;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const bool on = ((x / cellSize) + (y / cellSize)) % 2 == 0;
            const unsigned char v = on ? 220 : 40;
            const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 3;
            pixels[i + 0] = v;
            pixels[i + 1] = v;
            pixels[i + 2] = static_cast<unsigned char>(on ? 235 : 60);
        }
    }

    return fromPixels(pixels.data(), size, size, 3);
}

ImagePixels Texture::decodeThumbnail(const std::string& path, int maxDim) {
    // Decode to a tightly-packed 8-bit buffer (upright: no vertical flip). No GL
    // here -- this runs on worker threads. The flip flag is thread-local (see
    // stb_image_impl.cpp), so touching it can't disturb the render thread.
    ImagePixels img;
    std::vector<unsigned char>& buf = img.pixels;
    int w = 0, h = 0, ch = 0;
    if (endsWithExr(path)) {
        float* rgba = nullptr;
        const char* err = nullptr;
        if (LoadEXR(&rgba, &w, &h, path.c_str(), &err) != TINYEXR_SUCCESS) {
            if (err) FreeEXRErrorMessage(err);
            return {};
        }
        ch = 3;
        buf.resize(static_cast<std::size_t>(w) * h * 3);
        for (std::size_t i = 0; i < static_cast<std::size_t>(w) * h; ++i)
            for (int c = 0; c < 3; ++c)
                buf[i * 3 + c] = static_cast<unsigned char>(
                    std::clamp(rgba[i * 4 + c], 0.0f, 1.0f) * 255.0f + 0.5f);
        std::free(rgba);
    } else {
        stbi_set_flip_vertically_on_load_thread(0);
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &ch, 0);
        if (!data) return {};
        buf.assign(data, data + static_cast<std::size_t>(w) * h * ch);
        stbi_image_free(data);
    }
    if (w <= 0 || h <= 0 || ch <= 0) return {};

    // Repeatedly average 2x2 blocks until the longest side fits maxDim (mip-like
    // box filter -- cheap and clean enough for a thumbnail).
    while (std::max(w, h) > maxDim && w > 1 && h > 1) {
        const int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<unsigned char> half(static_cast<std::size_t>(nw) * nh * ch);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x) {
                const int x0 = x * 2, x1 = std::min(x * 2 + 1, w - 1);
                const int y0 = y * 2, y1 = std::min(y * 2 + 1, h - 1);
                for (int c = 0; c < ch; ++c) {
                    const int s = buf[(static_cast<std::size_t>(y0) * w + x0) * ch + c]
                                + buf[(static_cast<std::size_t>(y0) * w + x1) * ch + c]
                                + buf[(static_cast<std::size_t>(y1) * w + x0) * ch + c]
                                + buf[(static_cast<std::size_t>(y1) * w + x1) * ch + c];
                    half[(static_cast<std::size_t>(y) * nw + x) * ch + c] =
                        static_cast<unsigned char>(s / 4);
                }
            }
        buf.swap(half);
        w = nw; h = nh;
    }

    img.width = w; img.height = h; img.channels = ch;
    return img;
}

Texture Texture::fromImagePixels(const ImagePixels& img) {
    if (!img.valid()) return Texture{};
    return fromPixels(img.pixels.data(), img.width, img.height, img.channels);
}

Texture Texture::thumbnail(const std::string& path, int maxDim) {
    return fromImagePixels(decodeThumbnail(path, maxDim));
}

void Texture::bind(std::uint32_t unit) const {
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, m_id);
}

} // namespace fitzel
