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

// Anisotropic filtering level to use, or 0 where the driver has no such thing.
// Trilinear alone picks one mip from the *largest* UV derivative, so a surface
// seen at a grazing angle (road, terrain) either blurs across its short axis or
// undersamples along its long one -- the latter is what beats against the pixel
// grid as moire, and it comes and goes as the camera turns. Aniso takes several
// samples along the long axis instead and may keep the sharp mip.
// Queried once: the value is per-context and never changes, and this runs on the
// render thread with a context already bound (every caller is mid-upload).
float maxAnisotropy() {
    static const float level = [] {
        if (!GLAD_GL_ARB_texture_filter_anisotropic &&
            !GLAD_GL_EXT_texture_filter_anisotropic) {
            return 0.0f;
        }
        GLfloat cap = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &cap);
        // 16x is where the quality curve flattens; going higher just costs
        // bandwidth. Clamp to whatever the driver actually offers.
        return std::min(cap, 16.0f);
    }();
    return level;
}

// Expand ANY source (1/2/3/4 channels) into a tightly-packed RGBA buffer.
//
// Every upload in this file goes through here, because uploading GL_RED/GL_RG/
// GL_RGB trips an NVIDIA driver bug the engine hits constantly: glTexImage2D
// with those formats runs a JIT conversion loop that over-reads the tightly-
// packed source a few bytes past its end -- an AV inside driver JIT code (RIP
// ...FEEE) whenever the buffer ends near a page boundary. Handing the driver
// pre-expanded RGBA makes it take a straight copy, so that loop never runs.
// RGBA rows are inherently 4-aligned, so no GL_UNPACK_ALIGNMENT juggling.
std::vector<unsigned char> expandToRGBA(const unsigned char* pixels, int width,
                                        int height, int channels) {
    const std::size_t n = static_cast<std::size_t>(width) * height;
    std::vector<unsigned char> rgba(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        unsigned char r, g, b, a = 255;
        switch (channels) {
            case 1:  r = g = b = pixels[i]; break;
            case 2:  r = g = b = pixels[i * 2]; a = pixels[i * 2 + 1]; break;
            case 3:  r = pixels[i * 3]; g = pixels[i * 3 + 1]; b = pixels[i * 3 + 2]; break;
            default: r = pixels[i * 4]; g = pixels[i * 4 + 1]; b = pixels[i * 4 + 2];
                     a = pixels[i * 4 + 3]; break;
        }
        rgba[i * 4 + 0] = r; rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b; rgba[i * 4 + 3] = a;
    }
    return rgba;
}

// Never trust ambient pixel-store state: reset it explicitly before uploading.
// Dear ImGui's OpenGL3 backend sets GL_UNPACK_ROW_LENGTH to a texture width and
// (before its 2026-07-15 fix, #8802/#9473) left it set, corrupting the next
// caller's upload -- our tightly-packed buffer would then be read with a wrong
// row stride, over-reading past its end and crashing inside the driver (the
// ...FEEE AV). Forcing ROW_LENGTH=0 (= use `width`) and ALIGNMENT immunises us
// against any dependency's leaked state, regardless of which imgui tip we fetch.
void resetPixelStore() {
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
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
    Texture tex;
    tex.m_width  = width;
    tex.m_height = height;
    if (!pixels || width <= 0 || height <= 0) return tex;

    // Pre-expanded RGBA, uploaded as GL_RGBA8 -- see expandToRGBA for why the
    // driver is never handed a narrower format. (The other symptom once blamed on
    // this path -- corrupt/crashing mipmaps -- was really leaked pixel-store
    // state; see the glGenerateMipmap note below.)
    const std::vector<unsigned char> rgba =
        expandToRGBA(pixels, width, height, channels);

    glGenTextures(1, &tex.m_id);
    glBindTexture(GL_TEXTURE_2D, tex.m_id);
    resetPixelStore();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, rgba.data());

    // Mipmaps. The "glGenerateMipmap corrupts/crashes" symptom was never the mip
    // path itself -- it was leaked pixel-store state (imgui's GL_UNPACK_ROW_LENGTH,
    // pre-2026-07-15) making the driver over-read our upload. With ROW_LENGTH/
    // ALIGNMENT forced above and the base level uploaded as straight RGBA8, the
    // generator has clean input and behaves. Mips kill the grazing-angle aliasing
    // (terrain/road) and cut texture-cache thrashing at distance.
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    if (const float aniso = maxAnisotropy(); aniso > 1.0f) {
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, aniso);
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

Texture Texture::blank(int width, int height) {
    Texture tex;
    if (width <= 0 || height <= 0) return tex;
    tex.m_width  = width;
    tex.m_height = height;

    // Black, fully opaque -- a video quad shows this for the frame or two before
    // the first decode lands, and black reads as "screen is off", not as a bug.
    const std::vector<unsigned char> black(
        static_cast<std::size_t>(width) * height * 4, 0);

    glGenTextures(1, &tex.m_id);
    glBindTexture(GL_TEXTURE_2D, tex.m_id);
    resetPixelStore();
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, black.data());
    // No mipmaps: this level is rewritten every few frames, and regenerating the
    // chain each time would cost more than the sampling it saves. LINEAR only,
    // which also means MIN_FILTER must not name a mip level -- an incomplete
    // texture samples black.
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

bool Texture::update(const unsigned char* pixels, int width, int height,
                     int channels) {
    // Size changes would need a reallocation, which would break the "same GL
    // name throughout" contract callers rely on. A video's frames are all one
    // size, so a mismatch means the caller got its bookkeeping wrong.
    if (!m_id || !pixels || width != m_width || height != m_height) return false;

    const std::vector<unsigned char> rgba =
        expandToRGBA(pixels, width, height, channels);
    glBindTexture(GL_TEXTURE_2D, m_id);
    resetPixelStore();
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA,
                    GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
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
