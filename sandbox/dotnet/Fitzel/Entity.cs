using System.Collections.Generic;
using System.Numerics;
using Fitzel.Internal;

namespace Fitzel
{
    /// <summary>What an object is (EntityType in the engine).</summary>
    public enum EntityType { Box = 0, Ramp = 1, Cylinder = 2, Sphere = 3, Light = 4, Sun = 5, Model = 6, Empty = 7, Plane = 8 }

    /// <summary>How a spawned object takes part in physics.</summary>
    public enum PhysicsMode { None = 0, Static = 1, Dynamic = 2 }

    /// <summary>What to make in <see cref="Entity.Spawn(SpawnInfo)"/>.</summary>
    public class SpawnInfo
    {
        public EntityType Type = EntityType.Sphere;
        public Vector3 Position;
        public Vector3 HalfExtents = new Vector3(0.5f);
        public Vector3 Rotation;
        public Color Color = new Color(0.8f, 0.8f, 0.8f);
        public Vector3 Velocity;
        public float Mass = 1f;
        public PhysicsMode Physics = PhysicsMode.Dynamic;
        public string Name = "";
        /// <summary>A Model asset (file name or GUID); the object is sized from the model.</summary>
        public string Model = "";
        public float Scale = 1f;
        /// <summary>A library material (name or GUID).</summary>
        public string Material = "";
        /// <summary>A script file under scripts/ (.cs or .lua).</summary>
        public string Script = "";
        public Entity Parent;
    }

    /// <summary>
    /// An object in the scene, by id. Cheap to make and to compare; it stays
    /// valid as long as the object exists (<see cref="Exists"/>).
    /// </summary>
    public sealed unsafe partial class Entity
    {
        public int Id { get; }
        internal Entity(int id) { Id = id; }
        /// <summary>The entity with this id (as Lua scripts and the engine name them), or null for an id below 0.</summary>
        public static Entity Of(int id) => id >= 0 ? new Entity(id) : null;

        public override bool Equals(object o) => o is Entity e && e.Id == Id;
        public override int GetHashCode() => Id;
        public override string ToString() => $"{Name} (#{Id})";
        public static bool operator ==(Entity a, Entity b) => a is null ? b is null : a.Equals(b);
        public static bool operator !=(Entity a, Entity b) => !(a == b);

        // --- Finding and making ---------------------------------------------
        /// <summary>The first object of that name, or null.</summary>
        public static Entity Find(string name)
        {
            int id;
            fixed (byte* p = Utf8.Of(name)) id = Bridge.N->Find(p);
            return id >= 0 ? new Entity(id) : null;
        }

        /// <summary>Every object of that name.</summary>
        public static List<Entity> FindAll(string name)
        {
            const int Cap = 1024;
            var list = new List<Entity>();
            var buf = new int[Cap];
            int n;
            fixed (byte* p = Utf8.Of(name))
            fixed (int* b = buf)
                n = Bridge.N->FindAll(p, b, Cap);
            for (int i = 0; i < n && i < Cap; i++) list.Add(new Entity(buf[i]));
            return list;
        }

        /// <summary>Make an object. It appears next frame; the entity is good at once.</summary>
        public static Entity Spawn(SpawnInfo s)
        {
            var b = new SpawnBlock
            {
                Type = (int)s.Type, Physics = (int)s.Physics, Parent = s.Parent?.Id ?? -1,
                Mass = s.Mass, Scale = s.Scale,
            };
            Put(b.Pos, s.Position); Put(b.Half, s.HalfExtents); Put(b.Rot, s.Rotation);
            Put(b.Vel, s.Velocity);
            b.Color[0] = s.Color.R; b.Color[1] = s.Color.G; b.Color[2] = s.Color.B;
            int id;
            fixed (byte* n = Utf8.Of(s.Name))
            fixed (byte* m = Utf8.Of(s.Model))
            fixed (byte* mat = Utf8.Of(s.Material))
            fixed (byte* sc = Utf8.Of(s.Script))
            {
                b.Name = n; b.Model = m; b.Material = mat; b.Script = sc;
                id = Bridge.N->Spawn(&b);
            }
            return id > 0 ? new Entity(id) : null;
        }

        /// <summary>Make a primitive at a position.</summary>
        public static Entity Spawn(EntityType type, Vector3 position, PhysicsMode physics = PhysicsMode.Dynamic) =>
            Spawn(new SpawnInfo { Type = type, Position = position, Physics = physics });

        /// <summary>Put down a copy of a prefab (by name), turned by yaw degrees.</summary>
        public static Entity SpawnPrefab(string prefab, Vector3 position, float yaw = 0f)
        {
            float* v = stackalloc float[3];
            Put(v, position);
            int id;
            fixed (byte* p = Utf8.Of(prefab)) id = Bridge.N->SpawnPrefab(p, v, yaw);
            return id > 0 ? new Entity(id) : null;
        }

        /// <summary>A copy of this object (and its children).</summary>
        public Entity Clone(string name = "")
        {
            int id;
            fixed (byte* p = Utf8.Of(name)) id = Bridge.N->Clone(Id, p);
            return id > 0 ? new Entity(id) : null;
        }

