// The synth check: does the modular synth make the sound the patch describes?
//
// Sound is the one subsystem where "it ran without crashing" says almost
// nothing, and where listening -- the only test this project had for audio so
// far (audiocheck plays through a real device and leaves the verdict to your
// ears) -- cannot tell a filter that is 6 dB off from one that is right. So this
// renders patches OFFLINE, with no device anywhere near it, and measures what
// came out: the pitch of a tone, the shape of an envelope, how much of a 5 kHz
// tone survives a 500 Hz low-pass, where a delay put its echo.
//
// It also holds the two invariants that are easy to break later and impossible
// to hear until much later:
//   * a patch renders the same samples whatever block size it is asked for --
//     otherwise the sound depends on the audio device's buffer, and a bug only
//     appears on someone else's machine;
//   * nothing leaves the Out that is not finite and inside [-1, 1], however
//     absurd the patch, because that reaches real speakers.
//
// Console program, like modelcheck and shadercheck, and for the same reason:
// the editor is /SUBSYSTEM:WINDOWS in Release and has nowhere to print to.
//   build/release/bin/synthcheck.exe [--wav out.wav]
// Exits non-zero if any check fails.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <chrono>
#include <thread>

#include "fitzel/audio/Audio.hpp"
#include "fitzel/audio/Synth.hpp"
#include "fitzel/audio/SynthVoice.hpp"
#include "fitzel/audio/SynthDsp.hpp"
#include "fitzel/audio/SynthPlayer.hpp"
#include "fitzel/audio/SynthPoly.hpp"

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

constexpr double kRate = 48000.0;

// --- measurements -------------------------------------------------------------

float rms(const std::vector<float>& v, std::size_t from = 0) {
    if (from >= v.size()) return 0.0f;
    double sum = 0.0;
    for (std::size_t i = from; i < v.size(); ++i) sum += double(v[i]) * double(v[i]);
    return static_cast<float>(std::sqrt(sum / double(v.size() - from)));
}

float peak(const std::vector<float>& v) {
    float p = 0.0f;
    for (float s : v) p = std::max(p, std::fabs(s));
    return p;
}

bool allFinite(const std::vector<float>& v) {
    for (float s : v)
        if (!std::isfinite(s)) return false;
    return true;
}

// The pitch of a tone, from how often it crosses zero going up. Crude next to an
// FFT and exactly right for the job: a sine, a saw and a pulse all cross zero
// twice a cycle, and a check that needs a spectrum to say "this is 440 Hz" is a
// check nobody reads.
//
// Measured between the FIRST and LAST crossing, not across the whole buffer: a
// second of 440 Hz holds 440 crossings but only 439 whole cycles between them,
// and dividing by the buffer's length instead reports every tone about one hertz
// flat -- a bias that looks exactly like a detuned oscillator.
float frequencyOf(const std::vector<float>& v, double rate) {
    // Where the signal crossed, to a fraction of a sample: at 20 kHz a whole
    // sample is 4 % of a period, and rounding to one would swamp what is
    // measured here.
    auto crossingAt = [&](std::size_t i) {
        const float a = v[i - 1], b = v[i];
        const float t = (b != a) ? (-a / (b - a)) : 0.0f;
        return double(i - 1) + double(t);
    };
    double first = 0.0, last = 0.0;
    int    count = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        if (!(v[i - 1] <= 0.0f && v[i] > 0.0f)) continue;
        const double at = crossingAt(i);
        if (count == 0) first = at;
        last = at;
        ++count;
    }
    if (count < 2) return 0.0f;
    const double seconds = (last - first) / rate;
    return static_cast<float>((count - 1) / seconds);
}

// --- patches ------------------------------------------------------------------

fitzel::synth::ModuleDef module(int id, fitzel::synth::ModuleType t,
                                const std::string& mode = {}) {
    fitzel::synth::ModuleDef m;
    m.id   = id;
    m.type = t;
    m.mode = mode;
    return m;
}

// osc -> out, the smallest patch there is.
fitzel::synth::Patch tonePatch(const std::string& wave, float hz) {
    using namespace fitzel::synth;
    Patch p;
    p.name = "tone";
    ModuleDef osc = module(1, ModuleType::Osc, wave);
    osc.setParam("freq", hz);
    ModuleDef out = module(2, ModuleType::Out);
    p.modules = {osc, out};
    p.wires   = {{1, 2, 0}};
    return p;
}

std::vector<float> render(fitzel::synth::Graph& g, int frames, int block = 0) {
    std::vector<float> buf(static_cast<std::size_t>(frames), 0.0f);
    if (block <= 0) {
        g.process(buf.data(), frames);
        return buf;
    }
    for (int i = 0; i < frames; i += block)
        g.process(buf.data() + i, std::min(block, frames - i));
    return buf;
}

// --- 1. the primitives ---------------------------------------------------------

