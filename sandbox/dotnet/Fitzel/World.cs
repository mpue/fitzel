using System.Numerics;
using Fitzel.Internal;

namespace Fitzel
{
    /// <summary>What a ray hit.</summary>
    public struct RaycastHit
    {
        public Vector3 Point;
        public Vector3 Normal;
        public float Distance;
        /// <summary>The object hit, or null for the terrain and other things that are not objects.</summary>
        public Entity Entity;
    }

    public static unsafe partial class Physics
    {
        /// <summary>The first thing along the ray (bodies, models, terrain) within maxDistance metres.</summary>
        public static bool Raycast(Vector3 origin, Vector3 direction, out RaycastHit hit, float maxDistance = 200f)
        {
            hit = default;
            float* o = stackalloc float[3];
            float* d = stackalloc float[3];
            o[0] = origin.X; o[1] = origin.Y; o[2] = origin.Z;
            d[0] = direction.X; d[1] = direction.Y; d[2] = direction.Z;
            HitBlock h;
            if (Bridge.N->CastRay(o, d, maxDistance, &h) == 0) return false;
            hit.Point = new Vector3(h.Pos[0], h.Pos[1], h.Pos[2]);
            hit.Normal = new Vector3(h.Normal[0], h.Normal[1], h.Normal[2]);
            hit.Distance = h.Dist;
            hit.Entity = h.Id >= 0 ? new Entity(h.Id) : null;
            return true;
        }

        public static bool Raycast(Ray ray, out RaycastHit hit, float maxDistance = 200f) =>
            Raycast(ray.Origin, ray.Direction, out hit, maxDistance);
    }

    /// <summary>The camera the game is seen through.</summary>
    public static unsafe partial class Camera
    {
        static void Read(out Vector3 pos, out Vector3 dir, out Vector2 screen)
        {
            float* p = stackalloc float[3];
            float* d = stackalloc float[3];
            float* s = stackalloc float[2];
            Bridge.N->Camera(p, d, s);
            pos = new Vector3(p[0], p[1], p[2]);
            dir = new Vector3(d[0], d[1], d[2]);
            screen = new Vector2(s[0], s[1]);
        }

        static void Write(int what, Vector3 v)
        {
            float* p = stackalloc float[3];
            p[0] = v.X; p[1] = v.Y; p[2] = v.Z;
            Bridge.N->SetCamera(what, p);
        }

        /// <summary>Where the eye is. Setting it takes the camera over for this frame.</summary>
        public static Vector3 Position { get { Read(out var p, out _, out _); return p; } set => Write(0, value); }
        /// <summary>Which way it looks.</summary>
        public static Vector3 Direction { get { Read(out _, out var d, out _); return d; } set => Write(1, value); }
        /// <summary>Vertical field of view in degrees (0 gives it back to the game).</summary>
        public static float FieldOfView { set => Write(2, new Vector3(value, 0, 0)); }
        /// <summary>The view's size in pixels.</summary>
        public static Vector2 ScreenSize { get { Read(out _, out _, out var s); return s; } }

        /// <summary>Look at a point from where the camera is.</summary>
        public static void LookAt(Vector3 target) => Direction = target - Position;
    }

    public static unsafe partial class Audio
    {
        /// <summary>A sound asset (file name), heard everywhere.</summary>
        public static void Play(string sound, float volume = 1f, float pitch = 1f)
        {
            fixed (byte* p = Utf8.Of(sound)) Bridge.N->Sound(p, volume, pitch, null, 0, 0);
        }

        /// <summary>A sound at a point: full volume within near metres, gone at far.</summary>
        public static void PlayAt(string sound, Vector3 at, float volume = 1f, float pitch = 1f,
                                  float near = 15f, float far = 400f)
        {
            float* v = stackalloc float[3];
            v[0] = at.X; v[1] = at.Y; v[2] = at.Z;
            fixed (byte* p = Utf8.Of(sound)) Bridge.N->Sound(p, volume, pitch, v, near, far);
        }
    }

    /// <summary>The world around the objects.</summary>
    public static unsafe partial class World
    {
        /// <summary>The terrain's height at (x, z).</summary>
        public static float TerrainHeight(float x, float z) => Bridge.N->TerrainHeight(x, z);

        /// <summary>The hour of the day, 0..24.</summary>
        public static float TimeOfDay { get => Bridge.N->GetFloat(0); set => Bridge.N->SetFloat(0, value); }

        /// <summary>How many real seconds a whole day lasts (0 = the sun stands still).</summary>
        public static float DayLength { get => Bridge.N->GetFloat(1); set => Bridge.N->SetFloat(1, value); }
    }

    /// <summary>The game as a whole: score, scenes, quitting.</summary>
    public static unsafe partial class Game
    {
        /// <summary>The score the HUD shows (reset when Play starts).</summary>
        public static int Score { get => (int)Bridge.N->GetFloat(2); set => Bridge.N->SetFloat(2, value); }

        /// <summary>Load another scene of the project (by name, without .fitzel) at the end of the frame.</summary>
        public static void LoadScene(string name) { fixed (byte* p = Utf8.Of(name)) Bridge.N->Scene(0, p); }
        /// <summary>Start the current scene again.</summary>
        public static void Restart() => Bridge.N->Scene(1, null);
        /// <summary>Close the game (in the editor: leave Play).</summary>
        public static void Quit() => Bridge.N->Scene(2, null);
    }
}
