using System.Numerics;

namespace Fitzel
{
    /// <summary>
    /// The base of every script. Put a class deriving from it in a file of the
    /// same name under scripts/ (Rotator.cs holds class Rotator), add a Script
    /// component to an object and pick the file: while the game plays, the
    /// engine calls the methods it finds, as Unity does --
    /// <code>
    /// void Start()     // once, before the first Update
    /// void Update()    // every frame (Time.DeltaTime is the frame's length)
    /// void OnDestroy() // the object is removed, or Play starts again
    /// </code>
    /// Public fields (and private ones marked [SerializeField]) of type float,
    /// int, bool, string, Vector3 or Color appear on the Script card, where each
    /// object can set its own value.
    /// </summary>
    public abstract class Behaviour
    {
        /// <summary>The object this script runs on.</summary>
        public Entity Entity { get; internal set; }

        /// <summary>The object's name.</summary>
        public string Name { get => Entity.Name; set => Entity.Name = value; }

        /// <summary>Position relative to the parent (the world, for an object without one).</summary>
        public Vector3 Position { get => Entity.Position; set => Entity.Position = value; }

        /// <summary>Rotation relative to the parent, Euler degrees (applied X, then Y, then Z).</summary>
        public Vector3 Rotation { get => Entity.Rotation; set => Entity.Rotation = value; }

        /// <summary>Half the object's size along its own axes.</summary>
        public Vector3 HalfExtents { get => Entity.HalfExtents; set => Entity.HalfExtents = value; }

        /// <summary>Position in the world, whatever the parent.</summary>
        public Vector3 WorldPosition { get => Entity.WorldPosition; set => Entity.WorldPosition = value; }

        /// <summary>The object's forward direction in the world (its local +Z).</summary>
        public Vector3 Forward => Entity.Forward;
    }

    /// <summary>Shows a private field on the Script card.</summary>
    [System.AttributeUsage(System.AttributeTargets.Field)]
    public sealed class SerializeFieldAttribute : System.Attribute { }

    /// <summary>Keeps a public field off the Script card.</summary>
    [System.AttributeUsage(System.AttributeTargets.Field)]
    public sealed class HideInInspectorAttribute : System.Attribute { }

    /// <summary>A colour, 0..1 per channel. A field of this type gets a colour picker.</summary>
    public struct Color
    {
        public float R, G, B, A;
        public Color(float r, float g, float b, float a = 1f) { R = r; G = g; B = b; A = a; }
        public static readonly Color White = new Color(1, 1, 1);
        public static readonly Color Black = new Color(0, 0, 0);
        public static readonly Color Red = new Color(1, 0, 0);
        public static readonly Color Green = new Color(0, 1, 0);
        public static readonly Color Blue = new Color(0, 0, 1);
        public static readonly Color Yellow = new Color(1, 0.92f, 0.016f);
        public Color WithAlpha(float a) => new Color(R, G, B, a);
        public override string ToString() => $"({R:0.###}, {G:0.###}, {B:0.###}, {A:0.###})";

        // IM_COL32's layout, which the HUD hands to ImGui as it is.
        internal uint Packed()
        {
            static uint ch(float v) => (uint)(System.Math.Clamp(v, 0f, 1f) * 255f + 0.5f);
            return ch(R) | (ch(G) << 8) | (ch(B) << 16) | (ch(A) << 24);
        }
    }

    /// <summary>The frame's clock.</summary>
    public static class Time
    {
        /// <summary>Seconds since the last frame.</summary>
        public static float DeltaTime { get; internal set; }
        /// <summary>Seconds on the engine's clock (the time Lua's update gets).</summary>
        public static float Elapsed { get; internal set; }
    }

    /// <summary>Messages for the log (the console the editor was started from).</summary>
    public static class Debug
    {
        public static void Log(object message) => Internal.Bridge.Log(message?.ToString() ?? "null");
        public static void LogError(object message) => Internal.Bridge.Log("ERROR: " + (message?.ToString() ?? "null"));
    }
}