void checkPrimitives() {
    std::printf("\nThe primitives make the shape they are named after\n");
    using namespace fitzel::synth;

    Osc osc;
    osc.prepare(kRate);
    osc.setWave(Osc::Wave::Sine);
    osc.setFrequency(440.0);
    std::vector<float> sine(static_cast<std::size_t>(kRate));
    for (float& s : sine) s = osc.process();
    check(std::fabs(frequencyOf(sine, kRate) - 440.0f) < 1.0f,
          "a sine asked for 440 Hz comes out at 440 Hz");
    check(std::fabs(peak(sine) - 1.0f) < 0.01f, "...at full scale");
    check(std::fabs(rms(sine) - 0.707f) < 0.01f, "...and with a sine's RMS");

    // The wrap the port added: an oscillator left running for an hour has to
    // still be in tune. Twenty minutes of phase at 8 kHz is enough to show it.
    osc.reset();
    osc.setFrequency(8000.0);
    for (long i = 0; i < 20L * 60L * static_cast<long>(kRate); ++i) osc.process();
    osc.setFrequency(440.0);
    for (float& s : sine) s = osc.process();
    check(std::fabs(frequencyOf(sine, kRate) - 440.0f) < 1.0f,
          "...still in tune after twenty minutes of running");

    osc.reset();
    osc.setWave(Osc::Wave::Saw);
    osc.setFrequency(220.0);
    std::vector<float> saw(static_cast<std::size_t>(kRate));
    for (float& s : saw) s = osc.process();
    check(std::fabs(frequencyOf(saw, kRate) - 220.0f) < 2.0f, "a saw holds its pitch");
    check(allFinite(saw) && peak(saw) <= 1.2f, "...and stays in range");
    // The ported saw is a leaky-integrated BLIT, and it does NOT come out at
    // full scale -- it sits well under a sine at the same setting. That is the
    // oscillator's nature, not a fault, but a patch has to make the difference
    // up, so the level is pinned here: a later "fix" that normalises it would
    // change how every patch built against it sounds.
    check(peak(saw) > 0.3f && peak(saw) < 0.9f,
          "...and peaks well under full scale, as a leaky BLIT does");

    osc.reset();
    osc.setWave(Osc::Wave::Pulse);
    osc.setFrequency(110.0);
    osc.setPulseWidth(0.25f);
    std::vector<float> pulse(static_cast<std::size_t>(kRate));
    for (float& s : pulse) s = osc.process();
    check(std::fabs(frequencyOf(pulse, kRate) - 110.0f) < 1.0f, "a pulse holds its pitch");
    // A quarter-width pulse is high a quarter of the time, which is what makes
    // it sound different from a square -- and the thing the ported version can
    // do that the original (a clipped sine, always a square) could not.
    double high = 0.0;
    for (float s : pulse) high += (s > 0.0f) ? 1.0 : 0.0;
    check(std::fabs(high / double(pulse.size()) - 0.25) < 0.02,
          "...and a width of 0.25 is high a quarter of the time");

    osc.setWave(Osc::Wave::Noise);
    std::vector<float> noise(static_cast<std::size_t>(kRate));
    for (float& s : noise) s = osc.process();
    double mean = 0.0;
    for (float s : noise) mean += s;
    mean /= double(noise.size());
    check(std::fabs(mean) < 0.02, "noise sits around zero");
    check(rms(noise) > 0.4f && peak(noise) <= 1.0f, "...at a usable level");

    // Deterministic: the same patch has to render the same bytes twice, or no
    // check in this file can compare anything.
    Osc a, b;
    a.prepare(kRate);
    b.prepare(kRate);
    a.setWave(Osc::Wave::Noise);
    b.setWave(Osc::Wave::Noise);
    bool same = true;
    for (int i = 0; i < 1000; ++i) same = same && (a.process() == b.process());
    check(same, "...and the same noise every run");
}

void checkEnvelope() {
    std::printf("\nThe envelope opens, holds and closes\n");
    using namespace fitzel::synth;

    Adsr env;
    env.prepare(kRate);
    env.setAttack(0.01f);
    env.setDecay(0.05f);
    env.setSustain(0.5f);
    env.setRelease(0.1f);

    check(env.idle() && env.process() == 0.0f, "silent until it is gated");

    env.gate(true);
    std::vector<float> attack(static_cast<std::size_t>(kRate * 0.01));
    for (float& s : attack) s = env.process();
    check(peak(attack) > 0.98f, "attack reaches the top in the time it was given");

    for (int i = 0; i < static_cast<int>(kRate * 0.2); ++i) env.process();
    const float held = env.process();
    check(std::fabs(held - 0.5f) < 0.01f, "...then settles at the sustain level");

    env.gate(false);
    for (int i = 0; i < static_cast<int>(kRate * 0.5); ++i) env.process();
    check(env.idle(), "release ends at idle rather than crawling towards zero forever");

    // Seconds, not samples: the same patch at another rate must sound the same.
    Adsr slow;
    slow.prepare(kRate * 2.0);
    slow.setAttack(0.01f);
    slow.setDecay(0.05f);
    slow.setSustain(0.5f);
    slow.setRelease(0.1f);
    slow.gate(true);
    for (int i = 0; i < static_cast<int>(kRate * 2.0 * 0.01); ++i) slow.process();
    check(slow.process() > 0.98f, "and it is timed in seconds, not in samples");
}

