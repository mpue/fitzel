#include "fitzel/audio/SampleData.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <miniaudio.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include "fitzel/asset/Vfs.hpp"

namespace fitzel {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

// Walk a RIFF/WAVE file's chunks; `fn(id, data, size)` for each. False when the
// bytes are not a WAV.
template <class Fn>
bool eachChunk(const std::vector<std::uint8_t>& b, Fn fn) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 ||
        std::memcmp(b.data() + 8, "WAVE", 4) != 0)
        return false;
    std::size_t at = 12;
    while (at + 8 <= b.size()) {
        const std::uint32_t size = le32(b.data() + at + 4);
        const std::size_t   data = at + 8;
        const std::size_t   have = std::min<std::size_t>(size, b.size() - data);
        fn(b.data() + at, b.data() + data, have);
        at = data + size + (size & 1u);   // chunks are padded to an even length
    }
    return true;
}

void put32(std::vector<std::uint8_t>& o, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}
void put16(std::vector<std::uint8_t>& o, std::uint16_t v) {
    o.push_back(static_cast<std::uint8_t>(v));
    o.push_back(static_cast<std::uint8_t>(v >> 8));
}
void putId(std::vector<std::uint8_t>& o, const char* id) { o.insert(o.end(), id, id + 4); }

} // namespace

bool readWavLoop(const std::vector<std::uint8_t>& wav, std::int64_t& start, std::int64_t& end) {
    bool found = false;
    eachChunk(wav, [&](const std::uint8_t* hdr, const std::uint8_t* d, std::size_t n) {
        if (found || std::memcmp(hdr, "smpl", 4) != 0 || n < 36 + 24) return;
        if (le32(d + 28) == 0) return;            // no loops
        const std::uint8_t* loop = d + 36;
        const std::uint32_t s = le32(loop + 8), e = le32(loop + 12);   // e inclusive
        if (e < s) return;
        start = s;
        end   = static_cast<std::int64_t>(e) + 1;
        found = true;
    });
    return found;
}

bool readWavLoop(const std::string& path, std::int64_t& start, std::int64_t& end) {
    return readWavLoop(vfs::read(path), start, end);
}

bool loadSampleData(const std::string& path, SampleData& out, std::string* error) {
    out = SampleData{};
    const std::vector<std::uint8_t> bytes = vfs::read(path);
    if (bytes.empty()) {
        if (error) *error = "cannot read " + path;
        return false;
    }
    // Floats, the file's own channel count and rate (0 = as it is).
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder dec;
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &dec) != MA_SUCCESS) {
        if (error) *error = "not a sound this engine can decode: " + path;
        return false;
    }
    out.channels   = static_cast<int>(dec.outputChannels);
    out.sampleRate = static_cast<int>(dec.outputSampleRate);
    ma_uint64 total = 0;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &total) == MA_SUCCESS && total > 0)
        out.samples.reserve(static_cast<std::size_t>(total) * out.channels);
    std::vector<float> buf(4096 * static_cast<std::size_t>(std::max(out.channels, 1)));
    for (;;) {
        ma_uint64 got = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&dec, buf.data(), 4096, &got);
        out.samples.insert(out.samples.end(), buf.begin(),
                           buf.begin() + static_cast<std::ptrdiff_t>(got * out.channels));
        if (got < 4096 || r != MA_SUCCESS) break;
    }
    ma_decoder_uninit(&dec);
    if (out.channels <= 0 || out.sampleRate <= 0) {
        if (error) *error = "no channels in " + path;
        return false;
    }

    // What a WAV was written as, and its loop.
    eachChunk(bytes, [&](const std::uint8_t* hdr, const std::uint8_t* d, std::size_t n) {
        if (std::memcmp(hdr, "fmt ", 4) == 0 && n >= 16) {
            std::uint16_t tag = le16(d);
            if (tag == 0xFFFE && n >= 40) tag = le16(d + 24);   // extensible: the sub-format
            out.sourceBits  = le16(d + 14);
            out.sourceFloat = tag == 3;
        }
    });
    std::int64_t s = 0, e = 0;
    if (readWavLoop(bytes, s, e)) {
        out.loopStart = std::clamp<std::int64_t>(s, 0, out.frames());
        out.loopEnd   = std::clamp<std::int64_t>(e, 0, out.frames());
        if (!out.hasLoop()) out.loopStart = out.loopEnd = -1;
    }
    return true;
}

