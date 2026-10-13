using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using static Fitzel.LuaApi;

namespace Fitzel
{
    /// <summary>A tree of the procedural forest.</summary>
    public struct Tree
    {
        public float X, Z, Size;
    }

    public static unsafe partial class World
    {
        /// <summary>Water at (x, z): its surface and how deep it is there -- or null where it is dry.</summary>
        public static (float surface, float depth)? WaterAt(float x, float z)
        {
            var r = Call("game", "waterAt", x, z);
            return IsNil(r, 0) ? null : (Num(r, 0), Num(r, 1));
        }

        /// <summary>
        /// What something let go of at `from` would land on: the first road, bridge or
        /// floor straight below (within maxDistance), or the terrain as drawn.
        /// </summary>
        public static float? GroundHeight(Vector3 from, float maxDistance = 50f)
        {
            var r = Call("game", "groundHeight", from.X, from.Y, from.Z, maxDistance);
            return IsNil(r, 0) ? null : Num(r, 0);
        }

        /// <summary>Every tree of the forest in the rectangle, made here and now (the same on every machine).</summary>
        public static List<Tree> Trees(float x0, float z0, float x1, float z1)
        {
            var l = new List<Tree>();
            var flat = new List<float>();
            foreach (var v in Items(Call("game", "trees", x0, z0, x1, z1)[0])) flat.Add(Num(v));
            for (int i = 0; i + 2 < flat.Count; i += 3) l.Add(new Tree { X = flat[i], Z = flat[i + 1], Size = flat[i + 2] });
            return l;
        }

        /// <summary>The forest grows no more in this disc (something was built there).</summary>
        public static void ClearTrees(float x, float z, float radius) => Call("game", "clearTrees", x, z, radius);

        /// <summary>The tree at (x, z) tips over toward (dx, dz) (null: the way it faces). False: no tree there.</summary>
        public static bool FellTree(float x, float z, Vector2? toward = null) => toward.HasValue
            ? Bool(Call("game", "fellTree", x, z, toward.Value.X, toward.Value.Y), 0)
            : Bool(Call("game", "fellTree", x, z), 0);

        /// <summary>The tree at (x, z) burns for `seconds`. False: no tree there.</summary>
        public static bool BurnTree(float x, float z, float seconds = 20f) => Bool(Call("game", "burnTree", x, z, seconds), 0);

        /// <summary>The clock runs at the scene's own pace again (after setting DayLength).</summary>
        public static void ReleaseDayLength() => Call("game", "setDayLength");

        /// <summary>The towns' street lamps: "auto" (by the dusk), "on" or "off"; and whether they burn now.</summary>
        public static string StreetLampMode
        {
            get => Str(Call("game", "streetLamps"), 1, "auto");
            set => Call("game", "setStreetLamps", string.IsNullOrEmpty(value) ? "auto" : value);
        }

        public static bool StreetLampsLit => Bool(Call("game", "streetLamps"), 0);

        /// <summary>
        /// A decal where something hit: on the surface at `point` with `normal`, `size`
        /// metres wide, in a library material (null = the engine's bullet hole), turned
        /// by `spin` degrees. False when there is nothing there.
        /// </summary>
        public static bool Decal(Vector3 point, Vector3 normal, float size = 0.12f, string material = null, float spin = 0f) =>
            Bool(Call("game", "decal", point.X, point.Y, point.Z, normal.X, normal.Y, normal.Z, size, material, spin), 0);

        /// <summary>
        /// Glass breaks where a shot hit it: object `target` (null: find the glass at the
        /// point), the shot travelling along `direction`. False when there is no glass.
        /// </summary>
        public static bool Shatter(Entity target, Vector3 point, Vector3 direction, float strength = 1f) =>
            Bool(Call("game", "shatter", target?.Id ?? -1, point.X, point.Y, point.Z, direction.X, direction.Y, direction.Z, strength), 0);
    }

