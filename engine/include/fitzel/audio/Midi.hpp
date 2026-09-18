#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A Standard MIDI File, read down to what a synthesiser plays: notes, in
// seconds.
//
// A .mid file counts time in ticks, and what a tick is worth depends on the
// tempo in force at that point -- which can change anywhere in the song. So the
// whole tempo map is applied here, once, while reading, and what comes out is a
// flat list of note-ons and note-offs with their time in seconds. The player
// never has to know a tick existed.
//
// Formats 0 and 1 (one track, or several played together), both kinds of time
// division. Everything that is not a note or a tempo -- controllers, program
// changes, lyrics, SysEx -- is read past and dropped: this is a score for one
// patch, not a General MIDI renderer.
namespace fitzel::synth {

struct MidiEvent {
    double       time     = 0.0;   // seconds from the start
    std::uint8_t channel  = 0;     // 0..15 (the one people call "10" is 9)
    std::uint8_t note     = 60;    // MIDI note number, 60 = middle C
    float        velocity = 0.0f;  // 0..1; 0 on a note-off
    bool         on       = false;
};

struct MidiSequence {
    std::vector<MidiEvent> events;   // sorted by time; offs before ons at a tie
    double                 length = 0.0;   // seconds, to the last event
    int                    format = 0;
    int                    tracks = 0;

    // From the bytes of a .mid file. `error` says why on failure.
    static bool parse(const std::vector<std::uint8_t>& bytes, MidiSequence& out,
                      std::string* error = nullptr);
    // Through fitzel::vfs, so a song packed into the .fpak loads like any asset.
    static bool load(const std::string& path, MidiSequence& out,
                     std::string* error = nullptr);
};

} // namespace fitzel::synth
