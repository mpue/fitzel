// songcheck -- the song sequencer's engine, rendered offline and measured.
//
// A sequencer that is a few milliseconds off sounds "loose" and nobody can say
// why; one that drops a note-off leaves a key hanging only sometimes. So this
// renders songs with no audio device near them and checks, to the sample:
//   - a note lands on its sample, on a Synth patch and on a VST3 instrument
//     (vst3fixture's test synth, which puts out a flat level while a key is
//     held), and is let go on its sample;
//   - a loop comes round on the bar, cutting what it was holding;
//   - a looping clip repeats its content, a trimmed clip plays what is left;
//   - mute and solo (a soloed MIDI track brings its instrument with it);
//   - an audio clip starts on its bar, from its offset, at its gain, resampled;
//   - a new tempo keeps the playhead on its beat; an edit while a chord rings
//     does not cut it; a hand-played note sounds and lets go;
//   - every factory patch and every drum of the kit makes a finite sound;
//   - a song survives JSON, and its notes a MIDI file, both ways;
//   - a bounce is as long as the song plus its tail.
//   build/linux/bin/songcheck [--wav demo.wav]   (the demo song, bounced)
//   build/linux/bin/songcheck --plugin BS-1 [--wav out.wav]
//   build/linux/bin/songcheck --bench      (what a full arrangement costs a block)
// The second form plays a few bars through an installed VST3 instrument (by
// name, from the usual search paths) and says whether it made a sound.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if FITZEL_VST3
#include <dlfcn.h>
#endif

#include <fitzel/audio/SampleData.hpp>
#include <fitzel/audio/Song.hpp>
#include <fitzel/audio/SongEngine.hpp>
#include <fitzel/audio/Synth.hpp>
#include <fitzel/audio/Vst3.hpp>

namespace fs = std::filesystem;
using namespace fitzel::song;

namespace {

int g_fails = 0;
void check(bool ok, const std::string& what) {
    std::printf(ok ? "  ok   %s\n" : "[FAIL] %s\n", what.c_str());
    if (!ok) ++g_fails;
}

constexpr double kRate = 48000.0;

// `seconds` of the engine's output, as it would be played.
std::vector<float> run(Engine& e, double seconds, int block = 256) {
    const int frames = static_cast<int>(std::lround(seconds * kRate));
    std::vector<float> out(static_cast<std::size_t>(frames) * 2, 0.0f);
    for (int at = 0; at < frames; at += block) {
        const int n = std::min(block, frames - at);
        e.process(out.data() + static_cast<std::size_t>(at) * 2, n);
    }
    return out;
}

// First frame at or after `from` whose left sample is above `level` (-1 none).
long onset(const std::vector<float>& st, long from = 0, float level = 1e-4f) {
    for (std::size_t i = static_cast<std::size_t>(std::max(0L, from)); i * 2 < st.size(); ++i)
        if (std::fabs(st[i * 2]) > level) return static_cast<long>(i);
    return -1;
}
// First frame at or after `from` that is silent (|left| below `level`).
long offset(const std::vector<float>& st, long from, float level = 1e-4f) {
    for (std::size_t i = static_cast<std::size_t>(std::max(0L, from)); i * 2 < st.size(); ++i)
        if (std::fabs(st[i * 2]) <= level) return static_cast<long>(i);
    return -1;
}
float peakOf(const std::vector<float>& st, long a = 0, long b = -1) {
    if (b < 0) b = static_cast<long>(st.size() / 2);
    float p = 0.0f;
    for (long i = std::max(0L, a); i < b && static_cast<std::size_t>(i) * 2 + 1 < st.size(); ++i)
        p = std::max(p, std::max(std::fabs(st[static_cast<std::size_t>(i) * 2]), std::fabs(st[static_cast<std::size_t>(i) * 2 + 1])));
    return p;
}
bool finite(const std::vector<float>& st) {
    return std::all_of(st.begin(), st.end(), [](float v) { return std::isfinite(v); });
}

long frameOf(const Song& s, std::int64_t tick) { return std::lround(s.seconds(tick) * kRate); }

Clip noteClip(Song& s, std::int64_t start, std::int64_t length, std::initializer_list<Note> notes) {
    Clip c;
    c.uid    = s.newUid();
    c.start  = start;
    c.length = length;
    c.notes  = notes;
    c.sortNotes();
    return c;
}

Note note(std::int64_t tick, std::int64_t len, int pitch, int vel = 127) {
    Note n;
    n.tick = tick;
    n.length = len;
    n.pitch = pitch;
    n.velocity = vel;
    return n;
}

void writeWav(const fs::path& p, int rate, float seconds, float level) {
    const int n = static_cast<int>(seconds * rate);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(n), static_cast<std::int16_t>(level * 32767.0f));
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + n * 2); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(static_cast<std::uint32_t>(rate)); u32(static_cast<std::uint32_t>(rate * 2)); u16(2); u16(16);
    f.write("data", 4); u32(static_cast<std::uint32_t>(n * 2));
    f.write(reinterpret_cast<const char*>(pcm.data()), n * 2);
}

