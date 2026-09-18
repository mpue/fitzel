#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "fitzel/audio/SynthDsp.hpp"

// A small modular synthesiser: a patch is a handful of modules wired together,
// and a graph is that patch made ready to run in the audio callback.
//
// It is modular in the way Synthlab is -- modules, ports, wires -- but it is
// fitzel's own, and the differences are deliberate:
//
//  * DSP and UI are separate. A module here is data and a process step; nothing
//    in this header can draw itself. Synthlab's modules ARE their own widgets,
//    which is exactly what makes them impossible to lift out of it.
//  * It runs in blocks, not sample-by-sample pull through the graph, and the
//    order is a real topological sort done once at compile time.
//  * Nothing allocates, locks or throws while it plays. Buffers are sized when
//    the patch is compiled, and the only thing the game pushes in from another
//    thread is a set of named numbers, each one an atomic.
//  * A patch has named INPUTS -- "rpm", "speed", "hit" -- because that is how a
//    game plays a synthesiser. It does not send notes; it turns a dial while the
//    sound is running.
namespace fitzel::synth {

enum class ModuleType {
    Osc,      // an oscillator: in0 frequency offset (Hz), in1 amplitude (x)
    Lfo,      // a slow oscillator meant to modulate: in0 rate offset (Hz)
    Note,     // in0 a MIDI note number, out the frequency it means
    Filter,   // in0 signal, in1 cutoff offset (Hz)
    Adsr,     // in0 gate (over 0.5 is on)
    Gain,     // in0 signal, in1 gain (x)
    Mix,      // in0..in3, each with its own level
    Delay,    // in0 signal, in1 delay time offset (s)
    Drive,    // in0 signal, in1 drive offset -- waveshaping distortion
    Chorus,   // in0 signal
    Reverb,   // in0 signal
    Map,      // in0 through a range and a curve -- "rpm 800..7000 -> 40..320 Hz"
    Param,    // a named input from the game; no inputs of its own
    Const,    // a fixed number
    Out,      // in0 is what the voice plays; exactly one per patch
};

// The name a patch file uses for a type, and back. Unknown names give Const,
// which is silence rather than a failed load.
const char* typeName(ModuleType t);
ModuleType  typeFromName(const std::string& s);
// How many inputs a type reads. Ports beyond this are ignored when wiring.
int         inputCount(ModuleType t);

struct ModuleDef {
    int         id   = 0;             // unique within the patch; wires name these
    ModuleType  type = ModuleType::Const;
    std::string name;                 // free text for the editor
    // What the module is set to. Which keys matter depends on the type:
    //   Osc    freq, fine, pulseWidth, level   + `mode`: sine|saw|pulse|noise
    //   Lfo    rate, depth, offset             + `mode`: sine|saw|pulse|noise
    //   Note   transpose (semitones), fine (Hz)
    //   Filter cutoff, q                       + `mode`: lp|hp|bp
    //   Adsr   attack, decay, sustain, release
    //   Gain   gain
    //   Mix    gain, level1..level4
    //   Delay  time, feedback, mix
    //   Drive  drive, mix                      + `mode`: soft|hard|atan|fold
    //   Chorus rate, depth, mix
    //   Reverb size, damp, mix
    //   Map    inMin, inMax, outMin, outMax, curve
    //   Param  smooth (seconds)                + `mode`: the input's name
    //   Const  value
    //   Out    gain
    std::vector<std::pair<std::string, float>> params;
    std::string mode;                 // the one non-numeric setting a module has
    float       x = 0.0f, y = 0.0f;   // where the editor drew it

    float param(const std::string& key, float fallback = 0.0f) const;
    void  setParam(const std::string& key, float value);
};

struct Wire {
    int from = 0;      // module id
    int to   = 0;      // module id
    int port = 0;      // which input of `to`
};

// A dial the game turns. The range is what the editor's preview sweeps and what
// Map modules are written against; it does not clamp what the game sends.
struct PatchInput {
    std::string name;
    float       min     = 0.0f;
    float       max     = 1.0f;
    float       value   = 0.0f;    // where it starts
};

struct Patch {
    std::string             name;
    std::vector<ModuleDef>  modules;
    std::vector<Wire>       wires;
    std::vector<PatchInput> inputs;

    // JSON, the same shape the scene files use. `error` gets why it failed.
    static bool fromJson(const std::string& text, Patch& out, std::string* error = nullptr);
    std::string toJson() const;
    // Through fitzel::vfs, so a patch in a packed .fpak loads like any asset.
    static bool load(const std::string& path, Patch& out, std::string* error = nullptr);
    bool        save(const std::string& path) const;

    int  nextId() const;
    const ModuleDef* find(int id) const;
    ModuleDef*       find(int id);
};

// A compiled patch, ready to run. Compile on any thread; process() on the audio
// thread only, and setInput() from anywhere.
class Graph {
public:
    Graph();
    ~Graph();
    Graph(Graph&&) noexcept;
    Graph& operator=(Graph&&) noexcept;

    // Lays the patch out in processing order and sizes every buffer. Fails --
    // with a reason -- on a cycle or a patch with no Out module, and leaves the
    // graph unusable rather than half-built.
    bool compile(const Patch& patch, double sampleRate, int maxBlock,
                 std::string* error = nullptr);
    bool ready() const;
    void reset();

    // New numbers for the modules that are already there: same modules, same
    // wires, different settings. This is what an editor calls while a knob is
    // moving -- recompiling for every turn would rebuild the voice mid-note and
    // click. False when the patch's SHAPE changed (a module or wire added or
    // removed, a mode switched); compile() again for that.
    //
    // Safe to call while the graph is playing: the values cross to the audio
    // thread as atomics and are picked up at the start of the next block.
    bool updateParams(const Patch& patch);

    // The dials, in the order the patch lists them. Look the index up once and
    // set by index in anything that runs per frame; the name lookup is a string
    // compare and belongs in setup code.
    const std::vector<std::string>& inputNames() const;
    int   inputIndex(const std::string& name) const;
    void  setInput(int index, float value);
    void  setInput(const std::string& name, float value);
    float input(int index) const;

    // Render `frames` mono samples. Mono on purpose: this is what feeds a
    // positioned voice, and miniaudio's spatializer wants one channel to place
    // in the world. Stereo material (music, an ambience bed) is a later job for
    // a stereo Out module, not a reason to double every buffer now.
    void process(float* out, int frames);

    // What the last compile made of the patch, for the editor to show: the
    // processing order, and whether a module ended up feeding the Out at all.
    const std::vector<int>& order() const;
    bool  reaches(int moduleId) const;

    // The samples module `moduleId` produced in the last block, for a scope
    // beside it in the editor -- the quickest answer there is to "why can I not
    // hear this". Null for a module the graph does not have; valid until the
    // next process().
    const float* buffer(int moduleId, int& frames) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fitzel::synth
