// The music check: does MusicPlayer keep time, and does the band analysis see
// the beats a rhythm game builds its chart from?
//
// A rhythm game is judged in milliseconds, and neither half can be judged by
// ear. So:
//   * ANALYSIS (offline, no device): a synthetic track -- a kick every 0.5 s
//     (120 BPM) and a hi-hat on the off-beats -- is written as a WAV and
//     analysed. The bass flux has to peak on the kicks and nowhere else, the
//     high flux on the hats, and the flux of each band has to average 1.
//   * CLOCK (real device, skipped without one): a song started at 10 s has to
//     report ~10 s minus the device latency, run at 1.00x the wall clock, never
//     step backwards, stand still while paused and go on from there, and start
//     a lead-in (play(-1)) at -1 s.
//
//   build/release/bin/musiccheck.exe [song.mp3] [--dump bands.lua]
// With a song, the clock checks run on it and --dump writes its band analysis
// as a Lua table (for testing a game's beat tracking with a plain lua.exe).
// Exits non-zero if any check fails.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "fitzel/audio/Audio.hpp"
#include "fitzel/audio/MusicPlayer.hpp"

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}

void put16(std::FILE* f, std::uint16_t v) { std::fwrite(&v, 2, 1, f); }
void put32(std::FILE* f, std::uint32_t v) { std::fwrite(&v, 4, 1, f); }

bool writeWav(const std::string& path, const std::vector<float>& s, int rate) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const std::uint32_t bytes = static_cast<std::uint32_t>(s.size() * 2);
    std::fwrite("RIFF", 1, 4, f); put32(f, 36 + bytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); put32(f, 16); put16(f, 1); put16(f, 1);
    put32(f, static_cast<std::uint32_t>(rate)); put32(f, static_cast<std::uint32_t>(rate * 2));
    put16(f, 2); put16(f, 16);
    std::fwrite("data", 1, 4, f); put32(f, bytes);
    for (float v : s) {
        const auto q = static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767));
        std::fwrite(&q, 2, 1, f);
    }
    std::fclose(f);
    return true;
}

// Kick every 0.5 s from 1.0 s on, hat every 0.5 s from 1.25 s on, 12 s long.
std::vector<float> synthTrack(int rate) {
    std::vector<float> s(static_cast<std::size_t>(rate * 12), 0.0f);
    std::uint32_t noise = 12345;
    for (double t0 = 1.0; t0 < 11.5; t0 += 0.5) {
        const auto i0 = static_cast<std::size_t>(t0 * rate);
        double ph = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(0.25 * rate); ++i) {
            const double t = static_cast<double>(i) / rate;
            ph += 2 * 3.14159265 * (50 + 90 * std::exp(-t * 30)) / rate;
            s[i0 + i] += static_cast<float>(0.8 * std::sin(ph) * std::exp(-t * 12));
        }
        const auto h0 = static_cast<std::size_t>((t0 + 0.25) * rate);
        float prev = 0;
        for (std::size_t i = 0; i < static_cast<std::size_t>(0.05 * rate); ++i) {
            noise = noise * 1664525u + 1013904223u;
            const float n = static_cast<float>(noise >> 8) / 8388608.0f - 1.0f;
            const float hp = n - prev;    // crude high-pass: a hat is all top end
            prev = n;
            s[h0 + i] += 0.3f * hp * std::exp(-static_cast<float>(i) / rate * 60.0f);
        }
    }
    return s;
}

// Frame index of the largest value within +-w frames of `f`.
bool peakNear(const std::vector<float>& v, double sec, double fr, int w, float minRatio,
              float& ratio) {
    const int f = static_cast<int>(std::lround(sec * fr));
    float best = 0, around = 0;
    int   cnt  = 0;
    for (int i = f - 40; i <= f + 40; ++i) {
        if (i < 0 || i >= static_cast<int>(v.size())) continue;
        if (std::abs(i - f) <= w) best = std::max(best, v[i]);
        else { around += v[i]; ++cnt; }
    }
    ratio = best / (around / std::max(1, cnt) + 1e-6f);
    return ratio >= minRatio;
}

