using static Fitzel.LuaApi;

namespace Fitzel
{
    /// <summary>
    /// An object's Song Player component (Lua's <c>song.*</c>): a song from the
    /// Song Sequencer, played in the game. Its tracks are addressed by name -- mute a
    /// layer when the fight starts, change the tempo, cut on the next downbeat.
    /// <code>
    /// Song.Play(jukebox);
    /// Song.Mute(jukebox, "Drums", !inBattle);
    /// var (bar, beat) = Song.Bar(jukebox) ?? (0, 0);
    /// </code>
    /// </summary>
    public static class Song
    {
        /// <summary>Start the component's song from the top.</summary>
        public static bool Play(Entity e) => Bool(Call("song", "play", e.Id), 0);
        public static void Stop(Entity e) => Call("song", "stop", e.Id);
        public static bool IsPlaying(Entity e) => Bool(Call("song", "isPlaying", e.Id), 0);

        /// <summary>Seconds into the song, or null before it is started.</summary>
        public static float? Time(Entity e)
        {
            var r = Call("song", "time", e.Id);
            return IsNil(r, 0) ? null : Num(r, 0);
        }
        /// <summary>Beats from the start (fractional), or null before it is started.</summary>
        public static float? Beat(Entity e)
        {
            var r = Call("song", "beat", e.Id);
            return IsNil(r, 0) ? null : Num(r, 0);
        }
        /// <summary>The bar and the beat in it, both from 1; null before it is started.</summary>
        public static (int bar, int beat)? Bar(Entity e)
        {
            var r = Call("song", "bar", e.Id);
            return IsNil(r, 0) ? null : (Int(r, 0), Int(r, 1));
        }
        /// <summary>Move to a beat (from 0).</summary>
        public static bool Seek(Entity e, float beat) => Bool(Call("song", "seek", e.Id, beat), 0);

        /// <summary>A track, by its name in the sequencer, switched off (or on again).</summary>
        public static bool Mute(Entity e, string track, bool mute = true) => Bool(Call("song", "mute", e.Id, track, mute), 0);
        /// <summary>A track's fader, 0..2 (1 = as it was mixed).</summary>
        public static bool Volume(Entity e, string track, float volume) => Bool(Call("song", "volume", e.Id, track, volume), 0);
        public static bool SetTempo(Entity e, float bpm) => Bool(Call("song", "setTempo", e.Id, bpm), 0);

        /// <summary>Why the last call returned false.</summary>
        public static string LastError => Str(Call("song", "lastError"), 0, "");
    }
}
