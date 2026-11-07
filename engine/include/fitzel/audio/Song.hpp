#pragma once

#include <cstdint>
#include <string>
#include <vector>

// A song: what the Song Sequencer arranges and a Song Player plays.
//
// It is laid out the way every DAW lays one out, because that is the picture
// people already have: TRACKS stacked on one clock, CLIPS on the tracks, notes
// inside the MIDI clips. Three kinds of track:
//
//   Audio      -- clips of sound files (wav, ogg, mp3, flac), trimmed, faded.
//   Instrument -- an instrument and the MIDI clips it plays: one of fitzel's
//                 own synth patches, the built-in drum kit, or a VST3 instrument
//                 (fitzel/audio/Vst3.hpp) with its state saved in the song.
//   MIDI       -- MIDI clips that play ANOTHER track's instrument, on a MIDI
//                 channel of their own. One multitimbral plugin, many parts.
//
// TIME IS MUSICAL. Everything is placed in ticks (kPpq to the quarter note),
// so a change of tempo moves the whole song with it and the grid stays the
// grid. Seconds only exist where sound does: an audio clip's offset into its
// file and its fades are seconds, because a file plays at its own speed
// whatever the tempo says.
//
// Plain data, no audio in here (SongEngine.hpp plays it). Saved as JSON beside
// the project's other assets, in content/songs/.
namespace fitzel::song {

constexpr int kPpq = 960;   // ticks per quarter note

enum class TrackKind { Audio = 0, Instrument = 1, Midi = 2 };
enum class InstrumentKind { None = 0, Synth = 1, Drums = 2, Vst3 = 3 };

const char* kindName(TrackKind k);
const char* instrumentKindName(InstrumentKind k);

// One note in a MIDI clip. `tick` counts from the clip's content start (see
// Clip::offset), so moving a clip moves its notes without touching them.
struct Note {
    std::int64_t tick     = 0;
    std::int64_t length   = kPpq / 4;
    int          pitch    = 60;    // 0..127, 60 = middle C
    int          velocity = 100;   // 1..127
    bool         selected = false; // the piano roll's; not saved

    std::int64_t end() const { return tick + length; }
};

// One block on a track: [start, start + length) on the song.
//
// TRIMMING THE HEAD moves `start` and `offset` together, so what is under the
// playhead stays put -- which is what trimming means in every editor anyone
// has used. For MIDI the offset is content ticks; for audio, seconds into the
// file.
struct Clip {
    int           uid    = 0;
    std::int64_t  start  = 0;
    std::int64_t  length = kPpq * 4;
    std::string   name;
    std::uint32_t color  = 0;      // 0: the track's
    bool          mute   = false;

    // MIDI.
    std::int64_t      offset = 0;  // content tick heard at `start`
    // > 0: the content repeats every `loop` ticks for as long as the clip is
    // long -- a one-bar drum pattern stretched over sixteen bars.
    std::int64_t      loop   = 0;
    std::vector<Note> notes;       // kept sorted by tick

    // Audio.
    std::string file;              // sound name, resolved like every sound in the project
    double      sourceOffset = 0.0;   // seconds into the file at `start`
    float       gain    = 1.0f;       // linear, 0..4
    float       fadeIn  = 0.0f;       // seconds
    float       fadeOut = 0.0f;       // seconds

    std::int64_t end() const { return start + length; }
    bool covers(std::int64_t t) const { return t >= start && t < end(); }
    void sortNotes();
};

// The notes a clip plays, in SONG ticks, appended to `out` in order: its
// content seen through [start, end), a looping clip's content repeated, a note
// that runs past the clip's end (or its loop's) cut there. A note the head was
// trimmed into is not played -- nothing chases notes, as in most sequencers.
// Not transposed.
void playedNotes(const Clip& c, std::vector<Note>& out);

// What makes the sound on an Instrument track.
struct Instrument {
    InstrumentKind kind  = InstrumentKind::Synth;
    // Synth: a factory patch ("builtin:Saw Lead", see SongEngine.hpp) or the
    // name of one of the project's patches (content/patches, the Synth panel's).
    std::string    patch = "builtin:Saw Lead";
    int            voices = 12;      // Synth: notes at once
    // Vst3: which plugin, its name for when it is not installed, and what it
    // remembers (Instance::state) -- written back when the song is saved.
    std::string    pluginUid, pluginName;
    std::string    state;

    // The same instrument, state aside: a running one can be kept.
    bool sameEngine(const Instrument& o) const {
        return kind == o.kind && patch == o.patch && voices == o.voices && pluginUid == o.pluginUid;
    }
    std::string label() const;   // what a track header shows
};

struct Track {
    int           uid  = 0;
    TrackKind     kind = TrackKind::Instrument;
    std::string   name;
    std::uint32_t color = 0;       // ImGui-packed ABGR; 0 = by position
    float         volume = 0.8f;   // linear, 0..2 (Audio, Instrument)
    float         pan    = 0.0f;   // -1 left .. 1 right
    bool          mute = false, solo = false;
    int           transpose = 0;   // semitones, on every note the track plays
    Instrument    instrument;      // Instrument tracks
    int           target  = 0;     // MIDI tracks: uid of the Instrument track they play
    int           channel = 0;     // MIDI tracks: the channel the notes go out on, 0..15
    // Where it is heard (Audio, Instrument): "" the song's own mix -- the one
    // channel its player feeds -- or a mixer channel by name, whose inserts,
    // sends and fader it then goes through ("Song: Drums"). A channel that is
    // not on the desk means the song's own mix again.
    std::string   output;
    float         height  = 64.0f; // the arrangement's lane height (pixels)
    std::vector<Clip> clips;       // kept sorted by start

