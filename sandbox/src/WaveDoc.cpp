#include "WaveDoc.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <filesystem>

namespace wave {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kUndoBytes = std::size_t(1) << 30;   // 1 GiB of history at most
constexpr std::size_t kUndoSteps = 60;

void clampRange(const Document& d, Frames& a, Frames& b) {
    const Frames n = d.frames();
    a = std::clamp<Frames>(a, 0, n);
    b = std::clamp<Frames>(b, 0, n);
    if (b < a) std::swap(a, b);
}

bool on(unsigned mask, int c) { return c < 32 && ((mask >> c) & 1u); }

float& at(Document& d, Frames f, int c) {
    return d.data.samples[static_cast<std::size_t>(f * d.channels() + c)];
}

// Keep the loop on the material it marked when [a, b) goes away.
void loopAfterErase(fitzel::SampleData& s, Frames a, Frames b) {
    if (!s.hasLoop()) return;
    auto fix = [&](std::int64_t& p) {
        if (p >= b) p -= (b - a);
        else if (p > a) p = a;
    };
    fix(s.loopStart);
    fix(s.loopEnd);
    if (!s.hasLoop()) s.loopStart = s.loopEnd = -1;
}

// ...and when `n` frames come in at `at`: in front of the loop start moves it,
// at its end stays outside.
void loopAfterInsert(fitzel::SampleData& s, Frames at, Frames n) {
    if (!s.hasLoop()) return;
    if (s.loopStart >= at) s.loopStart += n;
    if (s.loopEnd > at) s.loopEnd += n;
}

// Windowed-sinc resampling (Blackman, 32 taps; the cutoff drops with the rate
// when going down so nothing folds back).
std::vector<float> resampleBuf(const std::vector<float>& in, int ch, int from, int to) {
    if (from == to || ch <= 0) return in;
    const Frames n   = static_cast<Frames>(in.size()) / ch;
    const double r   = static_cast<double>(to) / from;
    const Frames out = static_cast<Frames>(std::llround(static_cast<double>(n) * r));
    const double cut = std::min(1.0, r) * 0.95;
    constexpr int kHalf = 16;
    std::vector<float> o(static_cast<std::size_t>(out * ch), 0.0f);
    for (Frames i = 0; i < out; ++i) {
        const double pos = static_cast<double>(i) / r;
        const Frames c0  = static_cast<Frames>(std::floor(pos));
        double acc[16] = {}, wsum = 0.0;
        for (Frames k = c0 - kHalf + 1; k <= c0 + kHalf; ++k) {
            const double x = pos - static_cast<double>(k);
            const double sx = x * cut;
            const double sinc = std::abs(sx) < 1e-9 ? 1.0 : std::sin(kPi * sx) / (kPi * sx);
            const double t = (x + kHalf) / (2.0 * kHalf);
            const double win = (t <= 0.0 || t >= 1.0) ? 0.0
                : 0.42 - 0.5 * std::cos(2.0 * kPi * t) + 0.08 * std::cos(4.0 * kPi * t);
            const double w = sinc * win;
            wsum += w;
            const Frames kk = std::clamp<Frames>(k, 0, n - 1);
            for (int c = 0; c < ch && c < 16; ++c) acc[c] += w * in[static_cast<std::size_t>(kk * ch + c)];
        }
        for (int c = 0; c < ch && c < 16; ++c)
            o[static_cast<std::size_t>(i * ch + c)] = static_cast<float>(wsum != 0.0 ? acc[c] / wsum : 0.0);
    }
    return o;
}

// A clip in the document's shape.
Clip conform(const Clip& c, int ch, int rate) {
    Clip o;
    o.channels = ch;
    o.rate     = rate;
    const Frames n = c.frames();
    o.samples.resize(static_cast<std::size_t>(n * ch));
    for (Frames f = 0; f < n; ++f) {
        float mono = 0.0f;
        for (int k = 0; k < c.channels; ++k) mono += c.samples[static_cast<std::size_t>(f * c.channels + k)];
        mono /= static_cast<float>(std::max(c.channels, 1));
        for (int k = 0; k < ch; ++k) {
            float v;
            if (c.channels == ch)      v = c.samples[static_cast<std::size_t>(f * c.channels + k)];
            else if (ch == 1)          v = mono;
            else if (c.channels == 1)  v = c.samples[static_cast<std::size_t>(f)];
            else                       v = c.samples[static_cast<std::size_t>(f * c.channels + k % c.channels)];
            o.samples[static_cast<std::size_t>(f * ch + k)] = v;
        }
    }
    if (c.rate > 0 && rate > 0 && c.rate != rate) o.samples = resampleBuf(o.samples, ch, c.rate, rate);
    return o;
}

float mixed(const Document& d, Frames f) {
    float m = 0.0f;
    for (int c = 0; c < d.channels(); ++c) m += d.sample(f, c);
    return m / static_cast<float>(std::max(d.channels(), 1));
}

float curve(FadeCurve k, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    switch (k) {
        case FadeCurve::Linear:      return t;
        case FadeCurve::EqualPower:  return std::sin(t * static_cast<float>(kPi) * 0.5f);
        case FadeCurve::Exponential: return (std::exp(4.0f * t) - 1.0f) / (std::exp(4.0f) - 1.0f);
        case FadeCurve::Logarithmic: return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        case FadeCurve::SCurve:      return 0.5f - 0.5f * std::cos(t * static_cast<float>(kPi));
        default:                     return t;
    }
}

void fft(std::vector<std::complex<float>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * static_cast<float>(kPi) / static_cast<float>(len);
        const std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k]           = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

} // namespace