void checkFilterAndDelay() {
    std::printf("\nThe filter filters, the delay repeats\n");
    using namespace fitzel::synth;

    auto toneThrough = [](Biquad::Mode mode, float cutoff, float toneHz) {
        Osc osc;
        osc.prepare(kRate);
        osc.setWave(Osc::Wave::Sine);
        osc.setFrequency(toneHz);
        Biquad f;
        f.prepare(kRate);
        f.setMode(mode);
        f.set(cutoff, 0.707f);
        std::vector<float> v(static_cast<std::size_t>(kRate * 0.2));
        for (float& s : v) s = f.process(osc.process());
        return rms(v, v.size() / 2);   // after the filter has settled
    };

    check(toneThrough(Biquad::Mode::LowPass, 500.0f, 100.0f) > 0.6f,
          "a low-pass at 500 Hz lets 100 Hz through");
    check(toneThrough(Biquad::Mode::LowPass, 500.0f, 5000.0f) < 0.02f,
          "...and holds 5 kHz back");
    check(toneThrough(Biquad::Mode::HighPass, 500.0f, 5000.0f) > 0.6f,
          "a high-pass does the opposite");
    check(toneThrough(Biquad::Mode::HighPass, 500.0f, 100.0f) < 0.05f, "...both ways");

    // Swept past Nyquist by a modulator: the coefficients must stay sane, or one
    // careless patch takes the whole mix with it.
    Biquad wild;
    wild.prepare(kRate);
    wild.set(1e9f, 50.0f);
    float acc = 0.0f;
    for (int i = 0; i < 1000; ++i) acc += wild.process(0.5f);
    check(std::isfinite(acc), "a cutoff swept past Nyquist does not blow up");

    DelayLine d;
    d.prepare(kRate, 0.5f);
    const int at = static_cast<int>(kRate * 0.1);
    std::vector<float> tail;
    for (int i = 0; i < static_cast<int>(kRate * 0.2); ++i) {
        d.write(i == 0 ? 1.0f : 0.0f);
        tail.push_back(d.read(0.1f));
    }
    int loudest = 0;
    for (int i = 1; i < static_cast<int>(tail.size()); ++i)
        if (std::fabs(tail[static_cast<std::size_t>(i)]) >
            std::fabs(tail[static_cast<std::size_t>(loudest)])) loudest = i;
    check(std::abs(loudest - at) <= 2, "an impulse comes back a tenth of a second later");

    // The repair the port made: with an integer read position the interpolation
    // never ran, so a delay time between two samples sounded like the sample
    // below it. Half a sample between two impulses has to read half of each.
    DelayLine frac;
    frac.prepare(kRate, 0.1f);
    frac.write(1.0f);
    frac.write(0.0f);
    for (int i = 0; i < 8; ++i) frac.write(0.0f);
    const float halfway = frac.read(9.5f / static_cast<float>(kRate));
    check(std::fabs(halfway - 0.5f) < 0.01f,
          "a delay of nine and a half samples reads halfway between two");
}


// --- 1b. the effects and the helpers -------------------------------------------

void checkEffects() {
    std::printf("\nThe effects do what they are named after\n");
    using namespace fitzel::synth;

    // Drive: gain into a non-linearity. Hard clipping has to flatten, and
    // nothing may leave it unbounded whatever it is fed.
    Drive d;
    d.setShape(Drive::Shape::Hard);
    d.set(10.0f, 1.0f);
    check(std::fabs(d.process(0.5f) - 1.0f) < 1e-4f, "hard clipping flattens what is too loud");
    d.setShape(Drive::Shape::Soft);
    d.set(1.0f, 1.0f);
    check(d.process(0.2f) < 0.2f && d.process(0.2f) > 0.15f,
          "soft clipping bends the quiet part instead of flattening it");
    d.setShape(Drive::Shape::Fold);
    d.set(4.0f, 1.0f);
    bool bounded = true;
    for (int i = 0; i < 200; ++i) {
        const float v = d.process(static_cast<float>(i) / 100.0f - 1.0f);
        bounded = bounded && std::isfinite(v) && std::fabs(v) <= 1.001f;
    }
    check(bounded, "foldback stays inside full scale however hard it is driven");

    // Reverb: an impulse has to still be audible long after it stopped, which is
    // the one thing that separates a reverb from a filter.
    Reverb rv;
    rv.prepare(kRate);
    rv.set(0.7f, 0.3f, 1.0f);
    std::vector<float> tail;
    for (int i = 0; i < static_cast<int>(kRate * 0.5); ++i)
        tail.push_back(rv.process(i == 0 ? 1.0f : 0.0f));
    const float late = rms(tail, tail.size() * 3 / 4);
    check(late > 1e-4f, "a reverb is still sounding a third of a second later");
    check(allFinite(tail) && peak(tail) < 4.0f, "...without the tail running away");

    // Chorus: the same signal, delayed by a wandering amount, so the output must
    // differ from the input while staying the same kind of loud.
    Chorus ch;
    ch.prepare(kRate);
    ch.set(1.0f, 0.003f, 1.0f);
    Osc src;
    src.prepare(kRate);
    src.setWave(Osc::Wave::Sine);
    src.setFrequency(440.0);
    std::vector<float> dry, wet;
    for (int i = 0; i < static_cast<int>(kRate * 0.3); ++i) {
        const float x = src.process();
        dry.push_back(x);
        wet.push_back(ch.process(x));
    }
    float diff = 0.0f;
    for (std::size_t i = wet.size() / 2; i < wet.size(); ++i)
        diff = std::max(diff, std::fabs(wet[i] - dry[i]));
    check(diff > 0.05f, "a chorus changes the signal");
    check(rms(wet, wet.size() / 2) > 0.3f, "...without swallowing it");
}

