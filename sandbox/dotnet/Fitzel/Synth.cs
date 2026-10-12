using System.Collections.Generic;
using System.Text.Json;
using static Fitzel.LuaApi;

namespace Fitzel
{
    /// <summary>
    /// An object's Synth component (Lua's <c>synth.*</c>): notes, dials and MIDI
    /// songs. A note is a MIDI number (60 = middle C) or a name ("C4", "F#3", "Bb2");
    /// velocity 0..1. The synth is built on the first call.
    /// </summary>
    public static class Synth
    {
        /// <summary>Start the synth, and the component's song if it has one.</summary>
        public static bool Play(Entity e) => Bool(Call("synth", "play", e.Id), 0);
        /// <summary>Silence: the song stops, notes are released.</summary>
        public static void Stop(Entity e) => Call("synth", "stop", e.Id);
        public static bool NoteOn(Entity e, int note, float velocity = 0.8f) => Bool(Call("synth", "noteOn", e.Id, note, velocity), 0);
        public static bool NoteOn(Entity e, string note, float velocity = 0.8f) => Bool(Call("synth", "noteOn", e.Id, note, velocity), 0);
        /// <summary>Release a note, or all of them.</summary>
        public static void NoteOff(Entity e) => Call("synth", "noteOff", e.Id);
        public static void NoteOff(Entity e, int note) => Call("synth", "noteOff", e.Id, note);
        public static void NoteOff(Entity e, string note) => Call("synth", "noteOff", e.Id, note);
        /// <summary>Turn a dial of the patch ("rpm" ...).</summary>
        public static bool Set(Entity e, string dial, float value) => Bool(Call("synth", "set", e.Id, dial, value), 0);
        /// <summary>Play a song from the start: the component's (file null) or another; loop overrides "Loop song".</summary>
        public static bool PlayMidi(Entity e, string file = null, bool? loop = null) => loop.HasValue
            ? Bool(Call("synth", "playMidi", e.Id, file, loop.Value), 0)
            : Bool(Call("synth", "playMidi", e.Id, file), 0);
        public static void StopMidi(Entity e) => Call("synth", "stopMidi", e.Id);
        public static bool IsPlaying(Entity e) => Bool(Call("synth", "isPlaying", e.Id), 0);
        /// <summary>1 = as written, 2 = twice as fast.</summary>
        public static bool SetTempo(Entity e, float factor) => Bool(Call("synth", "setTempo", e.Id, factor), 0);
        /// <summary>A note name as a MIDI number ("A4" -> 69), or null.</summary>
        public static int? Note(string name)
        {
            var r = Call("synth", "note", name);
            return IsNil(r, 0) ? null : Int(r, 0);
        }
        /// <summary>Why the last call returned false.</summary>
        public static string LastError => Str(Call("synth", "lastError"), 0, "");
    }

    /// <summary>The band analysis of a song (music.analyze), for beat detection.</summary>
    public class MusicAnalysis
    {
        public float Duration;
        public int Rate, Hop, Frames;
        public float[] LowFlux, MidFlux, HighFlux, LowRms, MidRms, HighRms;
        /// <summary>When frame i (0-based) is, in seconds.</summary>
        public float TimeOf(int frame) => frame * (float)Hop / Rate;
    }

    /// <summary>
    /// The game's one song with a clock, a filter and analysis (Lua's <c>music.*</c>)
    /// -- for rhythm games and anything that must know where the music is.
    /// </summary>
    public static class Music
    {
        /// <summary>Load a song (replaces and stops the old one). Its length, or null + why.</summary>
        public static float? Load(string name, out string error)
        {
            var r = Call("music", "load", name);
            error = Bool(r, 0) ? null : Str(r, 1, "");
            return error == null ? Num(r, 1) : null;
        }

        /// <summary>Play from `from` seconds; negative = that much silence first.</summary>
        public static void Play(float from = 0f) => Call("music", "play", from);
        public static void Stop() => Call("music", "stop");
        public static void Pause() => Call("music", "pause");
        public static void Resume() => Call("music", "resume");
        public static void Fade(float seconds = 1f) => Call("music", "fade", seconds);
        /// <summary>The position in seconds, as heard.</summary>
        public static float Time => Num(Call("music", "time"), 0);
        public static float Duration => Num(Call("music", "duration"), 0);
        public static bool IsPlaying => Bool(Call("music", "isPlaying"), 0);
        public static bool IsPaused => Bool(Call("music", "isPaused"), 0);
        /// <summary>Low-pass at hz, linear gain, bass shelf (dB at 120 Hz), gliding with time constant `smooth` seconds.</summary>
        public static void SetFilter(float hz, float gain = 1f, float shelfDb = 0f, float smooth = 0.12f) =>
            Call("music", "setFilter", hz, gain, shelfDb, smooth);
        public static void SetVolume(float v) => Call("music", "setVolume", v);
        public static int SampleRate => Int(Call("music", "sampleRate"), 0);

        /// <summary>The spectrum 0..1 (512 bins). Call once a frame: its smoothing steps per call.</summary>
        public static float[] Spectrum() => Floats(Call("music", "spectrum")[0]);
        /// <summary>The last n samples, -1..1.</summary>
        public static float[] Waveform(int n = 128) => Floats(Call("music", "waveform", n)[0]);

        /// <summary>Decode a song and measure its bands (blocks while decoding). Null + why on failure.</summary>
        public static MusicAnalysis Analyze(string name, out string error)
        {
            var r = Call("music", "analyze", name);
            if (IsNil(r, 0)) { error = Str(r, 1, "could not analyze"); return null; }
            error = null;
            var t = r[0];
            return new MusicAnalysis
            {
                Duration = NumField(t, "duration"), Rate = IntField(t, "rate"), Hop = IntField(t, "hop"),
                Frames = IntField(t, "frames"),
                LowFlux = Floats(Field(t, "lowFlux")), MidFlux = Floats(Field(t, "midFlux")), HighFlux = Floats(Field(t, "highFlux")),
                LowRms = Floats(Field(t, "lowRms")), MidRms = Floats(Field(t, "midRms")), HighRms = Floats(Field(t, "highRms")),
            };
        }

        static float[] Floats(JsonElement e)
        {
            var l = new List<float>();
            foreach (var x in Items(e)) l.Add(Num(x));
            return l.ToArray();
        }
    }
}
