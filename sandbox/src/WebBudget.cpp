#include "WebBudget.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <type_traits>
#include <vector>

#include <stb_image.h>
// A private copy of the writer: this file is also linked into harnesses that
// do not carry the editor's (PathTracePanel.cpp).
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <tinyexr.h>

namespace webbudget {

namespace fs = std::filesystem;

namespace {

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return e;
}

// One 2x2 box step: half the size (at least 1), each texel the mean of the four
// it covers. An odd edge repeats its last row/column.
template <class T, class Acc>
void halve(std::vector<T>& px, int& w, int& h, int n) {
    const int w2 = std::max(1, w / 2), h2 = std::max(1, h / 2);
    std::vector<T> out(static_cast<std::size_t>(w2) * h2 * n);
    for (int y = 0; y < h2; ++y) {
        const int y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
        for (int x = 0; x < w2; ++x) {
            const int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1);
            for (int c = 0; c < n; ++c) {
                auto at = [&](int xx, int yy) {
                    return static_cast<Acc>(px[(static_cast<std::size_t>(yy) * w + xx) * n + c]);
                };
                const Acc sum = at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1);
                if constexpr (std::is_integral_v<T>)
                    out[(static_cast<std::size_t>(y) * w2 + x) * n + c] =
                        static_cast<T>((sum + 2) / 4);
                else
                    out[(static_cast<std::size_t>(y) * w2 + x) * n + c] = static_cast<T>(sum / 4);
            }
        }
    }
    px.swap(out);
    w = w2;
    h = h2;
}

bool shrinkLdr(const fs::path& file, const std::string& ext, int maxSize) {
    int w = 0, h = 0, n = 0;
    unsigned char* raw = stbi_load(file.string().c_str(), &w, &h, &n, 0);
    if (!raw) return false;
    if (std::max(w, h) <= maxSize) { stbi_image_free(raw); return false; }
    std::vector<unsigned char> px(raw, raw + static_cast<std::size_t>(w) * h * n);
    stbi_image_free(raw);
    while (std::max(w, h) > maxSize) halve<unsigned char, unsigned>(px, w, h, n);

    const std::string out = file.string();
    if (ext == ".jpg" || ext == ".jpeg")
        return stbi_write_jpg(out.c_str(), w, h, n, px.data(), 90) != 0;
    if (ext == ".tga") return stbi_write_tga(out.c_str(), w, h, n, px.data()) != 0;
    if (ext == ".bmp") return stbi_write_bmp(out.c_str(), w, h, n, px.data()) != 0;
    return stbi_write_png(out.c_str(), w, h, n, px.data(), w * n) != 0;
}

bool shrinkExr(const fs::path& file, int maxSize) {
    float* raw = nullptr;
    int w = 0, h = 0;
    const char* err = nullptr;
    if (LoadEXR(&raw, &w, &h, file.string().c_str(), &err) != TINYEXR_SUCCESS) {
        if (err) FreeEXRErrorMessage(err);
        return false;
    }
    if (std::max(w, h) <= maxSize) { std::free(raw); return false; }
    std::vector<float> px(raw, raw + static_cast<std::size_t>(w) * h * 4);
    std::free(raw);
    while (std::max(w, h) > maxSize) halve<float, float>(px, w, h, 4);

    // RGB unless the alpha says something: an HDRI or a normal map has none.
    bool alpha = false;
    for (std::size_t i = 3; i < px.size() && !alpha; i += 4) alpha = px[i] < 0.999f;
    std::vector<float> outPx;
    int comps = 4;
    if (!alpha) {
        comps = 3;
        outPx.reserve(static_cast<std::size_t>(w) * h * 3);
        for (std::size_t i = 0; i < px.size(); i += 4)
            outPx.insert(outPx.end(), {px[i], px[i + 1], px[i + 2]});
    }
    const char* serr = nullptr;
    const int r = SaveEXR(alpha ? px.data() : outPx.data(), w, h, comps,
                          /*save_as_fp16=*/1, file.string().c_str(), &serr);
    if (serr) FreeEXRErrorMessage(serr);
    return r == TINYEXR_SUCCESS;
}

// --- WAV ------------------------------------------------------------------

std::uint32_t le32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const unsigned char* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

// Decoded PCM, interleaved, as floats in [-1, 1].
struct Pcm {
    int rate = 0, channels = 0, bits = 0;
    std::vector<float> s;
    std::size_t frames() const { return channels ? s.size() / channels : 0; }
};