const char* fadeCurveName(FadeCurve c) {
    switch (c) {
        case FadeCurve::Linear:      return "Linear";
        case FadeCurve::EqualPower:  return "Equal power";
        case FadeCurve::Exponential: return "Exponential";
        case FadeCurve::Logarithmic: return "Logarithmic";
        case FadeCurve::SCurve:      return "S-curve";
        default:                     return "?";
    }
}

float toDb(float linear) { return linear > 1e-10f ? 20.0f * std::log10(linear) : -200.0f; }
float fromDb(float db) { return std::pow(10.0f, db / 20.0f); }

// --- Document ------------------------------------------------------------------

bool Document::load(const std::string& file, std::string* error) {
    fitzel::SampleData s;
    if (!fitzel::loadSampleData(file, s, error)) return false;
    data  = std::move(s);
    path  = file;
    dirty = false;
    m_undo.clear();
    m_redo.clear();
    touched();
    dirty = false;
    return true;
}

void Document::newSilence(int ch, int r, double seconds) {
    data = fitzel::SampleData{};
    data.channels   = std::max(ch, 1);
    data.sampleRate = std::max(r, 1000);
    data.samples.assign(static_cast<std::size_t>(std::llround(seconds * data.sampleRate)) *
                            static_cast<std::size_t>(data.channels), 0.0f);
    path.clear();
    m_undo.clear();
    m_redo.clear();
    touched();
    dirty = false;
}

bool Document::save(const std::string& file, fitzel::WavFormat fmt, std::string* error) {
    if (!fitzel::saveWav(file, data, fmt, error)) return false;
    path  = file;
    dirty = false;
    data.sourceBits  = fmt == fitzel::WavFormat::Pcm16 ? 16 : fmt == fitzel::WavFormat::Pcm24 ? 24 : 32;
    data.sourceFloat = fmt == fitzel::WavFormat::Float32;
    return true;
}

float Document::sample(Frames f, int ch) const {
    if (f < 0 || f >= frames() || ch < 0 || ch >= channels()) return 0.0f;
    return data.samples[static_cast<std::size_t>(f * channels() + ch)];
}

std::string Document::name() const {
    return path.empty() ? std::string("Untitled") : std::filesystem::path(path).filename().string();
}

void Document::checkpoint(const char* label) {
    m_undo.push_back({data, label ? label : ""});
    m_redo.clear();
    std::size_t bytes = 0;
    for (const Step& s : m_undo) bytes += s.data.samples.size() * sizeof(float);
    while (m_undo.size() > 1 && (m_undo.size() > kUndoSteps || bytes > kUndoBytes)) {
        bytes -= m_undo.front().data.samples.size() * sizeof(float);
        m_undo.pop_front();
    }
}