void waitSamples(Engine& e) {
    for (int i = 0; i < 4000 && e.samplesPending(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
}

#if FITZEL_VST3
using StatFn = int (*)(const char*);
StatFn g_stat = nullptr;
int stat(const char* what) { return g_stat ? g_stat(what) : -1; }
#endif

// --- The model -----------------------------------------------------------------
void modelChecks() {
    std::printf("model\n");
    Song s;
    s.name = "Check";
    s.bpm = 98.5;
    s.beatsPerBar = 3;
    Track& lead = s.addTrack(TrackKind::Instrument, "Lead");
    lead.instrument.kind = InstrumentKind::Vst3;
    lead.instrument.pluginUid = "0123456789ABCDEF0123456789ABCDEF";
    lead.instrument.pluginName = "Some Synth";
    const std::string state("\0\1\2binary\xff\xfe", 11);
    lead.instrument.state = state;
    lead.clips.push_back(noteClip(s, kPpq * 3, kPpq * 6, {note(0, 240, 60, 90), note(480, 240, 64, 1)}));
    const int leadUid = lead.uid;
    Track& midi = s.addTrack(TrackKind::Midi, "Strings part");
    midi.target = leadUid;
    midi.channel = 5;
    midi.transpose = -12;
    Track& audio = s.addTrack(TrackKind::Audio, "Vox");
    Clip ac;
    ac.uid = s.newUid();
    ac.file = "vox.wav";
    ac.sourceOffset = 1.25;
    ac.gain = 0.7f;
    ac.fadeIn = 0.1f;
    audio.clips.push_back(ac);

    Song back;
    std::string err;
    check(Song::fromJson(s.toJson(), back, &err), "a song reads back from its JSON (" + err + ")");
    check(back.bpm == 98.5 && back.beatsPerBar == 3 && back.tracks.size() == 3, "tempo, meter and tracks survive");
    const Track* bl = back.track(leadUid);
    check(bl && bl->instrument.kind == InstrumentKind::Vst3 && bl->instrument.state == state &&
              bl->instrument.pluginName == "Some Synth",
          "a plugin's state survives, bytes and all (base64)");
    check(bl && bl->clips.size() == 1 && bl->clips[0].notes.size() == 2 && bl->clips[0].notes[1].velocity == 1 &&
              bl->clips[0].start == kPpq * 3,
          "clips and notes survive");
    check(back.tracks[1].kind == TrackKind::Midi && back.tracks[1].target == leadUid &&
              back.tracks[1].channel == 5 && back.tracks[1].transpose == -12,
          "a MIDI track keeps its target, channel and transpose");
    check(back.tracks[2].clips.size() == 1 && back.tracks[2].clips[0].file == "vox.wav" &&
              std::fabs(back.tracks[2].clips[0].sourceOffset - 1.25) < 1e-9 && back.tracks[2].clips[0].gain == 0.7f,
          "an audio clip keeps its file, offset and gain");
    check(barBeatTick(s, 0) == "1.1.000" && barBeatTick(s, kPpq * 4 + 120) == "2.2.120", "bar.beat.tick (in 3/4)");
    check(noteName(60) == "C4" && noteName(69) == "A4" && noteName(54) == "F#3", "note names");

    // Clips as heard.
    Clip c = noteClip(s, kPpq * 4, kPpq * 4, {note(0, 100, 36), note(kPpq, 100, 38), note(kPpq * 2 - 10, 200, 42)});
    std::vector<Note> out;
    playedNotes(c, out);
    check(out.size() == 3 && out[0].tick == kPpq * 4 && out[1].tick == kPpq * 5, "a clip's notes in song ticks");
    c.loop = kPpq * 2;
    out.clear();
    playedNotes(c, out);
    check(out.size() == 6 && out[3].tick == kPpq * 6 && out[4].tick == kPpq * 7, "a looping clip repeats its content");
    check(out[2].length == 10, "...and a note that runs over the loop's end is cut there (" + std::to_string(out[2].length) + ")");
    c.loop = 0;
    c.start += kPpq;
    c.offset = kPpq;
    c.length = kPpq * 3;
    out.clear();
    playedNotes(c, out);
    check(out.size() == 2 && out[0].tick == kPpq * 5 && out[0].pitch == 38,
          "a clip trimmed at the head plays what is left, where it was");

    // MIDI out and back in.
    Song m;
    m.bpm = 140.0;
    Track& d = m.addTrack(TrackKind::Instrument, "Beat");
    d.instrument.kind = InstrumentKind::Drums;
    d.clips.push_back(noteClip(m, 0, kPpq * 4, {note(0, 120, 36, 100), note(kPpq, 120, 38, 80), note(kPpq * 2, 60, 42, 50)}));
    Track& b = m.addTrack(TrackKind::Instrument, "Bass");
    b.clips.push_back(noteClip(m, kPpq * 4, kPpq * 4, {note(0, kPpq, 40, 110), note(kPpq * 2, kPpq / 2, 43, 100)}));
    MidiFile mf;
    check(MidiFile::parse(exportMidi(m), mf, &err), "an exported MIDI file reads back (" + err + ")");
    check(mf.hasTempo && std::fabs(mf.bpm - 140.0) < 0.01, "...with its tempo");
    bool drumsOn9 = false, notesSame = false;
    for (const MidiPart& p : mf.parts) {
        if (p.channel == 9) drumsOn9 = p.notes.size() == 3 && p.notes[1].tick == kPpq && p.notes[1].velocity == 80;
        if (p.name == "Bass") notesSame = p.notes.size() == 2 && p.notes[0].tick == kPpq * 4 && p.notes[1].length == kPpq / 2;
    }
    check(drumsOn9, "the drum kit goes out on channel 10, notes where they were");
    check(notesSame, "the bass's notes come back on their ticks with their lengths");
    Song imp;
    const auto made = importMidi(imp, mf, kPpq * 8, true);
    check(made.size() == 2 && std::fabs(imp.bpm - 140.0) < 1e-6, "an import makes a track per part and takes the tempo");
    const Track* drumsBack = nullptr;
    for (const Track& t : imp.tracks) if (t.instrument.kind == InstrumentKind::Drums) drumsBack = &t;
    check(drumsBack && drumsBack->clips.size() == 1 && drumsBack->clips[0].start == kPpq * 8,
          "channel 10 comes in as the drum kit, at the place asked for");

    // A file of another division: 480 ticks to the beat, 100 BPM.
    std::vector<std::uint8_t> smf = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0,
                                     'M', 'T', 'r', 'k', 0, 0, 0, 23,
                                     0x00, 0xFF, 0x51, 0x03, 0x09, 0x27, 0xC0,   // 600000 us: 100 BPM
                                     0x00, 0xC0, 0x21,                           // program 33: a bass
                                     0x83, 0x60, 0x90, 0x30, 0x64,               // 480: note on 48
                                     0x83, 0x60, 0x80, 0x30, 0x00,               // 480 later: off
                                     0x00, 0xFF, 0x2F, 0x00};
    check(MidiFile::parse(smf, mf, &err) && mf.parts.size() == 1, "a format-0 file at 480 PPQ reads (" + err + ")");
    check(std::fabs(mf.bpm - 100.0) < 0.01 && mf.parts[0].notes[0].tick == kPpq && mf.parts[0].notes[0].length == kPpq,
          "...its ticks rescaled to 960, its tempo found");
    Song gm;
    importMidi(gm, mf, 0, false);
    check(gm.tracks.size() == 1 && gm.tracks[0].instrument.patch == "builtin:Bass",
          "a General MIDI bass program gets the Bass patch");
}

// --- Synth patches and the drum kit -----------------------------------------------
void soundChecks() {
    std::printf("sounds\n");
    int good = 0;
    std::string bad;
    for (const std::string& name : factoryPatchNames()) {
        Song s;
        Track& t = s.addTrack(TrackKind::Instrument);
        // A quarter of the fader, and the level read back times four: the
        // master's clip at full scale would otherwise hide an over.
        t.volume = 0.25f;
        t.instrument.patch = "builtin:" + name;
        t.clips.push_back(noteClip(s, 0, kPpq * 8, {note(0, kPpq * 2, 48, 100), note(0, kPpq * 2, 55, 100),
                                                     note(0, kPpq * 2, 60, 100), note(0, kPpq * 2, 64, 100)}));
        Engine e(kRate);
        e.setSong(s);
        e.play();
        const auto out = run(e, 2.0);
        const float pk = peakOf(out) * 4.0f;
        const bool ok = e.instrumentError(t.uid).empty() && finite(out) && pk > 0.2f && pk < 0.9f;
        if (ok) ++good;
        else bad += " " + name + "(" + std::to_string(pk) + (e.instrumentError(t.uid).empty() ? "" : ", " + e.instrumentError(t.uid)) + ")";
        // A four-note chord at full velocity on one patch should leave room for more.
        // ...and one note on its own still has some level.
        Song one = s;
        one.tracks[0].clips[0].notes.resize(1);
        Engine e1(kRate);
        e1.setSong(one);
        e1.play();
        const float single = peakOf(run(e1, 2.0)) * 4.0f;
        std::printf("       %-12s chord peak %.2f, one note %.2f\n", name.c_str(), pk, single);
        if (single < 0.12f) { bad += " " + name + "(one note " + std::to_string(single) + ")"; --good; }
    }
    check(good == static_cast<int>(factoryPatchNames().size()), "every factory patch compiles and sounds, a chord below full scale" + bad);

    Song s;
    Track& kit = s.addTrack(TrackKind::Instrument, "Kit");
    kit.instrument.kind = InstrumentKind::Drums;
    kit.volume = 0.25f;   // read back times four, as above
    const int drums[] = {36, 37, 38, 39, 41, 42, 44, 46, 49, 51, 53, 54, 56, 60, 64};
    Clip c;
    c.uid = s.newUid();
    c.length = kPpq * 32;
    int i = 0;
    for (int d : drums) c.notes.push_back(note(static_cast<std::int64_t>(i++) * kPpq * 2, 120, d, 127));
    kit.clips.push_back(c);
    Engine e(kRate);
    e.setSong(s);
    e.play();
    const auto out = run(e, s.seconds(kPpq * 2 * 15) + 3.5);
    int sounding = 0;
    float loudest = 0.0f;
    for (int k = 0; k < 15; ++k) {
        const long a = frameOf(s, static_cast<std::int64_t>(k) * kPpq * 2);
        const float pk = peakOf(out, a, a + frameOf(s, kPpq * 2)) * 4.0f;
        if (pk > 0.08f) ++sounding;
        loudest = std::max(loudest, pk);
        std::printf("       drum %2d peak %.2f\n", drums[k], pk);
    }
    check(sounding == 15, "every drum of the kit sounds (" + std::to_string(sounding) + "/15)");
    check(finite(out) && loudest < 0.9f, "...finite and below full scale (" + std::to_string(loudest) + ")");
    check(peakOf(out, static_cast<long>(out.size() / 2) - 4800) < 1e-4f, "...and every voice ends");
    const long kick = onset(out, 0, 0.0025f);
    check(kick >= 0 && kick < 8, "the kick starts on its sample (" + std::to_string(kick) + ")");
}

// --- Timing on a Synth patch ---------------------------------------------------------
void synthTiming() {
    std::printf("timing (synth patch)\n");
    Song s;
    Track& t = s.addTrack(TrackKind::Instrument);
    t.instrument.patch = "builtin:Organ";
    t.volume = 1.0f;
    // 1.5 beats in at 120 BPM: 0.75 s = frame 36000, not on a 256 boundary.
    t.clips.push_back(noteClip(s, 0, kPpq * 8, {note(kPpq + kPpq / 2, kPpq, 69, 100)}));
    for (int block : {64, 256, 1000}) {
        Engine e(kRate);
        e.setSong(s);
        e.play();
        const auto out = run(e, 2.0, block);
        const long on = onset(out, 0, 1e-6f);
        check(on >= 36000 && on <= 36002, "a note lands on its sample with " + std::to_string(block) +
                                               "-frame blocks (" + std::to_string(on) + ", want 36000)");
    }
}

#if FITZEL_VST3
// --- A VST3 instrument -------------------------------------------------------------------
void pluginChecks() {
    std::printf("VST3 instrument\n");
    setenv("VST3_PATH", FITZEL_VST3_FIXTURE_DIR, 1);
    fitzel::vst3::plugins(true);
    const std::string uid = "5A1E7A1100F17E107E57A11D00000003";
    const fitzel::vst3::PluginInfo* info = fitzel::vst3::find(uid);
    check(info && info->instrument && info->name == "Fitzel Test Synth", "the fixture's synth is found, as an instrument");
    if (!info) return;

    Song s;
    s.bpm = 120.0;
    Track& t = s.addTrack(TrackKind::Instrument, "Plugin");
    t.instrument.kind = InstrumentKind::Vst3;
    t.instrument.pluginUid = uid;
    t.instrument.pluginName = info->name;
    t.volume = 1.0f;
    // Velocity 127 = level 0.1 while held.
    t.clips.push_back(noteClip(s, kPpq * 4, kPpq * 4, {note(kPpq / 3, kPpq, 60, 127), note(kPpq * 2, kPpq / 2, 64, 127)}));
    const int instUid = t.uid;
    Track& part = s.addTrack(TrackKind::Midi, "Part");
    part.target = instUid;
    part.channel = 3;
    part.clips.push_back(noteClip(s, kPpq * 12, kPpq * 4, {note(0, kPpq, 72, 127)}));

    Engine e(kRate);
    e.setSong(s);
    check(e.instrumentError(instUid).empty() && e.plugin(instUid) != nullptr, "it loads on its track");
    {
        const fs::path so = fs::path(info->path) / "Contents" / "x86_64-linux" / (fs::path(info->path).stem().string() + ".so");
        void* h = dlopen(so.c_str(), RTLD_LAZY | RTLD_NOLOAD);
        g_stat = h ? reinterpret_cast<StatFn>(dlsym(h, "fitzelFixtureStat")) : nullptr;
    }
    e.play();
    const auto out = run(e, 9.0, 256);
    const long want = frameOf(s, kPpq * 4 + kPpq / 3);
    const long on = onset(out, 0);
    check(on == want, "a note reaches the plugin on its sample (" + std::to_string(on) + ", want " + std::to_string(want) + ")");
    const long off = offset(out, on);
    check(off == want + frameOf(s, kPpq), "...and its note-off on its sample (" + std::to_string(off) + ")");
    check(std::fabs(out[static_cast<std::size_t>(on) * 2] - 0.1f) < 1e-4f, "velocity 127 arrives as 1.0 (level 0.1)");
    const long part0 = onset(out, frameOf(s, kPpq * 11));
    check(part0 == frameOf(s, kPpq * 12), "a MIDI track plays the instrument it targets (" + std::to_string(part0) + ")");
    check(stat("channelMask") == ((1 << 0) | (1 << 3)), "...on its own channel (mask " + std::to_string(stat("channelMask")) + ")");
    check(stat("noteOns") == 3 && stat("noteOffs") == 3, "three notes on, three off");
    check(stat("tempo") == 120 && stat("playing") == 1, "the plugin is told the tempo and that the song plays");
    const int ppq = stat("ppq100");
    check(ppq > 1600 && ppq < 1800, "...and where it is in quarter notes (" + std::to_string(ppq / 100.0) + ")");

    // A hand on a key: it sounds from the next block, and lets go.
    e.stop();
    run(e, 0.05);
    e.liveNote(instUid, 50, 1.0f, true);
    auto held = run(e, 0.05);
    e.liveNote(instUid, 50, 0.0f, false);
    auto after = run(e, 0.05);
    check(std::fabs(held[held.size() / 2] - 0.1f) < 1e-4f && peakOf(after, 300) < 1e-6f,
          "a note played by hand sounds and is let go");

    // Played fast: down and up again before the audio thread has run once. The
    // note sounds (a block long) and is let go -- it does not hang.
    e.liveNote(instUid, 52, 1.0f, true);
    e.liveNote(instUid, 52, 0.0f, false);
    auto quick = run(e, 0.05, 256);
    check(std::fabs(quick[0] - 0.1f) < 1e-4f, "a key let go in the same block as it went down still sounds");
    check(peakOf(quick, 512) < 1e-6f, "...and does not hang (" + std::to_string(peakOf(quick, 512)) + ")");
    // A run of them, faster than the blocks, overlapping.
    for (int i = 0; i < 40; ++i) {
        e.liveNote(instUid, 60 + i % 12, 0.9f, true);
        if (i % 3 == 2) run(e, 0.002, 64);
        e.liveNote(instUid, 60 + i % 12, 0.0f, false);
    }
    auto rest = run(e, 0.1, 256);
    check(peakOf(rest, 1024) < 1e-6f, "forty notes played faster than the blocks: none left hanging");

    // State: captured from the running plugin into the song.
    Song copy = s;
    e.captureStates(copy);
    check(copy.track(instUid)->instrument.state.rfind("FZV3", 0) == 0, "the plugin's state is captured into the song");
}
#endif

// --- Transport: loop, solo, tempo, edits ------------------------------------------------
void transportChecks() {
    std::printf("transport\n");
    // The organ holds its level, which makes onsets easy to see.
    Song s;
    Track& a = s.addTrack(TrackKind::Instrument, "A");
    a.instrument.patch = "builtin:Organ";
    a.volume = 1.0f;
    a.clips.push_back(noteClip(s, 0, kPpq * 4, {note(0, kPpq * 8, 60, 100)}));   // held past the loop
    s.loop = true;
    s.loopStart = 0;
    s.loopEnd = kPpq * 4;   // one bar: 2 s
    Engine e(kRate);
    e.setSong(s);
    e.play();
    const auto out = run(e, 5.0);
    const long bar = frameOf(s, kPpq * 4);
    // Let go at the loop's end: the release dies away before the bar starts again?
    // The organ's release is 70 ms; what matters is that it STARTS again on the bar.
    const float before = peakOf(out, bar - 2400, bar - 100);
    const float after  = peakOf(out, bar + 4800, bar + 9600);
    check(before > 0.05f && after > 0.05f, "a loop plays its bar again");
    check(std::fabs(e.position() - static_cast<double>(kPpq * 2)) < 2.0,
          "...and the playhead came round (" + std::to_string(e.position() / kPpq) + " beats)");

    // Solo and mute.
    Song m;
    m.addTrack(TrackKind::Instrument, "Kept").instrument.kind = InstrumentKind::Drums;
    m.tracks[0].clips.push_back(noteClip(m, 0, kPpq * 4, {note(0, 100, 36)}));
    m.addTrack(TrackKind::Instrument, "Silenced").instrument.kind = InstrumentKind::Drums;
    m.tracks[1].clips.push_back(noteClip(m, 0, kPpq * 4, {note(kPpq * 2, 100, 36)}));
    m.tracks[0].solo = true;
    {
        Engine e2(kRate);
        e2.setSong(m);
        e2.play();
        const auto o = run(e2, 2.0);
        check(peakOf(o, 0, 9600) > 0.1f && peakOf(o, frameOf(m, kPpq * 2), frameOf(m, kPpq * 2) + 9600) < 1e-5f,
              "a soloed track sounds, the others do not");
    }
    m.tracks[0].solo = false;
    m.tracks[0].mute = true;
    {
        Engine e2(kRate);
        e2.setSong(m);
        e2.play();
        const auto o = run(e2, 2.0);
        check(peakOf(o, 0, 9600) < 1e-5f && peakOf(o, frameOf(m, kPpq * 2), frameOf(m, kPpq * 2) + 9600) > 0.1f,
              "a muted track does not");
    }
    m.tracks[0].mute = false;
    Track& part = m.addTrack(TrackKind::Midi, "Part");
    part.target = m.tracks[1].uid;
    part.clips.push_back(noteClip(m, 0, kPpq * 4, {note(kPpq, 100, 38)}));
    part.solo = true;
    {
        Engine e2(kRate);
        e2.setSong(m);
        e2.play();
        const auto o = run(e2, 2.0);
        const long b1 = frameOf(m, kPpq);
        check(peakOf(o, b1, b1 + 4800) > 0.05f && peakOf(o, 0, 9600) < 1e-5f,
              "a soloed MIDI track brings the instrument it plays (and only it)");
    }

    // A new tempo while playing: the playhead stays on its beat.
    Song t;
    Track& k = t.addTrack(TrackKind::Instrument);
    k.instrument.kind = InstrumentKind::Drums;
    t.bpm = 120.0;
    Engine e3(kRate);
    e3.setSong(t);
    e3.play();
    run(e3, 1.0);   // two beats at 120
    const double before2 = e3.position();
    t.bpm = 60.0;
    e3.setSong(t);
    run(e3, 1.0, 256);   // one beat at 60
    check(std::fabs(before2 - kPpq * 2) < 300.0 && std::fabs(e3.position() - kPpq * 3) < 300.0,
          "a tempo change keeps the playhead on its beat (" + std::to_string(before2 / kPpq) + " -> " +
              std::to_string(e3.position() / kPpq) + " beats)");

    // An edit while a chord rings: the new plan arrives, the chord rings on.
    Song c;
    Track& ch = c.addTrack(TrackKind::Instrument);
    ch.instrument.patch = "builtin:Organ";
    ch.volume = 1.0f;
    ch.clips.push_back(noteClip(c, 0, kPpq * 8, {note(0, kPpq * 4, 60), note(0, kPpq * 4, 64)}));
    Engine e4(kRate);
    e4.setSong(c);
    e4.play();
    run(e4, 0.5);
    c.tracks[0].clips[0].notes.push_back(note(kPpq * 6, kPpq, 72));   // somebody draws a note later on
    c.tracks[0].clips[0].sortNotes();
    e4.setSong(c);
    const auto o4 = run(e4, 1.0);
    check(peakOf(o4, 2400, 24000) > 0.05f, "an edit while a chord rings does not cut it");
    const auto o5 = run(e4, 1.2);
    // It ends at 2.0 s (70 ms of release); this block runs from 1.5 s.
    check(peakOf(o5, 28800, 57600) < 1e-3f, "...and it still ends where it was going to");

    // Fast playing on a synth patch: every note let go, the patch falls silent.
    {
        Song k;
        Track& kt = k.addTrack(TrackKind::Instrument);
        kt.instrument.patch = "builtin:Organ";
        kt.volume = 1.0f;
        Engine e6(kRate);
        e6.setSong(k);
        run(e6, 0.02);
        const int uid = k.tracks[0].uid;
        for (int i = 0; i < 60; ++i) {
            e6.liveNote(uid, 48 + (i * 7) % 24, 0.8f, true);
            if (i % 4 == 0) run(e6, 0.003, 128);
            e6.liveNote(uid, 48 + (i * 7) % 24, 0.0f, false);
        }
        const auto o6 = run(e6, 0.6);
        check(peakOf(o6, 0, 2400) > 0.01f && peakOf(o6, 14400) < 1e-4f,
              "sixty notes played fast on a patch: heard, then silence -- no hanging note");
    }

    // Stop at the end.
    Engine e5(kRate);
    Song shortSong;
    Track& st = shortSong.addTrack(TrackKind::Instrument);
    st.instrument.kind = InstrumentKind::Drums;
    st.clips.push_back(noteClip(shortSong, 0, kPpq * 2, {note(0, 100, 36)}));
    e5.setSong(shortSong);
    e5.setStopAtEnd(true);
    e5.play();
    run(e5, 2.0);
    check(!e5.playing() && e5.ended() && std::fabs(e5.position() - kPpq * 2) < 1.0,
          "a Song Player stops at the song's end (" + std::to_string(e5.position() / kPpq) + " beats)");
}

// --- Outputs: a track in a mixer channel of its own ---------------------------------------
void outputChecks() {
    std::printf("outputs\n");
    Song s;
    s.addTrack(TrackKind::Instrument, "Kick").instrument.kind = InstrumentKind::Drums;
    s.tracks[0].volume = 1.0f;
    s.tracks[0].clips.push_back(noteClip(s, 0, kPpq * 4, {note(0, 100, 36)}));
    s.addTrack(TrackKind::Instrument, "Snare").instrument.kind = InstrumentKind::Drums;
    s.tracks[1].volume = 1.0f;
    s.tracks[1].output = "Song: Snare";
    // On the fourth beat: the kick has rung out by then.
    s.tracks[1].clips.push_back(noteClip(s, 0, kPpq * 4, {note(kPpq * 3, 100, 38)}));
    Song back;
    check(Song::fromJson(s.toJson(), back) && back.tracks[1].output == "Song: Snare" && back.tracks[0].output.empty(),
          "a track's output survives the JSON");

    Engine e(kRate);
    e.setSong(s);
    check(e.outputs().size() == static_cast<std::size_t>(Engine::kMaxOutputs) && e.outputs()[0].empty() &&
              e.outputs()[1] == "Song: Snare",
          "the channel a track names gets an output of its own (slot 1)");
    e.play();
    const int frames = static_cast<int>(kRate * 2.0);
    std::vector<std::vector<float>> bufs(Engine::kMaxOutputs, std::vector<float>(static_cast<std::size_t>(frames) * 2));
    float* outs[Engine::kMaxOutputs];
    for (int k = 0; k < Engine::kMaxOutputs; ++k) outs[k] = bufs[static_cast<std::size_t>(k)].data();
    for (int at = 0; at < frames; at += 256) {
        float* o[Engine::kMaxOutputs];
        const int n = std::min(256, frames - at);
        for (int k = 0; k < Engine::kMaxOutputs; ++k) o[k] = outs[k] + static_cast<std::size_t>(at) * 2;
        e.processOutputs(o, n);
    }
    const long beat = frameOf(s, kPpq * 3);
    check(peakOf(bufs[0], 0, 9600) > 0.05f && peakOf(bufs[0], beat, beat + 9600) < 1e-4f,
          "the song's mix has the kick and not the snare");
    check(peakOf(bufs[1], beat, beat + 9600) > 0.05f && peakOf(bufs[1], 0, 9600) < 1e-4f,
          "...and the snare's channel has the snare and not the kick");
    const long snareOn = onset(bufs[1], 0, 1e-5f);
    check(snareOn >= beat && snareOn <= beat + 2,
          "the snare on its sample in its own output (" + std::to_string(snareOn) + ", want " + std::to_string(beat) + ")");
    bool quiet = true;
    for (int k = 2; k < Engine::kMaxOutputs; ++k) quiet = quiet && peakOf(bufs[static_cast<std::size_t>(k)]) == 0.0f;
    check(quiet, "the unused outputs are silent");

    // Another channel named: the snare keeps its slot, the new one takes the next.
    s.addTrack(TrackKind::Audio, "Vox").output = "Song: Vox";
    s.tracks[0].output = "Song: Vox";
    e.setSong(s);
    check(e.outputs()[1] == "Song: Snare" && e.outputs()[2] == "Song: Vox", "an output keeps its slot as the song is edited");
    s.tracks[1].output.clear();
    e.setSong(s);
    check(e.outputs()[1].empty() && e.outputs()[2] == "Song: Vox", "...and one no track names any more is freed");

    // One buffer only (a bounce): every output summed into it.
    Engine e2(kRate);
    Song both = s;
    both.tracks[0].output.clear();
    both.tracks[1].output = "Song: Snare";
    e2.setSong(both);
    e2.play();
    const auto mixed = run(e2, 2.0);
    check(peakOf(mixed, 0, 9600) > 0.05f && peakOf(mixed, beat, beat + 9600) > 0.05f,
          "a bounce has every track, whatever channel it goes to");
}

// --- Audio clips ---------------------------------------------------------------------------
void audioChecks(const fs::path& dir) {
    std::printf("audio clips\n");
    const fs::path wav = dir / "level.wav";
    writeWav(wav, 44100, 3.0f, 0.5f);   // a flat 0.5 for three seconds, at 44.1 kHz
    Song s;
    Track& t = s.addTrack(TrackKind::Audio, "Level");
    t.volume = 1.0f;
    Clip c;
    c.uid = s.newUid();
    c.file = "level.wav";
    c.start = kPpq * 4;           // bar 2: 2.0 s
    c.length = kPpq * 2;          // 1.0 s
    c.sourceOffset = 2.5;         // 0.5 s of file left: the clip runs out at 2.5 s
    c.gain = 0.5f;
    t.clips.push_back(c);
    Files files;
    files.sound = [dir](const std::string& n) { return (dir / n).generic_string(); };
    Engine e(kRate);
    e.setFiles(files);
    e.setSong(s);
    waitSamples(e);
    check(e.sampleErrors().empty(), "the file decodes");
    e.play();
    const auto out = run(e, 3.5);
    const long on = onset(out, 0, 1e-5f);
    check(on >= 96000 && on <= 96002, "an audio clip starts on its bar (" + std::to_string(on) + ", want 96000)");
    const float mid = out[static_cast<std::size_t>(96000 + 12000) * 2];
    check(std::fabs(mid - 0.25f) < 0.005f, "...at its gain, resampled from 44.1 kHz (" + std::to_string(mid) + ", want 0.25)");
    const long end = offset(out, on + 100, 1e-3f);
    check(end >= 119900 && end <= 120100, "...and runs out where the file does, not at the clip's end (" + std::to_string(end) + ")");
}

} // namespace