void checkAnalysis() {
    std::printf("analysis (synthetic 120 BPM kick + off-beat hat)\n");
    const std::string wav =
        (std::filesystem::temp_directory_path() / "fitzel_musiccheck.wav").string();
    if (!writeWav(wav, synthTrack(44100), 44100)) { check(false, "write the test WAV"); return; }
    fitzel::music::Bands b;
    std::string err;
    const bool ok = fitzel::music::analyzeBands(wav, b, &err);
    check(ok, ("analyse it" + (ok ? std::string() : ": " + err)).c_str());
    std::filesystem::remove(wav);
    if (!ok) return;
    const double fr = static_cast<double>(b.rate) / b.hop;
    check(std::abs(b.duration - 12.0) < 0.05, "duration 12 s");
    for (int k = 0; k < 3; ++k) {
        double m = 0;
        for (float v : b.flux[k]) m += v;
        m /= static_cast<double>(b.flux[k].size());
        if (std::abs(m - 1.0) > 0.01) { check(false, "band flux averages 1"); return; }
    }
    check(true, "every band's flux averages 1");
    int kicks = 0, hats = 0, n = 0;
    float r = 0, worstK = 1e9f, worstH = 1e9f;
    for (double t0 = 1.5; t0 < 11.0; t0 += 0.5, ++n) {
        if (peakNear(b.flux[0], t0, fr, 2, 6.0f, r)) ++kicks;
        worstK = std::min(worstK, r);
        if (peakNear(b.flux[2], t0 + 0.25, fr, 2, 6.0f, r)) ++hats;
        worstH = std::min(worstH, r);
    }
    char line[160];
    std::snprintf(line, sizeof line, "bass flux peaks on every kick (%d/%d, weakest %.1fx)", kicks, n, worstK);
    check(kicks == n, line);
    std::snprintf(line, sizeof line, "high flux peaks on every hat (%d/%d, weakest %.1fx)", hats, n, worstH);
    check(hats == n, line);
    // And the bass must NOT see the hats.
    float leak = 0, base = 0;
    for (double t0 = 1.5; t0 < 11.0; t0 += 0.5) {
        leak = std::max(leak, b.flux[0][static_cast<std::size_t>(std::lround((t0 + 0.25) * fr))]);
        base = std::max(base, b.flux[0][static_cast<std::size_t>(std::lround(t0 * fr))]);
    }
    std::snprintf(line, sizeof line, "the bass does not hear the hats (%.2f vs %.2f)", leak, base);
    check(leak < base * 0.1f, line);
}

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void checkClock(const std::string& song) {
    std::printf("clock (%s)\n", song.c_str());
    fitzel::Audio audio;
    if (!audio.ok()) { std::printf("  [skip] no audio device\n"); return; }
    std::string err;
    fitzel::MusicPlayer p = fitzel::MusicPlayer::create(audio, &err);
    check(p.isValid(), "create the voice");
    if (!p.isValid()) return;
    audio.setMasterVolume(0.15f);
    check(p.load(song, &err), ("load" + (err.empty() ? std::string() : ": " + err)).c_str());
    if (!p.loaded()) return;
    std::printf("  duration %.2f s, rate %d\n", p.duration(), p.sampleRate());

    p.play(10.0);
    const double t0 = p.time();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const double w0 = now(), s0 = p.time();
    // A jump is the song moving by a different amount than the wall clock did
    // between two looks (a Windows sleep of "2 ms" is often 15).
    double prev = s0, prevW = w0, maxStep = 0;
    bool   back = false;
    while (now() - w0 < 2.0) {
        const double t = p.time(), w = now();
        if (t < prev) back = true;
        maxStep = std::max(maxStep, std::abs((t - prev) - (w - prevW)));
        prev    = t;
        prevW   = w;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const double w1 = now(), s1 = p.time();
    char line[160];
    std::snprintf(line, sizeof line, "starts at 10 s minus the latency (%.3f)", t0);
    check(t0 > 9.7 && t0 <= 10.0, line);
    const double rate = (s1 - s0) / (w1 - w0);
    std::snprintf(line, sizeof line, "runs at the wall clock's pace (%.4fx)", rate);
    check(std::abs(rate - 1.0) < 0.005, line);
    check(!back, "never steps backwards");
    std::snprintf(line, sizeof line, "no jumps (largest step off the wall clock %.2f ms)", maxStep * 1000);
    check(maxStep < 0.004, line);

    std::vector<float> spec;
    p.spectrum(spec);
    float loud = 0;
    for (float v : spec) loud = std::max(loud, v);
    std::snprintf(line, sizeof line, "the spectrum sees the song (loudest bin %.2f)", loud);
    check(spec.size() == 512 && loud > 0.3f, line);

    p.pause();
    const double pa = p.time();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::snprintf(line, sizeof line, "stands still while paused (%.4f -> %.4f)", pa, p.time());
    check(std::abs(p.time() - pa) < 1e-9, line);
    p.resume();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const double re = p.time();
    std::snprintf(line, sizeof line, "goes on from the pause (%.3f after 0.2 s)", re - pa);
    check(re - pa > 0.12 && re - pa < 0.3, line);

    p.play(-1.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const double li = p.time();
    std::snprintf(line, sizeof line, "a lead-in counts up from -1 s (%.3f after 0.1 s)", li);
    check(li > -1.0 && li < -0.8, line);

    p.setFilter(300.0f, 1.0f, 0.0f, 0.0f);
    p.play(20.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    p.spectrum(spec);
    p.stop();
    const double st = p.time();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    check(std::abs(p.time() - st) < 1e-9, "stops the clock on stop");
}

void dump(const std::string& song, const std::string& out) {
    fitzel::music::Bands b;
    std::string err;
    const auto a = std::chrono::steady_clock::now();
    if (!fitzel::music::analyzeBands(song, b, &err)) {
        std::printf("dump: %s\n", err.c_str());
        ++g_fail;
        return;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
    std::FILE* f = std::fopen(out.c_str(), "w");
    if (!f) { ++g_fail; return; }
    std::fprintf(f, "return {duration=%.6f, rate=%d, hop=%d, frames=%zu,\n", b.duration, b.rate,
                 b.hop, b.flux[0].size());
    const char* names[6] = {"lowFlux", "midFlux", "highFlux", "lowRms", "midRms", "highRms"};
    for (int k = 0; k < 6; ++k) {
        const std::vector<float>& v = k < 3 ? b.flux[k] : b.rms[k - 3];
        std::fprintf(f, "%s={", names[k]);
        for (std::size_t i = 0; i < v.size(); ++i) std::fprintf(f, "%.7g,", v[i]);
        std::fprintf(f, "},\n");
    }
    std::fprintf(f, "}\n");
    std::fclose(f);
    std::printf("dump: %s -> %s (%.1f s, analysed in %.0f ms)\n", song.c_str(), out.c_str(),
                b.duration, ms);
}

} // namespace

int main(int argc, char** argv) {
    std::string song, dumpTo;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--dump" && i + 1 < argc) dumpTo = argv[++i];
        else song = a;
    }
    checkAnalysis();
    if (!song.empty()) {
        if (!dumpTo.empty()) dump(song, dumpTo);
        checkClock(song);
    }
    std::printf(g_fail ? "\nmusiccheck: %d FAILED\n" : "\nmusiccheck: all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