bool saveWav(const std::string& path, const SampleData& data, WavFormat format,
             std::string* error) {
    if (data.channels <= 0 || data.sampleRate <= 0) {
        if (error) *error = "nothing to save";
        return false;
    }
    const int bits  = format == WavFormat::Pcm16 ? 16 : format == WavFormat::Pcm24 ? 24 : 32;
    const int bytesPer = bits / 8;
    const std::uint16_t tag = format == WavFormat::Float32 ? 3 : 1;
    const std::size_t n = data.samples.size();

    std::vector<std::uint8_t> o;
    o.reserve(64 + n * bytesPer + 80);
    putId(o, "RIFF"); put32(o, 0); putId(o, "WAVE");
    putId(o, "fmt "); put32(o, 16);
    put16(o, tag);
    put16(o, static_cast<std::uint16_t>(data.channels));
    put32(o, static_cast<std::uint32_t>(data.sampleRate));
    put32(o, static_cast<std::uint32_t>(data.sampleRate * data.channels * bytesPer));
    put16(o, static_cast<std::uint16_t>(data.channels * bytesPer));
    put16(o, static_cast<std::uint16_t>(bits));
    if (format == WavFormat::Float32) {
        // A float WAV wants a fact chunk with the frame count (some readers insist).
        putId(o, "fact"); put32(o, 4); put32(o, static_cast<std::uint32_t>(data.frames()));
    }
    putId(o, "data"); put32(o, static_cast<std::uint32_t>(n * bytesPer));
    std::mt19937 rng(12345u);
    std::uniform_real_distribution<float> u(-0.5f, 0.5f);
    for (std::size_t i = 0; i < n; ++i) {
        const float v = data.samples[i];
        if (format == WavFormat::Float32) {
            std::uint32_t w;
            std::memcpy(&w, &v, 4);
            put32(o, w);
        } else if (format == WavFormat::Pcm16) {
            const float d = (u(rng) + u(rng));   // triangular, +-1 LSB
            const long  q = std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f + d);
            put16(o, static_cast<std::uint16_t>(static_cast<std::int16_t>(std::clamp(q, -32768L, 32767L))));
        } else {
            const long q = std::clamp(std::lround(std::clamp(v, -1.0f, 1.0f) * 8388607.0f),
                                      -8388608L, 8388607L);
            const std::uint32_t w = static_cast<std::uint32_t>(q);
            o.push_back(static_cast<std::uint8_t>(w));
            o.push_back(static_cast<std::uint8_t>(w >> 8));
            o.push_back(static_cast<std::uint8_t>(w >> 16));
        }
    }
    if (o.size() & 1u) o.push_back(0);
    if (data.hasLoop()) {
        putId(o, "smpl"); put32(o, 36 + 24);
        put32(o, 0); put32(o, 0);                                       // manufacturer, product
        put32(o, static_cast<std::uint32_t>(1000000000.0 / data.sampleRate));   // ns per sample
        put32(o, 60); put32(o, 0);                                      // unity note, fraction
        put32(o, 0); put32(o, 0);                                       // SMPTE
        put32(o, 1); put32(o, 0);                                       // one loop, no extra data
        put32(o, 0); put32(o, 0);                                       // cue id, forward
        put32(o, static_cast<std::uint32_t>(data.loopStart));
        put32(o, static_cast<std::uint32_t>(data.loopEnd - 1));         // inclusive
        put32(o, 0); put32(o, 0);                                       // fraction, forever
    }
    const std::uint32_t riff = static_cast<std::uint32_t>(o.size() - 8);
    for (int i = 0; i < 4; ++i) o[4 + i] = static_cast<std::uint8_t>(riff >> (8 * i));

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (error) *error = "cannot write " + path;
        return false;
    }
    f.write(reinterpret_cast<const char*>(o.data()), static_cast<std::streamsize>(o.size()));
    if (!f) {
        if (error) *error = "writing " + path + " failed";
        return false;
    }
    return true;
}

} // namespace fitzel
