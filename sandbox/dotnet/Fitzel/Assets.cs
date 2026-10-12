using System.Collections.Generic;
using System.Numerics;
using System.Text.Json;
using static Fitzel.LuaApi;

namespace Fitzel
{
    /// <summary>One entry of the asset database (game.assets / game.assetInfo).</summary>
    public class AssetInfo
    {
        /// <summary>The 32-character GUID.</summary>
        public string Id;
        public string Name, Path, Type, Source;

        internal static AssetInfo From(JsonElement t) => new AssetInfo
        {
            Id = StrField(t, "id"), Name = StrField(t, "name"), Path = StrField(t, "path"),
            Type = StrField(t, "type"), Source = StrField(t, "source"),
        };
    }

    /// <summary>
    /// Everything the asset database knows -- engine and project, textures, models,
    /// sounds, materials. Wherever an asset is wanted, a GUID, a file name
    /// ("brick.png"), a relative path or a stem ("brick") will do.
    /// </summary>
    public static class Assets
    {
        /// <summary>All assets, or those of one type: "Texture", "Model", "Sound", "Material".</summary>
        public static List<AssetInfo> List(string type = null)
        {
            var l = new List<AssetInfo>();
            foreach (var t in Items(Call("game", "assets", type)[0])) l.Add(AssetInfo.From(t));
            return l;
        }

        /// <summary>Resolve a reference to a GUID, or null.</summary>
        public static string Find(string name, string type = null) => Str(Call("game", "findAsset", name, type), 0);

        public static AssetInfo Info(string reference)
        {
            var r = Call("game", "assetInfo", reference);
            return IsNil(r, 0) ? null : AssetInfo.From(r[0]);
        }

        /// <summary>The absolute path on disk, or null.</summary>
        public static string Path(string reference) => Str(Call("game", "assetPath", reference), 0);

        /// <summary>Read the database from disk again (new files appear).</summary>
        public static void Refresh() => Call("game", "refreshAssets");
    }

    /// <summary>An imported model in the model library (game.modelInfo).</summary>
    public class ModelInfo
    {
        public string Name, Path;
        public Vector3 Min, Max, Size;
        public int Meshes;
        public bool Animated;
    }

    public static class Models
    {
        /// <summary>Import a Model asset into the library (reused when already there); its id, or -1.</summary>
        public static int Load(string reference)
        {
            var r = Call("game", "loadModel", reference);
            return IsNil(r, 0) ? -1 : Int(r, 0);
        }

        public static ModelInfo Info(int modelId)
        {
            var r = Call("game", "modelInfo", modelId);
            if (IsNil(r, 0)) return null;
            var t = r[0];
            return new ModelInfo
            {
                Name = StrField(t, "name"), Path = StrField(t, "path"), Min = Vec(Field(t, "min")),
                Max = Vec(Field(t, "max")), Size = Vec(Field(t, "size")), Meshes = IntField(t, "meshes"),
                Animated = BoolField(t, "animated"),
            };
        }
    }

    public enum AlphaMode { Opaque = 0, Cutout = 1, Blend = 2 }

    /// <summary>A library material as it is (game.materialInfo / game.materials).</summary>
    public class MaterialInfo
    {
        public string Id, Name, Texture, NormalMap, EmissionMap;
        public Color Color, Emission;
        public float Reflectivity, Roughness, Opacity, Cutoff, EmissionStrength;
        public bool Glass, DoubleSided, FromModel;
        public AlphaMode AlphaMode;

        internal static MaterialInfo From(JsonElement t)
        {
            var c = Vec(Field(t, "color"));
            var e = Vec(Field(t, "emission"));
            return new MaterialInfo
            {
                Id = StrField(t, "id"), Name = StrField(t, "name"), Color = new Color(c.X, c.Y, c.Z),
                Reflectivity = NumField(t, "reflectivity"), Roughness = NumField(t, "roughness"),
                Opacity = NumField(t, "opacity", 1f), Glass = BoolField(t, "glass"),
                AlphaMode = (AlphaMode)IntField(t, "alphaMode"), Cutoff = NumField(t, "cutoff"),
                DoubleSided = BoolField(t, "doubleSided"), Emission = new Color(e.X, e.Y, e.Z),
                EmissionStrength = NumField(t, "emissionStrength", 1f), Texture = StrField(t, "texture"),
                NormalMap = StrField(t, "normalMap"), EmissionMap = StrField(t, "emissionMap"),
                FromModel = BoolField(t, "fromModel"),
            };
        }
    }

    /// <summary>
    /// Fields to write into a material (game.createMaterial / game.setMaterialProps):
    /// only the ones set, so one value changes without knowing the rest. A map slot
    /// set to "" is cleared.
    /// </summary>
    public class MaterialEdit
    {
        public string Name, Texture, NormalMap, EmissionMap;
        public Color? Color, Emission;
        public float? Reflectivity, Roughness, Opacity, Cutoff, EmissionStrength;
        public bool? Glass, DoubleSided;
        public AlphaMode? AlphaMode;

        internal Dictionary<string, object> ToLua()
        {
            var t = new Dictionary<string, object>();
            if (Name != null) t["name"] = Name;
            if (Color.HasValue) t["color"] = Color.Value;
            if (Reflectivity.HasValue) t["reflectivity"] = Reflectivity.Value;
            if (Roughness.HasValue) t["roughness"] = Roughness.Value;
            if (Opacity.HasValue) t["opacity"] = Opacity.Value;
            if (Glass.HasValue) t["glass"] = Glass.Value;
            if (AlphaMode.HasValue) t["alphaMode"] = (int)AlphaMode.Value;
            if (DoubleSided.HasValue) t["doubleSided"] = DoubleSided.Value;
            if (Cutoff.HasValue) t["cutoff"] = Cutoff.Value;
            if (Emission.HasValue) t["emission"] = Emission.Value;
            if (EmissionStrength.HasValue) t["emissionStrength"] = EmissionStrength.Value;
            if (Texture != null) t["texture"] = Texture;
            if (NormalMap != null) t["normalMap"] = NormalMap;
            if (EmissionMap != null) t["emissionMap"] = EmissionMap;
            return t;
        }
    }

    /// <summary>The project's material library. Several objects may share one: a change shows on all of them.</summary>
    public static class Materials
    {
        public static List<MaterialInfo> All()
        {
            var l = new List<MaterialInfo>();
            foreach (var t in Items(Call("game", "materials")[0])) l.Add(MaterialInfo.From(t));
            return l;
        }

        /// <summary>A material's GUID by name, or null.</summary>
        public static string Find(string name) => Str(Call("game", "findMaterial", name), 0);

        public static MaterialInfo Info(string reference)
        {
            var r = Call("game", "materialInfo", reference);
            return IsNil(r, 0) ? null : MaterialInfo.From(r[0]);
        }

        /// <summary>A new material; its GUID.</summary>
        public static string Create(MaterialEdit m) => Str(Call("game", "createMaterial", m.ToLua()), 0);

        /// <summary>Change a material live -- everything using it shows it at once.</summary>
        public static bool Set(string reference, MaterialEdit m) => Bool(Call("game", "setMaterialProps", reference, m.ToLua()), 0);
    }
}
