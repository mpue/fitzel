#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "fitzel/audio/SampleData.hpp"
#include "fitzel/audio/Song.hpp"

namespace fitzel::synth { struct Patch; }
namespace fitzel::vst3 { class Instance; }

// A song, played: the render half of the song sequencer (Song.hpp is the data).
//
// No device in here -- process() is handed a buffer and fills it, the way
// synth::Poly does -- so songcheck renders a song offline and measures it, a
// bounce writes it to a WAV, and SongPlayer.hpp is only the thin voice that
// hangs this off the mixer.
//
// THREADS. The editor changes the song while it plays: a note dragged, a clip
// trimmed, the tempo turned. setSong() compiles the song into a PLAN -- every
// note in samples, every audio clip with its decoded file -- and hands it to
// the audio thread by pointer; the plan it replaces comes back and is freed on
// the main thread, never in the callback. Instruments are not part of a plan:
// one is kept per Instrument track for as long as the track asks for the same
// one, so editing a note does not restart a plugin or cut a ringing chord.
// Notes sounding when a new plan arrives end where they were going to end.
//
// Transport, hand-played notes and panic go through a small command queue and
// land at the start of the next block. Nothing the audio thread runs
// allocates, locks or waits.
namespace fitzel::song {

// fitzel's own patches for an Instrument track that wants a sound without a
// plugin: "Saw Lead", "Bass", "Soft Pad", ... A song names one "builtin:Bass".
const std::vector<std::string>& factoryPatchNames();
// With or without the "builtin:" in front. False for a name that is not one.
bool factoryPatch(const std::string& name, synth::Patch& out);

// Names to files, the engine's one window on the project.
struct Files {
    std::function<std::string(const std::string&)> sound;   // an audio clip's file
    std::function<std::string(const std::string&)> patch;   // a Synth instrument's patch
};

class Engine {
public:
    explicit Engine(double sampleRate, int maxBlock = 512);
    ~Engine();
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    double sampleRate() const;

    // --- Main thread ---------------------------------------------------------
    void setFiles(Files files);
    // The song to play from the next block on (see the threads note above).
    // Cheap enough to call on every edit; instruments are made here when a
    // track asks for a new one (VST3 wants them made on this thread).
    void setSong(const Song& song);
    // The instrument of one track made again at the next setSong() even if the
    // song still names the same one -- its patch file was saved again in the
    // Synth panel.
    void reloadInstrument(int trackUid);

    void play();
    void stop();                        // where it is; notes let go
    void seek(std::int64_t tick);       // notes let go; plays on if playing
    bool playing() const;
    // Where the song is, in ticks / seconds, as of the last block rendered.
    double position() const;
    double positionSeconds() const;
    double ticksPerSecond() const;   // the tempo, as the audio thread plays it
    // Stop by itself at the song's end when it is not looping. Off for the
    // editor (the playhead runs on past the last clip), on for a Song Player.
    void setStopAtEnd(bool on);
    // Ran to its end and stopped there (since the last play()).
    bool ended() const;
    void setMetronome(bool on);

    // A note played by hand: the piano roll's keyboard, the computer keyboard,
    // a script. A MIDI track's goes to the instrument it plays, on its channel.
    // Any thread.
    void liveNote(int trackUid, int pitch, float velocity, bool on);
    // Every note on every instrument let go now.
    void allNotesOff();

    // Loudest sample since the last call, after the fader (0..1+).
    float takePeak(int trackUid);
    float takeMasterPeak();

    // The plugin playing on a track (null: none, or not a VST3) -- for its
    // editor window and parameters.
    std::shared_ptr<vst3::Instance> plugin(int trackUid) const;
    // Why a track's instrument does not play ("" when it does).
    std::string instrumentError(int trackUid) const;
    // The plugins' state as it is now into the song, for saving it.
    void captureStates(Song& song) const;
    // Sound files still being decoded, and those that failed ("file: why").
    bool samplesPending() const;
    std::vector<std::string> sampleErrors() const;

    // --- Audio thread --------------------------------------------------------
    // `frames` stereo frames, interleaved L R.
    void process(float* stereo, int frames);

    // The whole song, offline, from the top to its end plus `tailSeconds` for
    // releases and echoes: a bounce. Loop ignored. Plugins load fresh with the
    // state the song holds (captureStates it first). `progress` gets 0..1 and
    // may return false to give up.
    static bool render(const Song& song, const Files& files, double sampleRate,
                       SampleData& out, std::string* error = nullptr,
                       double tailSeconds = 2.0,
                       const std::function<bool(float)>& progress = {});

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

// Engine::render a piece at a time, for an editor that has a frame to draw
// between pieces -- and that must make its plugins on the main thread, which a
// worker thread would not be. start(), then step() until done().
class Bounce {
public:
    Bounce();
    ~Bounce();
    // `problems` gets what will not sound (a missing plugin, a file that did
    // not decode) once the files are in -- see problems().
    void start(const Song& song, const Files& files, double sampleRate, double tailSeconds = 2.0);
    // Renders up to `frames` more; false while the sound files are still
    // being decoded (nothing rendered yet).
    bool step(int frames);
    bool  done() const;
    float progress() const;
    const std::string& problems() const;
    // The result, once done(); taken out of the bouncer.
    SampleData take();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace fitzel::song