// --bench: what a song costs. A full arrangement -- drums, bass, keys, pad,
// strings, a pluck in sixteenths, organ, a lead, 32 bars -- rendered in 512-frame
// blocks as the device asks for them; the time per block against the 10.7 ms a
// block lasts.
Song benchSong() {
    Song s;
    s.bpm = 120.0;
    const std::int64_t bar = s.ticksPerBar();
    const int bars = 32;
    auto clipOf = [&](std::vector<Note> notes, std::int64_t loop) {
        Clip c;
        c.uid = s.newUid();
        c.length = bar * bars;
        c.loop = loop;
        c.notes = std::move(notes);
        c.sortNotes();
        return c;
    };
    auto add = [&](const char* name, InstrumentKind k, const char* patch, std::vector<Note> notes, std::int64_t loop) {
        Track& t = s.addTrack(TrackKind::Instrument, name);
        t.instrument.kind = k;
        if (patch) t.instrument.patch = patch;
        t.clips.push_back(clipOf(std::move(notes), loop));
    };
    std::vector<Note> drums, bass, keys, pad, strings, pluck, organ, lead;
    for (int i = 0; i < 16; ++i) drums.push_back(note(i * kPpq / 4, 60, 42, 80));
    for (int i = 0; i < 4; ++i) drums.push_back(note(i * kPpq, 100, i % 2 ? 38 : 36, 110));
    for (int i = 0; i < 8; ++i) bass.push_back(note(i * kPpq / 2, kPpq / 2 - 30, 33 + (i % 4 == 3 ? 12 : 0), 100));
    for (int p : {57, 60, 64, 67}) keys.push_back(note(0, kPpq * 2, p, 90)), keys.push_back(note(kPpq * 2, kPpq * 2, p + 2, 90));
    for (int p : {45, 52, 57, 60, 64, 69}) pad.push_back(note(0, bar - 10, p, 80));
    for (int p : {57, 64, 69, 72}) strings.push_back(note(0, bar - 10, p, 80));
    for (int i = 0; i < 16; ++i) pluck.push_back(note(i * kPpq / 4, kPpq / 4, 69 + (i * 5) % 12, 90));
    for (int p : {48, 55, 60}) organ.push_back(note(0, bar - 10, p, 70));
    for (int i = 0; i < 8; ++i) lead.push_back(note(i * kPpq / 2, kPpq / 2 - 20, 72 + (i * 3) % 7, 100));
    add("Drums", InstrumentKind::Drums, nullptr, drums, bar);
    add("Bass", InstrumentKind::Synth, "builtin:Bass", bass, bar);
    add("Keys", InstrumentKind::Synth, "builtin:Keys", keys, bar);
    add("Pad", InstrumentKind::Synth, "builtin:Soft Pad", pad, bar);
    add("Strings", InstrumentKind::Synth, "builtin:Strings", strings, bar);
    add("Pluck", InstrumentKind::Synth, "builtin:Pluck", pluck, bar);
    add("Organ", InstrumentKind::Synth, "builtin:Organ", organ, bar);
    add("Lead", InstrumentKind::Synth, "builtin:Saw Lead", lead, bar);
    return s;
}

