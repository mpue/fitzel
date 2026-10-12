// The engine's way into C#: one entry point (Init) that swaps function tables,
// and behind it the script assembly in a collectible load context, the live
// Behaviours by handle, and the inspector's view of their fields.
//
// Every entry point catches what a script throws. An exception crossing back
// into native code would take the editor down with it; caught, it becomes a
// message on the Script card and that one script stops, as a Lua error does.
using System;
using System.Collections.Generic;
using System.IO;
using System.Numerics;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace Fitzel.Internal
{
    public static unsafe class Bridge
    {
        public const int Version = 2;

        internal static NativeApi* N;

        // The game's scripts: Assembly-CSharp.dll, loaded from bytes so the
        // file stays free for the next build, in a context that can be thrown
        // away whole when Play starts again with a newer build.
        sealed class ScriptContext : AssemblyLoadContext
        {
            public ScriptContext() : base("Fitzel scripts", isCollectible: true) { }
            protected override Assembly Load(AssemblyName name)
            {
                // The scripts were compiled against a copy of Fitzel.dll; they
                // must meet THIS one, or Behaviour would be two different types.
                if (name.Name == "Fitzel") return typeof(Behaviour).Assembly;
                return null; // the framework, from the default context
            }
        }

        sealed class Kind
        {
            public Type Type;
            public MethodInfo Start, Update, OnDestroy;
        }

        sealed class Live
        {
            public Behaviour B;
            public Action Start, Update, OnDestroy;
        }

        static ScriptContext context;
        static Assembly scripts;
        static readonly Dictionary<string, Kind> kinds = new Dictionary<string, Kind>();
        static readonly Dictionary<int, Live> live = new Dictionary<int, Live>();
        static int nextHandle = 1;

        [UnmanagedCallersOnly]
        public static int Init(NativeApi* native, ManagedApi* managed)
        {
            if (native == null || native->Version != Version) return Version;
            N = native;
            managed->Load = &Load;
            managed->Unload = &Unload;
            managed->Create = &Create;
            managed->SetParam = &SetParam;
            managed->Call = &Call;
            managed->Destroy = &Destroy;
            managed->Scan = &Scan;
            return Version;
        }

        // --- Reporting ------------------------------------------------------
        internal static void Log(string s)
        {
            if (N == null) return;
            fixed (byte* p = Utf8.Of(s)) N->Log(p);
        }

        internal static void Error(string s)
        {
            if (N == null) return;
            fixed (byte* p = Utf8.Of(s)) N->Error(p);
        }

        static void Report(Exception e, string who)
        {
            if (e is TargetInvocationException tie && tie.InnerException != null) e = tie.InnerException;
            Error($"{who}: {e.GetType().Name}: {e.Message}\n{e.StackTrace}");
        }

        // --- The assembly ---------------------------------------------------
        [UnmanagedCallersOnly]
        static int Load(byte* dll, int dllLen, byte* pdb, int pdbLen)
        {
            try
            {
                DropAll();
                context = new ScriptContext();
                using var image = new UnmanagedMemoryStream(dll, dllLen);
                if (pdb != null && pdbLen > 0)
                {
                    using var symbols = new UnmanagedMemoryStream(pdb, pdbLen);
                    scripts = context.LoadFromStream(image, symbols);
                }
                else
                {
                    scripts = context.LoadFromStream(image);
                }
                return 1;
            }
            catch (Exception e)
            {
                Report(e, "Assembly-CSharp");
                return 0;
            }
        }

        [UnmanagedCallersOnly]
        static void Unload()
        {
            try { DropAll(); } catch (Exception e) { Report(e, "unload"); }
        }

        static void DropAll()
        {
            foreach (var l in live.Values)
            {
                try { l.OnDestroy?.Invoke(); }
                catch (Exception e) { Report(e, l.B.GetType().Name + ".OnDestroy"); }
            }
            live.Clear();
            kinds.Clear();
            scripts = null;
            if (context != null)
            {
                context.Unload();
                context = null;
                GC.Collect();
                GC.WaitForPendingFinalizers();
            }
        }

        static Kind KindOf(string cls)
        {
            if (kinds.TryGetValue(cls, out var k)) return k;
            if (scripts == null) return null;
            Type[] all;
            try { all = scripts.GetTypes(); }
            catch (ReflectionTypeLoadException e) { all = e.Types; }
            Type found = null;
            foreach (var t in all)
            {
                if (t == null || t.IsAbstract || !typeof(Behaviour).IsAssignableFrom(t)) continue;
                if (t.Name == cls || t.FullName == cls) { found = t; break; }
            }
            if (found == null) return null;
            k = new Kind
            {
                Type = found,
                Start = Magic(found, "Start"),
                Update = Magic(found, "Update"),
                OnDestroy = Magic(found, "OnDestroy"),
            };
            kinds[cls] = k;
            return k;
        }

        // Unity's way: a method of that name without parameters, of any
        // visibility, declared anywhere between the class and Behaviour.
        static MethodInfo Magic(Type t, string name)
        {
            const BindingFlags F = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic |
                                   BindingFlags.DeclaredOnly;
            for (var c = t; c != null && c != typeof(Behaviour); c = c.BaseType)
            {
                var m = c.GetMethod(name, F, null, Type.EmptyTypes, null);
                if (m != null && m.ReturnType == typeof(void)) return m;
            }
            return null;
        }

        static Action Bind(MethodInfo m, Behaviour b) =>
            m == null ? null : (Action)Delegate.CreateDelegate(typeof(Action), b, m);

        // --- Behaviours -----------------------------------------------------
        [UnmanagedCallersOnly]
        static int Create(int entity, byte* cls)
        {
            string name = Utf8.From(cls);
            try
            {
                var k = KindOf(name);
                if (k == null)
                {
                    Error(scripts == null
                        ? $"{name}: the C# scripts are not built (see the C# Scripts window)"
                        : $"{name}: Assembly-CSharp has no class '{name}' deriving from Fitzel.Behaviour");
                    return 0;
                }
                // Not the constructor: field initialisers alone, so a script that
                // reaches for the engine in its constructor cannot run before it
                // knows its entity.
                var b = (Behaviour)RuntimeHelpers.GetUninitializedObject(k.Type);
                var ctor = k.Type.GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                                                 null, Type.EmptyTypes, null);
                b.Entity = new Entity(entity);
                ctor?.Invoke(b, null);
                int h = nextHandle++;
                live[h] = new Live
                {
                    B = b,
                    Start = Bind(k.Start, b),
                    Update = Bind(k.Update, b),
                    OnDestroy = Bind(k.OnDestroy, b),
                };
                return h;
            }
            catch (Exception e)
            {
                Report(e, name);
                return 0;
            }
        }

        [UnmanagedCallersOnly]
        static int Call(int h, int fn, float dt, float time)
        {
            if (!live.TryGetValue(h, out var l)) return 0;
            var a = fn == 0 ? l.Start : l.Update;
            if (a == null) return 1;
            Time.DeltaTime = dt;
            Time.Elapsed = time;
            try
            {
                a();
                return 1;
            }
            catch (Exception e)
            {
                Report(e, l.B.GetType().Name + (fn == 0 ? ".Start" : ".Update"));
                return 0;
            }
        }

        [UnmanagedCallersOnly]
        static void Destroy(int h)
        {
            if (!live.Remove(h, out var l)) return;
            try { l.OnDestroy?.Invoke(); }
            catch (Exception e) { Report(e, l.B.GetType().Name + ".OnDestroy"); }
        }

        // --- Inspector fields -----------------------------------------------
        // Public instance fields, and private ones marked [SerializeField], of a
        // type the inspector can edit; [HideInInspector] leaves one out.
        // ScriptParam's types: 0 number, 1 bool, 2 string, 3 vec3, 4 colour.
        static IEnumerable<FieldInfo> Fields(Type t)
        {
            const BindingFlags F = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
            foreach (var f in t.GetFields(F))
            {
                if (f.IsInitOnly || f.IsLiteral || f.GetCustomAttribute<NonSerializedAttribute>() != null) continue;
                if (f.GetCustomAttribute<HideInInspectorAttribute>() != null) continue;
                if (!f.IsPublic && f.GetCustomAttribute<SerializeFieldAttribute>() == null) continue;
                if (TypeCode(f.FieldType) < 0) continue;
                yield return f;
            }
        }

        static int TypeCode(Type t)
        {
            if (t == typeof(float) || t == typeof(double) || t == typeof(int) || t == typeof(long) ||
                t == typeof(short) || t == typeof(byte)) return 0;
            if (t == typeof(bool)) return 1;
            if (t == typeof(string)) return 2;
            if (t == typeof(Vector3)) return 3;
            if (t == typeof(Color)) return 4;
            return -1;
        }

        [UnmanagedCallersOnly]
        static void SetParam(int h, byte* nameP, int type, double num, int b, byte* str, float* vec)
        {
            if (!live.TryGetValue(h, out var l)) return;
            string name = Utf8.From(nameP);
            try
            {
                FieldInfo f = null;
                foreach (var c in Fields(l.B.GetType())) if (c.Name == name) { f = c; break; }
                if (f == null) return;
                var ft = f.FieldType;
                object v = TypeCode(ft) switch
                {
                    0 => Convert.ChangeType(ft == typeof(float) || ft == typeof(double) ? num : Math.Round(num), ft),
                    1 => b != 0,
                    2 => Utf8.From(str),
                    3 => new Vector3(vec[0], vec[1], vec[2]),
                    4 => new Color(vec[0], vec[1], vec[2]),
                    _ => null,
                };
                if (v != null) f.SetValue(l.B, v);
            }
            catch (Exception e)
            {
                Report(e, l.B.GetType().Name + "." + name);
            }
        }

        [UnmanagedCallersOnly]
        static int Scan(byte* cls, void* ctx, delegate* unmanaged<void*, byte*, int, double, int, byte*, float*, void> sink)
        {
            string name = Utf8.From(cls);
            try
            {
                var k = KindOf(name);
                if (k == null) return 0;
                // The defaults are what the field initialisers say: an instance
                // made for the purpose, never started, never seen by the game.
                var probe = RuntimeHelpers.GetUninitializedObject(k.Type);
                k.Type.GetConstructor(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
                                      null, Type.EmptyTypes, null)?.Invoke(probe, null);
                float* v = stackalloc float[3];
                foreach (var f in Fields(k.Type))
                {
                    int tc = TypeCode(f.FieldType);
                    object val = f.GetValue(probe);
                    double num = 0;
                    int bl = 0;
                    string s = "";
                    v[0] = v[1] = v[2] = 0;
                    switch (tc)
                    {
                        case 0: num = Convert.ToDouble(val); break;
                        case 1: bl = (bool)val ? 1 : 0; break;
                        case 2: s = (string)val ?? ""; break;
                        case 3: { var x = (Vector3)val; v[0] = x.X; v[1] = x.Y; v[2] = x.Z; break; }
                        case 4: { var c = (Color)val; v[0] = c.R; v[1] = c.G; v[2] = c.B; break; }
                    }
                    fixed (byte* np = Utf8.Of(f.Name))
                    fixed (byte* sp = Utf8.Of(s))
                        sink(ctx, np, tc, num, bl, sp, v);
                }
                return 1;
            }
            catch (Exception e)
            {
                Report(e, name);
                return 0;
            }
        }
    }
}