        /// <summary>Remove the object (at the end of the frame).</summary>
        public void Destroy() => Bridge.N->Destroy(Id);

        // --- What it is -----------------------------------------------------
        int Peek(out int type, out int parent, out bool active, out string name)
        {
            int* tpa = stackalloc int[3];
            byte* buf = stackalloc byte[512];
            tpa[0] = tpa[1] = tpa[2] = 0;
            buf[0] = 0;
            int ok = Bridge.N->Info(Id, tpa, buf, 512);
            type = tpa[0]; parent = tpa[1]; active = tpa[2] != 0;
            name = ok != 0 ? Utf8.From(buf) : "";
            return ok;
        }

        /// <summary>Is the object still in the scene?</summary>
        public bool Exists => Peek(out _, out _, out _, out _) != 0;

        public string Name
        {
            get { Peek(out _, out _, out _, out var n); return n; }
            set { fixed (byte* p = Utf8.Of(value)) Bridge.N->SetName(Id, p); }
        }

        public EntityType Type { get { Peek(out var t, out _, out _, out _); return (EntityType)t; } }

        /// <summary>Switched on? An inactive object and its children are hidden and do not run.</summary>
        public bool Active
        {
            get { Peek(out _, out _, out var a, out _); return a; }
            set => Bridge.N->SetActive(Id, value ? 1 : 0);
        }

        /// <summary>The parent, or null. Setting it keeps the object where it is in the world.</summary>
        public Entity Parent
        {
            get { Peek(out _, out var p, out _, out _); return p >= 0 ? new Entity(p) : null; }
            set => Bridge.N->SetParent(Id, value?.Id ?? -1);
        }

        // --- Where it is ----------------------------------------------------
        Vector3 Get(int what)
        {
            float* v = stackalloc float[3];
            v[0] = v[1] = v[2] = 0;
            Bridge.N->GetVec(Id, what, v);
            return new Vector3(v[0], v[1], v[2]);
        }

        void Set(int what, Vector3 x)
        {
            float* v = stackalloc float[3];
            Put(v, x);
            Bridge.N->SetVec(Id, what, v);
        }

        static void Put(float* v, Vector3 x) { v[0] = x.X; v[1] = x.Y; v[2] = x.Z; }

        /// <summary>Position relative to the parent (the world, for an object without one).</summary>
        public Vector3 Position { get => Get(0); set => Set(0, value); }
        /// <summary>Rotation relative to the parent, Euler degrees (applied X, then Y, then Z).</summary>
        public Vector3 Rotation { get => Get(1); set => Set(1, value); }
        /// <summary>Half the object's size along its own axes.</summary>
        public Vector3 HalfExtents { get => Get(2); set => Set(2, value); }
        /// <summary>Position in the world. Last frame's, for a child whose parent moved this frame.</summary>
        public Vector3 WorldPosition { get => Get(3); set => Set(3, value); }
        /// <summary>Rotation in the world, Euler degrees.</summary>
        public Vector3 WorldRotation { get => Get(4); set => Set(4, value); }

        /// <summary>The object's forward direction in the world (its local +Z).</summary>
        public Vector3 Forward
        {
            get
            {
                var r = WorldRotation * (System.MathF.PI / 180f);
                // The engine turns by Rz * Ry * Rx on column vectors; System.Numerics
                // multiplies row vectors, so the same turn reads X * Y * Z here.
                var m = Matrix4x4.CreateRotationX(r.X) * Matrix4x4.CreateRotationY(r.Y) * Matrix4x4.CreateRotationZ(r.Z);
                return Vector3.Normalize(Vector3.TransformNormal(Vector3.UnitZ, m));
            }
        }

        // --- Physics --------------------------------------------------------
        /// <summary>Linear velocity of a dynamic body (m/s).</summary>
        public Vector3 Velocity { get => Get(5); set => Set(5, value); }
        /// <summary>Angular velocity of a dynamic body (radians/s).</summary>
        public Vector3 AngularVelocity { set => Set(6, value); }

        /// <summary>A push (N*s) through the centre, or at a world point.</summary>
        public void AddImpulse(Vector3 impulse, Vector3? at = null)
        {
            float* i = stackalloc float[3];
            float* a = stackalloc float[3];
            Put(i, impulse);
            if (at.HasValue) Put(a, at.Value);
            Bridge.N->Impulse(Id, i, at.HasValue ? a : null);
        }

        // --- Animation (its AnimGraph component) -----------------------------
        public void AnimTrigger(string name) { fixed (byte* p = Utf8.Of(name)) Bridge.N->Anim(Id, 0, p, 0); }
        public void AnimSetBool(string name, bool v) { fixed (byte* p = Utf8.Of(name)) Bridge.N->Anim(Id, 1, p, v ? 1 : 0); }
        public void AnimSetNumber(string name, float v) { fixed (byte* p = Utf8.Of(name)) Bridge.N->Anim(Id, 2, p, v); }
    }
}
