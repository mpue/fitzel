#include "fitzel/graphics/Texture.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include <stb_image.h>
#include <tinyexr.h>

#include "fitzel/asset/Vfs.hpp"
#include "fitzel/rhi/Device.hpp"

namespace fitzel {

namespace {

// Every decode here starts from bytes rather than a path: in an exported game
// the file has no path, it is a range inside the mounted archive. Loose files
// come back through the same call (vfs::read falls through to disk).
std::vector<std::uint8_t> readBytes(const std::string& path) {
    std::vector<std::uint8_t> bytes = vfs::read(path);
    if (bytes.empty())
        std::fprintf(stderr, "[Fitzel] cannot read '%s'\n", path.c_str());
    return bytes;
}

// Anisotropic filtering level to use (1 where the device has no such thing).
// Trilinear alone picks one mip from the *largest* UV derivative, so a surface
// seen at a grazing angle (road, terrain) either blurs across its short axis or
// undersamples along its long one -- the latter is what beats against the pixel
// grid as moire, and it comes and goes as the camera turns. Aniso takes several
// samples along the long axis instead and may keep the sharp mip. 16x is where
// the quality curve flattens; going higher just costs bandwidth.
float maxAnisotropy() {
    return std::min(rhi::device().caps().maxAnisotropy, 16.0f);
}

// Expand ANY source (1/2/3/4 channels) into a tightly-packed RGBA buffer.
//
// Every upload in this file goes through here, because uploading R/RG/RGB data
// trips an NVIDIA OpenGL driver bug the engine hits constantly: the upload runs
// a JIT conversion loop that over-reads the tightly-packed source a few bytes
// past its end -- an AV inside driver JIT code (RIP ...FEEE) whenever the buffer
// ends near a page boundary. Handing the driver pre-expanded RGBA makes it take
// a straight copy, so that loop never runs.
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

bool endsWithExr(const std::string& p) {
    if (p.size() < 4) return false;
    std::string ext = p.substr(p.size() - 4);
    for (char& c : ext) c = static_cast<char>(std::tolower(c));
    return ext == ".exr";
}

} // namespace

Texture::~Texture() {
    if (m_id) rhi::device().destroy(m_id);
}

Texture::Texture(Texture&& other) noexcept
    : m_id(std::exchange(other.m_id, rhi::TextureHandle{})),
      m_width(std::exchange(other.m_width, 0)),
      m_height(std::exchange(other.m_height, 0)),
      m_bottomUp(std::exchange(other.m_bottomUp, false)),
      m_layers(std::exchange(other.m_layers, 0)) {}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        if (m_id) rhi::device().destroy(m_id);
        m_id     = std::exchange(other.m_id, rhi::TextureHandle{});
        m_width  = std::exchange(other.m_width, 0);
        m_height = std::exchange(other.m_height, 0);
        m_bottomUp = std::exchange(other.m_bottomUp, false);
        m_layers   = std::exchange(other.m_layers, 0);
    }
    return *this;
}

Texture Texture::fromPixels(const unsigned char* pixels, int width, int height,
                            int channels) {
    Texture tex;
    tex.m_width  = width;
    tex.m_height = height;
    if (!pixels || width <= 0 || height <= 0) return tex;

    // Pre-expanded RGBA, uploaded as RGBA8 -- see expandToRGBA for why the
    // driver is never handed a narrower format.
    const std::vector<unsigned char> rgba =
        expandToRGBA(pixels, width, height, channels);

    // Mipmapped, trilinear and anisotropic: mips kill the grazing-angle aliasing
    // (terrain/road) and cut texture-cache thrashing at distance. (A "mipmap
    // generation corrupts/crashes" symptom once blamed on this path was really
    // leaked pixel-store state making the driver over-read the upload; the
    // device sets the row layout before every transfer now.)
    rhi::TextureDesc d;
    d.format    = rhi::Format::RGBA8;
    d.width     = width;
    d.height    = height;
    d.mipLevels = 0;
    d.sampler   = rhi::SamplerDesc::mipmappedRepeat(maxAnisotropy());
    tex.m_id = rhi::device().createTexture(d, rgba.data());
    rhi::device().generateMipmaps(tex.m_id);
    return tex;
}

