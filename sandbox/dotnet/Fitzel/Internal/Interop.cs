// The C ABI between the engine and Fitzel.dll. Mirrors sandbox/src/CSharpHost.cpp
// field for field: change one, change the other, and bump Version on both sides
// -- the engine refuses a Fitzel.dll whose Version it does not know, which turns
// a stale copy in a project's Library/ folder into a message instead of a crash.
using System.Runtime.InteropServices;

namespace Fitzel.Internal
{
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct SpawnBlock
    {
        public int Type, Physics, Parent;
        public float Mass, Scale;
        public fixed float Pos[3];
        public fixed float Half[3];
        public fixed float Rot[3];
        public fixed float Color[3];
        public fixed float Vel[3];
        public byte* Name, Model, Material, Script;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct HitBlock
    {
        public fixed float Pos[3];
        public fixed float Normal[3];
        public int Id;
        public float Dist;
    }

    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct HudBlock
    {
        public int Kind;
        public fixed float A[6];
        public float Size, Align;
        public int Bold;
        public uint Col, Col2;
    }

    /// <summary>What the engine offers (filled in by the engine).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct NativeApi
    {
        public int Version;
        public delegate* unmanaged<byte*, void> Log;
        public delegate* unmanaged<byte*, void> Error;
        public delegate* unmanaged<int, int, int> Input;          // what, code
        public delegate* unmanaged<float*, int> MousePos;
        public delegate* unmanaged<float> MouseWheel;
        public delegate* unmanaged<int, void> ShowCursor;
        public delegate* unmanaged<float*, float*, int> MouseRay;
        public delegate* unmanaged<byte*, int> Find;
        public delegate* unmanaged<byte*, int*, int, int> FindAll;
        // what: 0 local position, 1 local rotation, 2 half extents, 3 world
        // position, 4 world rotation, 5 velocity, 6 angular velocity (set only)
        public delegate* unmanaged<int, int, float*, int> GetVec;
        public delegate* unmanaged<int, int, float*, void> SetVec;
        public delegate* unmanaged<int, float*, float*, void> Impulse;
        public delegate* unmanaged<int, int*, byte*, int, int> Info;
        public delegate* unmanaged<int, int, void> SetActive;
        public delegate* unmanaged<int, byte*, void> SetName;
        public delegate* unmanaged<int, int, void> SetParent;
        public delegate* unmanaged<int, void> Destroy;
        public delegate* unmanaged<SpawnBlock*, int> Spawn;
        public delegate* unmanaged<byte*, float*, float, int> SpawnPrefab;
        public delegate* unmanaged<int, byte*, int> Clone;
        public delegate* unmanaged<float*, float*, float, HitBlock*, int> CastRay;
        public delegate* unmanaged<float*, float*, float*, void> Camera;
        public delegate* unmanaged<int, float*, void> SetCamera;
        public delegate* unmanaged<byte*, float, float, float*, float, float, void> Sound;
        public delegate* unmanaged<HudBlock*, byte*, void> Hud;
        public delegate* unmanaged<float, float, float> TerrainHeight;
        public delegate* unmanaged<int, float> GetFloat;
        public delegate* unmanaged<int, float, void> SetFloat;
        public delegate* unmanaged<byte*, void> SetHudText;
        public delegate* unmanaged<int, byte*, void> Scene;
        public delegate* unmanaged<int, int, byte*, float, void> Anim;
        // table, fn, JSON arguments, ok -> JSON results (or the error message)
        public delegate* unmanaged<byte*, byte*, byte*, int*, byte*> CallApi;
    }

    /// <summary>What Fitzel.dll offers back (filled in by Bridge.Init).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ManagedApi
    {
        public delegate* unmanaged<byte*, int, byte*, int, int> Load;
        public delegate* unmanaged<void> Unload;
        public delegate* unmanaged<int, byte*, int> Create;
        public delegate* unmanaged<int, byte*, int, double, int, byte*, float*, void> SetParam;
        public delegate* unmanaged<int, int, float, float, int> Call;   // handle, fn, dt, time
        public delegate* unmanaged<int, void> Destroy;
        public delegate* unmanaged<byte*, void*, delegate* unmanaged<void*, byte*, int, double, int, byte*, float*, void>, int> Scan;
    }

    /// <summary>UTF-8 for the engine: a NUL-terminated copy to pin.</summary>
    internal static class Utf8
    {
        public static byte[] Of(string s) => System.Text.Encoding.UTF8.GetBytes((s ?? "") + "\0");
        public static unsafe string From(byte* p) => p == null ? "" : Marshal.PtrToStringUTF8((System.IntPtr)p) ?? "";
    }
}
