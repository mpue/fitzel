#pragma once

#include <cstdint>
#include <vector>

// The sound-making primitives the modular synth is built from: oscillators, an
// envelope, a filter and a delay line. Nothing in here knows about graphs,
// patches or the audio device -- each one is a small object you feed a sample
// rate and then ask for one sample at a time.
//
// Most of these are ported from Synthlab (D:/devel/Synthlab,
// Source/AudioEngine/), which is the same author's modular synthesiser. Only
// the JUCE-free parts came over, and where the original leaned on JUCE the
// dependency was replaced rather than carried in: JUCE's audio modules are
// permissively licensed but its GUI ones are not, and fitzel ships closed
// binaries and targets the browser, where JUCE does not go.
//
// Ported file by file, with the original named in the comment above each class
// so the two can still be compared.
namespace fitzel::synth {

// An oscillator. One object, four waveforms, because a modular patch changes
// which one it wants and a class per shape would mean rebuilding the graph to
// turn a saw into a pulse.
//
// From Synthlab Source/AudioEngine/{Oszillator,Sine,Sawtooth,Pulse,WhiteNoise}.
class Osc {
public:
    enum class Wave { Sine, Saw, Pulse, Noise };

    void  prepare(double sampleRate);
    void  setWave(Wave w);
    Wave  wave() const { return m_wave; }
    // Hz. Cheap enough to call per sample, which is what frequency modulation
    // does -- an engine note is never at one pitch for long.
    void  setFrequency(double hz);
    // Detune in Hz, added to the frequency (the "fine" of the original).
    void  setFine(float hz);
    // Pulse width, 0..1, for the pulse wave; 0.5 is a square.
    void  setPulseWidth(float w);
    void  reset();
    float process();

private:
    Wave     m_wave       = Wave::Sine;
    double   m_sampleRate = 48000.0;
    double   m_frequency  = 440.0;
    float    m_fine       = 0.0f;
    float    m_pulseWidth = 0.5f;

    // Sine and pulse run off a phase in radians, as the originals do. The phase
    // is wrapped every cycle, which the original did not do: an editor left open
    // for an afternoon feeds it hundreds of millions of radians, and a double's
    // precision there is coarse enough to hear as the tone going gritty.
    double   m_phase = 0.0;
    double   m_step  = 0.0;

    // The saw is Synthlab's leaky-integrated BLIT, kept as it was: the shape is
    // the one that project's saw has always had.
    float    m_p = 0.0f, m_dp = 1.0f, m_saw = 0.0f, m_lastSaw = 0.0f;
    float    m_pmax = 100.0f, m_dc = 0.0f;
    const float m_leak = 0.995f;

    // Noise: a small xorshift instead of juce::Random. Deterministic per voice,
    // which is what makes a rendered check reproducible.
    std::uint32_t m_rng = 0x9E3779B9u;

    void recompute();
};

// The classic four-stage envelope, with exponential-ish segments.
//
// From Synthlab Source/AudioEngine/ADSR.{h,cpp}, which is in turn Nigel
// Redmon's EarLevel envelope (earlevel.com, 2012). His header grants free use
// including commercial, which is why this one could come over unchanged.
class Adsr {
public:
    void  prepare(double sampleRate);
    // All four in SECONDS, unlike the original's raw sample counts: a patch file
    // that said "attack: 2400" would mean a different envelope at every sample
    // rate, and the rate is the device's, not the patch's.
    void  setAttack(float seconds);
    void  setDecay(float seconds);
    void  setSustain(float level);
    void  setRelease(float seconds);
    void  gate(bool on);
    bool  idle() const { return m_state == State::Idle; }
    void  reset();
    float process();

private:
    enum class State { Idle, Attack, Decay, Sustain, Release };

    double m_sampleRate = 48000.0;
    State  m_state      = State::Idle;
    float  m_output     = 0.0f;
    float  m_attack = 0.01f, m_decay = 0.1f, m_sustain = 0.7f, m_release = 0.2f;
    float  m_attackCoef = 0.0f, m_decayCoef = 0.0f, m_releaseCoef = 0.0f;
    float  m_attackBase = 0.0f, m_decayBase = 0.0f, m_releaseBase = 0.0f;
    // How far past the target each segment aims, which is what gives the curve
    // its shape. Redmon's defaults.
    float  m_ratioA = 0.3f, m_ratioDR = 0.0001f;