const std::string& Document::undoLabel() const {
    static const std::string none;
    return m_undo.empty() ? none : m_undo.back().label;
}
const std::string& Document::redoLabel() const {
    static const std::string none;
    return m_redo.empty() ? none : m_redo.back().label;
}

bool Document::undo() {
    if (m_undo.empty()) return false;
    m_redo.push_back({std::move(data), m_undo.back().label});
    data = std::move(m_undo.back().data);
    m_undo.pop_back();
    touched();
    return true;
}

bool Document::redo() {
    if (m_redo.empty()) return false;
    m_undo.push_back({std::move(data), m_redo.back().label});
    data = std::move(m_redo.back().data);
    m_redo.pop_back();
    touched();
    return true;
}

void Document::touched() {
    ++m_version;
    dirty = true;
    rebuildLods();
}

void Document::rebuildLods() {
    m_lods.clear();
    const int ch = channels();
    const Frames n = frames();
    if (ch <= 0) return;
    // Level 0 from the samples, each next one from the one below it.
    for (int shift : {6, 10, 14}) {
        Lod l;
        l.shift = shift;
        const Frames block = Frames(1) << shift;
        const std::size_t nb = static_cast<std::size_t>((n + block - 1) / block);
        l.mn.assign(ch, std::vector<float>(nb, 0.0f));
        l.mx.assign(ch, std::vector<float>(nb, 0.0f));
        l.ms.assign(ch, std::vector<float>(nb, 0.0f));
        if (m_lods.empty()) {
            for (int c = 0; c < ch; ++c)
                for (std::size_t b = 0; b < nb; ++b) {
                    float lo = 1e9f, hi = -1e9f, sq = 0.0f;
                    const Frames f0 = static_cast<Frames>(b) * block, f1 = std::min(n, f0 + block);
                    for (Frames f = f0; f < f1; ++f) {
                        const float v = data.samples[static_cast<std::size_t>(f * ch + c)];
                        lo = std::min(lo, v); hi = std::max(hi, v); sq += v * v;
                    }
                    l.mn[c][b] = lo; l.mx[c][b] = hi;
                    l.ms[c][b] = sq / static_cast<float>(std::max<Frames>(f1 - f0, 1));
                }
        } else {
            const Lod& p = m_lods.back();
            const std::size_t k = std::size_t(1) << (shift - p.shift);
            for (int c = 0; c < ch; ++c)
                for (std::size_t b = 0; b < nb; ++b) {
                    float lo = 1e9f, hi = -1e9f, sq = 0.0f;
                    std::size_t cnt = 0;
                    for (std::size_t j = b * k; j < std::min(p.mn[c].size(), (b + 1) * k); ++j, ++cnt) {
                        lo = std::min(lo, p.mn[c][j]); hi = std::max(hi, p.mx[c][j]); sq += p.ms[c][j];
                    }
                    l.mn[c][b] = cnt ? lo : 0.0f; l.mx[c][b] = cnt ? hi : 0.0f;
                    l.ms[c][b] = cnt ? sq / static_cast<float>(cnt) : 0.0f;
                }
        }
        m_lods.push_back(std::move(l));
    }
}

// --- Operations ------------------------------------------------------------------

Clip copy(const Document& d, Frames a, Frames b) {
    clampRange(d, a, b);
    Clip c;
    c.channels = d.channels();
    c.rate     = d.rate();
    c.samples.assign(d.data.samples.begin() + static_cast<std::ptrdiff_t>(a * d.channels()),
                     d.data.samples.begin() + static_cast<std::ptrdiff_t>(b * d.channels()));
    return c;
}

void erase(Document& d, Frames a, Frames b) {
    clampRange(d, a, b);
    if (a == b) return;
    auto& s = d.data.samples;
    s.erase(s.begin() + static_cast<std::ptrdiff_t>(a * d.channels()),
            s.begin() + static_cast<std::ptrdiff_t>(b * d.channels()));
    loopAfterErase(d.data, a, b);
    d.touched();
}

void insert(Document& d, Frames at, const Clip& c) {
    if (c.frames() <= 0) return;
    at = std::clamp<Frames>(at, 0, d.frames());
    const Clip k = conform(c, d.channels(), d.rate());
    auto& s = d.data.samples;
    s.insert(s.begin() + static_cast<std::ptrdiff_t>(at * d.channels()), k.samples.begin(),
             k.samples.end());
    loopAfterInsert(d.data, at, k.frames());
    d.touched();
}

