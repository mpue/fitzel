using System;
using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using Fitzel.Internal;

namespace Fitzel
{
    /// <summary>
    /// The engine's whole script API as Lua sees it: <c>game.*</c>, <c>net.*</c>,
    /// <c>llm.*</c>, <c>synth.*</c>, <c>music.*</c>. The typed classes of this
    /// library (Materials, Net, Music ...) call through here, so a C# script does
    /// exactly what the same Lua call does. Use it directly for anything not
    /// wrapped yet -- the arguments go as Lua values (numbers, strings, bools,
    /// arrays, dictionaries / anonymous objects as tables), the results come back
    /// one <see cref="JsonElement"/> per Lua return value.
    /// <code>
    /// var r = LuaApi.Call("game", "waterAt", x, z);   // surface, depth -- or nil
    /// if (!LuaApi.IsNil(r, 0)) depth = r[1].GetSingle();
    /// </code>
    /// A Lua error (a wrong argument, say) is thrown as an <see cref="InvalidOperationException"/>.
    /// </summary>
    public static unsafe class LuaApi
    {
        static readonly JsonSerializerOptions Opts = MakeOptions();

        static JsonSerializerOptions MakeOptions()
        {
            var o = new JsonSerializerOptions
            {
                IncludeFields = true,
                NumberHandling = System.Text.Json.Serialization.JsonNumberHandling.AllowNamedFloatingPointLiterals,
            };
            o.Converters.Add(new Vec3Json());
            o.Converters.Add(new ColorJson());
            o.Converters.Add(new EntityJson());
            return o;
        }

        // Engine types in the shape Lua wants them, wherever they sit in an argument:
        // a vector or a colour as {x, y, z}, an entity as its id.
        sealed class Vec3Json : System.Text.Json.Serialization.JsonConverter<Vector3>
        {
            public override Vector3 Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
            {
                using var d = JsonDocument.ParseValue(ref r);
                return Vec(d.RootElement);
            }
            public override void Write(Utf8JsonWriter w, Vector3 v, JsonSerializerOptions o)
            {
                w.WriteStartArray(); w.WriteNumberValue(v.X); w.WriteNumberValue(v.Y); w.WriteNumberValue(v.Z); w.WriteEndArray();
            }
        }
        sealed class ColorJson : System.Text.Json.Serialization.JsonConverter<Color>
        {
            public override Color Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o)
            {
                using var d = JsonDocument.ParseValue(ref r);
                var v = Vec(d.RootElement);
                return new Color(v.X, v.Y, v.Z);
            }
            public override void Write(Utf8JsonWriter w, Color c, JsonSerializerOptions o)
            {
                w.WriteStartArray(); w.WriteNumberValue(c.R); w.WriteNumberValue(c.G); w.WriteNumberValue(c.B); w.WriteEndArray();
            }
        }
        sealed class EntityJson : System.Text.Json.Serialization.JsonConverter<Entity>
        {
            public override Entity Read(ref Utf8JsonReader r, Type t, JsonSerializerOptions o) =>
                r.TokenType == JsonTokenType.Number ? Entity.Of(r.GetInt32()) : null;
            public override void Write(Utf8JsonWriter w, Entity e, JsonSerializerOptions o) => w.WriteNumberValue(e.Id);
        }

        public static JsonElement[] Call(string table, string fn, params object[] args)
        {
            // A trailing null would reach Lua as an explicit nil, and some functions
            // tell "nothing given" from nil (game.showCursor()).
            int n = args?.Length ?? 0;
            while (n > 0 && args[n - 1] == null) n--;
            var list = new object[n];
            for (int i = 0; i < n; i++) list[i] = args[i] is Enum en ? Convert.ToInt32(en) : args[i];
            string json = JsonSerializer.Serialize(list, Opts);
            int ok = 0;
            byte* res;
            fixed (byte* t = Utf8.Of(table))
            fixed (byte* f = Utf8.Of(fn))
            fixed (byte* a = Utf8.Of(json))
                res = Bridge.N->CallApi(t, f, a, &ok);
            string text = Utf8.From(res);
            if (ok == 0) throw new InvalidOperationException($"{table}.{fn}: {text}");
            using var doc = JsonDocument.Parse(text);
            var outv = new List<JsonElement>();
            foreach (var e in doc.RootElement.EnumerateArray()) outv.Add(e.Clone());
            return outv.ToArray();
        }

