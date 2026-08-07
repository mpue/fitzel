#include "fitzel/graphics/VideoTexture.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

#include <stb_image.h>

#include "fitzel/graphics/Texture.hpp"

namespace fitzel {

namespace {

// Little-endian reads out of the header. Written by hand rather than memcpy'd
// into a packed struct so the file layout does not depend on the compiler's
// padding rules -- an .fvid written by one build must play in another.
std::uint32_t readU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

float readF32(const unsigned char* p) {
    const std::uint32_t bits = readU32(p);
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

// magic + version + w + h + frameCount + fps
constexpr std::size_t kHeaderSize = 4 + 4 * 5;

} // namespace

float VideoTexture::duration() const {
    if (m_frames.empty() || m_fps <= 0.0f) return 0.0f;
    return static_cast<float>(m_frames.size()) / m_fps;
}

std::shared_ptr<VideoTexture> VideoTexture::open(const std::string& path) {
    auto fail = [&](const char* why) {
        std::fprintf(stderr, "[Fitzel] can't open video '%s': %s\n",
                     path.c_str(), why);
        return std::shared_ptr<VideoTexture>{};
    };

    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return fail("no such file");
    const std::streamoff size = f.tellg();
    if (size < static_cast<std::streamoff>(kHeaderSize)) return fail("truncated");
    f.seekg(0);

    auto v = std::make_shared<VideoTexture>();
    v->m_bytes.resize(static_cast<std::size_t>(size));
    if (!f.read(reinterpret_cast<char*>(v->m_bytes.data()), size))
        return fail("read failed");

    const unsigned char* h = v->m_bytes.data();
    if (std::memcmp(h, kMagic, 4) != 0)          return fail("not an .fvid");
    if (readU32(h + 4) != kVersion)              return fail("unsupported version");
    v->m_width  = static_cast<int>(readU32(h + 8));
    v->m_height = static_cast<int>(readU32(h + 12));
    const std::uint32_t count = readU32(h + 16);
    v->m_fps    = readF32(h + 20);
    if (v->m_width <= 0 || v->m_height <= 0)     return fail("bad dimensions");
    if (count == 0)                              return fail("no frames");
    if (v->m_fps <= 0.0f) v->m_fps = 15.0f;

    // The index must fit, and every frame it names must lie inside the file --
    // this is the only validation between a corrupt/truncated file and a decode
    // reading off the end of the buffer.
    const std::size_t indexBytes = static_cast<std::size_t>(count) * 8;
    if (v->m_bytes.size() < kHeaderSize + indexBytes) return fail("truncated index");
    v->m_frames.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const unsigned char* e = h + kHeaderSize + static_cast<std::size_t>(i) * 8;
        FrameRef fr{readU32(e), readU32(e + 4)};
        if (fr.size == 0 ||
            static_cast<std::size_t>(fr.offset) + fr.size > v->m_bytes.size())
            return fail("frame index points outside the file");
        v->m_frames.push_back(fr);
    }

    v->m_tex = std::make_shared<Texture>(Texture::blank(v->m_width, v->m_height));
    if (!v->m_tex->isValid()) return fail("couldn't allocate the texture");

    v->decodeCurrentFrame();
    return v;
}

void VideoTexture::seek(float seconds) {
    const float len = duration();
    if (len <= 0.0f) return;
    m_time = std::fmod(seconds, len);
    if (m_time < 0.0f) m_time += len;
}

void VideoTexture::advance(float dt) {
    if (!isValid()) return;
    const float len = duration();
    if (len <= 0.0f) return;

    m_time += dt;
    // A long stall (a load, a breakpoint) can leave dt at several seconds. fmod
    // rather than a subtract loop so the catch-up is O(1) and the clip resumes at
    // the right phase instead of sprinting through frames it would never show.
    if (m_time >= len || m_time < 0.0f) {
        m_time = std::fmod(m_time, len);
        if (m_time < 0.0f) m_time += len;
    }
    decodeCurrentFrame();
}

void VideoTexture::decodeCurrentFrame() {
    int idx = static_cast<int>(m_time * m_fps);
    if (idx < 0) idx = 0;
    if (idx >= static_cast<int>(m_frames.size()))
        idx = static_cast<int>(m_frames.size()) - 1;
    if (idx == m_current) return;   // the common case: nothing to do

    const FrameRef& fr = m_frames[static_cast<std::size_t>(idx)];
    int w = 0, h = 0, ch = 0;
    // Explicitly unflipped: the importer already wrote the frames in the
    // orientation the UVs expect, and stb's flip flag is thread-local ambient
    // state that some other decode may have left set.
    stbi_set_flip_vertically_on_load(0);
    unsigned char* px = stbi_load_from_memory(m_bytes.data() + fr.offset,
                                              static_cast<int>(fr.size),
                                              &w, &h, &ch, 4);
    if (!px) {
        // Keep the last good frame on screen and don't retry this one: a bad
        // frame in a loop would otherwise log once per pass, forever.
        std::fprintf(stderr, "[Fitzel] video frame %d won't decode (%s)\n", idx,
                     stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        m_current = idx;
        return;
    }
    if (w == m_width && h == m_height) m_tex->update(px, w, h, 4);
    stbi_image_free(px);
    m_current = idx;
}

} // namespace fitzel
