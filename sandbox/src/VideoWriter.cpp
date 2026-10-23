#include "VideoWriter.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#ifndef _WIN32
#include <csignal>
#endif

#include <stb_image_write.h>

#include "VideoImport.hpp"   // ffmpegPath

namespace videoout {
namespace {

namespace fs = std::filesystem;

std::string quoted(const std::string& s) { return "\"" + s + "\""; }

// cmd.exe strips the outermost quotes of a command (see VideoImport.cpp).
std::string shellCommand(const std::string& cmd) {
#ifdef _WIN32
    return "\"" + cmd + "\"";
#else
    return cmd;
#endif
}

void put32(FILE* fp, std::uint32_t v) {
    const unsigned char b[4] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8),
                                static_cast<unsigned char>(v >> 16),
                                static_cast<unsigned char>(v >> 24)};
    std::fwrite(b, 1, 4, fp);
}
void put16(FILE* fp, std::uint16_t v) {
    const unsigned char b[2] = {static_cast<unsigned char>(v), static_cast<unsigned char>(v >> 8)};
    std::fwrite(b, 1, 2, fp);
}
void fourcc(FILE* fp, const char* c) { std::fwrite(c, 1, 4, fp); }

// A 32-bit field written once its value is known.
void patch32(FILE* fp, long at, std::uint32_t v) {
    const long here = std::ftell(fp);
    std::fseek(fp, at, SEEK_SET);
    put32(fp, v);
    std::fseek(fp, here, SEEK_SET);
}

// The AVI header's offsets, for patching at close. Fixed by the layout below.
constexpr long kRiffSize    = 4;
constexpr long kAvihFrames  = 48;    // avih.dwTotalFrames
constexpr long kAvihBuffer  = 60;    // avih.dwSuggestedBufferSize
constexpr long kStrhLength  = 140;   // strh.dwLength
constexpr long kStrhBuffer  = 144;   // strh.dwSuggestedBufferSize
constexpr long kMoviSize    = 216;   // LIST 'movi' size
constexpr long kMoviFourcc  = 220;

void appendJpg(void* ctx, void* data, int size) {
    auto* out = static_cast<std::vector<unsigned char>*>(ctx);
    const auto* p = static_cast<const unsigned char*>(data);
    out->insert(out->end(), p, p + size);
}

} // namespace

bool ffmpegAvailable() {
    static int known = -1;
    if (known < 0) {
#ifdef _WIN32
        const std::string sink = " -version > NUL 2>&1";
#else
        const std::string sink = " -version > /dev/null 2>&1";
#endif
        known = std::system(shellCommand(quoted(videoimport::ffmpegPath()) + sink).c_str()) == 0;
    }
    return known == 1;
}

Writer::~Writer() {
    std::string ignored;
    if (m_open) close(ignored);
}

