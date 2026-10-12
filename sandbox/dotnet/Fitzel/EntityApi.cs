using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using static Fitzel.LuaApi;

namespace Fitzel
{
    /// <summary>What <see cref="Entity.Info"/> tells about an object (game.entityInfo).</summary>
    public class EntityInfo
    {
        public int Id, Parent;
        public EntityType Type;
        public string Name, Script, Material, Model;
        public bool Active, ActiveInHierarchy, Physics, Dynamic;
    }

    /// <summary>A Light component's settings (game.getLight / game.setLight). Null fields are left as they are.</summary>
    public class LightSettings
    {
        public Color? Color;
        public float? Intensity, Range, SpotAngle, SpotBlend;
        public LightType? Type;
        public bool? Shadows, Baked;

        internal Dictionary<string, object> ToLua()
        {
            var t = new Dictionary<string, object>();
            if (Color.HasValue) t["color"] = Color.Value;
            if (Intensity.HasValue) t["intensity"] = Intensity.Value;
            if (Range.HasValue) t["range"] = Range.Value;
            if (Type.HasValue) t["type"] = (int)Type.Value;
            if (SpotAngle.HasValue) t["spotAngle"] = SpotAngle.Value;
            if (SpotBlend.HasValue) t["spotBlend"] = SpotBlend.Value;
            if (Shadows.HasValue) t["shadows"] = Shadows.Value;
            if (Baked.HasValue) t["baked"] = Baked.Value;
            return t;
        }
    }

    public enum LightType { Point = 0, Spot = 1 }

    /// <summary>A Collectible component (game.collectible).</summary>
    public class CollectibleInfo
    {
        public string Item, Icon, Category, Description, Sound;
        public int Count;
        public bool Inventory;
        public float Radius, Points;
    }

    /// <summary>Where game.moveCharacter put a figure's feet.</summary>
    public struct CharacterStep
    {
        public Vector3 Feet;
        public bool OnGround, OnTerrain;
    }

    /// <summary>Where a ray met a figure's skin (game.rayFigure).</summary>
    public struct FigureHit
    {
        public Vector3 Point, Normal;
        public string Bone;
    }

    public sealed unsafe partial class Entity
    {
        static Entity E(JsonElement[] r, int i) => IsNil(r, i) ? null : Of(Int(r, i));
        static List<Entity> List(JsonElement e)
        {
            var l = new List<Entity>();
            foreach (int id in Ids(e)) l.Add(new Entity(id));
            return l;
        }

        // --- Finding --------------------------------------------------------------
        /// <summary>Every object of the scene (game.entities).</summary>
        public static List<Entity> All() => List(Call("game", "entities")[0]);

        /// <summary>The object's profile (game.entityInfo), null when it is gone.</summary>
        public EntityInfo Info
        {
            get
            {
                var r = Call("game", "entityInfo", Id);
                if (IsNil(r, 0)) return null;
                var t = r[0];
                return new EntityInfo
                {
                    Id = IntField(t, "id"), Parent = IntField(t, "parent", -1), Type = (EntityType)IntField(t, "type"),
                    Name = StrField(t, "name"), Script = StrField(t, "script"), Material = StrField(t, "material"),
                    Model = StrField(t, "model"), Active = BoolField(t, "active"),
                    ActiveInHierarchy = BoolField(t, "activeInHierarchy"), Physics = BoolField(t, "physics"),
                    Dynamic = BoolField(t, "dynamic"),
                };
            }
        }

        /// <summary>Visible in effect: the object and every parent switched on (game.isActive).</summary>
        public bool ActiveInHierarchy => Bool(Call("game", "isActive", Id), 0);

        /// <summary>The direct children (game.children).</summary>
        public List<Entity> Children => List(Call("game", "children", Id)[0]);

        /// <summary>Set the local transform in one go (game.setLocal); rotation optional.</summary>
        public void SetLocal(Vector3 position, Vector3? rotation = null)
        {
            if (rotation.HasValue)
                Call("game", "setLocal", Id, position.X, position.Y, position.Z, rotation.Value.X, rotation.Value.Y, rotation.Value.Z);
            else
                Call("game", "setLocal", Id, position.X, position.Y, position.Z);
        }

