#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "fitzel/audio/Midi.hpp"
#include "fitzel/audio/Synth.hpp"

// One patch, several notes at once, and a MIDI sequencer to play them.
//
// A patch is written for ONE voice: a dial called "pitch" (a MIDI note number),
// one called "gate" (over 0.5 while the key is down) and, if it wants one,
// "velocity" (0..1). Poly compiles that patch once per voice and hands each
// incoming note to a voice of its own -- the way a polyphonic synthesiser is
// built out of copies of a monophonic one. Every other dial is shared: turning
// "brightness" turns it on all of them.
//
// The sequencer runs INSIDE process(): a block that has a note-on in the middle
// of it is split there and rendered in two pieces, so notes land on the sample
// they are due at rather than on the next block boundary. That matters for a
// song -- 256-sample steps are 5 ms of swing on every note.
//
// No threads and no device in here (see SynthPlayer for the voice the mixer
// plays), which is what lets synthcheck render a song offline and measure it.
namespace fitzel::synth {

class Poly {
public:
    // `voices` copies of the patch. Fails like Graph::compile does.
    bool compile(const Patch& patch, double sampleRate, int voices, int maxBlock,
                 std::string* error = nullptr);
    bool ready() const { return !m_voices.empty(); }
    int  voiceCount() const { return static_cast<int>(m_voices.size()); }
    // New numbers for all voices, see Graph::updateParams.
    bool updateParams(const Patch& patch);
    void reset();

    // --- Dials shared by every voice --------------------------------------
    int  inputIndex(const std::string& name) const;
    void setInput(int index, float value);

    // --- Notes --------------------------------------------------------------
    void noteOn(int note, float velocity);
    void noteOff(int note);
    void allNotesOff();
    int  heldNotes() const;
    // Voices still making sound -- held, or ringing out after a release.
    int  soundingVoices() const;

    // --- The sequencer ------------------------------------------------------
    // `seq` is NOT owned and has to outlive its use here (SynthPlayer hands
    // them over between threads and keeps them alive).
    void setSequence(const MidiSequence* seq);
    void play(bool fromStart);
    void stop();                 // stops the song and lets its notes go
    bool playing() const { return m_playing; }
    double position() const { return m_clock; }
    void setLoop(bool loop) { m_loop = loop; }
    // 1 = as written, 2 = twice as fast.
    void setTempo(float scale) { m_tempo = scale > 0.01f ? scale : 0.01f; }
    // 0..15 plays one channel, -1 all of them.
    void setChannel(int channel) { m_channel = channel; }
    // Channel 10 is the General MIDI drum kit: its notes are not pitches but
    // drums, and played through a melodic patch they come out as a random
    // scatter of notes over everything else. Skipped unless asked for.
    void setSkipDrums(bool skip) { m_skipDrums = skip; }

    // Level of the voices summed. Several at full scale is more than full
    // scale, so the default leaves room for a chord.
    void setGain(float g) { m_gain = g; }

    // Render `frames` mono samples: every sounding voice, summed, plus whatever
    // the song asks for along the way. Clamped to [-1, 1], finite.
    void process(float* out, int frames);

private:
    struct Voice {
        Graph         graph;
        int           note = -1;     // what it is playing, -1 none
        bool          held = false;  // key still down
        bool          silent = true; // released and faded out (or never used)
        std::uint64_t started = 0, released = 0;   // stamps, for picking one to reuse
        std::uint64_t releasedBlock = 0;           // block it was let go in
    };

    std::vector<Voice> m_voices;
    std::vector<float> m_tmp;        // one voice's block, sized at compile
    int                m_pitch = -1, m_gate = -1, m_vel = -1;
    double             m_rate  = 48000.0;
    std::uint64_t      m_stamp = 0, m_block = 0;
    float              m_gain  = 0.5f;

    const MidiSequence* m_seq   = nullptr;
    std::size_t         m_pos   = 0;
    double              m_clock = 0.0;
    bool                m_playing = false, m_loop = false, m_skipDrums = true;
    float               m_tempo = 1.0f;
    int                 m_channel = -1;

    // Song time is rounded to the nearest sample. The clock is a running sum
    // of chunk lengths and lands a hair either side of where an event is; a
    // strict comparison turns that hair into a whole extra sample, and on a
    // looped song one more every time round.
    double halfSample() const { return 0.5 * static_cast<double>(m_tempo) / m_rate; }
    void fireDue();
    void render(float* out, int frames);
};

} // namespace fitzel::synth