Texture Texture::blank(int width, int height, rhi::Format format) {
    Texture tex;
    if (width <= 0 || height <= 0) return tex;
    tex.m_width  = width;
    tex.m_height = height;

    // Black, fully opaque -- a video quad shows this for the frame or two before
    // the first decode lands, and black reads as "screen is off", not as a bug.
    const std::vector<unsigned char> black(
        static_cast<std::size_t>(width) * height * 4, 0);

    // No mipmaps: this level is rewritten every few frames, and regenerating the
    // chain each time would cost more than the sampling it saves. Linear only,
    // which also means the sampler must not name a mip level -- an incomplete
    // texture samples black.
    rhi::TextureDesc d;
    d.format  = format;
    d.width   = width;
    d.height  = height;
    d.sampler = rhi::SamplerDesc::linearClamp();
    tex.m_id = rhi::device().createTexture(d, black.data(), rhi::Format::RGBA8);
    return tex;
}

bool Texture::update(const unsigned char* pixels, int width, int height,
                     int channels) {
    // Size changes would need a reallocation, which would break the "same
    // handle throughout" contract callers rely on. A video's frames are all one
    // size, so a mismatch means the caller got its bookkeeping wrong.
    if (!m_id || !pixels || width != m_width || height != m_height) return false;

    const std::vector<unsigned char> rgba =
        expandToRGBA(pixels, width, height, channels);
    rhi::device().updateTexture(m_id, rhi::TextureRegion{}, rhi::Format::RGBA8, rgba.data());
    return true;
}