    public static unsafe partial class Physics
    {
        /// <summary>
        /// A ray against the objects' pick boxes only (game.raycast: axis-aligned, rotation
        /// ignored) -- the editor's notion of "what is there". Raycast is the real one.
        /// </summary>
        public static bool PickBoxes(Vector3 origin, Vector3 direction, out RaycastHit hit, float maxDistance = 1000f)
        {
            hit = default;
            var r = Call("game", "raycast", origin.X, origin.Y, origin.Z, direction.X, direction.Y, direction.Z, maxDistance);
            if (IsNil(r, 0)) return false;
            hit.Entity = Entity.Of(Int(r, 0));
            hit.Point = new Vector3(Num(r, 1), Num(r, 2), Num(r, 3));
            hit.Distance = Num(r, 4);
            return true;
        }
    }

    /// <summary>A named place of a town (game.townPlaces).</summary>
    public class TownPlace
    {
        public string Name, Kind, Street;
        public int Number, Town;
        /// <summary>On the pavement in front of it: where a figure stops.</summary>
        public Vector3 Position;
        /// <summary>The place itself, to look at.</summary>
        public Vector2 At;
    }

    /// <summary>The towns of the town generator as goals for figures on foot.</summary>
    public static class Town
    {
        /// <summary>All places of all towns, or of one kind ("school", "stop", "home" ...).</summary>
        public static List<TownPlace> Places(string kind = null)
        {
            var l = new List<TownPlace>();
            foreach (var t in Items(Call("game", "townPlaces", kind)[0]))
                l.Add(new TownPlace
                {
                    Name = StrField(t, "name"), Kind = StrField(t, "kind"), Street = StrField(t, "street"),
                    Number = IntField(t, "number"), Town = IntField(t, "town"),
                    Position = new Vector3(NumField(t, "x"), NumField(t, "y"), NumField(t, "z")),
                    At = new Vector2(NumField(t, "ax"), NumField(t, "az")),
                });
            return l;
        }

        /// <summary>A footpath from the pavement nearest `from` to the one nearest `to`, or null.</summary>
        public static List<Vector3> Path(Vector2 from, Vector2 to)
        {
            var r = Call("game", "townPath", from.X, from.Y, to.X, to.Y);
            if (IsNil(r, 0)) return null;
            var l = new List<Vector3>();
            foreach (var p in Items(r[0])) l.Add(Vec(p));
            return l;
        }

        /// <summary>The nearest named street within maxDistance metres, and how far it is -- or null.</summary>
        public static (string name, float distance)? StreetAt(Vector2 at, float maxDistance = 40f)
        {
            var r = Call("game", "streetAt", at.X, at.Y, maxDistance);
            return IsNil(r, 0) ? null : (Str(r, 0), Num(r, 1));
        }
    }

    public static unsafe partial class Game
    {
        /// <summary>Raise the score by n.</summary>
        public static void AddScore(int n = 1) => Call("game", "addScore", n);

        /// <summary>
        /// Save game data (numbers, strings, bools, lists, dictionaries, plain objects) as
        /// JSON in the user's folder, per game, written atomically (game.saveData).
        /// </summary>
        public static bool SaveData(string slot, object value) => Bool(Call("game", "saveData", slot, value), 0);

        /// <summary>The value saved in a slot, or default when there is none.</summary>
        public static T LoadData<T>(string slot)
        {
            var r = Call("game", "loadData", slot);
            return IsNil(r, 0) ? default : As<T>(r[0]);
        }

        /// <summary>The raw saved value (null kind when there is none).</summary>
        public static JsonElement LoadData(string slot)
        {
            var r = Call("game", "loadData", slot);
            return IsNil(r, 0) ? default : r[0];
        }

        /// <summary>
        /// "Nothing moves by itself this frame": Play draws at most `fps` frames a second
        /// and otherwise waits for input. Call every frame while it holds.
        /// </summary>
        public static void Rest(float fps = 10f) => Call("game", "rest", fps);
    }

    /// <summary>The scene's sequences (View &gt; Presentation &gt; Sequencer): cutscenes of shots, animations and sound.</summary>
    public static class Sequence
    {
        /// <summary>Run the sequence called `name` from the top. False: the scene has none of that name.</summary>
        public static bool Play(string name) => Bool(Call("game", "playSequence", name), 0);