void checkHelperModules() {
    std::printf("\nNote and LFO\n");
    using namespace fitzel::synth;

    // A Note module is the one conversion Map cannot do, because pitch is
    // exponential: 69 is the A a tuner shows as 440 Hz.
    Patch p;
    p.name = "note";
    p.inputs.push_back({"n", 0.0f, 127.0f, 69.0f});
    ModuleDef par = module(1, ModuleType::Param, "n");
    par.setParam("smooth", 0.0f);
    ModuleDef nt  = module(2, ModuleType::Note);
    ModuleDef osc = module(3, ModuleType::Osc, "sine");
    osc.setParam("freq", 0.0f);
    ModuleDef out = module(4, ModuleType::Out);
    p.modules = {par, nt, osc, out};
    p.wires   = {{1, 2, 0}, {2, 3, 0}, {3, 4, 0}};

    Graph g;
    check(g.compile(p, kRate, 256, nullptr), "a note patch compiles");
    g.setInput("n", 69.0f);
    check(std::fabs(frequencyOf(render(g, static_cast<int>(kRate)), kRate) - 440.0f) < 1.0f,
          "MIDI note 69 is 440 Hz");
    g.setInput("n", 57.0f);
    check(std::fabs(frequencyOf(render(g, static_cast<int>(kRate)), kRate) - 220.0f) < 1.0f,
          "...and an octave down is half of it");

    // An LFO is a slow oscillator with a level and a centre, so a patch can send
    // it straight at a cutoff in hertz.
    Patch l;
    l.name = "lfo";
    ModuleDef lfo = module(1, ModuleType::Lfo, "sine");
    lfo.setParam("rate", 4.0f);
    lfo.setParam("depth", 100.0f);
    lfo.setParam("offset", 500.0f);
    ModuleDef lout = module(2, ModuleType::Out);
    l.modules = {lfo, lout};
    l.wires   = {{1, 2, 0}};
    Graph lg;
    lg.compile(l, kRate, 256, nullptr);
    std::vector<float> v(static_cast<std::size_t>(kRate));
    lg.process(v.data(), static_cast<int>(v.size()));
    float lo = v[0], hi = v[0];
    for (float x : v) { lo = std::min(lo, x); hi = std::max(hi, x); }
    // Out clamps to full scale, so the range is checked before that bites: what
    // matters here is that the centre and the depth are where they were put.
    check(hi > 0.99f && lo > 0.0f, "an LFO swings around its centre, not around zero");
}

// --- 2. the graph ---------------------------------------------------------------

void checkGraph() {
    std::printf("\nA patch renders what it says\n");
    using namespace fitzel::synth;

    Graph g;
    std::string err;
    check(g.compile(tonePatch("sine", 440.0f), kRate, 256, &err), "a tone patch compiles");
    std::vector<float> tone = render(g, static_cast<int>(kRate));
    check(std::fabs(frequencyOf(tone, kRate) - 440.0f) < 1.0f, "...and plays its tone");

    // Block size must not change the sound. The device picks it, not the patch.
    Graph g2;
    g2.compile(tonePatch("saw", 220.0f), kRate, 256, nullptr);
    const std::vector<float> whole = render(g2, 4800);
    g2.reset();
    const std::vector<float> chopped = render(g2, 4800, 37);   // a deliberately odd block
    check(whole == chopped, "the same patch renders the same samples at any block size");

    // A dial the game turns: rpm -> Map -> the oscillator's frequency.
    Patch p;
    p.name = "engine";
    p.inputs.push_back({"rpm", 800.0f, 7000.0f, 800.0f});
    ModuleDef par = module(1, ModuleType::Param, "rpm");
    par.setParam("smooth", 0.0f);          // no smoothing, so the check is exact
    ModuleDef map = module(2, ModuleType::Map);
    map.setParam("inMin", 800.0f);
    map.setParam("inMax", 7000.0f);
    map.setParam("outMin", 40.0f);
    map.setParam("outMax", 320.0f);
    ModuleDef osc = module(3, ModuleType::Osc, "sine");
    osc.setParam("freq", 0.0f);            // the frequency comes from the wire
    ModuleDef out = module(4, ModuleType::Out);
    p.modules = {par, map, osc, out};
    p.wires   = {{1, 2, 0}, {2, 3, 0}, {3, 4, 0}};

    Graph eng;
    check(eng.compile(p, kRate, 256, &err), "a patch with a named input compiles");
    check(eng.inputIndex("rpm") == 0, "...and the input can be found by name");
    eng.setInput("rpm", 800.0f);
    std::vector<float> idle = render(eng, static_cast<int>(kRate));
    check(std::fabs(frequencyOf(idle, kRate) - 40.0f) < 1.0f, "idle sits at 40 Hz");
    eng.setInput("rpm", 7000.0f);
    std::vector<float> full = render(eng, static_cast<int>(kRate));
    check(std::fabs(frequencyOf(full, kRate) - 320.0f) < 2.0f,
          "...and full throttle at 320 Hz -- the game turned one dial");

    // Modules that feed nothing are not an error, but the editor has to know.
    Patch stray = tonePatch("sine", 440.0f);
    stray.modules.push_back(module(9, ModuleType::Osc, "saw"));
    Graph sg;
    check(sg.compile(stray, kRate, 256, nullptr) && !sg.reaches(9) && sg.reaches(1),
          "a module wired to nothing compiles, and reports that it is not heard");
}