    bool hasNotes() const { return kind != TrackKind::Audio; }
    bool makesSound() const { return kind != TrackKind::Midi; }
    void sortClips();
};

struct Song {
    std::string name = "Song";
    double      bpm  = 120.0;
    int         beatsPerBar = 4;
    int         beatUnit    = 4;      // 4 = the beat is a quarter note
    bool        loop        = false;
    std::int64_t loopStart  = 0;
    std::int64_t loopEnd    = kPpq * 16;
    // Where the song ends; 0 = where its last clip does. A Song Player that
    // does not loop stops here.
    std::int64_t endTick    = 0;
    float       masterVolume = 1.0f;
    std::vector<Track> tracks;
    int         nextUid = 1;

    int newUid() { return nextUid++; }
    Track&       addTrack(TrackKind kind, const std::string& name = {});
    Track*       track(int uid);
    const Track* track(int uid) const;
    Clip*        clip(int uid, Track** owner = nullptr);
    const Clip*  clip(int uid, const Track** owner = nullptr) const;
    // Make every uid in the song unique again and `nextUid` past them all --
    // after a load or a paste, so a hand-edited or merged file cannot alias.
    void         fixUids();

    // --- Time ---------------------------------------------------------------
    std::int64_t ticksPerBeat() const { return (static_cast<std::int64_t>(kPpq) * 4) / (beatUnit > 0 ? beatUnit : 4); }
    std::int64_t ticksPerBar() const { return ticksPerBeat() * (beatsPerBar > 0 ? beatsPerBar : 4); }
    double       secondsPerTick() const { return 60.0 / ((bpm > 1.0 ? bpm : 1.0) * kPpq); }
    double       seconds(std::int64_t tick) const { return static_cast<double>(tick) * secondsPerTick(); }
    std::int64_t tickAt(double seconds) const;
    // Where the last clip ends (0 for an empty song).
    std::int64_t contentEnd() const;
    // endTick, or the content's end when that is unset.
    std::int64_t length() const { return endTick > 0 ? endTick : contentEnd(); }

    // --- Files --------------------------------------------------------------
    std::string toJson() const;
    static bool fromJson(const std::string& text, Song& out, std::string* error = nullptr);
    // Through fitzel::vfs, so a song packed into the .fpak loads like any asset.
    static bool load(const std::string& path, Song& out, std::string* error = nullptr);
    // Straight to disk.
    bool        save(const std::string& path, std::string* error = nullptr) const;
};

// "17.3.240": bar, beat (both from 1) and tick in the beat, as a DAW shows it.
std::string barBeatTick(const Song& s, std::int64_t tick);
// "1:23.456".
std::string clockTime(double seconds);
// "C4" for 60, "F#3" for 54.
std::string noteName(int pitch);

// --- Standard MIDI Files -----------------------------------------------------
//
// Read keeping TICKS, unlike synth::MidiSequence (which flattens to seconds for
// a player that never needs a grid): an imported part has to land on the bars
// it was written on. Tempo and meter come from the file's first tempo and time
// signature; ticks are rescaled from the file's division to kPpq.
struct MidiPart {
    std::string       name;          // the track name in the file, if it has one
    int               channel = 0;   // 0..15
    int               program = -1;  // the first program change, -1 none
    std::vector<Note> notes;         // ticks from the start of the file
};

struct MidiFile {
    double                bpm = 120.0;
    int                   beatsPerBar = 4, beatUnit = 4;
    bool                  hasTempo = false, hasMeter = false;
    std::vector<MidiPart> parts;     // one per (file track, channel) that has notes
    std::int64_t          length = 0;   // ticks to the last note's end

    static bool parse(const std::vector<std::uint8_t>& bytes, MidiFile& out, std::string* error = nullptr);
    static bool load(const std::string& path, MidiFile& out, std::string* error = nullptr);
};

// Every part of `file` as an Instrument track of its own, starting at `at`:
// channel 10 gets the drum kit, the rest a factory patch picked by its General
// MIDI program. `adoptTempo` takes the file's tempo and meter for the song.
// Returns the uids of the tracks it made.
std::vector<int> importMidi(Song& song, const MidiFile& file, std::int64_t at, bool adoptTempo);

// The song's notes as a format-1 file: a tempo track, then one track per track
// that plays notes (muted clips left out), each on its channel (MIDI tracks
// their own; Instrument tracks 0, the drum kit 9).
std::vector<std::uint8_t> exportMidi(const Song& song);

} // namespace fitzel::song