        /// <summary>Stop the running sequence.</summary>
        public static void Stop() => Call("game", "stopSequence");

        /// <summary>Seconds into the running sequence, or null when none runs.</summary>
        public static float? Time
        {
            get { var r = Call("game", "sequenceTime"); return IsNil(r, 0) ? null : Num(r, 0); }
        }
    }

    public static unsafe partial class Camera
    {
        /// <summary>Look through an object's Camera component (null: back to the script/player camera).</summary>
        public static void Use(Entity camera) => Call("game", "setCamera", camera?.Id ?? -1);

        /// <summary>Depth of field of the script camera: sharp up to `near`, fully blurred from `far` metres.</summary>
        public static void SetFocus(float near, float far) => Call("game", "setFocus", near, far);

        /// <summary>The view's own depth of field again.</summary>
        public static void ResetFocus() => Call("game", "setFocus");

        /// <summary>
        /// Frame the orbit camera differently for this frame (aiming over the shoulder):
        /// weight 0..1, distance from the pivot, pivot side/up offsets, field of view (0 = keep).
        /// </summary>
        public static void OrbitFrame(float weight, float distance, float side, float up, float fov = 0f) =>
            Call("game", "orbitFrame", weight, distance, side, up, fov);
    }

    public static unsafe partial class Input
    {
        /// <summary>What was typed since the last call (UTF-8 text, no Backspace/Enter -- those are keys).</summary>
        public static string TextInput() => Str(Call("game", "textInput"), 0, "");

        /// <summary>
        /// Hold the keys and mouse buttons for this script (a menu): call every frame while
        /// it is open. True = this script holds them; every other script, Lua or C#, sees none.
        /// </summary>
        public static bool Capture() => Bool(Call("game", "captureInput"), 0);
    }

    public static unsafe partial class Audio
    {
        /// <summary>A sound on a mixer channel ("UI", "SFX" ...), heard everywhere.</summary>
        public static void Play(string sound, float volume, float pitch, string channel) =>
            Call("game", "sound", sound, volume, pitch, channel);

        /// <summary>A sound at a point on a mixer channel.</summary>
        public static void PlayAt(string sound, Vector3 at, float volume, float pitch, float near, float far, string channel) =>
            Call("game", "sound", sound, volume, pitch, at.X, at.Y, at.Z, near, far, channel);
    }

    public static unsafe partial class Hud
    {
        /// <summary>The size of a text on the canvas.</summary>
        public static Vector2 TextSize(string text, float size = 32f, bool bold = false)
        {
            var r = Call("game", "hudTextSize", text, size, bold);
            return new Vector2(Num(r, 0), Num(r, 1));
        }

        /// <summary>
        /// A picture (Texture asset: file name or GUID), tinted, cropped to the uv box
        /// (u0, v0, u1, v1; v runs down) -- the whole picture by default.
        /// </summary>
        public static void DrawImage(string image, float x, float y, float w, float h, Color? tint = null, Vector4? uv = null)
        {
            var c = tint ?? Color.White;
            var u = uv ?? new Vector4(0, 0, 1, 1);
            Call("game", "hudImage", image, x, y, w, h, c.R, c.G, c.B, c.A, u.X, u.Y, u.Z, u.W);
        }

        /// <summary>
        /// A picture's size in pixels (as the HUD holds it) and the box its visible part
        /// fills (u0, v0, u1, v1) -- or null for an unknown picture.
        /// </summary>
        public static (Vector2 size, Vector4 visible)? ImageSize(string image)
        {
            var r = Call("game", "imageSize", image);
            if (IsNil(r, 0)) return null;
            return (new Vector2(Num(r, 0), Num(r, 1)), new Vector4(Num(r, 2), Num(r, 3), Num(r, 4), Num(r, 5)));
        }

        /// <summary>Where a world point lies on the canvas -- null behind the camera.</summary>
        public static Vector2? WorldToHud(Vector3 p)
        {
            var r = Call("game", "worldToHud", p.X, p.Y, p.Z);
            return IsNil(r, 0) ? null : new Vector2(Num(r, 0), Num(r, 1));
        }
    }
}
