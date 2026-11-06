#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "fitzel/audio/Mixer.hpp"

// VST3 plugins as insert effects on the mixer (fitzel/audio/Mixer.hpp).
//
// The host is the one Steinberg describes, cut down to what an insert needs: one
// stereo bus in, one out, 32-bit floats, parameters both ways, the plugin's own
// state saved as bytes, and its editor in a window of its own. No side chains,
// no latency compensation.
//
// ...and instruments, for the song sequencer (SongEngine.hpp): the same
// instance played with notes through processNotes(), each landing on the
// sample it is due at, with the song's tempo and position in the process
// context so an arpeggiator or a synced LFO follows the song.
//
// Threads: everything here belongs to the main thread -- the one that runs the
// frame -- except Instance::process(), which the mixer calls on the audio thread.
// VST3 asks for exactly that split. idle() has to run once a frame for open
// editors to draw and react (their timers and sockets are serviced there).
//
// Plugins are looked for in ~/.vst3, /usr/local/lib/vst3, /usr/lib/vst3 and
// every folder in $VST3_PATH (colon-separated). Linux only for now: elsewhere
// supported() is false and there is nothing to find.
namespace fitzel::vst3 {

// One audio-processor class in one bundle.
struct PluginInfo {
    std::string uid;        // the class id, 32 hex digits: what a saved insert names
    std::string name;
    std::string vendor;
    std::string version;
    std::string category;   // the VST3 sub-categories, "Fx|Delay"
    std::string path;       // the .vst3 bundle
    bool        instrument = false;  // makes sound from notes, not from its input
};

bool supported();
std::vector<std::string> searchPaths();

// Every plugin in the search paths. The first call scans; later ones return
// what it found unless `rescan`. A bundle's moduleinfo.json is read when it has
// one, so scanning runs no plugin code; a bundle without is loaded and asked.
const std::vector<PluginInfo>& plugins(bool rescan = false);
const PluginInfo*              find(const std::string& uid);

struct ParamInfo {
    uint32_t    id = 0;
    std::string title;
    std::string units;
    double      defaultValue = 0.0;  // normalized, 0..1
    int         steps = 0;           // 0: continuous; n: n+1 values
    bool        readOnly = false;
    bool        hidden = false;
    bool        bypass = false;      // the plugin's own bypass switch
};

// A note for an instrument, `frame` samples into the block it is handed with.
struct NoteEvent {
    uint32_t frame    = 0;
    uint8_t  channel  = 0;     // 0..15
    uint8_t  pitch    = 60;
    float    velocity = 0.8f;  // 0..1 (0 on a note-off)
    bool     on       = true;
};

// Where the song is, for the process context. `ppq` is quarter notes from the
// start of the song at the first sample of the block.
struct Transport {
    double  tempo   = 120.0;
    int     beatsPerBar = 4, beatUnit = 4;
    bool    playing = false;
    double  ppq     = 0.0;
    double  barPpq  = 0.0;     // where the current bar started, quarter notes
    int64_t samples = 0;       // song position in samples
    bool    cycle   = false;
    double  cycleStart = 0.0, cycleEnd = 0.0;   // quarter notes
};

// One plugin, running. Made by create() (null when the plugin would not load --
// `error` says why), gone when the last reference goes: the desk's insert holds
// one, the mixer's chain another while it plays through it.
class Instance final : public mixfx::External {
public:
    static std::shared_ptr<Instance> create(const PluginInfo& info, double sampleRate,
                                            std::string* error = nullptr);
    ~Instance() override;
    Instance(const Instance&)            = delete;
    Instance& operator=(const Instance&) = delete;

    const PluginInfo&             info() const;
    const std::vector<ParamInfo>& params() const;   // the parameters as of the last change
    double      param(uint32_t id) const;           // normalized
    // As if turned in the plugin's own editor: the editor follows, and the audio
    // thread gets it at the start of its next block.
    void        setParam(uint32_t id, double normalized);
    std::string display(uint32_t id, double normalized) const;   // the plugin's own text
    std::string display(uint32_t id) const { return display(id, param(id)); }

    // Everything the plugin remembers, as bytes (processor and editor state),
    // and the way back. setState is for a fresh instance, before it plays.
    std::string state() const;
    bool        setState(const std::string& bytes);

    bool hasEditor() const;
    bool editorOpen() const;
    bool openEditor(const std::string& title = {});
    void closeEditor();

    // Audio thread: in place, interleaved, any number of frames.
    void process(float* interleaved, unsigned frames, unsigned channels) override;
    // Audio thread, for an instrument: `events` (sorted by frame, frames within
    // this call) go to its event input, its output lands in `interleaved` (what
    // was there is its audio input, if it has one -- silence for a synth).
    // `transport` null keeps the last one, advanced by the block.
    void processNotes(float* interleaved, unsigned frames, unsigned channels,
                      const NoteEvent* events, int count, const Transport* transport = nullptr);
    // Takes notes at all: the plugin has an event input.
    bool acceptsNotes() const;

    struct Impl;

private:
    explicit Instance(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m;
};

// Services open editors: their timers, their sockets, their window's events.
// Once a frame on the main thread; cheap when nothing is open.
void idle();

} // namespace fitzel::vst3
