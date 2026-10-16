#include "Ocean.hpp"

#include <algorithm>
#include <cmath>
#include <random>

namespace ocean {

namespace {

using cf = std::complex<float>;

constexpr float kG   = 9.81f;
constexpr float kPi  = 3.14159265358979f;
// How long foam lingers after the crest that made it has passed, seconds.
constexpr float kFoamLife = 2.6f;

// The boundary between two cascades, as a wavenumber: waves shorter than a
// quarter of the finer patch belong to it, longer ones to the coarser. A
// quarter, so every cascade still has several cells per wave at its short end.
float bandEdge(int cascade) {
    return 2.0f * kPi / kPatch[cascade] * 4.0f;
}

// JONSWAP, the spectrum measured in the North Sea and used ever since: how much
// energy sits at each frequency for a wind of speed U that has blown over F
// metres of water. Short fetch moves the peak to short, steep waves and takes
// the energy away -- which is what a pond in a gale is.
float jonswap(float w, float U, float F, float gamma) {
    if (w <= 1e-4f || U <= 0.05f) return 0.0f;
    F = std::max(F, 10.0f);
    float alpha = 0.076f * std::pow(U * U / (F * kG), 0.22f);
    float wp    = 22.0f * std::pow(kG * kG / (U * F), 1.0f / 3.0f);
    // Past a long enough fetch the sea stops growing: fully developed
    // (Pierson-Moskowitz), and the formulas above would keep going.
    const float wpFull = 0.855f * kG / U;
    if (wp < wpFull) { wp = wpFull; alpha = 0.0081f; }
    alpha = std::max(alpha, 0.0081f);
    const float sigma = w <= wp ? 0.07f : 0.09f;
    const float d     = (w - wp) / (sigma * wp);
    const float r     = std::exp(-0.5f * d * d);
    const float q     = wp / w;
    return alpha * kG * kG / std::pow(w, 5.0f) * std::exp(-1.25f * q * q * q * q) *
           std::pow(gamma, r);
}

// How the energy spreads around the wind's direction: cos^2s(theta / 2), a
// wide lobe for a wind sea (it is choppy, waves run every which way within it)
// and a narrow one for swell (it has come a long way and kept its heading).
float spreadNorm(float s) {
    double sum = 0.0;
    const int n = 720;
    for (int i = 0; i < n; ++i) {
        const double th = -kPi + (i + 0.5) * 2.0 * kPi / n;
        sum += std::pow(std::cos(th * 0.5), 2.0 * s) * (2.0 * kPi / n);
    }
    return static_cast<float>(1.0 / sum);
}
float spread(float theta, float s, float norm) {
    return norm * std::pow(std::fabs(std::cos(theta * 0.5f)), 2.0f * s);
}

// The 2-D spectrum at wave vector (kx, kz): energy per unit k-area. The
// frequency spectrum carried over to wavenumbers (deep water: w = sqrt(g k)),
// times the spread around the wind.
struct Spectrum {
    float U, F, windRad, swell;
    float seaNorm, swellNorm;
    float operator()(float kx, float kz) const {
        const float k = std::sqrt(kx * kx + kz * kz);
        if (k < 1e-5f) return 0.0f;
        const float w     = std::sqrt(kG * k);
        const float dwdk  = kG / (2.0f * w);
        const float theta = std::atan2(kz, kx);
        float s = jonswap(w, U, F, 3.3f) * spread(theta - windRad, 8.0f, seaNorm);
        if (swell > 0.0f)
            // A distant storm's sea: long, low, regular, from a little off the
            // wind -- it was raised by a different one.
            s += swell * swell * jonswap(w, 9.0f, 4.0e5f, 7.0f) *
                 spread(theta - windRad - 0.45f, 40.0f, swellNorm);
        // Below a few centimetres surface tension takes over from gravity and
        // the spectrum falls away; without this the finest cascade fizzes.
        const float damp = std::exp(-k * k * 0.0004f);
        return s * dwdk / k * damp;
    }
};

// An in-place radix-2 transform, inverse sign (e^{+i}), unnormalised: the
// surface is the plain sum of its waves. Real and imaginary parts in separate
// planes and every pointer restrict-qualified -- written over std::complex the
// same loop ran four times slower, the compiler unable to prove the data and the
// tables apart.
void fft(float* __restrict re, float* __restrict im, int n, const int* __restrict rev,
         const float* __restrict twr, const float* __restrict twi) {
    for (int i = 0; i < n; ++i) {
        const int r = rev[i];
        if (i < r) {
            std::swap(re[i], re[r]);
            std::swap(im[i], im[r]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int j = 0; j < half; ++j) {
                const float wr = twr[j * step], wi = twi[j * step];
                const float br = re[i + j + half], bi = im[i + j + half];
                const float vr = br * wr - bi * wi, vi = br * wi + bi * wr;
                const float ur = re[i + j], ui = im[i + j];
                re[i + j]        = ur + vr;
                im[i + j]        = ui + vi;
                re[i + j + half] = ur - vr;
                im[i + j + half] = ui - vi;
            }
    }
}

int clampDetail(int d) { return d <= 64 ? 64 : (d >= 256 ? 256 : 128); }

} // namespace

Ocean::Ocean() : m_worker([this] { workerLoop(); }) {}

Ocean::~Ocean() {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_quit = true;
    }
    m_cv.notify_all();
    m_worker.join();
}

void Ocean::waitIdle(std::unique_lock<std::mutex>& lk) {
    m_cv.wait(lk, [&] { return !m_pending && !m_busy; });
}

void Ocean::allocate(std::vector<Field>& f) const {
    f.assign(kCascades, Field{});
    for (Field& c : f) {
        c.disp.assign(static_cast<std::size_t>(m_n) * m_n, glm::vec4(0.0f));
        c.slope.assign(static_cast<std::size_t>(m_n) * m_n, glm::vec2(0.0f));
    }
}

void Ocean::configure(const Settings& in) {
    Settings s = in;
    s.detail     = clampDetail(s.detail);
    s.windSpeed  = std::clamp(s.windSpeed, 0.0f, 40.0f);
    s.fetch      = std::clamp(s.fetch, 10.0f, 1.0e6f);
    s.choppiness = std::clamp(s.choppiness, 0.0f, 2.0f);
    s.swell      = std::clamp(s.swell, 0.0f, 1.0f);
    if (m_configured && s.windSpeed == m_set.windSpeed && s.windDir == m_set.windDir &&
        s.fetch == m_set.fetch && s.choppiness == m_set.choppiness &&
        s.swell == m_set.swell && s.detail == m_set.detail)
        return;

    std::unique_lock<std::mutex> lk(m_mx);
    waitIdle(lk);
    const bool resized = !m_configured || s.detail != m_n;
    m_set = s;
    if (resized) {
        m_n = s.detail;
        allocate(m_front);
        allocate(m_back);
        allocate(m_prev);
        m_done = false;
        // FFT tables for this size.
        int bits = 0;
        while ((1 << bits) < m_n) ++bits;
        m_bitrev.assign(static_cast<std::size_t>(m_n), 0);
        for (int i = 0; i < m_n; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            m_bitrev[static_cast<std::size_t>(i)] = r;
        }
        m_twRe.resize(static_cast<std::size_t>(m_n / 2));
        m_twIm.resize(static_cast<std::size_t>(m_n / 2));
        for (int j = 0; j < m_n / 2; ++j) {
            const float a = 2.0f * kPi * static_cast<float>(j) / static_cast<float>(m_n);
            m_twRe[static_cast<std::size_t>(j)] = std::cos(a);   // inverse: e^{+i}
            m_twIm[static_cast<std::size_t>(j)] = std::sin(a);
        }
        for (Scratch& sc : m_scratch) {
            for (int f = 0; f < 4; ++f) {
                sc.re[f].assign(static_cast<std::size_t>(m_n) * m_n, 0.0f);
                sc.im[f].assign(static_cast<std::size_t>(m_n) * m_n, 0.0f);
            }
            sc.colRe.assign(static_cast<std::size_t>(m_n), 0.0f);
            sc.colIm.assign(static_cast<std::size_t>(m_n), 0.0f);
        }
        // One set of random numbers for the life of this size: change the wind
        // and the same sea grows or calms, rather than a new one being dealt.
        std::mt19937 rng(0x5eaf00du);
        std::normal_distribution<float> nd(0.0f, 1.0f);
        m_noise.resize(static_cast<std::size_t>(kCascades) * m_n * m_n);
        for (glm::vec4& v : m_noise) v = glm::vec4(nd(rng), nd(rng), nd(rng), nd(rng));
    }
    rebuildSpectrum();
    m_configured = true;
    if (resized) {
        // Something to draw from the first frame, not a flat sea for one.
        compute(m_frontTime, 0.0, m_front, m_back);
        m_prev = m_front;
        m_prevTime = m_frontTime;
        ++m_gen;
    }
}

void Ocean::rebuildSpectrum() {
    const int N = m_n;
    Spectrum spec{m_set.windSpeed, m_set.fetch, m_set.windDir * kPi / 180.0f, m_set.swell,
                  spreadNorm(8.0f), spreadNorm(40.0f)};
    m_casc.assign(kCascades, Cascade{});
    double variance = 0.0;
    for (int c = 0; c < kCascades; ++c) {
        Cascade& cs = m_casc[static_cast<std::size_t>(c)];
        const std::size_t nn = static_cast<std::size_t>(N) * N;
        cs.h0.assign(nn, cf(0.0f));
        cs.h0mk.assign(nn, cf(0.0f));
        cs.omega.assign(nn, 0.0f);
        cs.k.assign(nn, glm::vec2(0.0f));
        const float L  = kPatch[c];
        const float dk = 2.0f * kPi / L;
        const float kLo = c == 0 ? 0.0f : bandEdge(c);
        const float kHi = c == kCascades - 1 ? 1e9f : bandEdge(c + 1);
        for (int n = 0; n < N; ++n)
            for (int m = 0; m < N; ++m) {
                const std::size_t i = static_cast<std::size_t>(n) * N + m;
                const int mm = m < N / 2 ? m : m - N;
                const int nm = n < N / 2 ? n : n - N;
                const glm::vec2 kv(dk * static_cast<float>(mm), dk * static_cast<float>(nm));
                const float k = glm::length(kv);
                cs.k[i]     = kv;
                cs.omega[i] = std::sqrt(kG * k);
                if (k < kLo || k >= kHi || k < 1e-6f) continue;
                const float S = spec(kv.x, kv.y);
                variance += static_cast<double>(S) * dk * dk;
                const glm::vec4& r = m_noise[static_cast<std::size_t>(c) * nn + i];
                // E|h0|^2 = S dk^2 / 2: the k and -k terms of the real surface
                // between them carry the cell's whole share of the variance.
                const float amp = std::sqrt(S * dk * dk * 0.5f) / std::sqrt(2.0f);
                // Waves under a micrometre are none at all -- and the spectrum's
                // far tails are numbers so small they go denormal, which every
                // multiply in the transforms then pays for a hundredfold.
                if (!(amp > 1e-7f)) continue;
                cs.h0[i] = cf(r.x * amp, r.y * amp);
            }
        for (int n = 0; n < N; ++n)
            for (int m = 0; m < N; ++m) {
                const int m2 = (N - m) % N, n2 = (N - n) % N;
                cs.h0mk[static_cast<std::size_t>(n) * N + m] =
                    std::conj(cs.h0[static_cast<std::size_t>(n2) * N + m2]);
            }
    }
    m_hs = 4.0f * static_cast<float>(std::sqrt(variance));
    // The peak, from the same formulas the spectrum uses.
    const float U = std::max(m_set.windSpeed, 0.05f);
    float wp = 22.0f * std::pow(kG * kG / (U * m_set.fetch), 1.0f / 3.0f);
    wp = std::max(wp, 0.855f * kG / U);
    m_peakLambda = 2.0f * kPi * kG / (wp * wp);
}

void Ocean::compute(double time, double dt, std::vector<Field>& out,
                    const std::vector<Field>& prevFoam) {
    const float t = static_cast<float>(std::fmod(time, 3600.0));   // keep the phase precise
    const float decay = std::exp(-static_cast<float>(std::max(dt, 0.0)) / kFoamLife);
    // The cascades share nothing but read-only tables: one each on its own
    // thread, the first on this one.
    std::thread others[kCascades - 1];
    for (int c = 1; c < kCascades; ++c)
        others[c - 1] = std::thread([&, c] {
            computeCascade(c, t, decay, out[static_cast<std::size_t>(c)],
                           prevFoam[static_cast<std::size_t>(c)]);
        });
    computeCascade(0, t, decay, out[0], prevFoam[0]);
    for (std::thread& th : others) th.join();
}

void Ocean::computeCascade(int c, float t, float decay, Field& f, const Field& pf) {
    const int N = m_n;
    const std::size_t nn = static_cast<std::size_t>(N) * N;
    const float lambda = m_set.choppiness;
    // Where a crest counts as breaking. Each cascade sees only its own band,
    // and the short steep waves that break first in a real gale are what a
    // linear spectrum is thinnest on -- so the stronger the wind, the earlier a
    // fold counts: whitecaps from a fresh breeze up (Beaufort 4), the sea
    // streaked white in a gale, as an observer would describe them.
    const float wind   = std::clamp((m_set.windSpeed - 5.0f) / 15.0f, 0.0f, 1.0f);
    const float foamAt = 0.5f + 0.45f * wind;
    const Cascade& cs = m_casc[static_cast<std::size_t>(c)];
    Scratch& sc = m_scratch[c];

    for (std::size_t i = 0; i < nn; ++i) {
        const glm::vec2 kv = cs.k[i];
        const float k = glm::length(kv);
        const cf a = cs.h0[i], b = cs.h0mk[i];
        if (k < 1e-6f || (a.real() == 0.0f && a.imag() == 0.0f && b.real() == 0.0f && b.imag() == 0.0f)) {
            for (int fi = 0; fi < 4; ++fi) { sc.re[fi][i] = 0.0f; sc.im[fi][i] = 0.0f; }
            continue;
        }
        // h(k, t): this cell's wave travelling WITH k (so the energy the
        // spectrum put downwind goes downwind), and its mirror image that makes
        // the sum real -- h0 e^{-i ph} + conj(h0(-k)) e^{+i ph}, by hand.
        const float ph = cs.omega[i] * t;
        const float co = std::cos(ph), si = std::sin(ph);
        const float hr = a.real() * co + a.imag() * si + b.real() * co - b.imag() * si;
        const float hi = a.imag() * co - a.real() * si + b.imag() * co + b.real() * si;
        const float kx = kv.x / k, kz = kv.y / k;
        // Choppy displacement (-i k/|k| h) and the derivatives the slopes and
        // the folding test need -- two real fields packed into each transform
        // as A + iB:  -i*c*h = (c*hi, -c*hr),  i*c*h = (-c*hi, c*hr).
        const float dxr = kx * hi,  dxi = -kx * hr;          // Dx
        const float dzr = kz * hi,  dzi = -kz * hr;          // Dz
        const float xx = kv.x * kx, zz = kv.y * kz, xz = kv.x * kz;
        const float sxr = -kv.x * hi, sxi = kv.x * hr;       // dh/dx
        const float szr = -kv.y * hi, szi = kv.y * hr;       // dh/dz
        sc.re[0][i] = dxr - dzi;          sc.im[0][i] = dxi + dzr;           // Dx + i Dz
        sc.re[1][i] = hr - xx * hi;       sc.im[1][i] = hi + xx * hr;        // h + i dDx/dx
        sc.re[2][i] = sxr - szi;          sc.im[2][i] = sxi + szr;           // dh/dx + i dh/dz
        sc.re[3][i] = zz * hr - xz * hi;  sc.im[3][i] = zz * hi + xz * hr;   // dDz/dz + i dDx/dz
    }
    const int* rev = m_bitrev.data();
    const float* twr = m_twRe.data();
    const float* twi = m_twIm.data();
    for (int fi = 0; fi < 4; ++fi) {
        float* re = sc.re[fi].data();
        float* im = sc.im[fi].data();
        for (int r = 0; r < N; ++r) fft(re + static_cast<std::size_t>(r) * N, im + static_cast<std::size_t>(r) * N, N, rev, twr, twi);
        float* cr = sc.colRe.data();
        float* ci = sc.colIm.data();
        for (int col = 0; col < N; ++col) {
            for (int r = 0; r < N; ++r) {
                cr[r] = re[static_cast<std::size_t>(r) * N + col];
                ci[r] = im[static_cast<std::size_t>(r) * N + col];
            }
            fft(cr, ci, N, rev, twr, twi);
            for (int r = 0; r < N; ++r) {
                re[static_cast<std::size_t>(r) * N + col] = cr[r];
                im[static_cast<std::size_t>(r) * N + col] = ci[r];
            }
        }
    }
    for (std::size_t i = 0; i < nn; ++i) {
        const float dx = sc.re[0][i] * lambda, dz = sc.im[0][i] * lambda;
        const float dy = sc.re[1][i];
        const float jxx = 1.0f + lambda * sc.im[1][i];
        const float jzz = 1.0f + lambda * sc.re[3][i];
        const float jxz = lambda * sc.im[3][i];
        // Where the surface folds over itself (Jacobian below about a half) a
        // crest is breaking. Foam appears at once and fades over a couple of
        // seconds, so it trails behind the crest that made it.
        const float J = jxx * jzz - jxz * jxz;
        const float fresh = std::clamp((foamAt - J) * 2.2f, 0.0f, 1.0f);
        const float foam = std::max(pf.disp.empty() ? 0.0f : pf.disp[i].w * decay, fresh);
        f.disp[i]  = glm::vec4(dx, dy, dz, foam);
        // The slope of the DISPLACED surface: the horizontal squeeze of a
        // choppy crest makes it steeper than its height alone says.
        f.slope[i] = glm::vec2(sc.re[2][i] / std::max(jxx, 0.25f), sc.im[2][i] / std::max(jzz, 0.25f));
    }
}

void Ocean::workerLoop() {
    std::unique_lock<std::mutex> lk(m_mx);
    for (;;) {
        m_cv.wait(lk, [&] { return m_pending || m_quit; });
        if (m_quit) return;
        m_pending = false;
        m_busy    = true;
        const double t = m_jobTime, dt = m_jobDt;
        lk.unlock();
        compute(t, dt, m_back, m_front);
        lk.lock();
        m_busy = false;
        m_done = true;
        m_cv.notify_all();
    }
}

void Ocean::update(double time) {
    std::unique_lock<std::mutex> lk(m_mx);
    if (!m_configured) return;
    if (m_done) {
        // prev <- front <- back, and the old prev becomes the next back.
        std::swap(m_prev, m_front);
        std::swap(m_front, m_back);
        m_prevTime  = m_frontTime;
        m_frontTime = m_jobTime;
        m_done = false;
        ++m_gen;
    }
    if (!m_pending && !m_busy) {
        m_jobTime = time;
        m_jobDt   = std::clamp(time - m_frontTime, 0.0, 0.25);
        m_pending = true;
        lk.unlock();
        m_cv.notify_all();
    }
}

void Ocean::computeNow(double time) {
    std::unique_lock<std::mutex> lk(m_mx);
    if (!m_configured) return;
    waitIdle(lk);
    m_done = false;
    compute(time, std::clamp(time - m_frontTime, 0.0, 0.25), m_back, m_front);
    std::swap(m_prev, m_front);
    std::swap(m_front, m_back);
    m_prevTime  = m_frontTime;
    m_frontTime = time;
    ++m_gen;
}

glm::vec3 Ocean::sampleDisp(const std::vector<Field>& f, float x, float z) const {
    glm::vec3 sum(0.0f);
    if (f.empty() || m_n == 0) return sum;
    const int N = m_n, mask = N - 1;
    for (int c = 0; c < kCascades; ++c) {
        const float fx = x / kPatch[c] * static_cast<float>(N);
        const float fz = z / kPatch[c] * static_cast<float>(N);
        const float x0f = std::floor(fx), z0f = std::floor(fz);
        const float ax = fx - x0f, az = fz - z0f;
        const int x0 = static_cast<int>(x0f) & mask, z0 = static_cast<int>(z0f) & mask;
        const int x1 = (x0 + 1) & mask, z1 = (z0 + 1) & mask;
        const auto& d = f[static_cast<std::size_t>(c)].disp;
        auto at = [&](int xi, int zi) {
            return glm::vec3(d[static_cast<std::size_t>(zi) * N + xi]);
        };
        sum += glm::mix(glm::mix(at(x0, z0), at(x1, z0), ax),
                        glm::mix(at(x0, z1), at(x1, z1), ax), az);
    }
    return sum;
}

glm::vec3 Ocean::displacementAt(float x, float z) const { return sampleDisp(m_front, x, z); }

float Ocean::heightAt(float x, float z) const {
    // The water above (x, z) started somewhere else and was carried here by
    // the horizontal displacement. Three fixed-point steps find where: the
    // displacement is smooth and small against its own wavelength, so it
    // converges long before the third.
    glm::vec2 p(x, z);
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 d = sampleDisp(m_front, p.x, p.y);
        p = glm::vec2(x - d.x, z - d.z);
    }
    return sampleDisp(m_front, p.x, p.y).y;
}