void crop(Document& d, Frames a, Frames b) {
    clampRange(d, a, b);
    if (a == b) return;
    const Frames n = d.frames();
    auto& s = d.data.samples;
    s.erase(s.begin() + static_cast<std::ptrdiff_t>(b * d.channels()), s.end());
    s.erase(s.begin(), s.begin() + static_cast<std::ptrdiff_t>(a * d.channels()));
    loopAfterErase(d.data, b, n);
    loopAfterErase(d.data, 0, a);
    d.touched();
}

void insertSilence(Document& d, Frames at, Frames n) {
    if (n <= 0) return;
    Clip c;
    c.channels = d.channels();
    c.rate     = d.rate();
    c.samples.assign(static_cast<std::size_t>(n * d.channels()), 0.0f);
    insert(d, at, c);
}

void silence(Document& d, Frames a, Frames b, unsigned mask) {
    clampRange(d, a, b);
    for (Frames f = a; f < b; ++f)
        for (int c = 0; c < d.channels(); ++c)
            if (on(mask, c)) at(d, f, c) = 0.0f;
    d.touched();
}

void gain(Document& d, Frames a, Frames b, float db, unsigned mask) {
    clampRange(d, a, b);
    const float g = fromDb(db);
    for (Frames f = a; f < b; ++f)
        for (int c = 0; c < d.channels(); ++c)
            if (on(mask, c)) at(d, f, c) *= g;
    d.touched();
}

float normalize(Document& d, Frames a, Frames b, float targetDb, unsigned mask) {
    clampRange(d, a, b);
    float peak = 0.0f;
    for (Frames f = a; f < b; ++f)
        for (int c = 0; c < d.channels(); ++c)
            if (on(mask, c)) peak = std::max(peak, std::abs(d.sample(f, c)));
    if (peak < 1e-9f) return 0.0f;
    const float db = targetDb - toDb(peak);
    gain(d, a, b, db, mask);
    return db;
}

void fade(Document& d, Frames a, Frames b, bool in, FadeCurve k, unsigned mask) {
    clampRange(d, a, b);
    const Frames n = b - a;
    if (n <= 0) return;
    for (Frames f = a; f < b; ++f) {
        // In: silent on the first frame, whole on the last; out the other way.
        const float t = n > 1 ? static_cast<float>(f - a) / static_cast<float>(n - 1) : 1.0f;
        const float g = in ? curve(k, t) : curve(k, 1.0f - t);
        for (int c = 0; c < d.channels(); ++c)
            if (on(mask, c)) at(d, f, c) *= g;
    }
    d.touched();
}

void reverse(Document& d, Frames a, Frames b, unsigned mask) {
    clampRange(d, a, b);
    for (Frames i = a, j = b - 1; i < j; ++i, --j)
        for (int c = 0; c < d.channels(); ++c)
            if (on(mask, c)) std::swap(at(d, i, c), at(d, j, c));
    d.touched();
}

void invert(Document& d, Frames a, Frames b, unsigned mask) {
    clampRange(d, a, b);
    for (Frames f = a; f < b; ++f)
        for (int c = 0; c < d.channels(); ++c)
            if (on(mask, c)) at(d, f, c) = -at(d, f, c);
    d.touched();
}

void removeDc(Document& d, Frames a, Frames b, unsigned mask) {
    clampRange(d, a, b);
    if (b <= a) return;
    for (int c = 0; c < d.channels(); ++c) {
        if (!on(mask, c)) continue;
        double sum = 0.0;
        for (Frames f = a; f < b; ++f) sum += d.sample(f, c);
        const float mean = static_cast<float>(sum / static_cast<double>(b - a));
        for (Frames f = a; f < b; ++f) at(d, f, c) -= mean;
    }
    d.touched();
}

void toMono(Document& d) {
    if (d.channels() <= 1) return;
    const Frames n = d.frames();
    std::vector<float> m(static_cast<std::size_t>(n));
    for (Frames f = 0; f < n; ++f) m[static_cast<std::size_t>(f)] = mixed(d, f);
    d.data.samples   = std::move(m);
    d.data.channels  = 1;
    d.touched();
}