void checkRefusals() {
    std::printf("\nA patch that cannot work says so\n");
    using namespace fitzel::synth;

    std::string err;
    Patch loop;
    loop.modules = {module(1, ModuleType::Gain), module(2, ModuleType::Gain),
                    module(3, ModuleType::Out)};
    loop.wires   = {{1, 2, 0}, {2, 1, 0}, {2, 3, 0}};
    Graph g;
    check(!g.compile(loop, kRate, 256, &err), "a feedback loop is refused");
    check(err.find("loop") != std::string::npos, "...and the reason names the loop");
    check(!g.ready(), "...and leaves nothing half-built behind");

    Patch noOut;
    noOut.modules = {module(1, ModuleType::Osc, "sine")};
    check(!g.compile(noOut, kRate, 256, &err), "a patch with no Out is refused");

    Patch twoOut;
    twoOut.modules = {module(1, ModuleType::Out), module(2, ModuleType::Out)};
    check(!g.compile(twoOut, kRate, 256, &err), "...and so is one with two");

    // Whatever the patch does, the speakers get something finite and in range.
    Patch loud = tonePatch("saw", 55.0f);
    loud.modules.push_back([] {
        ModuleDef m = module(3, ModuleType::Gain);
        m.setParam("gain", 1000.0f);
        return m;
    }());
    loud.wires = {{1, 3, 0}, {3, 2, 0}};
    Graph lg;
    check(lg.compile(loud, kRate, 256, nullptr), "an absurdly loud patch still compiles");
    const std::vector<float> hot = render(lg, 4800);
    check(allFinite(hot) && peak(hot) <= 1.0f, "...but nothing over full scale leaves it");
}

void checkFile() {
    std::printf("\nA patch survives the round trip through its file\n");
    using namespace fitzel::synth;

    Patch p;
    p.name = "round trip";
    p.inputs.push_back({"rpm", 800.0f, 7000.0f, 1200.0f});
    ModuleDef osc = module(1, ModuleType::Osc, "pulse");
    osc.setParam("freq", 123.5f);
    osc.setParam("pulseWidth", 0.3f);
    osc.x = 40.0f;
    osc.y = -12.0f;
    p.modules = {osc, module(2, ModuleType::Out)};
    p.wires   = {{1, 2, 0}};

    Patch back;
    std::string err;
    check(Patch::fromJson(p.toJson(), back, &err), "the patch reads back as JSON");
    check(back.name == p.name && back.modules.size() == 2 && back.wires.size() == 1,
          "...with its modules and wires");
    check(back.modules[0].mode == "pulse" &&
              std::fabs(back.modules[0].param("pulseWidth") - 0.3f) < 1e-6f,
          "...its settings");
    check(!back.inputs.empty() && back.inputs[0].name == "rpm" &&
              std::fabs(back.inputs[0].max - 7000.0f) < 1e-3f,
          "...and its dials");
    check(std::fabs(back.modules[0].x - 40.0f) < 1e-3f,
          "...including where the editor had put it");

    Patch broken;
    check(!Patch::fromJson("{ this is not json", broken, &err),
          "a corrupt file is refused rather than half-read");
}

// --- 5. songs --------------------------------------------------------------------

// A playable patch, the way the Poly convention wants it: pitch -> Note -> the
// oscillator's frequency, gate -> ADSR -> its amplitude.
fitzel::synth::Patch keysPatch(float attack = 0.001f, float release = 0.05f) {
    using namespace fitzel::synth;
    Patch p;
    p.name = "keys";
    p.inputs.push_back({"pitch", 0.0f, 127.0f, 60.0f});
    p.inputs.push_back({"gate", 0.0f, 1.0f, 0.0f});
    ModuleDef pitch = module(1, ModuleType::Param, "pitch");
    pitch.setParam("smooth", 0.0f);
    ModuleDef gate = module(2, ModuleType::Param, "gate");
    gate.setParam("smooth", 0.0f);
    ModuleDef note = module(3, ModuleType::Note);
    ModuleDef env  = module(4, ModuleType::Adsr);
    env.setParam("attack", attack);
    env.setParam("decay", 0.05f);
    env.setParam("sustain", 1.0f);
    env.setParam("release", release);
    ModuleDef osc = module(5, ModuleType::Osc, "sine");
    osc.setParam("freq", 0.0f);
    ModuleDef out = module(6, ModuleType::Out);
    p.modules = {pitch, gate, note, env, osc, out};
    p.wires   = {{1, 3, 0}, {3, 5, 0}, {2, 4, 0}, {4, 5, 1}, {5, 6, 0}};
    return p;
}