Surface Ocean::surfaceAt(float x, float z) const {
    Surface s;
    if (m_front.empty() || m_n == 0) return s;
    glm::vec2 p(x, z);
    for (int i = 0; i < 3; ++i) {
        const glm::vec3 d = sampleDisp(m_front, p.x, p.y);
        p = glm::vec2(x - d.x, z - d.z);
    }
    const glm::vec3 d = sampleDisp(m_front, p.x, p.y);
    s.height = d.y;
    // Slopes, summed over the cascades at the same point.
    glm::vec2 sl(0.0f);
    const int N = m_n, mask = N - 1;
    for (int c = 0; c < kCascades; ++c) {
        const float fx = p.x / kPatch[c] * static_cast<float>(N);
        const float fz = p.y / kPatch[c] * static_cast<float>(N);
        const float x0f = std::floor(fx), z0f = std::floor(fz);
        const float ax = fx - x0f, az = fz - z0f;
        const int x0 = static_cast<int>(x0f) & mask, z0 = static_cast<int>(z0f) & mask;
        const int x1 = (x0 + 1) & mask, z1 = (z0 + 1) & mask;
        const auto& g = m_front[static_cast<std::size_t>(c)].slope;
        auto at = [&](int xi, int zi) { return g[static_cast<std::size_t>(zi) * N + xi]; };
        sl += glm::mix(glm::mix(at(x0, z0), at(x1, z0), ax), glm::mix(at(x0, z1), at(x1, z1), ax), az);
    }
    s.normal = glm::normalize(glm::vec3(-sl.x, 1.0f, -sl.y));
    const double span = m_frontTime - m_prevTime;
    if (span > 1e-4 && !m_prev.empty())
        s.velocity = (d - sampleDisp(m_prev, p.x, p.y)) / static_cast<float>(span);
    return s;
}