void toStereo(Document& d) {
    if (d.channels() != 1) return;
    const Frames n = d.frames();
    std::vector<float> s(static_cast<std::size_t>(n * 2));
    for (Frames f = 0; f < n; ++f)
        s[static_cast<std::size_t>(2 * f)] = s[static_cast<std::size_t>(2 * f + 1)] =
            d.data.samples[static_cast<std::size_t>(f)];
    d.data.samples  = std::move(s);
    d.data.channels = 2;
    d.touched();
}

void resample(Document& d, int newRate) {
    if (newRate <= 0 || newRate == d.rate()) return;
    const double r = static_cast<double>(newRate) / d.rate();
    d.data.samples = resampleBuf(d.data.samples, d.channels(), d.rate(), newRate);
    if (d.data.hasLoop()) {
        d.data.loopStart = std::llround(static_cast<double>(d.data.loopStart) * r);
        d.data.loopEnd   = std::llround(static_cast<double>(d.data.loopEnd) * r);
    }
    d.data.sampleRate = newRate;
    d.touched();
}

Frames trimSilence(Document& d, float thresholdDb) {
    const float thr = fromDb(thresholdDb);
    const Frames n = d.frames();
    auto loud = [&](Frames f) {
        for (int c = 0; c < d.channels(); ++c)
            if (std::abs(d.sample(f, c)) > thr) return true;
        return false;
    };
    Frames a = 0, b = n;
    while (a < n && !loud(a)) ++a;
    while (b > a && !loud(b - 1)) --b;
    if (a == 0 && b == n) return 0;
    crop(d, a, b);
    return a;
}

// --- Loops ---------------------------------------------------------------------------

Frames nearestZeroCrossing(const Document& d, Frames f, Frames reach) {
    const Frames n = d.frames();
    auto crosses = [&](Frames x) {
        if (x <= 0 || x >= n) return false;
        const float p = mixed(d, x - 1), q = mixed(d, x);
        return q == 0.0f || (p < 0.0f) != (q < 0.0f);
    };
    for (Frames k = 0; k <= reach; ++k) {
        if (crosses(f + k)) return f + k;
        if (k > 0 && crosses(f - k)) return f - k;
    }
    return f;
}

Frames bestLoopEnd(const Document& d, Frames start, Frames from, Frames to, Frames window,
                   float* score) {
    const Frames n = d.frames();
    window = std::max<Frames>(16, window);
    from = std::max<Frames>(from, start + 1);
    to   = std::min<Frames>(to, n);
    if (to <= from || start < 0 || start + window > n) return -1;
    std::vector<float> ref(static_cast<std::size_t>(window));
    double refE = 0.0;
    for (Frames i = 0; i < window; ++i) {
        ref[static_cast<std::size_t>(i)] = mixed(d, start + i);
        refE += static_cast<double>(ref[static_cast<std::size_t>(i)]) * ref[static_cast<std::size_t>(i)];
    }
    // After the wrap the loop plays from `start`; so what follows a good end
    // sounds like what follows the start. Past the file's end it is silence.
    auto rate = [&](Frames c) {
        double dot = 0.0, e = 0.0;
        for (Frames i = 0; i < window; ++i) {
            const float v = mixed(d, c + i);
            dot += static_cast<double>(v) * ref[static_cast<std::size_t>(i)];
            e += static_cast<double>(v) * v;
        }
        if (refE < 1e-12 || e < 1e-12) return (refE < 1e-12 && e < 1e-12) ? 1.0 : 0.0;
        const double corr  = dot / std::sqrt(refE * e);
        const double level = std::min(refE, e) / std::max(refE, e);
        return std::max(0.0, corr) * std::sqrt(level);
    };
    const Frames step = std::max<Frames>(1, (to - from) / 4000);
    Frames best = from;
    double bestS = -1.0;
    for (Frames c = from; c < to; c += step) {
        const double s = rate(c);
        if (s > bestS) { bestS = s; best = c; }
    }
    if (step > 1) {
        const Frames lo = std::max(from, best - step), hi = std::min(to - 1, best + step);
        for (Frames c = lo; c <= hi; ++c) {
            const double s = rate(c);
            if (s > bestS) { bestS = s; best = c; }
        }
    }
    if (score) *score = static_cast<float>(std::max(0.0, bestS));
    return best;
}