int bench() {
    Song s = benchSong();
    Engine e(kRate);
    e.setSong(s);
    e.play();
    const int block = 512;
    std::vector<float> buf(block * 2);
    const int blocks = static_cast<int>(s.seconds(s.length()) * kRate / block);
    double worst = 0.0, total = 0.0;
    for (int i = 0; i < blocks; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        e.process(buf.data(), block);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        total += us;
        worst = std::max(worst, us);
    }
    const double budget = block / kRate * 1e6;
    std::printf("engine: %d blocks of %d, mean %.0f us, worst %.0f us, budget %.0f us (%.1f%% of a core)\n", blocks, block,
                total / blocks, worst, budget, 100.0 * total / blocks / budget);
    // ...and recompiling the song, as an edit does every frame of a drag.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) e.setSong(s);
    e.process(buf.data(), block);
    std::printf("setSong: %.0f us\n", std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 100.0);
    return 0;
}

// --plugin: an installed instrument, played.
int playPlugin(const std::string& name, const std::string& wavOut) {
    const fitzel::vst3::PluginInfo* info = nullptr;
    for (const auto& p : fitzel::vst3::plugins())
        if (p.name == name) info = &p;
    if (!info) {
        std::printf("no plugin named %s\n", name.c_str());
        return 1;
    }
    std::printf("%s by %s (%s)%s\n", info->name.c_str(), info->vendor.c_str(), info->category.c_str(),
                info->instrument ? "" : " -- not an instrument");
    Song s;
    s.bpm = 110.0;
    Track& t = s.addTrack(TrackKind::Instrument, info->name);
    t.instrument.kind = InstrumentKind::Vst3;
    t.instrument.pluginUid = info->uid;
    t.instrument.pluginName = info->name;
    Clip c;
    c.uid = s.newUid();
    c.length = kPpq * 16;
    const int arp[8] = {48, 55, 60, 64, 67, 64, 60, 55};
    for (int i = 0; i < 24; ++i) c.notes.push_back(note(i * kPpq / 2, kPpq / 2 - 40, arp[i % 8] + (i >= 16 ? 5 : 0), 100));
    c.notes.push_back(note(kPpq * 12, kPpq * 4, 36, 110));
    t.clips.push_back(c);
    fitzel::SampleData out;
    std::string err;
    const bool ok = Engine::render(s, {}, 48000.0, out, &err, 2.0);
    std::vector<float> v(out.samples.begin(), out.samples.end());
    const float pk = peakOf(v);
    std::printf("%s: %.1f s rendered, peak %.3f%s%s\n", ok ? "played" : "failed", out.seconds(), pk,
                err.empty() ? "" : ", ", err.c_str());
    if (!wavOut.empty() && fitzel::saveWav(wavOut, out, fitzel::WavFormat::Pcm16)) std::printf("wrote %s\n", wavOut.c_str());
    return ok && pk > 1e-3f && finite(v) ? 0 : 1;
}