// A small .mid, byte by byte, with the things real files do: two tracks, a
// tempo change half-way, running status, note-offs written as velocity-0 ons,
// a drum hit on channel 10 and a SysEx to read past.
std::vector<std::uint8_t> testMidi() {
    std::vector<std::uint8_t> f = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1, 0, 2, 0x01, 0xE0,   // format 1, 2 tracks, 480 ppq
    };
    auto track = [&](const std::vector<std::uint8_t>& body) {
        const std::size_t n = body.size();
        f.insert(f.end(), {'M', 'T', 'r', 'k', std::uint8_t(n >> 24), std::uint8_t(n >> 16),
                           std::uint8_t(n >> 8), std::uint8_t(n)});
        f.insert(f.end(), body.begin(), body.end());
    };
    // Conductor: 120 BPM, then 240 BPM from beat 2 (tick 960 = 1.0 s).
    track({
        0x00, 0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20,           // 500000 us/beat
        0x87, 0x40, 0xFF, 0x51, 0x03, 0x03, 0xD0, 0x90,     // +960: 250000 us/beat
        0x00, 0xFF, 0x2F, 0x00,
    });
    track({
        0x00, 0xF0, 0x03, 0x7E, 0x09, 0xF7,                 // SysEx, skipped
        0x00, 0x90, 60, 100,                                // C E G at once,
        0x00, 64, 100,                                      //   running status
        0x00, 67, 100,
        0x00, 0x99, 36, 120,                                // kick drum, ch 10
        0x83, 0x60, 0x90, 60, 0,                            // +480: offs as vel-0 ons
        0x00, 64, 0,
        0x00, 67, 0,
        0x00, 0x89, 36, 0,
        0x87, 0x40, 0x90, 72, 90,                           // +960 = tick 1440: 1.25 s
        0x83, 0x60, 0x80, 72, 0,                            // +480 = tick 1920: 1.5 s
        0x00, 0xFF, 0x2F, 0x00,
    });
    return f;
}

void checkMidi() {
    std::printf("\nA MIDI file\n");
    using namespace fitzel::synth;
    MidiSequence s;
    std::string  err;
    const std::vector<std::uint8_t> bytes = testMidi();
    check(MidiSequence::parse(bytes, s, &err), "a two-track file with a tempo change parses");
    check(s.events.size() == 10 && s.tracks == 2 && s.format == 1,
          "...every note is read, running status and all");

    const MidiEvent* high = nullptr;
    for (const MidiEvent& e : s.events)
        if (e.note == 72 && e.on) high = &e;
    check(high && std::fabs(high->time - 1.25) < 1e-6,
          "a note after the tempo change lands where the new tempo puts it");
    check(std::fabs(s.length - 1.5) < 1e-6, "...and the song is as long as its last note");
    check(!s.events.empty() && s.events.front().time == 0.0 && s.events[3].channel == 9,
          "the drum stays on its own channel");
    bool sorted = true;
    for (std::size_t i = 1; i < s.events.size(); ++i)
        sorted = sorted && s.events[i - 1].time <= s.events[i].time;
    check(sorted, "the events come out in time order");

    std::vector<std::uint8_t> cut(bytes.begin(), bytes.end() - 9);
    MidiSequence bad;
    check(!MidiSequence::parse(cut, bad, &err) && bad.events.empty(),
          "a file cut short is refused, not half-played");
    check(!MidiSequence::parse({'R', 'I', 'F', 'F', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, bad, &err),
          "...and so is something that is not MIDI at all");
}

// The first sample louder than a whisper.
int onset(const std::vector<float>& v) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (std::fabs(v[i]) > 1e-4f) return static_cast<int>(i);
    return -1;
}