Texture Texture::fromFile(const std::string& path, bool flipVertically) {
    const std::vector<std::uint8_t> bytes = readBytes(path);
    if (bytes.empty()) return Texture{};

    // OpenEXR (e.g. PBR normal maps): load via tinyexr, convert to 8-bit RGB.
    if (endsWithExr(path)) {
        float* rgba = nullptr;
        int w = 0, h = 0;
        const char* err = nullptr;
        if (LoadEXRFromMemory(&rgba, &w, &h, bytes.data(), bytes.size(), &err) !=
            TINYEXR_SUCCESS) {
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
        Texture tex = fromPixels(px.data(), w, h, 3);
        tex.m_bottomUp = flipVertically;
        return tex;
    }

    // Thread-local, and back to 0 straight after (see stb_image_impl.cpp).
    stbi_set_flip_vertically_on_load_thread(flipVertically ? 1 : 0);

    int width = 0, height = 0, channels = 0;
    unsigned char* data =
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()),
                              &width, &height, &channels, 0);
    stbi_set_flip_vertically_on_load_thread(0);
    if (!data) {
        std::fprintf(stderr, "[Fitzel] failed to load texture '%s': %s\n",
                     path.c_str(), stbi_failure_reason());
        return Texture{};
    }

    Texture tex = fromPixels(data, width, height, channels);
    tex.m_bottomUp = flipVertically;
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
    const std::vector<std::uint8_t> bytes = vfs::read(path);
    if (bytes.empty()) return {};
    if (endsWithExr(path)) {
        float* rgba = nullptr;
        const char* err = nullptr;
        if (LoadEXRFromMemory(&rgba, &w, &h, bytes.data(), bytes.size(), &err) !=
            TINYEXR_SUCCESS) {
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
        unsigned char* data = stbi_load_from_memory(
            bytes.data(), static_cast<int>(bytes.size()), &w, &h, &ch, 0);
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

ImagePixels Texture::readback() const {
    ImagePixels img;
    if (!m_id || m_width <= 0 || m_height <= 0) return img;

    img.width    = m_width;
    img.height   = m_height;
    img.channels = 4;
    img.pixels.resize(static_cast<std::size_t>(m_width) * m_height * 4);
    rhi::device().readTexture(m_id, 0, isArray() ? 0 : -1, rhi::Format::RGBA8, img.pixels.data());
    return img;
}

void Texture::bind(std::uint32_t unit) const {
    rhi::device().bindTexture(static_cast<int>(unit), m_id);
}

Texture Texture::arrayOf(const std::vector<const Texture*>& layers, int size,
                         const unsigned char fill[4]) {
    Texture tex;
    if (layers.empty() || size <= 0) return tex;
    const int n = static_cast<int>(layers.size());
    rhi::Device& gpu = rhi::device();

    rhi::TextureDesc d;
    d.type      = rhi::TextureType::Tex2DArray;
    d.format    = rhi::Format::RGBA8;
    d.width     = size;
    d.height    = size;
    d.depth     = n;
    d.mipLevels = 0;
    d.sampler   = rhi::SamplerDesc::mipmappedRepeat(maxAnisotropy());
    tex.m_id     = gpu.createTexture(d);
    tex.m_width  = size;
    tex.m_height = size;
    tex.m_layers = n;

    // This runs mid-frame, between passes that assume their framebuffer and
    // scissor untouched: both are put back. A clear is clipped by the scissor.
    const rhi::FramebufferHandle prevFb = gpu.boundFramebuffer();
    const bool scissor = gpu.renderState().raster.scissor;
    gpu.setScissorTest(false);

    rhi::FramebufferDesc dstDesc;
    const rhi::FramebufferHandle dst = gpu.createFramebuffer(dstDesc);
    const rhi::FramebufferHandle src = gpu.createFramebuffer(dstDesc);
    const glm::vec4 fillF(fill[0] / 255.0f, fill[1] / 255.0f, fill[2] / 255.0f, fill[3] / 255.0f);
    for (int i = 0; i < n; ++i) {
        dstDesc.color[0] = rhi::Attachment{tex.m_id, 0, i};
        gpu.setAttachments(dst, dstDesc);
        const Texture* srcTex = layers[static_cast<std::size_t>(i)];
        bool copied = false;
        // A float source cannot be blitted into RGBA8: the layer takes the fill.
        if (srcTex && srcTex->isValid() && !srcTex->isArray() &&
            !rhi::isFloatFormat(gpu.textureDesc(srcTex->id()).format)) {
            // From the mip nearest above the target size: a linear blit reads
            // four texels per pixel, and shrinking a 4K map to 1K from level 0
            // would alias. A texture without mips falls back to level 0.
            int level = 0;
            while ((std::max(srcTex->width(), srcTex->height()) >> (level + 1)) >= size) ++level;
            for (int tryLevel : {level, 0}) {
                rhi::FramebufferDesc srcDesc;
                srcDesc.color[0] = rhi::Attachment{srcTex->id(), tryLevel, -1};
                gpu.setAttachments(src, srcDesc);
                if (gpu.framebufferComplete(src) && gpu.framebufferComplete(dst)) {
                    gpu.drainErrors();
                    gpu.blit(src,
                             rhi::Rect{0, 0, std::max(1, srcTex->width() >> tryLevel),
                                       std::max(1, srcTex->height() >> tryLevel)},
                             dst, rhi::Rect{0, 0, size, size}, rhi::BlitColor,
                             rhi::Filter::Linear);
                    copied = !gpu.drainErrors();
                    break;
                }
            }
        }
        if (!copied) {
            gpu.bindFramebuffer(dst);
            gpu.clearColorAttachment(0, fillF);
        }
    }
    gpu.bindFramebuffer(prevFb);
    gpu.destroy(src);
    gpu.destroy(dst);
    gpu.setScissorTest(scissor);

    gpu.generateMipmaps(tex.m_id);
    return tex;
}

} // namespace fitzel