int main(int argc, char** argv) {
    std::string wavOut, plugin;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--wav") wavOut = argv[i + 1];
        if (std::string(argv[i]) == "--plugin") plugin = argv[i + 1];
    }
    if (!plugin.empty()) return playPlugin(plugin, wavOut);
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--bench") return bench();

    const fs::path dir = fs::temp_directory_path() / "fitzel-songcheck";
    std::error_code ec;
    fs::create_directories(dir, ec);

    modelChecks();
    soundChecks();
    synthTiming();
#if FITZEL_VST3
    pluginChecks();
#else
    std::printf("VST3 instrument: no VST3 host in this build, skipped\n");
#endif
    transportChecks();
    outputChecks();
    audioChecks(dir);

    std::printf("bounce\n");
    Song demo;
    demo.bpm = 124.0;
    Track& drums = demo.addTrack(TrackKind::Instrument, "Drums");
    drums.instrument.kind = InstrumentKind::Drums;
    Clip beat;
    beat.uid = demo.newUid();
    beat.length = kPpq * 16;
    beat.loop = kPpq * 4;
    for (int i = 0; i < 4; ++i) beat.notes.push_back(note(i * kPpq, 100, 36, 120));
    for (int i = 0; i < 2; ++i) beat.notes.push_back(note(kPpq + i * kPpq * 2, 100, 38, 110));
    for (int i = 0; i < 8; ++i) beat.notes.push_back(note(i * kPpq / 2, 60, 42, i % 2 ? 60 : 90));
    beat.sortNotes();
    drums.clips.push_back(beat);
    Track& bass = demo.addTrack(TrackKind::Instrument, "Bass");
    bass.instrument.patch = "builtin:Bass";
    Clip bl;
    bl.uid = demo.newUid();
    bl.length = kPpq * 16;
    const int roots[4] = {33, 33, 36, 31};
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 4; ++i) bl.notes.push_back(note(b * kPpq * 4 + i * kPpq, kPpq / 2, roots[b] + (i == 3 ? 12 : 0), 100));
    bass.clips.push_back(bl);
    Track& pad = demo.addTrack(TrackKind::Instrument, "Pad");
    pad.instrument.patch = "builtin:Soft Pad";
    pad.volume = 0.5f;
    Clip pc;
    pc.uid = demo.newUid();
    pc.length = kPpq * 16;
    const int chords[4][3] = {{57, 60, 64}, {57, 60, 64}, {60, 64, 67}, {55, 59, 62}};
    for (int b = 0; b < 4; ++b)
        for (int n : chords[b]) pc.notes.push_back(note(b * kPpq * 4, kPpq * 4 - 60, n, 90));
    pad.clips.push_back(pc);
    fitzel::SampleData bounce;
    std::string err;
    const bool ok = Engine::render(demo, {}, 44100.0, bounce, &err, 2.0);
    const double want = demo.seconds(demo.length()) + 2.0;
    check(ok && err.empty() && std::fabs(bounce.seconds() - want) < 0.01 && bounce.channels == 2,
          "a bounce is the song plus its tail (" + std::to_string(bounce.seconds()) + " s)");
    std::vector<float> asVec(bounce.samples.begin(), bounce.samples.end());
    check(finite(asVec) && peakOf(asVec) > 0.2f, "...and has the song in it (peak " + std::to_string(peakOf(asVec)) + ")");
    if (!wavOut.empty()) {
        std::string werr;
        if (fitzel::saveWav(wavOut, bounce, fitzel::WavFormat::Pcm16, &werr)) std::printf("wrote %s\n", wavOut.c_str());
        else std::printf("could not write %s: %s\n", wavOut.c_str(), werr.c_str());
    }

    fs::remove_all(dir, ec);
    std::printf(g_fails ? "\n%d check(s) FAILED\n" : "\nall song checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