bool crossfadeLoop(Document& d, Frames len) {
    auto& s = d.data;
    if (!s.hasLoop() || len <= 0 || s.loopStart - len < 0 || s.loopEnd - len < s.loopStart)
        return false;
    for (Frames i = 0; i < len; ++i) {
        const Frames x = s.loopEnd - len + i, y = s.loopStart - len + i;
        const float t = static_cast<float>(i + 1) / static_cast<float>(len);
        const float go = std::cos(t * static_cast<float>(kPi) * 0.5f);
        const float come = std::sin(t * static_cast<float>(kPi) * 0.5f);
        for (int c = 0; c < d.channels(); ++c) at(d, x, c) = at(d, x, c) * go + d.sample(y, c) * come;
    }
    d.touched();
    return true;
}

// --- Analysis ---------------------------------------------------------------------------

Stats stats(const Document& d, Frames a, Frames b) {
    clampRange(d, a, b);
    Stats st;
    const int ch = std::min(d.channels(), 2);
    for (int c = 0; c < ch; ++c) {
        double sq = 0.0, sum = 0.0;
        float  pk = 0.0f;
        for (Frames f = a; f < b; ++f) {
            const float v = d.sample(f, c);
            pk = std::max(pk, std::abs(v));
            sq += static_cast<double>(v) * v;
            sum += v;
            if (std::abs(v) >= 0.9999f) ++st.clipped;
        }
        const double n = static_cast<double>(std::max<Frames>(b - a, 1));
        st.peakDb[c] = toDb(pk);
        st.rmsDb[c]  = toDb(static_cast<float>(std::sqrt(sq / n)));
        st.dc[c]     = static_cast<float>(sum / n);
    }
    if (ch == 1) { st.peakDb[1] = st.peakDb[0]; st.rmsDb[1] = st.rmsDb[0]; st.dc[1] = st.dc[0]; }
    return st;
}

std::vector<float> spectrum(const Document& d, Frames a, Frames b, int bins) {
    constexpr int N = 2048;
    std::vector<float> out(static_cast<std::size_t>(std::max(bins, 1)), -120.0f);
    clampRange(d, a, b);
    if (b - a < 64 || d.rate() <= 0) return out;
    std::vector<double> power(N / 2, 0.0);
    std::vector<std::complex<float>> buf(N);
    const Frames span = b - a;
    const int windows = static_cast<int>(std::clamp<Frames>(span / (N / 2), 1, 256));
    for (int w = 0; w < windows; ++w) {
        const Frames s0 = windows > 1 ? a + (span - N) * w / (windows - 1) : a;
        for (int i = 0; i < N; ++i) {
            const Frames f = s0 + i;
            const float v = (f >= a && f < b) ? mixed(d, f) : 0.0f;
            const float hann = 0.5f - 0.5f * std::cos(2.0f * static_cast<float>(kPi) * i / (N - 1));
            buf[i] = {v * hann, 0.0f};
        }
        fft(buf);
        for (int k = 0; k < N / 2; ++k) power[k] += std::norm(buf[k]);
    }
    const double norm = (N / 4.0) * (N / 4.0) * windows;   // a full-scale sine = 0 dB
    const double nyq = d.rate() * 0.5;
    for (int i = 0; i < bins; ++i) {
        const double f0 = 20.0 * std::pow(nyq / 20.0, static_cast<double>(i) / bins);
        const double f1 = 20.0 * std::pow(nyq / 20.0, static_cast<double>(i + 1) / bins);
        int k0 = static_cast<int>(f0 / nyq * (N / 2)), k1 = static_cast<int>(f1 / nyq * (N / 2));
        k0 = std::clamp(k0, 1, N / 2 - 1);
        k1 = std::clamp(std::max(k1, k0 + 1), k0 + 1, N / 2);
        double m = 0.0;
        for (int k = k0; k < k1; ++k) m = std::max(m, power[k]);
        out[static_cast<std::size_t>(i)] =
            static_cast<float>(10.0 * std::log10(std::max(m / norm, 1e-12)));
    }
    return out;
}

} // namespace wave
