using System.Numerics;
using Fitzel.Internal;

namespace Fitzel
{
    /// <summary>Keyboard keys (GLFW's codes, the same numbers as Lua's game.KEY_*).</summary>
    public enum Key
    {
        Space = 32,
        D0 = 48, D1, D2, D3, D4, D5, D6, D7, D8, D9,
        A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        Escape = 256, Enter = 257, Tab = 258, Backspace = 259, Insert = 260, Delete = 261,
        Right = 262, Left = 263, Down = 264, Up = 265,
        PageUp = 266, PageDown = 267, Home = 268, End = 269,
        F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        LeftShift = 340, LeftControl = 341, LeftAlt = 342,
        RightShift = 344, RightControl = 345, RightAlt = 346,
    }

    public enum MouseButton { Left = 0, Right = 1, Middle = 2 }

    /// <summary>Keyboard and mouse, as the game sees them this frame.</summary>
    public static unsafe partial class Input
    {
        /// <summary>Is the key held?</summary>
        public static bool GetKey(Key k) => Bridge.N->Input(0, (int)k) != 0;
        /// <summary>Did the key go down this frame?</summary>
        public static bool GetKeyDown(Key k) => Bridge.N->Input(1, (int)k) != 0;
        /// <summary>Is the mouse button held?</summary>
        public static bool GetMouseButton(MouseButton b) => Bridge.N->Input(2, (int)b) != 0;
        /// <summary>Did the mouse button go down this frame?</summary>
        public static bool GetMouseButtonDown(MouseButton b) => Bridge.N->Input(3, (int)b) != 0;

        /// <summary>The pointer on the HUD's canvas (1080 units high, see <see cref="Hud"/>).</summary>
        public static Vector2 MousePosition
        {
            get
            {
                float* p = stackalloc float[2];
                p[0] = p[1] = 0;
                Bridge.N->MousePos(p);
                return new Vector2(p[0], p[1]);
            }
        }

        /// <summary>Is the pointer over the game view (and free, not locked for mouse look)?</summary>
        public static bool MouseOverView
        {
            get
            {
                float* p = stackalloc float[2];
                return Bridge.N->MousePos(p) != 0;
            }
        }

        /// <summary>How far the wheel turned this frame (notches; up is positive).</summary>
        public static float MouseWheel => Bridge.N->MouseWheel();

        /// <summary>Show the pointer (true) or hide it for mouse look (false).</summary>
        public static void ShowCursor(bool show) => Bridge.N->ShowCursor(show ? 1 : 0);

        /// <summary>The ray from the camera through the pointer, or null without a view.</summary>
        public static Ray? MouseRay
        {
            get
            {
                float* o = stackalloc float[3];
                float* d = stackalloc float[3];
                if (Bridge.N->MouseRay(o, d) == 0) return null;
                return new Ray(new Vector3(o[0], o[1], o[2]), new Vector3(d[0], d[1], d[2]));
            }
        }
    }

    /// <summary>A half-line: where it starts and which way it goes.</summary>
    public readonly struct Ray
    {
        public readonly Vector3 Origin;
        public readonly Vector3 Direction;
        public Ray(Vector3 origin, Vector3 direction) { Origin = origin; Direction = direction; }
        public Vector3 At(float t) => Origin + Direction * t;
    }
}