    void recompute();
};

// A biquad, in the three modes a patch actually reaches for. Written here
// rather than ported: Synthlab's filters are juce::IIRFilter underneath, and
// that is one of the JUCE modules fitzel cannot take. The coefficients are the
// standard RBJ cookbook ones, so the sound is the same family.
class Biquad {
public:
    enum class Mode { LowPass, HighPass, BandPass };

    void  prepare(double sampleRate);
    void  setMode(Mode m);
    // Cutoff in Hz and resonance as Q (0.707 is flat, higher rings). Clamped
    // below Nyquist: a cutoff swept past it by a modulator makes the
    // coefficients blow up, and a filter that explodes takes the whole mix with
    // it rather than just going quiet.
    void  set(float cutoffHz, float q);
    void  reset();
    float process(float in);

private:
    double m_sampleRate = 48000.0;
    Mode   m_mode       = Mode::LowPass;
    float  m_cutoff = 1000.0f, m_q = 0.707f;
    float  m_a1 = 0, m_a2 = 0, m_b0 = 1, m_b1 = 0, m_b2 = 0;
    float  m_z1 = 0, m_z2 = 0;

    void recompute();
};

// A delay line read at a fractional position, so the delay time can be moved
// without stepping between samples.
//
// From Synthlab Source/AudioEngine/FractionalDelayBuffer.{h,cpp}, with its
// interpolation repaired: the original took the read position as an `int`, so
// the fraction was always zero and it interpolated between a sample and itself.
class DelayLine {
public:
    // `maxSeconds` is what the buffer is sized for; asking for a longer delay
    // later reads the oldest sample there is rather than off the end.
    void  prepare(double sampleRate, float maxSeconds);
    void  reset();
    void  write(float in);
    // `delaySeconds` back from the write head.
    float read(float delaySeconds) const;

private:
    double             m_sampleRate = 48000.0;
    std::vector<float> m_buffer;
    int                m_write = 0;
};

// Waveshaping distortion: gain into a non-linearity, mixed back with the dry
// signal. From Synthlab Source/AudioEngine/Distortion.{h,cpp} -- four of its
// eight shapes, the ones that sound different from each other.
class Drive {
public:
    enum class Shape { Soft, Hard, Arctan, Fold };

    void  setShape(Shape s);
    // `drive` is the gain before the shape (1 = none, higher = dirtier);
    // `mix` blends dry to shaped, 0..1.
    void  set(float drive, float mix);
    float process(float in);

private:
    Shape m_shape = Shape::Soft;
    float m_drive = 1.0f, m_mix = 1.0f;
};

// Chorus: a short delay whose length is moved by a slow oscillator, added back
// to the dry signal. Not ported -- Synthlab's chorus is a module rather than a
// primitive -- but it is a DelayLine and an Osc, both of which are here.
class Chorus {
public:
    void  prepare(double sampleRate);
    // `rate` in Hz (how fast it wanders), `depth` in seconds (how far),
    // `mix` 0..1.
    void  set(float rateHz, float depthSeconds, float mix);
    void  reset();
    float process(float in);

private:
    DelayLine m_delay;
    Osc       m_lfo;
    float     m_depth = 0.002f, m_mix = 0.5f;
    // The centre the wander happens around. Long enough that the shortest
    // excursion is still a delay rather than a comb filter on top of the dry.
    float     m_centre = 0.012f;
};

// A plain Schroeder reverb: four comb filters in parallel into two allpasses.
// Written here rather than ported -- Synthlab's reverb is juce::Reverb
// underneath, which fitzel cannot take. Mono, like everything else in this
// synth: the room is put in the world by the spatializer, not by the patch.
class Reverb {
public:
    void  prepare(double sampleRate);
    // `size` 0..1 scales the delays (a cupboard to a hall), `damp` 0..1 rolls
    // the top off each repeat, `mix` 0..1 blends it in.
    void  set(float size, float damp, float mix);
    void  reset();
    float process(float in);

private:
    struct Comb {
        DelayLine line;
        float     time  = 0.03f;
        float     feed  = 0.8f;
        float     store = 0.0f;   // one-pole damping state
    };
    Comb      m_comb[4];
    DelayLine m_allpass[2];
    float     m_apTime[2] = {0.005f, 0.0017f};
    float     m_damp = 0.4f, m_mix = 0.3f;
};

} // namespace fitzel::synth