void checkPoly() {
    std::printf("\nSeveral notes, and a song\n");
    using namespace fitzel::synth;
    std::string err;

    Poly poly;
    check(poly.compile(keysPatch(), kRate, 8, 256, &err), "a keys patch compiles eight times over");
    poly.noteOn(60, 0.8f);
    poly.noteOn(64, 0.8f);
    poly.noteOn(67, 0.8f);
    std::vector<float> chord(4800);
    poly.process(chord.data(), static_cast<int>(chord.size()));
    check(poly.heldNotes() == 3 && poly.soundingVoices() == 3, "a chord takes three voices");
    check(allFinite(chord) && peak(chord) <= 1.0f && rms(chord, 1000) > 0.1f,
          "...and sounds, inside full scale");

    poly.allNotesOff();
    std::vector<float> tail(static_cast<std::size_t>(kRate * 0.5));
    poly.process(tail.data(), static_cast<int>(tail.size()));
    check(poly.heldNotes() == 0 && poly.soundingVoices() == 0,
          "let go, the voices ring out and fall silent");

    Poly few;
    few.compile(keysPatch(), kRate, 4, 256, nullptr);
    for (int n = 60; n < 66; ++n) few.noteOn(n, 0.8f);
    check(few.heldNotes() == 4, "more notes than voices steals the oldest, never overruns");

    // A song with one A at half a second, played in blocks of an awkward size:
    // the note starts on its own sample, not on the next block edge.
    MidiSequence song;
    song.events = {{0.5, 0, 69, 0.8f, true}, {0.9, 0, 69, 0.0f, false},
                   {0.6, 9, 36, 1.0f, true}, {0.7, 9, 36, 0.0f, false}};
    std::sort(song.events.begin(), song.events.end(),
              [](const MidiEvent& a, const MidiEvent& b) { return a.time < b.time; });
    song.length = 1.0;

    auto play = [&](int block, bool drums) {
        Poly p;
        p.compile(keysPatch(), kRate, 4, 256, nullptr);
        p.setSkipDrums(!drums);
        p.setSequence(&song);
        p.play(true);
        std::vector<float> buf(static_cast<std::size_t>(kRate * 1.2));
        for (int i = 0; i < static_cast<int>(buf.size()); i += block)
            p.process(buf.data() + i, std::min(block, static_cast<int>(buf.size()) - i));
        return std::make_pair(buf, p.playing());
    };
    const auto [a, aPlaying] = play(333, false);
    const auto [b, bPlaying] = play(4096, false);
    check(std::abs(onset(a) - 24000) <= 1, "a note due at 0.5 s starts at sample 24000");
    check(onset(a) == onset(b), "...whatever block size the device asks for");
    const std::vector<float> note(a.begin() + 26000, a.begin() + 40000);
    check(std::fabs(frequencyOf(note, kRate) - 440.0f) < 1.0f, "...at the pitch it was written");
    check(!aPlaying && rms(std::vector<float>(a.begin() + 50000, a.end())) < 1e-3f,
          "the song stops at its end and leaves silence");
    const auto [d, dPlaying] = play(512, true);
    check(rms(std::vector<float>(d.begin() + 29000, d.begin() + 33000)) >
              rms(std::vector<float>(a.begin() + 29000, a.begin() + 33000)) * 1.2f,
          "the drum channel plays only when asked for");

    Poly lp;
    lp.compile(keysPatch(), kRate, 4, 256, nullptr);
    lp.setSequence(&song);
    lp.setLoop(true);
    lp.play(true);
    std::vector<float> loop(static_cast<std::size_t>(kRate * 1.6));
    lp.process(loop.data(), static_cast<int>(loop.size()));
    check(lp.playing() &&
              onset(std::vector<float>(loop.begin() + 48000, loop.end())) == onset(loop),
          "a looped song comes round again on the very same sample");

    Poly fast;
    fast.compile(keysPatch(), kRate, 4, 256, nullptr);
    fast.setSequence(&song);
    fast.setTempo(2.0f);
    fast.play(true);
    std::vector<float> quick(static_cast<std::size_t>(kRate));
    fast.process(quick.data(), static_cast<int>(quick.size()));
    check(std::abs(onset(quick) - 12000) <= 1, "twice the tempo, twice as soon");

    MidiSequence empty;
    Poly ep;
    ep.compile(keysPatch(), kRate, 2, 256, nullptr);
    ep.setSequence(&empty);
    ep.setLoop(true);
    ep.play(true);
    std::vector<float> none(4800);
    ep.process(none.data(), static_cast<int>(none.size()));
    check(!ep.playing() && peak(none) == 0.0f, "an empty song does not start (or spin)");
}

// A rendered patch to listen to, for when a number is not the question.
void writeWav(const char* path) {
    using namespace fitzel::synth;
    Patch p;
    p.name = "sweep";
    p.inputs.push_back({"rpm", 800.0f, 7000.0f, 800.0f});
    ModuleDef par = module(1, ModuleType::Param, "rpm");
    par.setParam("smooth", 0.05f);
    ModuleDef map = module(2, ModuleType::Map);
    map.setParam("inMin", 800.0f);
    map.setParam("inMax", 7000.0f);
    map.setParam("outMin", 45.0f);
    map.setParam("outMax", 260.0f);
    ModuleDef osc = module(3, ModuleType::Osc, "saw");
    osc.setParam("freq", 0.0f);
    ModuleDef flt = module(4, ModuleType::Filter, "lp");
    flt.setParam("cutoff", 900.0f);
    flt.setParam("q", 1.4f);
    ModuleDef out = module(5, ModuleType::Out);
    out.setParam("gain", 0.7f);
    p.modules = {par, map, osc, flt, out};
    p.wires   = {{1, 2, 0}, {2, 3, 0}, {3, 4, 0}, {4, 5, 0}};

    Graph g;
    if (!g.compile(p, kRate, 256, nullptr)) return;

    const int frames = static_cast<int>(kRate * 6.0);
    std::vector<float> buf(static_cast<std::size_t>(frames));
    const int step = 480;
    for (int i = 0; i < frames; i += step) {
        const float t = static_cast<float>(i) / static_cast<float>(frames);
        g.setInput("rpm", 800.0f + 6200.0f * (t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f));
        g.process(buf.data() + i, std::min(step, frames - i));
    }

    std::FILE* f = std::fopen(path, "wb");
    if (!f) return;
    auto u32 = [&](unsigned v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](unsigned short v) { std::fwrite(&v, 2, 1, f); };
    const unsigned dataBytes = static_cast<unsigned>(frames) * 2u;
    std::fwrite("RIFF", 1, 4, f); u32(36u + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16u); u16(1); u16(1);
    u32(static_cast<unsigned>(kRate)); u32(static_cast<unsigned>(kRate) * 2u); u16(2); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);
    for (float s : buf) {
        const short v = static_cast<short>(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
    std::printf("\nwrote %s (a six-second engine sweep)\n", path);
}

} // namespace