        /// <summary>
        /// Many local transforms in one call (game.setLocals): a whole army a frame.
        /// Each entry: the object, its position, its rotation (null = position only
        /// for the whole batch).
        /// </summary>
        public static void SetLocals(IReadOnlyList<(Entity e, Vector3 pos, Vector3? rot)> list)
        {
            bool rot = list.Count > 0 && list[0].rot.HasValue;
            var flat = new List<float>(list.Count * (rot ? 7 : 4));
            foreach (var (e, p, r) in list)
            {
                flat.Add(e.Id); flat.Add(p.X); flat.Add(p.Y); flat.Add(p.Z);
                if (rot) { var v = r ?? Vector3.Zero; flat.Add(v.X); flat.Add(v.Y); flat.Add(v.Z); }
            }
            Call("game", "setLocals", flat, rot ? 7 : 4);
        }

        /// <summary>A point in the object's own space (metres from its middle, turned with it) in the world.</summary>
        public Vector3? ToWorld(Vector3 local)
        {
            var r = Call("game", "toWorld", Id, local.X, local.Y, local.Z);
            return IsNil(r, 0) ? null : new Vector3(Num(r, 0), Num(r, 1), Num(r, 2));
        }

        // --- Animation, light, material, sound ----------------------------------
        /// <summary>The Animation Graph's current state (game.animState), null without one.</summary>
        public string AnimState => Str(Call("game", "animState", Id), 0);

        /// <summary>The Light component (null without one). Setting writes only the fields given.</summary>
        public LightSettings Light
        {
            get
            {
                var r = Call("game", "getLight", Id);
                if (IsNil(r, 0)) return null;
                var t = r[0];
                var c = Vec(Field(t, "color"));
                return new LightSettings
                {
                    Color = new Color(c.X, c.Y, c.Z), Intensity = NumField(t, "intensity"), Range = NumField(t, "range"),
                    Type = (LightType)IntField(t, "type"), SpotAngle = NumField(t, "spotAngle"), SpotBlend = NumField(t, "spotBlend"),
                    Shadows = BoolField(t, "shadows"), Baked = BoolField(t, "baked"),
                };
            }
            set { if (value != null) Call("game", "setLight", Id, value.ToLua()); }
        }

        /// <summary>The library material (GUID; a name works when setting), null for none.</summary>
        public string Material
        {
            get => Str(Call("game", "getMaterial", Id), 0);
            set => Call("game", "setMaterial", Id, value ?? "");
        }

        /// <summary>Colour the object: it gets its own material (game.setColor), never colouring others.</summary>
        public bool SetColor(Color c) => Bool(Call("game", "setColor", Id, c.R, c.G, c.B), 0);

        /// <summary>Start / stop the AudioSource component.</summary>
        public void PlayAudio() => Call("game", "playAudio", Id);
        public void StopAudio() => Call("game", "stopAudio", Id);

        /// <summary>Fire the Particle component's burst where the object stands (game.emit).</summary>
        public void Emit() => Call("game", "emit", Id);

        /// <summary>
        /// The Door component: open (true), close (false), or ask (null). Returns how
        /// far it is open (0..1) and where it is heading, or null without a Door.
        /// </summary>
        public (float t, bool open)? Door(bool? open = null)
        {
            var r = open.HasValue ? Call("game", "door", Id, open.Value) : Call("game", "door", Id);
            return IsNil(r, 0) ? null : (Num(r, 0), Bool(r, 1));
        }

        /// <summary>The Collectible component, null without one.</summary>
        public CollectibleInfo Collectible
        {
            get
            {
                var r = Call("game", "collectible", Id);
                if (IsNil(r, 0)) return null;
                var t = r[0];
                return new CollectibleInfo
                {
                    Item = StrField(t, "item"), Icon = StrField(t, "icon"), Category = StrField(t, "category"),
                    Description = StrField(t, "description"), Count = IntField(t, "count"),
                    Inventory = BoolField(t, "inventory"), Radius = NumField(t, "radius"),
                    Points = NumField(t, "points"), Sound = StrField(t, "sound"),
                };
            }
        }

        /// <summary>Every active object with a Collectible component.</summary>
        public static List<Entity> Collectibles() => List(Call("game", "collectibles")[0]);

        // --- Figures ----------------------------------------------------------------
        /// <summary>
        /// Walk the figure as a capsule through the world (game.moveCharacter): vx, vz in
        /// m/s for dt seconds. Returns where its feet stand -- the script moves the object
        /// there itself -- or null outside Play.
        /// </summary>
        public CharacterStep? MoveCharacter(float vx, float vz, float dt = 1f / 60f)
        {
            var r = Call("game", "moveCharacter", Id, vx, vz, dt);
            if (IsNil(r, 0)) return null;
            return new CharacterStep { Feet = new Vector3(Num(r, 0), Num(r, 1), Num(r, 2)), OnGround = Bool(r, 3), OnTerrain = Bool(r, 4) };
        }

