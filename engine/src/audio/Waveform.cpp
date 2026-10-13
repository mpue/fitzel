#include "fitzel/audio/Waveform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

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

int bucketOf(const Waveform& w, float t) {
    return static_cast<int>(std::floor(t * w.rate));
}

} // namespace

float Waveform::peakAt(float t) const {
    const int i = bucketOf(*this, t);
    return (i >= 0 && i < static_cast<int>(peak.size())) ? peak[i] : 0.0f;
}

float Waveform::rmsAt(float t) const {
    const int i = bucketOf(*this, t);
    return (i >= 0 && i < static_cast<int>(rms.size())) ? rms[i] : 0.0f;
}

float Waveform::peakOver(float t0, float t1) const {
    int a = std::max(0, bucketOf(*this, std::min(t0, t1)));
    int b = std::min(static_cast<int>(peak.size()) - 1, bucketOf(*this, std::max(t0, t1)));
    float m = 0.0f;
    for (int i = a; i <= b; ++i) m = std::max(m, peak[i]);
    return m;
}

float Waveform::rmsOver(float t0, float t1) const {
    int a = std::max(0, bucketOf(*this, std::min(t0, t1)));
    int b = std::min(static_cast<int>(rms.size()) - 1, bucketOf(*this, std::max(t0, t1)));
    if (b < a) return 0.0f;
    float sum = 0.0f;
    for (int i = a; i <= b; ++i) sum += rms[i] * rms[i];
    return std::sqrt(sum / static_cast<float>(b - a + 1));
}

bool readWaveform(const std::string& path, Waveform& out, float bucketsPerSecond) {
    out = Waveform{};
    out.rate = std::max(1.0f, bucketsPerSecond);
    const std::vector<std::uint8_t> bytes = vfs::read(path);
    if (bytes.empty()) return false;

    // Mono f32 at the file's own rate: summing the channels is exactly the
    // fold-down the strip shows, and keeping the rate avoids a resampler for a
    // number that only has to be roughly right.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0);
    ma_decoder dec;
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &dec) != MA_SUCCESS) return false;

    const ma_uint32 sr = dec.outputSampleRate > 0 ? dec.outputSampleRate : 48000;
    const double perBucket = static_cast<double>(sr) / out.rate;
    std::vector<float> buf(4096);
    double sq = 0.0, inBucket = 0.0;
    float  pk = 0.0f;
    std::uint64_t total = 0;
    for (;;) {
        ma_uint64 got = 0;
        if (ma_decoder_read_pcm_frames(&dec, buf.data(), buf.size(), &got) != MA_SUCCESS && got == 0)
            break;
        if (got == 0) break;
        for (ma_uint64 i = 0; i < got; ++i) {
            const float v = buf[static_cast<std::size_t>(i)];
            pk = std::max(pk, std::fabs(v));
            sq += static_cast<double>(v) * v;
            inBucket += 1.0;
            if (inBucket >= perBucket) {
                out.peak.push_back(std::min(1.0f, pk));
                out.rms.push_back(std::min(1.0f, static_cast<float>(std::sqrt(sq / inBucket))));
                pk = 0.0f; sq = 0.0; inBucket -= perBucket;
                if (inBucket < 0.0) inBucket = 0.0;
            }
        }
        total += got;
    }
    if (inBucket > 0.5) {
        out.peak.push_back(std::min(1.0f, pk));
        out.rms.push_back(std::min(1.0f, static_cast<float>(std::sqrt(sq / inBucket))));
    }
    ma_decoder_uninit(&dec);
    out.seconds = static_cast<float>(static_cast<double>(total) / sr);
    return total > 0;
}

float soundFileSeconds(const std::string& path) {
    const std::vector<std::uint8_t> bytes = vfs::read(path);
    if (bytes.empty()) return 0.0f;
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0);
    ma_decoder dec;
    if (ma_decoder_init_memory(bytes.data(), bytes.size(), &cfg, &dec) != MA_SUCCESS) return 0.0f;
    ma_uint64 frames = 0;
    float secs = 0.0f;
    if (ma_decoder_get_length_in_pcm_frames(&dec, &frames) == MA_SUCCESS && dec.outputSampleRate > 0)
        secs = static_cast<float>(static_cast<double>(frames) / dec.outputSampleRate);
    ma_decoder_uninit(&dec);
    return secs;
}

} // namespace fitzel