// A voice on the real mixer, for the one question an offline render cannot
// answer: does a patch handed to miniaudio actually come out of it again? Needs
// an audio device, so it is behind a flag rather than in the run above.
void checkVoice() {
    std::printf("\nOn the real mixer\n");
    fitzel::Audio audio;
    if (!audio.ok()) {
        std::printf("  ..   no audio device; skipped\n");
        return;
    }
    std::string err;
    fitzel::SynthVoice v =
        fitzel::SynthVoice::fromPatch(audio, tonePatch("saw", 220.0f), &err);
    check(v.isValid(), "a patch becomes a voice");
    if (!v.isValid()) {
        std::printf("       (%s)\n", err.c_str());
        return;
    }
    v.setVolume(0.2f);
    v.play();
    // Long enough for the device to have asked for several buffers.
    for (int i = 0; i < 40 && v.isPlaying(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    std::vector<float> seen(2048, 0.0f);
    const int n = v.peek(seen.data(), static_cast<int>(seen.size()));
    seen.resize(static_cast<std::size_t>(std::max(0, n)));
    check(n > 0, "it reports what it played");
    check(rms(seen) > 0.05f, "...and the mixer really pulled samples from it");
    check(std::fabs(frequencyOf(seen, kRate) - 220.0f) < 3.0f, "...at the patch's pitch");

    // The crash the editor hit: rebuilding a voice while it plays and assigning
    // the new one over it. The old one has to leave the mixer as it goes -- if
    // it stays hooked in with its memory freed, the audio thread reads garbage
    // within a callback or two. Twenty swaps with the device running is plenty
    // of callbacks for that to show.
    for (int i = 0; i < 20; ++i) {
        fitzel::SynthVoice next = fitzel::SynthVoice::fromPatch(
            audio, tonePatch(i % 2 ? "sine" : "saw", 220.0f + 20.0f * i), nullptr);
        next.setVolume(0.2f);
        next.play();
        v = std::move(next);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    check(v.isPlaying(), "a voice swapped for a rebuilt one twenty times over still plays");
    v.stop();

    // A player: a song handed across to the audio thread, played by it.
    fitzel::SynthPlayer pl = fitzel::SynthPlayer::create(audio, keysPatch(), 8, &err);
    check(pl.isValid(), "a keys patch becomes a player");
    if (!pl.isValid()) {
        std::printf("       (%s)\n", err.c_str());
        return;
    }
    pl.setVolume(0.2f);
    fitzel::synth::MidiSequence song;
    check(fitzel::synth::MidiSequence::parse(testMidi(), song, nullptr), "...and takes a song");
    pl.setMidi(song);
    pl.playMidi();
    check(pl.midiPlaying() && std::fabs(pl.midiLength() - 1.5) < 1e-6,
          "...which is playing the moment it is asked to");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    std::vector<float> heard(2048, 0.0f);
    heard.resize(static_cast<std::size_t>(std::max(0, pl.peek(heard.data(), 2048))));
    check(rms(heard) > 0.02f, "...and the mixer hears the chord");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    check(!pl.midiPlaying(), "...and knows when it is over");

    // The same handoff under pressure: songs swapped in faster than the
    // audio thread collects them, players swapped out while they sound.
    for (int i = 0; i < 20; ++i) {
        fitzel::SynthPlayer next = fitzel::SynthPlayer::create(audio, keysPatch(), 4, nullptr);
        next.setVolume(0.1f);
        for (int k = 0; k < 5; ++k) next.setMidi(song);
        next.setMidiLoop(true);
        next.playMidi();
        next.noteOn(48 + i);
        pl = std::move(next);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    check(pl.isRunning() && pl.midiPlaying(),
          "a player swapped twenty times, songs and all, still plays");
    pl.stop();
}

int main(int argc, char** argv) {
    std::printf("synthcheck -- the primitives, the graph and the patch file\n");
    checkPrimitives();
    checkEnvelope();
    checkFilterAndDelay();
    checkEffects();
    checkHelperModules();
    checkGraph();
    checkRefusals();
    checkFile();
    checkMidi();
    checkPoly();

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--voice") == 0) checkVoice();
        if (std::strcmp(argv[i], "--wav") == 0 && i + 1 < argc) writeWav(argv[i + 1]);
    }

    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