float Ocean::foamAt(float x, float z) const {
    if (m_front.empty() || m_n == 0) return 0.0f;
    const int N = m_n, mask = N - 1;
    float sum = 0.0f;
    for (int c = 0; c < 2; ++c) {   // the two big patches: the ripples do not break
        const float fx = x / kPatch[c] * static_cast<float>(N);
        const float fz = z / kPatch[c] * static_cast<float>(N);
        const int x0 = static_cast<int>(std::floor(fx)) & mask, z0 = static_cast<int>(std::floor(fz)) & mask;
        sum += m_front[static_cast<std::size_t>(c)].disp[static_cast<std::size_t>(z0) * N + x0].w *
               (c == 0 ? 0.8f : 1.0f);
    }
    return std::min(sum, 1.0f);
}

int beaufort(float v) {
    static const float edge[] = {0.3f, 1.6f, 3.4f, 5.5f, 8.0f, 10.8f,
                                 13.9f, 17.2f, 20.8f, 24.5f, 28.5f, 32.7f};
    int f = 0;
    while (f < 12 && v >= edge[f]) ++f;
    return f;
}

const char* beaufortName(int f) {
    static const char* names[] = {"calm", "light air", "light breeze", "gentle breeze",
                                  "moderate breeze", "fresh breeze", "strong breeze",
                                  "near gale", "gale", "strong gale", "storm",
                                  "violent storm", "hurricane"};
    return names[std::clamp(f, 0, 12)];
}

float windForHeight(float hs) {
    return std::sqrt(std::max(hs, 0.0f) * kG / 0.21f);
}

} // namespace ocean