        /// <summary>Take the figure's capsule out of the world again (getting into a car).</summary>
        public void RemoveCharacter() => Call("game", "removeCharacter", Id);

        /// <summary>The figure's bones, in skeleton order.</summary>
        public List<string> Bones
        {
            get
            {
                var l = new List<string>();
                foreach (var b in Items(Call("game", "bones", Id)[0])) l.Add(Str(b, ""));
                return l;
            }
        }

        /// <summary>A bone's world position and rotation as the figure was last drawn, or null.</summary>
        public (Vector3 pos, Vector3 rot)? BonePose(string bone)
        {
            var r = Call("game", "bonePos", Id, bone);
            if (IsNil(r, 0)) return null;
            return (new Vector3(Num(r, 0), Num(r, 1), Num(r, 2)), new Vector3(Num(r, 3), Num(r, 4), Num(r, 5)));
        }

        /// <summary>
        /// Hang this object on a figure's bone (game.attach). Without offset it stays where
        /// it is, relative to the bone; with one it sits there (m, degrees in bone space),
        /// blending over `blend` seconds.
        /// </summary>
        public bool AttachTo(Entity figure, string bone, Vector3? offset = null, Vector3? rotation = null, float blend = 0f)
        {
            if (!offset.HasValue) return Bool(Call("game", "attach", Id, figure.Id, bone), 0);
            var o = offset.Value;
            var r = rotation ?? Vector3.Zero;
            return blend > 0f
                ? Bool(Call("game", "attach", Id, figure.Id, bone, o.X, o.Y, o.Z, r.X, r.Y, r.Z, blend), 0)
                : Bool(Call("game", "attach", Id, figure.Id, bone, o.X, o.Y, o.Z, r.X, r.Y, r.Z), 0);
        }

        /// <summary>Let go of the bone; the object stays where it last was.</summary>
        public void Detach() => Call("game", "detach", Id);

        /// <summary>Scale a bone and everything under it (0 = gone, 1 = back).</summary>
        public bool BoneScale(string bone, float scale) => Bool(Call("game", "boneScale", Id, bone, scale), 0);

        /// <summary>A ray against the figure's animated skin (game.rayFigure).</summary>
        public FigureHit? RayFigure(Vector3 origin, Vector3 dir, float maxDistance = 100f)
        {
            var r = Call("game", "rayFigure", Id, origin.X, origin.Y, origin.Z, dir.X, dir.Y, dir.Z, maxDistance);
            if (IsNil(r, 0)) return null;
            return new FigureHit
            {
                Point = new Vector3(Num(r, 0), Num(r, 1), Num(r, 2)),
                Normal = new Vector3(Num(r, 3), Num(r, 4), Num(r, 5)),
                Bone = Str(r, 6, ""),
            };
        }

        /// <summary>
        /// Bring a hand ("left"/"right") or foot ("leftFoot"/"rightFoot") to a point for
        /// this frame (inverse kinematics); weight 0..1 blends from the animation.
        /// </summary>
        public void Reach(string side, Vector3 target, float weight = 1f) =>
            Call("game", "reach", Id, side, target.X, target.Y, target.Z, weight);

        // --- Vehicles ---------------------------------------------------------------
        /// <summary>Put this vehicle into the physics world, parked (game.spawnVehicle).</summary>
        public bool SpawnVehicle() => Bool(Call("game", "spawnVehicle", Id), 0);
        /// <summary>The player's controls go to this vehicle (game.driveVehicle).</summary>
        public bool DriveVehicle() => Bool(Call("game", "driveVehicle", Id), 0);
    }

    /// <summary>The vehicle the player drives.</summary>
    public static class Vehicle
    {
        /// <summary>Controls back to the player; the car brakes where it is.</summary>
        public static void Leave() => Call("game", "leaveVehicle");

        /// <summary>The driven car: speed m/s, steer -1..1, throttle -1..1 -- or null.</summary>
        public static (Entity car, float speed, float steer, float throttle)? Driven
        {
            get
            {
                var r = Call("game", "drivenVehicle");
                if (IsNil(r, 0)) return null;
                return (Entity.Of(Int(r, 0)), Num(r, 1), Num(r, 2), Num(r, 3));
            }
        }
    }
}