bool Writer::open(const std::string& path, int width, int height, double fps, Format fmt,
                  int quality, std::string& err) {
    if (m_open) { err = "already writing a video"; return false; }
    m_srcW = width;
    m_srcH = height;
    m_w = width & ~1;
    m_h = height & ~1;
    if (m_w < 2 || m_h < 2) { err = "frame too small"; return false; }
    m_fmt = fmt;
    m_path = path;
    m_fps = std::clamp(fps, 1.0, 240.0);
    m_quality = std::clamp(quality, 1, 100);
    m_frames = 0;
    m_failed = false;
    m_index.clear();
    m_maxChunk = 0;

    std::error_code ec;
    if (fs::path(path).has_parent_path()) fs::create_directories(fs::path(path).parent_path(), ec);

    if (fmt == Format::Mp4) {
        if (!ffmpegAvailable()) {
            err = "no ffmpeg -- install it (apt install ffmpeg / winget install Gyan.FFmpeg) "
                  "or export as AVI";
            return false;
        }
        // A pipe ffmpeg stops reading from (a bad path, a crash) must fail
        // the write, not kill the editor with SIGPIPE.
#ifndef _WIN32
        std::signal(SIGPIPE, SIG_IGN);
#endif
        // The frames arrive bottom-up: vflip. quality 1..100 maps onto x264's
        // CRF 40..14 (lower is better; 18 is visually lossless).
        const int crf = 40 - (m_quality * 26) / 100;
        const std::string cmd =
            quoted(videoimport::ffmpegPath()) + " -y -v error -f rawvideo -pix_fmt rgba" +
            " -s " + std::to_string(m_w) + "x" + std::to_string(m_h) +
            " -r " + std::to_string(m_fps) + " -i - -vf vflip -c:v libx264 -preset medium" +
            " -crf " + std::to_string(crf) + " -pix_fmt yuv420p -movflags +faststart " +
            quoted(path);
#ifdef _WIN32
        m_fp = _popen(shellCommand(cmd).c_str(), "wb");
#else
        m_fp = popen(cmd.c_str(), "w");
#endif
        if (!m_fp) { err = "couldn't start ffmpeg"; return false; }
        m_open = true;
        return true;
    }

    m_fp = std::fopen(path.c_str(), "wb");
    if (!m_fp) { err = "can't write " + path; return false; }
    const std::uint32_t usPerFrame = static_cast<std::uint32_t>(1e6 / m_fps + 0.5);
    // Rate as a fraction: whole fps exactly, 29.97 within a hair.
    const std::uint32_t scale = 1000;
    const std::uint32_t rate  = static_cast<std::uint32_t>(m_fps * scale + 0.5);

    fourcc(m_fp, "RIFF"); put32(m_fp, 0); fourcc(m_fp, "AVI ");
    fourcc(m_fp, "LIST"); put32(m_fp, 192); fourcc(m_fp, "hdrl");
    // MainAVIHeader
    fourcc(m_fp, "avih"); put32(m_fp, 56);
    put32(m_fp, usPerFrame);
    put32(m_fp, 0);            // max bytes/sec
    put32(m_fp, 0);            // padding granularity
    put32(m_fp, 0x10);         // AVIF_HASINDEX
    put32(m_fp, 0);            // total frames (patched)
    put32(m_fp, 0);            // initial frames
    put32(m_fp, 1);            // streams
    put32(m_fp, 0);            // suggested buffer (patched)
    put32(m_fp, static_cast<std::uint32_t>(m_w));
    put32(m_fp, static_cast<std::uint32_t>(m_h));
    for (int i = 0; i < 4; ++i) put32(m_fp, 0);
    // The one stream.
    fourcc(m_fp, "LIST"); put32(m_fp, 116); fourcc(m_fp, "strl");
    fourcc(m_fp, "strh"); put32(m_fp, 56);
    fourcc(m_fp, "vids"); fourcc(m_fp, "MJPG");
    put32(m_fp, 0);            // flags
    put16(m_fp, 0); put16(m_fp, 0);   // priority, language
    put32(m_fp, 0);            // initial frames
    put32(m_fp, scale);
    put32(m_fp, rate);
    put32(m_fp, 0);            // start
    put32(m_fp, 0);            // length (patched)
    put32(m_fp, 0);            // suggested buffer (patched)
    put32(m_fp, 0xFFFFFFFFu);  // quality: default
    put32(m_fp, 0);            // sample size: varies
    put16(m_fp, 0); put16(m_fp, 0);
    put16(m_fp, static_cast<std::uint16_t>(m_w)); put16(m_fp, static_cast<std::uint16_t>(m_h));
    // BITMAPINFOHEADER
    fourcc(m_fp, "strf"); put32(m_fp, 40);
    put32(m_fp, 40);
    put32(m_fp, static_cast<std::uint32_t>(m_w));
    put32(m_fp, static_cast<std::uint32_t>(m_h));
    put16(m_fp, 1); put16(m_fp, 24);
    fourcc(m_fp, "MJPG");
    put32(m_fp, static_cast<std::uint32_t>(m_w * m_h * 3));
    put32(m_fp, 0); put32(m_fp, 0); put32(m_fp, 0); put32(m_fp, 0);
    // The frames.
    fourcc(m_fp, "LIST"); put32(m_fp, 0); fourcc(m_fp, "movi");
    m_moviStart = kMoviFourcc;
    if (std::ftell(m_fp) != kMoviFourcc + 4) {
        std::fclose(m_fp);
        m_fp = nullptr;
        err = "AVI header layout is off";   // a bug here, not the user's
        return false;
    }
    m_open = true;
    return true;
}

bool Writer::frame(const unsigned char* rgba) {
    if (!m_open || m_failed) return false;
    if (m_fmt == Format::Avi) return aviFrame(rgba);
    // Into ffmpeg: whole rows of the even-sized picture.
    const std::size_t row = static_cast<std::size_t>(m_w) * 4;
    const std::size_t srcRow = static_cast<std::size_t>(m_srcW) * 4;
    const int y0 = m_srcH - m_h;   // bottom-up: the dropped odd row is the bottom one
    for (int y = 0; y < m_h; ++y)
        if (std::fwrite(rgba + (y + y0) * srcRow, 1, row, m_fp) != row) {
            m_failed = true;
            return false;
        }
    ++m_frames;
    return true;
}