bool readWav(const fs::path& file, Pcm& pcm) {
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 ||
        std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return false;
    int format = 0;
    const unsigned char* data = nullptr;
    std::size_t dataLen = 0;
    for (std::size_t at = 12; at + 8 <= b.size();) {
        const std::uint32_t len = le32(&b[at + 4]);
        const unsigned char* body = &b[at + 8];
        const std::size_t avail = std::min<std::size_t>(len, b.size() - at - 8);
        if (std::memcmp(&b[at], "fmt ", 4) == 0 && avail >= 16) {
            format       = le16(body);
            pcm.channels = le16(body + 2);
            pcm.rate     = static_cast<int>(le32(body + 4));
            pcm.bits     = le16(body + 14);
            // WAVE_FORMAT_EXTENSIBLE: the real format is the sub-format GUID's
            // first two bytes.
            if (format == 0xFFFE && avail >= 26) format = le16(body + 24);
        } else if (std::memcmp(&b[at], "data", 4) == 0) {
            data = body;
            dataLen = avail;
        }
        at += 8 + len + (len & 1);
    }
    if (!data || format != 1 || pcm.channels <= 0 || pcm.rate <= 0) return false;
    const int bytes = pcm.bits / 8;
    if (bytes < 1 || bytes > 4) return false;
    const std::size_t n = dataLen / bytes;
    pcm.s.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char* p = data + i * bytes;
        float v = 0.0f;
        switch (bytes) {
            case 1: v = (p[0] - 128) / 128.0f; break;
            case 2: v = static_cast<std::int16_t>(le16(p)) / 32768.0f; break;
            case 3: v = static_cast<float>(static_cast<std::int32_t>(
                            (p[0] << 8) | (p[1] << 16) | (static_cast<std::uint32_t>(p[2]) << 24)) >> 8) /
                        8388608.0f; break;
            case 4: v = static_cast<float>(static_cast<std::int32_t>(le32(p))) / 2147483648.0f; break;
        }
        pcm.s[i] = v;
    }
    pcm.s.resize(pcm.frames() * pcm.channels);
    return true;
}

bool writeWav16(const fs::path& file, const Pcm& pcm) {
    const std::uint32_t dataLen = static_cast<std::uint32_t>(pcm.s.size() * 2);
    std::vector<unsigned char> b;
    b.reserve(44 + dataLen);
    auto put = [&b](std::uint32_t v, int n) {
        for (int i = 0; i < n; ++i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
    };
    auto tag = [&b](const char* t) { b.insert(b.end(), t, t + 4); };
    tag("RIFF"); put(36 + dataLen, 4); tag("WAVE");
    tag("fmt "); put(16, 4); put(1, 2); put(static_cast<std::uint32_t>(pcm.channels), 2);
    put(static_cast<std::uint32_t>(pcm.rate), 4);
    put(static_cast<std::uint32_t>(pcm.rate * pcm.channels * 2), 4);
    put(static_cast<std::uint32_t>(pcm.channels * 2), 2); put(16, 2);
    tag("data"); put(dataLen, 4);
    for (const float v : pcm.s) {
        const int q = static_cast<int>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
        put(static_cast<std::uint32_t>(static_cast<std::uint16_t>(static_cast<std::int16_t>(q))), 2);
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(out);
}

bool shrinkWav(const fs::path& file, float maxSeconds) {
    Pcm pcm;
    if (!readWav(file, pcm)) return false;
    const std::size_t maxFrames =
        static_cast<std::size_t>(std::max(1.0f, maxSeconds) * static_cast<float>(pcm.rate));
    const bool tooLong = pcm.frames() > maxFrames;
    if (pcm.bits == 16 && pcm.rate <= 48000 && !tooLong) return false;

    const int ch = pcm.channels;
    // Halve the rate (mean of each pair) while over 48 kHz.
    while (pcm.rate > 48000) {
        std::vector<float> half((pcm.frames() / 2) * ch);
        for (std::size_t f = 0; f < pcm.frames() / 2; ++f)
            for (int c = 0; c < ch; ++c)
                half[f * ch + c] = 0.5f * (pcm.s[(2 * f) * ch + c] + pcm.s[(2 * f + 1) * ch + c]);
        pcm.s.swap(half);
        pcm.rate /= 2;
    }
    // Cut to length; the part past the cut fades into the beginning, so the
    // end runs on seamlessly into the start when it loops.
    const std::size_t keep =
        static_cast<std::size_t>(std::max(1.0f, maxSeconds) * static_cast<float>(pcm.rate));
    if (pcm.frames() > keep) {
        const std::size_t fade = std::min<std::size_t>(
            std::min<std::size_t>(static_cast<std::size_t>(2 * pcm.rate), keep / 4),
            pcm.frames() - keep);
        for (std::size_t f = 0; f < fade; ++f) {
            const float t = static_cast<float>(f) / static_cast<float>(fade);
            for (int c = 0; c < ch; ++c)
                pcm.s[f * ch + c] = pcm.s[f * ch + c] * t + pcm.s[(keep + f) * ch + c] * (1.0f - t);
        }
        pcm.s.resize(keep * ch);
    }
    return writeWav16(file, pcm);
}

} // namespace

Report shrinkSounds(const fs::path& root, float maxSeconds) {
    Report rep;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec) && lowerExt(it->path()) == ".wav")
            files.push_back(it->path());
    }
    for (const fs::path& f : files) {
        const std::uintmax_t before = fs::file_size(f, ec);
        if (!shrinkWav(f, maxSeconds)) continue;
        ++rep.shrunk;
        rep.before += before;
        rep.after  += fs::file_size(f, ec);
    }
    return rep;
}

Report shrinkImages(const fs::path& root, int maxSize) {
    Report rep;
    if (maxSize <= 0) return rep;
    std::vector<fs::path> files;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string e = lowerExt(it->path());
        if (e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp" ||
            e == ".exr")
            files.push_back(it->path());
    }
    for (const fs::path& f : files) {
        const std::uintmax_t before = fs::file_size(f, ec);
        const std::string e = lowerExt(f);
        const bool done = (e == ".exr") ? shrinkExr(f, maxSize) : shrinkLdr(f, e, maxSize);
        if (!done) continue;
        ++rep.shrunk;
        rep.before += before;
        rep.after  += fs::file_size(f, ec);
    }
    return rep;
}

} // namespace webbudget