        // --- Reading what came back -----------------------------------------------
        /// <summary>Is result i missing or nil?</summary>
        public static bool IsNil(JsonElement[] r, int i) =>
            r == null || i >= r.Length || r[i].ValueKind == JsonValueKind.Null || r[i].ValueKind == JsonValueKind.Undefined;

        public static float Num(JsonElement[] r, int i, float fallback = 0f) =>
            IsNil(r, i) ? fallback : Num(r[i], fallback);

        public static float Num(JsonElement e, float fallback = 0f) =>
            e.ValueKind == JsonValueKind.Number ? (float)e.GetDouble() : fallback;

        public static int Int(JsonElement[] r, int i, int fallback = 0) =>
            IsNil(r, i) ? fallback : Int(r[i], fallback);

        public static int Int(JsonElement e, int fallback = 0) =>
            e.ValueKind == JsonValueKind.Number ? (int)Math.Round(e.GetDouble()) : fallback;

        public static bool Bool(JsonElement[] r, int i, bool fallback = false) =>
            IsNil(r, i) ? fallback : Bool(r[i], fallback);

        public static bool Bool(JsonElement e, bool fallback = false) =>
            e.ValueKind == JsonValueKind.True ? true : e.ValueKind == JsonValueKind.False ? false : fallback;

        public static string Str(JsonElement[] r, int i, string fallback = null) =>
            IsNil(r, i) ? fallback : Str(r[i], fallback);

        public static string Str(JsonElement e, string fallback = null) =>
            e.ValueKind == JsonValueKind.String ? e.GetString() : fallback;

        /// <summary>A field of a Lua table, or default.</summary>
        public static JsonElement Field(JsonElement t, string name) =>
            t.ValueKind == JsonValueKind.Object && t.TryGetProperty(name, out var v) ? v : default;

        public static float NumField(JsonElement t, string name, float fallback = 0f) => Num(Field(t, name), fallback);
        public static int IntField(JsonElement t, string name, int fallback = 0) => Int(Field(t, name), fallback);
        public static bool BoolField(JsonElement t, string name, bool fallback = false) => Bool(Field(t, name), fallback);
        public static string StrField(JsonElement t, string name, string fallback = "") => Str(Field(t, name), fallback);

        /// <summary>A 3-vector as Lua writes them: {x,y,z}, {r,g,b} or {1,2,3}.</summary>
        public static Vector3 Vec(JsonElement e)
        {
            if (e.ValueKind == JsonValueKind.Array)
            {
                var a = new float[3];
                int i = 0;
                foreach (var x in e.EnumerateArray()) { if (i < 3) a[i] = Num(x); i++; }
                return new Vector3(a[0], a[1], a[2]);
            }
            if (e.ValueKind != JsonValueKind.Object) return Vector3.Zero;
            if (e.TryGetProperty("x", out _)) return new Vector3(NumField(e, "x"), NumField(e, "y"), NumField(e, "z"));
            if (e.TryGetProperty("r", out _)) return new Vector3(NumField(e, "r"), NumField(e, "g"), NumField(e, "b"));
            return new Vector3(NumField(e, "1"), NumField(e, "2"), NumField(e, "3"));
        }

        /// <summary>A Lua array (a table 1..n; an empty table comes as {}).</summary>
        public static IEnumerable<JsonElement> Items(JsonElement e)
        {
            if (e.ValueKind == JsonValueKind.Array)
                foreach (var x in e.EnumerateArray()) yield return x;
        }

        public static List<int> Ids(JsonElement e)
        {
            var l = new List<int>();
            foreach (var x in Items(e)) l.Add(Int(x));
            return l;
        }

        /// <summary>Turn a Lua value into a C# one (JSON rules, as game.saveData stores it).</summary>
        public static T As<T>(JsonElement e) => e.ValueKind == JsonValueKind.Undefined || e.ValueKind == JsonValueKind.Null
            ? default
            : JsonSerializer.Deserialize<T>(e.GetRawText(), Opts);
    }

    /// <summary>
    /// The <c>shared</c> table Lua scripts use to talk to each other -- and to C#.
    /// Data only (numbers, strings, bools, tables); a Lua function stored there
    /// reads as null. C# scripts among themselves can simply use static fields.
    /// </summary>
    public static class Shared
    {
        public static JsonElement Get(string key) => LuaApi.Call("@shared", "get", key)[0];
        public static T Get<T>(string key) => LuaApi.As<T>(Get(key));
        public static void Set(string key, object value) => LuaApi.Call("@shared", "set", key, value);
    }
}