bool Writer::aviFrame(const unsigned char* rgba) {
    // Top-down RGB, as JPEG wants it.
    m_rgb.resize(static_cast<std::size_t>(m_w) * m_h * 3);
    const std::size_t srcRow = static_cast<std::size_t>(m_srcW) * 4;
    for (int y = 0; y < m_h; ++y) {
        const unsigned char* s = rgba + static_cast<std::size_t>(m_srcH - 1 - y) * srcRow;
        unsigned char* d = &m_rgb[static_cast<std::size_t>(y) * m_w * 3];
        for (int x = 0; x < m_w; ++x, s += 4, d += 3) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
    }
    m_jpg.clear();
    if (!stbi_write_jpg_to_func(appendJpg, &m_jpg, m_w, m_h, 3, m_rgb.data(), m_quality)) {
        m_failed = true;
        return false;
    }
    const std::uint32_t size = static_cast<std::uint32_t>(m_jpg.size());
    const long at = std::ftell(m_fp);
    // AVI 1.0 offsets are 32 bits.
    if (at < 0 || static_cast<unsigned long long>(at) + size + 16ull * (m_frames + 2) > 0xFFFF0000ull) {
        m_failed = true;
        return false;
    }
    fourcc(m_fp, "00dc");
    put32(m_fp, size);
    std::fwrite(m_jpg.data(), 1, size, m_fp);
    if (size & 1) std::fputc(0, m_fp);
    if (std::ferror(m_fp)) { m_failed = true; return false; }
    m_index.push_back(static_cast<std::uint32_t>(at - m_moviStart));
    m_index.push_back(size);
    m_maxChunk = std::max(m_maxChunk, size);
    ++m_frames;
    return true;
}

bool Writer::aviClose(std::string& msg) {
    const long moviEnd = std::ftell(m_fp);
    fourcc(m_fp, "idx1");
    put32(m_fp, static_cast<std::uint32_t>(m_index.size() / 2 * 16));
    for (std::size_t i = 0; i < m_index.size(); i += 2) {
        fourcc(m_fp, "00dc");
        put32(m_fp, 0x10);   // AVIIF_KEYFRAME: every MJPEG frame is one
        put32(m_fp, m_index[i]);
        put32(m_fp, m_index[i + 1]);
    }
    const long end = std::ftell(m_fp);
    patch32(m_fp, kRiffSize, static_cast<std::uint32_t>(end - 8));
    patch32(m_fp, kAvihFrames, static_cast<std::uint32_t>(m_frames));
    patch32(m_fp, kAvihBuffer, m_maxChunk + 8);
    patch32(m_fp, kStrhLength, static_cast<std::uint32_t>(m_frames));
    patch32(m_fp, kStrhBuffer, m_maxChunk + 8);
    patch32(m_fp, kMoviSize, static_cast<std::uint32_t>(moviEnd - kMoviFourcc));
    const bool ok = !std::ferror(m_fp);
    if (std::fclose(m_fp) != 0 || !ok || m_failed) {
        msg = m_failed ? "video stopped early (disk full, or past AVI's 4 GB)" : "write failed";
        return false;
    }
    return true;
}

bool Writer::close(std::string& msg) {
    if (!m_open) { msg = "no video open"; return false; }
    m_open = false;
    bool ok;
    if (m_fmt == Format::Avi) {
        ok = aviClose(msg);
    } else {
#ifdef _WIN32
        const int rc = _pclose(m_fp);
#else
        const int rc = pclose(m_fp);
#endif
        ok = rc == 0 && !m_failed;
        if (!ok) msg = "ffmpeg failed (see the console for its error)";
    }
    m_fp = nullptr;
    if (ok) {
        std::error_code ec;
        const auto bytes = fs::file_size(m_path, ec);
        char buf[160];
        std::snprintf(buf, sizeof buf, "%d frames, %dx%d @ %.0f fps, %.1f MB", m_frames, m_w, m_h,
                      m_fps, ec ? 0.0 : static_cast<double>(bytes) / (1024.0 * 1024.0));
        msg = buf;
    }
    return ok;
}

} // namespace videoout
