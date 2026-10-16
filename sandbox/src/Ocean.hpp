#pragma once

#include <complex>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

// The open water's waves: a spectral ocean in the manner of Tessendorf
// ("Simulating Ocean Water"), the method behind every convincing sea in a game.
//
// WHY A SPECTRUM AND NOT A HANDFUL OF WAVES. Water was four Gerstner waves,
// and four waves are a pattern: the eye finds the repeat within seconds and the
// surface reads as a moving texture. Real water is thousands of waves of every
// length at once, each travelling at the speed its length gives it (long waves
// outrun short ones), with the energy spread among them in a way oceanography
// measured decades ago. That spread is the spectrum -- here JONSWAP, from the
// wind speed and how far the wind has blown over the water -- and an inverse FFT
// turns one random sample of it into a surface. Nothing about it is a pattern.
//
// WHY THE AUTHOR SETS WIND AND NOT WAVE HEIGHT. A wave height alone describes a
// pond and an ocean equally badly: the same 30 cm is a choppy lake (short,
// steep, close together) or a glassy swell (long, low, far apart). Wind speed
// and FETCH -- the distance the wind has had to build the sea -- say which, and
// they are what anybody who has stood by water already knows: a pond in a gale
// stays a pond.
//
// WHY ON THE CPU. Floating things must ride the surface that is DRAWN, and a
// GPU surface can only be asked about a frame or two late through a readback.
// Here the same numbers feed both, so a boat sits in its own trough. It runs on
// a worker thread, one frame behind the clock, which nobody can see in water.
//
// No GL here: OceanRenderer.hpp uploads the results. That keeps this testable
// (tools/oceancheck.cpp) without a window.
namespace ocean {

// The author's dials. Everything else is derived.
struct Settings {
    float windSpeed  = 6.0f;     // m/s, ten metres above the water
    float windDir    = 30.0f;    // degrees: the direction the wind blows TOWARD (0 = +x)
    float fetch      = 8000.0f;  // m of open water upwind -- what keeps a pond calm
    float choppiness = 0.85f;    // 0 = round crests, ~1.2 = sharp crests (horizontal pull)
    float swell      = 0.0f;     // 0..1: long waves from a storm far away
    int   detail     = 128;      // grid per cascade: 64, 128 or 256
};

// Three patches of different size, each carrying its own band of wavelengths:
// the long rollers, the waves on them, and the ripples on those. Sizes that do
// not divide one another, so their repeats never line up -- and the largest
// near a kilometre, because a patch repeats, and from a few hundred metres up a
// 250 m one showed its foam and its swells laid out on a grid.
constexpr int   kCascades = 3;
constexpr float kPatch[kCascades] = {953.0f, 117.0f, 15.1f};

// One cascade's surface, sampled on its grid: displacement (x, height, z) with
// the foam the folding crests left in w, and the slope of the surface.
struct Field {
    std::vector<glm::vec4> disp;    // N*N: dx, dy, dz, foam
    std::vector<glm::vec2> slope;   // N*N: d(height)/dx, d(height)/dz
};

struct Surface {
    float     height = 0.0f;        // above the still water level
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec3 velocity{0.0f};       // of the water there (orbital motion), m/s
};

class Ocean {
public:
    Ocean();
    ~Ocean();
    Ocean(const Ocean&) = delete;
    Ocean& operator=(const Ocean&) = delete;

    // Apply the author's settings. Cheap when nothing changed; otherwise the
    // spectrum is resampled -- with the SAME random numbers, so turning the wind
    // up grows the waves that are there rather than dealing a new sea.
    void configure(const Settings& s);
    const Settings& settings() const { return m_set; }

    // Once a frame, on the main thread: take the surface the worker finished
    // and start it on the next. `time` in seconds.
    void update(double time);
    // The same, finished before it returns -- for tests and offline renders.
    void computeNow(double time);

    int   size() const { return m_n; }
    // Bumped whenever a new surface becomes current, for whoever uploads it.
    std::uint64_t generation() const { return m_gen; }
    const Field& field(int cascade) const { return m_front[cascade]; }

    // --- Asking the surface --------------------------------------------------
    // The displacement of the water that started at (x, z) at rest.
    glm::vec3 displacementAt(float x, float z) const;
    // The height of the surface ABOVE (x, z) -- not the same thing: a choppy
    // crest leans, and the water under a point came from somewhere else.
    float     heightAt(float x, float z) const;
    Surface   surfaceAt(float x, float z) const;
    // How much foam the folding crests have left at the water that rests at
    // (x, z): 0 none, ~1 a breaking crest. Where spray flies off in a gale.
    float     foamAt(float x, float z) const;

    // The significant wave height (the average of the highest third, the number
    // a forecast gives), metres. From the spectrum, so it holds for any instant.
    float significantHeight() const { return m_hs; }
    // Wavelength of the waves carrying the most energy, metres.
    float peakWavelength() const { return m_peakLambda; }

private:
    struct Cascade {
        std::vector<std::complex<float>> h0;    // h0(k)
        std::vector<std::complex<float>> h0mk;  // conj(h0(-k)), precomputed
        std::vector<float>     omega;   // dispersion: angular frequency per k
        std::vector<glm::vec2> k;       // wave vector per grid cell
    };

    void rebuildSpectrum();
    void allocate(std::vector<Field>& f) const;
    // Compute the surface at `time` into `out`. `prevFoam` carries the foam over.
    void compute(double time, double dt, std::vector<Field>& out,
                 const std::vector<Field>& prevFoam);
    glm::vec3 sampleDisp(const std::vector<Field>& f, float x, float z) const;
    void workerLoop();
    void waitIdle(std::unique_lock<std::mutex>& lk);

    Settings m_set;
    bool     m_configured = false;
    int      m_n = 0;
    float    m_hs = 0.0f, m_peakLambda = 0.0f;
    std::vector<Cascade> m_casc;
    // Seeds per cell, drawn once: the sea keeps its identity across changes.
    std::vector<glm::vec4> m_noise;    // two complex gaussians per cell

    std::vector<Field> m_front, m_back, m_prev;
    double m_frontTime = 0.0, m_prevTime = 0.0;
    std::uint64_t m_gen = 0;

    // FFT tables for the current size, and each cascade's scratch: the four
    // packed spectra as separate real and imaginary planes (see Ocean.cpp).
    struct Scratch {
        std::vector<float> re[4], im[4];
        std::vector<float> colRe, colIm;
    };
    std::vector<int>   m_bitrev;
    std::vector<float> m_twRe, m_twIm;
    Scratch            m_scratch[kCascades];
    void computeCascade(int c, float t, float decay, Field& out, const Field& prevFoam);

    std::thread             m_worker;
    std::mutex              m_mx;
    std::condition_variable m_cv;
    bool   m_pending = false, m_busy = false, m_done = false, m_quit = false;
    double m_jobTime = 0.0, m_jobDt = 0.0;
};

// Wind speed (m/s) to Beaufort force and to a word an author recognises.
int         beaufort(float windSpeed);
const char* beaufortName(int force);

// The wind that, blowing for ever over open water, raises a sea of this
// significant height (Pierson-Moskowitz). For reading scenes that only knew a
// wave height.
float windForHeight(float significantHeight);

} // namespace ocean
