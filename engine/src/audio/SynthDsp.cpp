#include "fitzel/audio/SynthDsp.hpp"

#include <algorithm>
#include <cmath>

namespace fitzel::synth {

namespace {
constexpr double kPi  = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;
} // namespace

// =============================================================================
// Osc
// =============================================================================

void Osc::prepare(double sampleRate) {
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
    recompute();
}

void Osc::setWave(Wave w) { m_wave = w; }

void Osc::setFrequency(double hz) {
    m_frequency = hz;
    recompute();
}

void Osc::setFine(float hz) {
    m_fine = hz;
    recompute();
}

void Osc::setPulseWidth(float w) { m_pulseWidth = std::clamp(w, 0.01f, 0.99f); }

void Osc::reset() {
    m_phase   = 0.0;
    m_p       = 0.0f;
    m_dp      = 1.0f;
    m_saw     = 0.0f;
    m_lastSaw = 0.0f;
}

void Osc::recompute() {
    // Below zero is silence rather than a reflected tone: a modulator that dips
    // a frequency negative should fade the voice out, not make it climb again.
    const double f = std::max(0.0, m_frequency + static_cast<double>(m_fine));
    m_step  = f * kTau / m_sampleRate;
    // The saw's two constants, exactly as Synthlab derives them. pmax is half a
    // period in samples, and dc the offset that keeps the leaky integrator from
    // walking away from zero.
    m_pmax  = static_cast<float>(0.5 * m_sampleRate / std::max(f, 1e-6));
    m_dc    = -0.498f / m_pmax;
}

float Osc::process() {
    switch (m_wave) {
    case Wave::Sine: {
        m_phase += m_step;
        if (m_phase >= kTau) m_phase -= kTau;   // the wrap the original lacked
        return static_cast<float>(std::sin(m_phase));
    }
    case Wave::Pulse: {
        // The original derives the pulse by hard-clipping a sine, which is a
        // square and nothing else. Comparing the phase against the width is the
        // same waveform at 0.5 and gives the patch a width to sweep, which is
        // most of what a pulse is worth having for.
        m_phase += m_step;
        if (m_phase >= kTau) m_phase -= kTau;
        return (m_phase < kTau * static_cast<double>(m_pulseWidth)) ? 1.0f : -1.0f;
    }
    case Wave::Saw: {
        // Synthlab's leaky-integrated BLIT, carried over as it stands.
        m_p += m_dp;
        if (m_p < 0.0f) {
            m_p  = -m_p;
            m_dp = -m_dp;
        } else if (m_p > m_pmax) {
            m_p  = m_pmax + m_pmax - m_p;
            m_dp = -m_dp;
        }
        float x = static_cast<float>(kPi) * m_p;
        if (x < 0.00001f) x = 0.00001f;         // don't divide by 0
        m_saw = m_leak * m_saw + m_dc + std::sin(x) / x;
        if (m_saw > 1.0f) m_saw = m_lastSaw;    // the original's runaway guard
        else              m_lastSaw = m_saw;
        return m_saw;
    }
    case Wave::Noise:
    default: {
        // xorshift32, in place of juce::Random. Full scale, unlike the
        // original's quarter-amplitude noise: levels belong to the patch, and a
        // primitive that is quietly 12 dB down is a trap when one module's
        // output is compared against another's.
        m_rng ^= m_rng << 13;
        m_rng ^= m_rng >> 17;
        m_rng ^= m_rng << 5;
        return static_cast<float>(static_cast<std::int32_t>(m_rng)) *
               (1.0f / 2147483648.0f);
    }
    }
}

// =============================================================================
// Adsr
// =============================================================================

void Adsr::prepare(double sampleRate) {
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
    recompute();
}

void Adsr::setAttack(float seconds)  { m_attack  = std::max(0.0f, seconds); recompute(); }
void Adsr::setDecay(float seconds)   { m_decay   = std::max(0.0f, seconds); recompute(); }
void Adsr::setRelease(float seconds) { m_release = std::max(0.0f, seconds); recompute(); }
void Adsr::setSustain(float level)   { m_sustain = std::clamp(level, 0.0f, 1.0f); recompute(); }

void Adsr::reset() {
    m_state  = State::Idle;
    m_output = 0.0f;
}

void Adsr::gate(bool on) {
    if (on) m_state = State::Attack;
    else if (m_state != State::Idle) m_state = State::Release;
}

namespace {
// Redmon's coefficient: how fast a segment approaches its target, given the
// segment's length in samples and how far past the target it aims.
float calcCoef(float rateSamples, float targetRatio) {
    if (rateSamples <= 0.0f) return 0.0f;
    return std::exp(-std::log((1.0f + targetRatio) / targetRatio) / rateSamples);
}
} // namespace

void Adsr::recompute() {
    const float sr = static_cast<float>(m_sampleRate);
    const float a  = m_attack * sr;
    const float d  = m_decay * sr;
    const float r  = m_release * sr;

    m_attackCoef  = calcCoef(a, m_ratioA);
    m_attackBase  = (1.0f + m_ratioA) * (1.0f - m_attackCoef);
    m_decayCoef   = calcCoef(d, m_ratioDR);
    m_decayBase   = (m_sustain - m_ratioDR) * (1.0f - m_decayCoef);
    m_releaseCoef = calcCoef(r, m_ratioDR);
    m_releaseBase = -m_ratioDR * (1.0f - m_releaseCoef);
}

float Adsr::process() {
    switch (m_state) {
    case State::Idle:
        break;
    case State::Attack:
        m_output = m_attackBase + m_output * m_attackCoef;
        if (m_output >= 1.0f) {
            m_output = 1.0f;
            m_state  = State::Decay;
        }
        break;
    case State::Decay:
        m_output = m_decayBase + m_output * m_decayCoef;
        if (m_output <= m_sustain) {
            m_output = m_sustain;
            m_state  = State::Sustain;
        }
        break;
    case State::Sustain:
        break;
    case State::Release:
        m_output = m_releaseBase + m_output * m_releaseCoef;
        // Not "== 0": the curve approaches zero and never arrives, so a voice
        // waiting for exactly zero to free itself waits forever.
        if (m_output <= 0.0001f) {
            m_output = 0.0f;
            m_state  = State::Idle;
        }
        break;
    }
    return m_output;
}

// =============================================================================
// Biquad
// =============================================================================

void Biquad::prepare(double sampleRate) {
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    reset();
    recompute();
}

void Biquad::setMode(Mode m) {
    m_mode = m;
    recompute();
}

void Biquad::set(float cutoffHz, float q) {
    // 20 Hz to just under Nyquist, and never a Q small enough to divide by.
    const float nyquist = static_cast<float>(m_sampleRate * 0.5);
    m_cutoff = std::clamp(cutoffHz, 20.0f, nyquist * 0.98f);
    m_q      = std::clamp(q, 0.05f, 20.0f);
    recompute();
}

void Biquad::reset() { m_z1 = m_z2 = 0.0f; }

void Biquad::recompute() {
    const float w0    = static_cast<float>(kTau) * m_cutoff / static_cast<float>(m_sampleRate);
    const float cosw0 = std::cos(w0);
    const float alpha = std::sin(w0) / (2.0f * m_q);

    float b0 = 0, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (m_mode) {
    case Mode::LowPass:
        b0 = (1.0f - cosw0) * 0.5f; b1 = 1.0f - cosw0; b2 = b0;
        a0 = 1.0f + alpha;          a1 = -2.0f * cosw0; a2 = 1.0f - alpha;
        break;
    case Mode::HighPass:
        b0 = (1.0f + cosw0) * 0.5f; b1 = -(1.0f + cosw0); b2 = b0;
        a0 = 1.0f + alpha;          a1 = -2.0f * cosw0;   a2 = 1.0f - alpha;
        break;
    case Mode::BandPass:
        b0 = alpha;        b1 = 0.0f;          b2 = -alpha;
        a0 = 1.0f + alpha; a1 = -2.0f * cosw0; a2 = 1.0f - alpha;
        break;
    }
    const float inv = 1.0f / a0;
    m_b0 = b0 * inv; m_b1 = b1 * inv; m_b2 = b2 * inv;
    m_a1 = a1 * inv; m_a2 = a2 * inv;
}

float Biquad::process(float in) {
    // Transposed direct form II: two state variables, and the one that behaves
    // best when the coefficients are changed while it runs -- which is every
    // block here, because a patch sweeps its cutoff.
    const float out = m_b0 * in + m_z1;
    m_z1 = m_b1 * in - m_a1 * out + m_z2;
    m_z2 = m_b2 * in - m_a2 * out;
    return out;
}

// =============================================================================
// DelayLine
// =============================================================================

void DelayLine::prepare(double sampleRate, float maxSeconds) {
    m_sampleRate = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int n  = std::max(4, static_cast<int>(m_sampleRate * std::max(0.001f, maxSeconds)));
    m_buffer.assign(static_cast<std::size_t>(n), 0.0f);
    m_write = 0;
}

void DelayLine::reset() {
    std::fill(m_buffer.begin(), m_buffer.end(), 0.0f);
    m_write = 0;
}

void DelayLine::write(float in) {
    if (m_buffer.empty()) return;
    m_buffer[static_cast<std::size_t>(m_write)] = in;
    m_write = (m_write + 1) % static_cast<int>(m_buffer.size());
}

float DelayLine::read(float delaySeconds) const {
    if (m_buffer.empty()) return 0.0f;
    const int   n   = static_cast<int>(m_buffer.size());
    // One sample is the shortest read that is behind the write head; asking for
    // less would read the sample about to be overwritten.
    const float want = std::clamp(delaySeconds * static_cast<float>(m_sampleRate),
                                  1.0f, static_cast<float>(n - 2));
    float pos = static_cast<float>(m_write) - want;
    while (pos < 0.0f) pos += static_cast<float>(n);

    const int   i0   = static_cast<int>(pos);
    const int   i1   = (i0 + 1) % n;
    const float frac = pos - static_cast<float>(i0);   // the fraction the original threw away
    return m_buffer[static_cast<std::size_t>(i0)] * (1.0f - frac) +
           m_buffer[static_cast<std::size_t>(i1)] * frac;
}

// =============================================================================
// Drive
// =============================================================================

void Drive::setShape(Shape s) { m_shape = s; }

void Drive::set(float drive, float mix) {
    m_drive = std::max(0.0f, drive);
    m_mix   = std::clamp(mix, 0.0f, 1.0f);
}

float Drive::process(float in) {
    const float x = in * m_drive;
    float       y = x;
    switch (m_shape) {
    case Shape::Soft:
        // Cubic soft clip, the original's mode 1: bends before it flattens.
        if (x < -1.0f)      y = -2.0f / 3.0f;
        else if (x > 1.0f)  y =  2.0f / 3.0f;
        else                y = x - (x * x * x) / 3.0f;
        break;
    case Shape::Hard:
        y = std::clamp(x, -1.0f, 1.0f);
        break;
    case Shape::Arctan:
        y = (2.0f / static_cast<float>(kPi)) * std::atan(x);
        break;
    case Shape::Fold:
        // Foldback: past the threshold it turns back on itself instead of
        // flattening, which is what makes it sound like nothing else here.
        while (y > 1.0f || y < -1.0f) {
            if (y > 1.0f)  y =  2.0f - y;
            if (y < -1.0f) y = -2.0f - y;
        }
        break;
    }
    return (1.0f - m_mix) * in + m_mix * y;
}

// =============================================================================
// Chorus
// =============================================================================

void Chorus::prepare(double sampleRate) {
    // Room for the centre plus the deepest wander the panel allows.
    m_delay.prepare(sampleRate, 0.08f);
    m_lfo.prepare(sampleRate);
    m_lfo.setWave(Osc::Wave::Sine);
    m_lfo.setFrequency(0.6);
    reset();
}

void Chorus::set(float rateHz, float depthSeconds, float mix) {
    m_lfo.setFrequency(std::clamp(rateHz, 0.01f, 20.0f));
    m_depth = std::clamp(depthSeconds, 0.0f, 0.01f);
    m_mix   = std::clamp(mix, 0.0f, 1.0f);
}

void Chorus::reset() {
    m_delay.reset();
    m_lfo.reset();
}

float Chorus::process(float in) {
    const float wander = m_lfo.process() * m_depth;
    m_delay.write(in);
    const float wet = m_delay.read(m_centre + wander);
    return (1.0f - m_mix) * in + m_mix * wet;
}

// =============================================================================
// Reverb
// =============================================================================

void Reverb::prepare(double sampleRate) {
    // Schroeder's comb lengths, in seconds and deliberately not in any simple
    // ratio: delays that share a factor line their repeats up and the tail
    // rings on one note instead of blurring.
    static const float kComb[4]    = {0.0297f, 0.0371f, 0.0411f, 0.0437f};
    static const float kAllpass[2] = {0.0050f, 0.0017f};
    for (int i = 0; i < 4; ++i) {
        m_comb[i].time = kComb[i];
        m_comb[i].line.prepare(sampleRate, kComb[i] * 2.0f + 0.01f);
    }
    for (int i = 0; i < 2; ++i) {
        m_apTime[i] = kAllpass[i];
        m_allpass[i].prepare(sampleRate, kAllpass[i] * 2.0f + 0.01f);
    }
    set(0.5f, 0.4f, 0.3f);
    reset();
}

void Reverb::set(float size, float damp, float mix) {
    const float s = std::clamp(size, 0.0f, 1.0f);
    m_damp = std::clamp(damp, 0.0f, 0.95f);
    m_mix  = std::clamp(mix, 0.0f, 1.0f);
    // Feedback below 1, always: the comb filters are a loop, and a tail that
    // gains energy every pass is a patch that gets louder until it clips.
    for (Comb& c : m_comb) c.feed = 0.7f + 0.28f * s;
}

void Reverb::reset() {
    for (Comb& c : m_comb) {
        c.line.reset();
        c.store = 0.0f;
    }
    for (DelayLine& a : m_allpass) a.reset();
}

float Reverb::process(float in) {
    float wet = 0.0f;
    for (Comb& c : m_comb) {
        const float out = c.line.read(c.time);
        // One-pole damping inside the loop: each pass loses more of the top,
        // which is what a real room does and what keeps a long tail from
        // turning into a metallic ring.
        c.store = out * (1.0f - m_damp) + c.store * m_damp;
        c.line.write(in + c.store * c.feed);
        wet += out;
    }
    wet *= 0.25f;
    for (int i = 0; i < 2; ++i) {
        const float delayed = m_allpass[i].read(m_apTime[i]);
        const float v       = wet + delayed * 0.5f;
        m_allpass[i].write(v);
        wet = delayed - v * 0.5f;
    }
    return (1.0f - m_mix) * in + m_mix * wet;
}

} // namespace fitzel::synth
