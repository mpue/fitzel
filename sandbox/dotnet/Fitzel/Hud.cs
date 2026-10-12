using System.Numerics;
using Fitzel.Internal;

namespace Fitzel
{
    /// <summary>
    /// Drawing on the screen, redrawn every frame: call these from Update. The
    /// canvas is 1080 units high and as wide as the view's aspect makes it
    /// (<see cref="Size"/>), so a HUD laid out once fits every window.
    /// </summary>
    public static unsafe partial class Hud
    {
        // ScriptHudCmd::Kind
        const int Rect = 0, Gradient = 1, Frame = 2, Line = 3, Circle = 4, Ring = 5, Tri = 6, Text = 7;

        static void Emit(HudBlock* b, string text = null)
        {
            if (text == null) { Bridge.N->Hud(b, null); return; }
            fixed (byte* p = Utf8.Of(text)) Bridge.N->Hud(b, p);
        }

        /// <summary>The canvas: 1080 high, as wide as the view's aspect.</summary>
        public static Vector2 Size
        {
            get
            {
                var s = Camera.ScreenSize;
                float aspect = s.Y > 0 ? s.X / s.Y : 16f / 9f;
                return new Vector2(1080f * aspect, 1080f);
            }
        }

        public static void DrawRect(float x, float y, float w, float h, Color c, float rounding = 0f)
        {
            var b = new HudBlock { Kind = Rect, Col = c.Packed() };
            b.A[0] = x; b.A[1] = y; b.A[2] = w; b.A[3] = h; b.A[4] = rounding;
            Emit(&b);
        }

        /// <summary>A rectangle fading from top to bottom.</summary>
        public static void DrawGradient(float x, float y, float w, float h, Color top, Color bottom)
        {
            var b = new HudBlock { Kind = Gradient, Col = top.Packed(), Col2 = bottom.Packed() };
            b.A[0] = x; b.A[1] = y; b.A[2] = w; b.A[3] = h;
            Emit(&b);
        }

        /// <summary>A rectangle's outline.</summary>
        public static void DrawFrame(float x, float y, float w, float h, Color c, float thickness = 2f, float rounding = 0f)
        {
            var b = new HudBlock { Kind = Frame, Col = c.Packed(), Size = thickness };
            b.A[0] = x; b.A[1] = y; b.A[2] = w; b.A[3] = h; b.A[4] = rounding;
            Emit(&b);
        }

        public static void DrawLine(float x1, float y1, float x2, float y2, Color c, float thickness = 2f)
        {
            var b = new HudBlock { Kind = Line, Col = c.Packed(), Size = thickness };
            b.A[0] = x1; b.A[1] = y1; b.A[2] = x2; b.A[3] = y2;
            Emit(&b);
        }

        /// <summary>A disc, or a ring when thickness is above 0.</summary>
        public static void DrawCircle(float x, float y, float radius, Color c, float thickness = 0f)
        {
            var b = new HudBlock { Kind = thickness > 0 ? Ring : Circle, Col = c.Packed(), Size = thickness };
            b.A[0] = x; b.A[1] = y; b.A[2] = radius;
            Emit(&b);
        }

        public static void DrawTriangle(Vector2 a, Vector2 b2, Vector2 c2, Color c)
        {
            var b = new HudBlock { Kind = Tri, Col = c.Packed() };
            b.A[0] = a.X; b.A[1] = a.Y; b.A[2] = b2.X; b.A[3] = b2.Y; b.A[4] = c2.X; b.A[5] = c2.Y;
            Emit(&b);
        }

        /// <summary>Text with its top at y; align 0 = left of x, 0.5 = centred on it, 1 = right of it.</summary>
        public static void DrawText(float x, float y, string text, float size, Color c, float align = 0f, bool bold = false)
        {
            var b = new HudBlock { Kind = Text, Col = c.Packed(), Size = size, Align = align, Bold = bold ? 1 : 0 };
            b.A[0] = x; b.A[1] = y;
            Emit(&b, text ?? "");
        }

        public static void DrawText(float x, float y, string text, float size = 32f) =>
            DrawText(x, y, text, size, Color.White);

        /// <summary>The engine's own one-line HUD message (the same as Lua's game.setHud).</summary>
        public static string Message { set { fixed (byte* p = Utf8.Of(value)) Bridge.N->SetHudText(p); } }

        /// <summary>Show the engine's crosshair in the middle of the view.</summary>
        public static bool Crosshair { set => Bridge.N->SetFloat(3, value ? 1 : 0); }
    }
}
