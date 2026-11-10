# The scene walk: Blender objects in, a Fitzel scene (.fitzel) and its models out.
#
# What lands in the project folder:
#   <project>/<scene>.fitzel         the scene (JSON, schema v3)
#   <project>/models/<scene>/*.glb   one model per distinct mesh (+ .meta GUIDs)
#   <project>/scripts/*.lua          the objects' Lua scripts
#   <project>/hdri/*                 the World as a panorama: its image, or rendered
#   <project>/game.json              only when the project has none yet
#
# The scene file is SHARED with the Fitzel editor. Every Blender object keeps
# the id of its entity (Object.fitzel.entity_id), and models/<scene>/
# .fitzel-blender lists the ids Blender wrote last time: an entity with any
# other id was made in Fitzel and is kept as it is, and sync.py brings the
# lights changed in Fitzel back into Blender by the same ids.
#
# Each distinct mesh is written ONCE, in its own local space, and every object
# using it becomes a Model entity pointing at that file -- linked duplicates and
# collection instances cost one model, not one each. A Fitzel model entity is
# drawn so that its file's bounding box fills center +/- half (SceneSubmit.cpp),
# so an entity's center is where the box's centre lands in the world and its
# half is the box's half size times the object's scale.
#
# Re-exporting keeps every GUID (the .meta files are read, not rewritten) and
# rewrites a .glb only when its bytes changed, so the engine's caches hold.

import json
import math
import os
import re
import shutil
import struct
import uuid

import bpy
import numpy as np
from mathutils import Matrix, Vector

from . import convert

GEOMETRY = {"MESH", "CURVE", "SURFACE", "FONT", "META"}
MARKER = ".fitzel-blender"   # in models/<scene>/: this exporter owns the folder

# Fitzel's EntityType (sandbox/src/SceneTypes.hpp) -- the serialized index.
T_LIGHT, T_SUN, T_MODEL, T_EMPTY = 4, 5, 6, 7
# game::StartMode (sandbox/src/GameSettings.hpp); -1 = the project's game.json.
START_MODE = {"FPS": 0, "CAMERA": 3, "GAME": -1}
# Fitzel's budget per frame (Renderer.hpp): more spots are dropped, more
# shadowed points lose their shadows to the nearer ones.
MAX_SPOTS, MAX_SHADOWED = 8, 8
# Fitzel's import brightens a glTF glow 1.5x, 3x over an emission map
# (ModelLibrary.cpp); "Like Blender" writes the glow that much darker.
EMISSION_BOOST, EMISSION_BOOST_MAPPED = 1.5, 3.0
# The look settings "Like Blender" takes over, at Fitzel's own defaults
# (PostLook.hpp, WeatherPreset.hpp) -- written back when Fitzel's Look returns.
FITZEL_LOOK = {"exposure": 1.0, "autoExposure": True, "tonemapCurve": 1, "hue": 0.0,
               "saturation": 1.35, "value": 1.0, "warmth": 0.18, "contrast": 0.16,
               "gradeSplit": 0.0, "gradeVibrance": 0.0, "vignette": 0.2, "filmGrain": 0.0,
               "postBloom": True, "postLens": True, "fogDensity": 0.0006}

# Scene files as this exporter left them (path -> mtime): the live link's way
# of telling its own write from Fitzel's.
OWN_WRITES = {}
# The panorama camera's turn that makes Cycles' equirectangular render come out
# laid out as an environment image is read: checked by rendering a World of
# known directions both ways (see tools/blender/README.md).
WORLD_CAMERA_EULER = (math.pi / 2.0, 0.0, -math.pi / 2.0)


class ExportError(Exception):
    pass


def safe_name(s):
    s = re.sub(r"[^A-Za-z0-9_.-]+", "_", s).strip("._")
    return s or "object"


def project_paths(settings, scene):
    """(project folder, scene name) as the exporter and the launcher see them."""
    if not settings.project_dir.strip():
        raise ExportError("Set the Fitzel project folder first (Scene > Fitzel)")
    project = os.path.normpath(os.path.abspath(bpy.path.abspath(settings.project_dir)))
    return project, safe_name(settings.scene_name.strip() or scene.name)


def scene_file(project, scene_name):
    return os.path.join(project, scene_name + ".fitzel")


def read_json(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def read_owned(models_dir):
    """What Blender wrote into the scene last time: (entity ids, instance name -> id,
    the rest of the record)."""
    data = read_json(os.path.join(models_dir, MARKER))
    if not isinstance(data, dict):
        return set(), {}, {}
    ids = {i for i in data.get("owned", []) if isinstance(i, int)}
    inst = {k: v for k, v in data.get("instances", {}).items() if isinstance(v, int)}
    return ids, inst, data


def scene_world(entities):
    """Every entity's Fitzel world (centre, rotation), its parents composed in:
    translation and rotation only, as Fitzel's hierarchy does (SceneGraph.hpp)."""
    by = {e["id"]: e for e in entities if isinstance(e, dict) and isinstance(e.get("id"), int)}
    out = {}

    def resolve(i, depth):
        if i in out:
            return out[i]
        e = by[i]
        c = Vector(e.get("center", [0.0, 0.0, 0.0]))
        r = convert.euler_matrix(e.get("rotation", [0.0, 0.0, 0.0]))
        p = e.get("parent", -1)
        if p in by and p != i and depth < 64:
            pc, pr = resolve(p, depth + 1)
            c, r = pc + pr @ c, pr @ r
        out[i] = (c, r)
        return out[i]

    for i in by:
        resolve(i, 0)
    return out


def adjust_glb(path, glow_plain=1.0, glow_mapped=1.0):
    """Fitzel's reading of a glTF material, undone in the file (in place):
    - the base colour factor is linear in glTF, but Fitzel takes it as an
      sRGB value and decodes it again (lit.frag, pow 2.2) -- so it is written
      encoded, and 0.5 grey stays 0.5 grey instead of 0.22;
    - every glow is multiplied by `glow_plain` (`glow_mapped` where an emission
      map drives it), to take back the boost Fitzel's import gives it."""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 20 or data[:4] != b"glTF":
        return
    clen, ctype = struct.unpack_from("<II", data, 12)
    if ctype != 0x4E4F534A:   # "JSON"
        return
    doc = json.loads(data[20:20 + clen])
    changed = False
    for m in doc.get("materials", []):
        pbr = m.setdefault("pbrMetallicRoughness", {})
        base = pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0])
        enc = [round(max(0.0, c) ** (1.0 / 2.2), 7) for c in base[:3]] + list(base[3:])
        if enc != base:
            pbr["baseColorFactor"] = enc
            changed = True
        if glow_plain == 1.0 and glow_mapped == 1.0:
            continue
        f = m.get("emissiveFactor")
        if not f or max(f) <= 0.0:
            continue
        k = glow_mapped if "emissiveTexture" in m else glow_plain
        ext = m.get("extensions", {}).get("KHR_materials_emissive_strength")
        if ext is not None:
            ext["emissiveStrength"] = ext.get("emissiveStrength", 1.0) * k
        else:
            m["emissiveFactor"] = [x * k for x in f]
        changed = True
    if not changed:
        return
    js = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    js += b" " * ((4 - len(js) % 4) % 4)
    rest = data[20 + clen:]
    with open(path, "wb") as f:
        f.write(struct.pack("<4sII", b"glTF", 2, 20 + len(js) + len(rest)))
        f.write(struct.pack("<II", len(js), 0x4E4F534A))
        f.write(js)
        f.write(rest)


def write_hdr(path, rgb, w, h):
    """A Radiance .hdr of one linear colour. Run-length encoded scanlines, as
    Blender writes them: stb_image, which Fitzel reads panoramas with, decodes
    flat ones wrongly (garbage, or a factor of 1e-36)."""
    peak = max(rgb[:3])
    if peak < 1e-32:
        texel = bytes(4)
    else:
        m, e = math.frexp(peak)
        f = m * 256.0 / peak
        texel = bytes([min(255, int(c * f)) for c in rgb[:3]] + [e + 128])
    line = bytearray([2, 2, w >> 8, w & 255])
    for channel in texel:   # each channel its own runs, 127 texels at most
        left = w
        while left:
            n = min(127, left)
            line += bytes([128 + n, channel])
            left -= n
    with open(path, "wb") as out:
        out.write(f"#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y {h} +X {w}\n".encode("ascii"))
        out.write(bytes(line) * h)


def light_threshold(scene):
    return getattr(getattr(scene, "eevee", None), "light_threshold", 0.01) or 0.01


def light_values(obj, world, scene):
    """A Blender lamp as Fitzel has it: (light component, rotation kind). Shared by
    the export and by sync.py, which compares a file against it to find what
    was changed in Fitzel."""
    light, fz, s = obj.data, obj.fitzel, scene.fitzel
    spot = light.type in {"SPOT", "AREA"}
    if light.type == "SPOT":
        angle, blend = convert.spot_cone(light)
    elif light.type == "AREA":
        angle, blend = convert.area_cone(light)
    else:
        angle, blend = 32.0, 0.2
    if fz.light_override or fz.light_falloff == "CLASSIC":
        intensity, rng = fz.light_intensity, fz.light_range
    else:
        intensity = convert.lamp_intensity(light, s.lighting, world)
        rng = convert.lamp_range(light, light_threshold(scene), world)
    shadows = light.use_shadow if fz.shadow_mode == "BLENDER" else fz.shadow_mode == "ON"
    lc = {"type": "light", "lightType": 1 if spot else 0,
          "color": convert.vec3(light.color),
          "intensity": round(intensity, 5), "range": round(rng, 4),
          "spotAngle": round(angle, 4), "spotBlend": round(blend, 4),
          "castShadows": bool(shadows and not spot), "shadowBias": 0.003,
          "bakedOnly": bool(fz.light_baked and not spot),
          "falloff": 0 if fz.light_falloff == "CLASSIC" else 1}
    return lc, "SPOT" if spot else "OBJECT"


def exported_objects(scene, view_layer):
    """The objects an export writes, parents before children (the walk's order)."""
    s = scene.fitzel

    def wanted(obj):
        o = obj
        while o is not None:   # a Do Not Export parent takes its children along
            if o.fitzel.role == "IGNORE":
                return False
            o = o.parent
        return not s.only_visible or obj.visible_get(view_layer=view_layer)

    def depth(o):
        d = 0
        while o.parent is not None:
            d, o = d + 1, o.parent
        return d
    objs = [o for o in view_layer.objects if wanted(o)]
    objs.sort(key=lambda o: (depth(o), o.name))
    return objs


def find_sun(objs):
    """The sun lamp that drives Fitzel's sky: the first, as the walk orders them."""
    for o in objs:
        if o.type == "LIGHT" and o.data.type == "SUN" and o.fitzel.role == "AUTO":
            return o
    return None


def sun_values(sun, scene):
    """(Sun component, timeOfDay, sunDeclination) for a sun lamp (None: the
    scene's clock and Fitzel's own sun)."""
    s = scene.fitzel
    if sun is None:
        # Fitzel's own sun -- unless the scene is lit like Blender, where a scene
        # without a sun lamp has no sun.
        return ({"type": "sun", "color": [1.0, 0.97, 0.9],
                 "intensity": 0.0 if s.lighting == "MATCH" else 1.0},
                s.time_of_day, -10.4)
    fz = sun.fitzel
    hour, dec = convert.sun_clock(sun.matrix_world)
    tint, intensity = convert.sun_light(sun.data, sun.matrix_world, s.lighting)
    if fz.light_override:
        intensity = fz.light_intensity
    return ({"type": "sun", "color": convert.vec3(tint), "intensity": round(intensity, 5)},
            hour, dec)


def read_guid(meta_path):
    try:
        with open(meta_path, "r", encoding="utf-8") as f:
            guid = json.load(f).get("guid", "")
        if isinstance(guid, str) and re.fullmatch(r"[0-9a-fA-F]{32}", guid):
            return guid.lower()
    except (OSError, ValueError, AttributeError):
        pass
    return None


def ensure_meta(path, asset_type):
    """The asset's GUID: the one its .meta already has, else a fresh one written
    now. Fitzel's own minting has repeated GUIDs before, so ours are uuid4."""
    meta = path + ".meta"
    guid = read_guid(meta)
    if guid:
        return guid
    guid = uuid.uuid4().hex
    write_text(meta, json.dumps({"guid": guid, "type": asset_type}, indent=2) + "\n")
    return guid


def write_text(path, text):
    """Write through a temporary file, so a reader never sees half a file."""
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    os.replace(tmp, path)


def same_bytes(a, b):
    try:
        if os.path.getsize(a) != os.path.getsize(b):
            return False
        with open(a, "rb") as fa, open(b, "rb") as fb:
            return fa.read() == fb.read()
    except OSError:
        return False


def mesh_bounds(mesh):
    """Bounding box of the vertices faces use -- what the glTF exporter writes
    and therefore what Fitzel measures. None for a mesh without faces."""
    if not mesh.polygons:
        return None
    co = np.empty(len(mesh.vertices) * 3, dtype=np.float64)
    mesh.vertices.foreach_get("co", co)
    used = np.empty(len(mesh.loops), dtype=np.int64)
    mesh.loops.foreach_get("vertex_index", used)
    pts = co.reshape(-1, 3)[np.unique(used)]
    lo, hi = pts.min(axis=0), pts.max(axis=0)
    return Vector((lo + hi) * 0.5), Vector(hi - lo)


class Model:
    """One .glb to write: the mesh copy it is made from and where it goes."""

    def __init__(self, name, mesh, center, size, obj_materials):
        self.name = name
        self.mesh = mesh                    # a temporary copy, removed after writing
        self.center = center                # bounding box, Blender local space
        self.size = size
        self.obj_materials = obj_materials  # [(slot index, material)] linked to the object
        self.file = ""
        self.guid = ""


class Exporter:
    def __init__(self, context, report=None):
        self.context = context
        self.scene = context.scene
        self.s = self.scene.fitzel
        self.report = report or (lambda level, msg: print(f"[Fitzel] {msg}"))
        self.project, self.scene_name = project_paths(self.s, self.scene)
        self.models_rel = f"models/{self.scene_name}"
        self.models_dir = os.path.join(self.project, "models", self.scene_name)
        self.models = {}         # key -> Model, or None when it has no faces
        self.file_names = set()
        self.entities = []
        self.placed = {}         # Blender object name -> (entity id, center, rotation)
        self.scripts = {}        # file name in scripts/ -> source text
        self.bake_count = 0
        self.player_start = False
        self.main_camera = False
        self.warnings = []
        self.sun = None          # the Blender sun lamp object driving the sky
        self.depsgraph = None
        self.mode = self.s.lighting
        self.scene_path = scene_file(self.project, self.scene_name)
        # The scene as it is on disk -- Fitzel may have added to it -- and what
        # of it Blender wrote last time.
        old = read_json(self.scene_path)
        self.old_entities = [e for e in (old or {}).get("entities", [])
                             if isinstance(e, dict) and isinstance(e.get("id"), int)]
        self.old_settings = (old or {}).get("settings", {})
        if not isinstance(self.old_settings, dict):
            self.old_settings = {}
        self.owned_prev, self.inst_prev, self.record_prev = read_owned(self.models_dir)
        self.taken = {0}         # entity ids given out in this export (0 is the Sun)
        self.next_free = 1 + max([0] + [e["id"] for e in self.old_entities] +
                                 list(self.owned_prev) + list(self.inst_prev.values()) +
                                 [o.fitzel.entity_id for o in bpy.data.objects])
        self.inst_ids = {}       # instance name -> id, kept for the next export
        self.rewritten = 0       # .glb files whose bytes changed
        self.spots = 0
        self.shadowed = 0

    # --- entities -------------------------------------------------------------

    def fresh_id(self):
        while self.next_free in self.taken:
            self.next_free += 1
        self.taken.add(self.next_free)
        return self.next_free

    def id_for(self, obj):
        """The object's entity id: the one it was given before, so Fitzel's
        changes find it again -- a fresh one for a new object, or for a copy
        (Shift+D duplicates the id with the rest of the object)."""
        eid = obj.fitzel.entity_id
        if eid > 0 and eid not in self.taken:
            self.taken.add(eid)
            return eid
        eid = self.fresh_id()
        try:
            obj.fitzel.entity_id = eid
        except AttributeError:   # linked from a library: read-only, id changes per export
            pass
        return eid

    def id_for_instance(self, name):
        eid = self.inst_prev.get(name, 0)
        if eid > 0 and eid not in self.taken:
            self.taken.add(eid)
        else:
            eid = self.fresh_id()
        self.inst_ids[name] = eid
        return eid

    def add_entity(self, eid, name, etype, center, rot, half, comps, parent):
        """Append one entity. `center`/`rot` are Fitzel world values; the file
        stores them relative to the parent (its world position and rotation --
        scale is not part of Fitzel's hierarchy, SceneGraph.hpp)."""
        if parent:
            pid, pc, pr = parent
            prt = pr.transposed()
            local_c, local_r = prt @ (center - pc), prt @ rot
        else:
            pid, local_c, local_r = -1, center, rot
        e = {"type": etype, "name": name, "id": eid, "parent": pid, "active": True,
             "center": convert.vec3(local_c), "rotation": convert.euler_deg(local_r),
             "half": convert.vec3(half)}
        if comps:
            e["components"] = comps
        self.entities.append(e)
        return eid, center, rot

    def physics_of(self, obj):
        mode = obj.fitzel.physics
        if mode == "AUTO":
            mode = self.s.collision
        if mode == "STATIC":
            return [{"type": "physics", "dynamic": False, "mass": 1.0}]
        if mode == "DYNAMIC":
            return [{"type": "physics", "dynamic": True, "mass": obj.fitzel.mass}]
        return []

    def behaviour_of(self, obj):
        """Scripts and extra components: what any kind of entity can carry."""
        comps = []
        for item in obj.fitzel.scripts:
            name = self.add_script(item, obj)
            if name:
                comps.append({"type": "script", "file": name})
        if obj.fitzel.extra:
            try:
                data = json.loads(obj.fitzel.extra.as_string())
            except ValueError as ex:
                self.warn(f"{obj.name}: extra components are not JSON ({ex})")
                data = []
            for c in data if isinstance(data, list) else [data]:
                if isinstance(c, dict) and isinstance(c.get("type"), str):
                    comps.append(c)
                else:
                    self.warn(f"{obj.name}: an extra component needs a \"type\"")
        return comps

    def add_script(self, item, obj):
        if item.text:
            name = item.text.name
            src = item.text.as_string()
        elif item.filepath:
            path = bpy.path.abspath(item.filepath)
            try:
                with open(path, "r", encoding="utf-8") as f:
                    src = f.read()
            except OSError as ex:
                self.warn(f"{obj.name}: cannot read script {path} ({ex})")
                return None
            name = os.path.basename(path)
        else:
            return None
        name = safe_name(name)
        if not name.lower().endswith(".lua"):
            name += ".lua"
        if self.scripts.get(name, src) != src:
            self.warn(f"two different scripts are called {name}; the first one is kept")
        else:
            self.scripts[name] = src
        return name

    def warn(self, msg):
        self.warnings.append(msg)
        self.report({"WARNING"}, msg)

    # --- models ---------------------------------------------------------------

    def model_for(self, key, eval_obj, orig, base_name, bake=None):
        """The model a piece of geometry is drawn with, made on first use."""
        if key in self.models:
            return self.models[key]
        try:
            if key[0] == "PTR":
                # Geometry a node tree instanced: new_from_object would read the
                # HOST's evaluated mesh (often empty); the instance's own is .data.
                mesh = eval_obj.data.copy()
            else:
                mesh = bpy.data.meshes.new_from_object(
                    eval_obj, preserve_all_data_layers=True, depsgraph=self.depsgraph)
        except (RuntimeError, AttributeError):
            mesh = None
        if mesh is None:
            self.models[key] = None
            return None
        if bake is not None:
            mesh.transform(bake.to_4x4())
            if bake.determinant() < 0.0:
                mesh.flip_normals()
        bounds = mesh_bounds(mesh)
        if bounds is None:
            bpy.data.meshes.remove(mesh)
            self.models[key] = None
            return None
        obj_mats = []
        if orig is not None and hasattr(orig, "material_slots"):
            obj_mats = [(i, slot.material) for i, slot in enumerate(orig.material_slots)
                        if slot.link == "OBJECT"]
        name = safe_name(base_name)
        unique, n = name, 2
        while unique.lower() in self.file_names:
            unique, n = f"{name}_{n}", n + 1
        self.file_names.add(unique.lower())
        m = Model(unique, mesh, bounds[0], bounds[1], obj_mats)
        m.file = unique + ".glb"
        m.guid = ensure_meta(os.path.join(self.models_dir, m.file), "Model")
        self.models[key] = m
        return m

    def geometry_key(self, eval_obj, orig, instancer=None):
        """Which pieces of geometry are the same file. A plain mesh is its mesh
        data (linked duplicates share it); anything a modifier or an object
        material changes is the object's own; geometry a node tree made up on
        the spot is whatever the depsgraph hands out."""
        own_mats = any(s.link == "OBJECT" for s in getattr(orig, "material_slots", []))
        modified = any(m.show_viewport for m in getattr(orig, "modifiers", []))
        if instancer is not None and orig == instancer:
            return ("PTR", eval_obj.data.as_pointer())
        if orig.type == "MESH" and not modified and not own_mats:
            return ("MESH", orig.data.name_full)
        return ("OBJ", orig.name_full)

    def add_model_entity(self, eid, name, world, model, baked, comps, parent):
        if baked:
            rot = Matrix.Identity(3)
            scale = Vector((1.0, 1.0, 1.0))
            center = convert.point(world.translation + model.center)
        else:
            rot = convert.rotation(world)
            scale = convert.scale_of(world)
            center = convert.point(world @ model.center)
        size = convert.axes(model.size)
        half = Vector([max(size[i], 1e-4) * scale[i] * 0.5 for i in range(3)])
        mc = {"type": "model", "model": model.guid, "modelFile": model.file,
              "scale": round((scale.x + scale.y + scale.z) / 3.0, 6)}
        return self.add_entity(eid, name, T_MODEL, center, rot, half, [mc] + comps, parent)

    def place_geometry(self, eid, name, world, eval_obj, orig, behaviour, parent,
                       instancer=None):
        bake = None
        if convert.needs_bake(world):
            bake = world.to_3x3()
            self.bake_count += 1
            key = ("BAKE", self.bake_count)
        else:
            key = self.geometry_key(eval_obj, orig, instancer)
        model = self.model_for(key, eval_obj, orig, name if bake else self.base_name(key, orig),
                               bake)
        if model is None:
            return None
        return self.add_model_entity(eid, name, world, model, bake is not None,
                                     self.physics_of(orig) + behaviour, parent)

    @staticmethod
    def base_name(key, orig):
        if key[0] == "MESH":
            return orig.data.name
        return orig.name

    # --- the other kinds ------------------------------------------------------

    def place_light(self, eid, name, world, obj, comps, parent):
        lc, kind = light_values(obj, world, self.scene)
        if lc["lightType"] == 1:
            self.spots += 1
        elif lc["castShadows"] and not lc["bakedOnly"]:
            self.shadowed += 1
        return self.add_entity(eid, name, T_LIGHT, convert.point(world.translation),
                               convert.rotation(world, kind), Vector((0.15, 0.15, 0.15)),
                               [lc] + comps, parent)

    def place_camera(self, eid, name, world, obj, comps, parent):
        # A camera marked active-on-start IS the view in Play, whatever the start
        # mode (main.cpp, startPlay) -- so the scene camera only becomes it when
        # the scene is watched, never over the walking player's eyes.
        main = obj.fitzel.camera_main or (obj == self.scene.camera and
                                          self.s.start_mode != "FPS")
        if main and self.main_camera:
            main = False   # one main camera; the scene camera came first
        self.main_camera = self.main_camera or main
        cc = {"type": "camera", "mode": 0,
              "fov": round(convert.vertical_fov(obj.data, self.scene.render), 4),
              "activeOnStart": bool(main), "shotTarget": -1}
        return self.add_entity(eid, name, T_EMPTY, convert.point(world.translation),
                               convert.rotation(world, "VIEW"), Vector((0.5, 0.5, 0.5)),
                               [cc] + comps, parent)

    def place_player_start(self, eid, name, world, obj, comps, parent):
        if self.player_start:
            self.warn(f"{name}: there is already a Player Start; this one is an empty")
            return self.place_empty(eid, name, world, comps, parent)
        self.player_start = True
        local_fwd = Vector((0.0, 0.0, -1.0)) if obj.type == "CAMERA" else Vector((0.0, 1.0, 0.0))
        fwd = convert.point(world.to_3x3().normalized() @ local_fwd)
        yaw, _ = convert.yaw_pitch(fwd)
        # Fitzel reads the marker's Y rotation as the player's yaw, and its
        # centre as where the feet stand (main.cpp, startPlay).
        rot = Matrix.Rotation(math.radians(yaw), 3, "Y")
        ps = {"type": "player_start", "moveSpeed": self.s.move_speed,
              "runSpeed": self.s.run_speed, "stepHeight": self.s.step_height,
              "stepLength": 2.1 if self.s.run_speed > 0 else 1.04}
        e = {"type": T_EMPTY, "name": name, "id": eid, "parent": -1, "active": True,
             "center": convert.vec3(convert.point(world.translation)),
             "rotation": [0.0, round(yaw, 4), 0.0], "half": [0.3, 0.9, 0.3],
             "components": [ps] + comps}
        self.entities.append(e)
        return e["id"], convert.point(world.translation), rot

    def place_empty(self, eid, name, world, comps, parent):
        return self.add_entity(eid, name, T_EMPTY, convert.point(world.translation),
                               convert.rotation(world), Vector((0.5, 0.5, 0.5)), comps, parent)

    # --- the walk -------------------------------------------------------------

    def parent_of(self, obj):
        p = obj.parent
        while p is not None:
            if p.name_full in self.placed:
                return self.placed[p.name_full]
            p = p.parent
        return None

    def walk(self):
        view_layer = self.context.view_layer
        # Evaluated once: asking again after a mesh copy was added to
        # bpy.data would re-evaluate the scene -- and pull the instance
        # iterator below out from under itself.
        depsgraph = self.depsgraph = self.context.evaluated_depsgraph_get()
        objs = exported_objects(self.scene, view_layer)

        self.sun = find_sun(objs)
        # Ids first, all of them, so an object keeps its id even when one
        # processed before it was a copy carrying the same.
        ids = {o.name_full: self.id_for(o) for o in objs if o is not self.sun}
        for obj in objs:
            if obj.type == "LIGHT" and obj.data.type == "SUN" and obj.fitzel.role == "AUTO":
                if obj is not self.sun:
                    self.warn(f"{obj.name}: Fitzel has one sun; {self.sun.name} drives it")
                continue
            eid = ids[obj.name_full]
            world = obj.matrix_world.copy()
            parent = self.parent_of(obj)
            comps = self.behaviour_of(obj)
            placed = None
            if obj.fitzel.role == "PLAYER_START":
                placed = self.place_player_start(eid, obj.name, world, obj, comps, parent)
            elif obj.type in GEOMETRY and not (obj.is_instancer and
                                                not obj.show_instancer_for_viewport):
                placed = self.place_geometry(eid, obj.name, world,
                                             obj.evaluated_get(depsgraph), obj, comps, parent)
            elif obj.type == "LIGHT":
                placed = self.place_light(eid, obj.name, world, obj, comps, parent)
            elif obj.type == "CAMERA":
                placed = self.place_camera(eid, obj.name, world, obj, comps, parent)
            if placed is None:   # empties, armatures, faceless curves: keep the hierarchy
                placed = self.place_empty(eid, obj.name, world, comps, parent)
            if obj.hide_render:  # off in renders: there, but switched off (and back)
                for e in reversed(self.entities):
                    if e["id"] == eid:
                        e["active"] = False
                        break
            self.placed[obj.name_full] = placed

        # Collection instances, particles and geometry-node instances: every
        # copy hangs under the object that made it. Matrices are copied -- the
        # iterator reuses them.
        count = {}
        for inst in depsgraph.object_instances:
            if not inst.is_instance or inst.parent is None:
                continue
            host = inst.parent.original
            parent = self.placed.get(host.name_full)
            if parent is None:
                continue
            ob = inst.object
            orig = ob.original
            n = count[host.name_full] = count.get(host.name_full, 0) + 1
            name = f"{host.name}/{orig.name}.{n:03d}"
            world = inst.matrix_world.copy()
            if ob.type in GEOMETRY:
                self.place_geometry(self.id_for_instance(name), name, world, ob, orig, [],
                                    parent, instancer=host)
            elif ob.type == "LIGHT" and ob.data.type != "SUN":
                self.place_light(self.id_for_instance(name), name, world, orig, [], parent)

    # --- writing --------------------------------------------------------------

    def write_models(self):
        """One glTF export per distinct mesh, each from a lone temporary object
        at the origin, so the file holds the mesh in its own space."""
        todo = [m for m in self.models.values() if m is not None]
        if not todo:
            return
        op_props = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
        wanted = dict(
            export_format="GLB", use_selection=True, export_apply=False, export_yup=True,
            export_texcoords=True, export_normals=True, export_tangents=False,
            export_materials="EXPORT", export_image_format="AUTO",
            export_cameras=False, export_lights=False, export_extras=False,
            export_animations=False, export_skins=False, export_morph=False,
            export_draco_mesh_compression_enable=False,   # cgltf in Fitzel reads no Draco
            export_gpu_instances=False, export_image_add_webp=False,
            export_image_webp_fallback=False, will_save_settings=False, check_existing=False)
        opts = {k: v for k, v in wanted.items() if k in op_props}

        view_layer = self.context.view_layer
        selected = [o for o in view_layer.objects if o.select_get()]
        active = view_layer.objects.active
        coll = bpy.data.collections.new("fitzel_export_tmp")
        self.scene.collection.children.link(coll)
        try:
            for o in selected:
                o.select_set(False)
            for m in todo:
                # The names end up in the file: the same every time, so an
                # unchanged mesh writes the same bytes and its file is kept.
                m.mesh.name = "fitzel:" + m.name
                obj = bpy.data.objects.new("fitzel:" + m.name, m.mesh)
                coll.objects.link(obj)
                for i, mat in m.obj_materials:
                    if i < len(obj.material_slots):
                        obj.material_slots[i].link = "OBJECT"
                        obj.material_slots[i].material = mat
                view_layer.update()
                obj.select_set(True)
                view_layer.objects.active = obj
                path = os.path.join(self.models_dir, m.file)
                tmp = path[:-4] + ".export-tmp.glb"
                try:
                    bpy.ops.export_scene.gltf(filepath=tmp, **opts)
                finally:
                    bpy.data.objects.remove(obj)
                if self.mode == "MATCH":
                    adjust_glb(tmp, 1.0 / EMISSION_BOOST, 1.0 / EMISSION_BOOST_MAPPED)
                else:
                    adjust_glb(tmp)
                if same_bytes(tmp, path):
                    os.remove(tmp)      # unchanged: keep the file and its date
                else:
                    os.replace(tmp, path)
                    self.rewritten += 1
        finally:
            bpy.data.collections.remove(coll)
            for o in selected:
                try:
                    o.select_set(True)
                except RuntimeError:
                    pass
            view_layer.objects.active = active
            for m in todo:
                bpy.data.meshes.remove(m.mesh)
                m.mesh = None

    def drop_stale_models(self):
        """Models of objects deleted in Blender since the last export. Only in
        a folder this exporter made (the marker says so)."""
        if not os.path.exists(os.path.join(self.models_dir, MARKER)):
            return
        keep = {m.file.lower() for m in self.models.values() if m is not None}
        for f in os.listdir(self.models_dir):
            low = f.lower()
            base = low[:-5] if low.endswith(".meta") else low
            if base.endswith(".glb") and base not in keep:
                os.remove(os.path.join(self.models_dir, f))

    def write_scripts(self):
        if not self.scripts:
            return
        d = os.path.join(self.project, "scripts")
        os.makedirs(d, exist_ok=True)
        for name, src in self.scripts.items():
            path = os.path.join(d, name)
            try:
                with open(path, "r", encoding="utf-8") as f:
                    if f.read() == src:
                        continue
            except OSError:
                pass
            write_text(path, src)

    # --- the World ----------------------------------------------------------

    def world_kind(self):
        """What the World is: ("IMAGE", node, strength) for an environment image
        straight into the Background, ("COLOR", rgb, strength) for a plain
        colour, ("NODES",) for anything else -- rendered to a panorama."""
        world = self.scene.world
        if world is None:
            return None
        tree = world.node_tree   # None: an old World without nodes
        if tree is None:
            return ("COLOR", tuple(world.color), 1.0)
        out = next((n for n in tree.nodes if n.type == "OUTPUT_WORLD" and n.is_active_output),
                   None) or next((n for n in tree.nodes if n.type == "OUTPUT_WORLD"), None)
        if out is None or not out.inputs["Surface"].is_linked:
            return ("COLOR", (0.0, 0.0, 0.0), 1.0)
        bg = out.inputs["Surface"].links[0].from_node
        if bg.type != "BACKGROUND" or bg.inputs["Strength"].is_linked:
            return ("NODES",)
        strength = bg.inputs["Strength"].default_value
        col = bg.inputs["Color"]
        if not col.is_linked:
            return ("COLOR", tuple(col.default_value[:3]), strength)
        env = col.links[0].from_node
        if env.type != "TEX_ENVIRONMENT" or env.image is None or env.inputs["Vector"].is_linked \
                or env.projection != "EQUIRECTANGULAR":
            return ("NODES",)
        tm = env.texture_mapping
        if max(abs(a) for a in tm.rotation) > 1e-6 or max(abs(a) for a in tm.translation) > 1e-6 \
                or tuple(tm.scale) != (1.0, 1.0, 1.0):
            return ("NODES",)
        return ("IMAGE", env, strength)

    def world_signature(self):
        """Everything a World render depends on, to render it only when it changed."""
        world = self.scene.world
        parts = [self.s.world_resolution, world.name_full]

        def val(v):
            try:
                return repr(tuple(round(x, 6) for x in v))
            except TypeError:
                return repr(round(v, 6) if isinstance(v, float) else v)
        skip = {"location", "width", "height", "dimensions", "select", "show_options",
                "show_preview", "hide", "show_texture", "name", "label", "location_absolute"}
        for n in world.node_tree.nodes:
            parts.append(n.bl_idname + ":" + n.name)
            for p in n.bl_rna.properties:
                if p.type in {"BOOLEAN", "INT", "FLOAT", "ENUM", "STRING"} and \
                        p.identifier not in skip:
                    try:
                        parts.append(p.identifier + "=" + val(getattr(n, p.identifier)))
                    except (AttributeError, TypeError):
                        pass
            img = getattr(n, "image", None)
            if img is not None:
                parts.append("img=" + img.filepath + str(tuple(img.size)))
            for i in n.inputs:
                if hasattr(i, "default_value") and not i.is_linked:
                    parts.append(i.identifier + "=" + val(i.default_value))
        for link in world.node_tree.links:
            parts.append(f"{link.from_node.name}.{link.from_socket.identifier}>"
                         f"{link.to_node.name}.{link.to_socket.identifier}")
        return uuid.uuid5(uuid.NAMESPACE_OID, "|".join(parts)).hex

    def copy_world_image(self, env):
        img = env.image
        src = bpy.path.abspath(img.filepath, library=img.library) if img.filepath else ""
        name = safe_name(os.path.basename(src) if src else img.name)
        if "." not in name:
            name += ".hdr" if img.file_format == "HDR" else ".exr"
        dst = os.path.join(self.project, "hdri", name)
        if img.packed_file:
            data = img.packed_file.data
            if not (os.path.exists(dst) and os.path.getsize(dst) == len(data)):
                with open(dst, "wb") as f:
                    f.write(data)
        elif src and os.path.isfile(src):
            if not same_bytes(src, dst):
                shutil.copyfile(src, dst)
        else:
            self.warn(f"World texture {img.name} not found on disk")
            return None
        return f"hdri/{name}"

    def color_world(self, rgb):
        """A plain colour as a small panorama of that colour (an HDR, so it may
        be brighter than white)."""
        name = f"{self.scene_name}_world.hdr"
        dst = os.path.join(self.project, "hdri", name)
        tmp = dst[:-4] + ".export-tmp.hdr"
        # Written here, not through bpy.types.Image.save, which encodes a float
        # image for the display (sRGB) on the way out.
        write_hdr(tmp, rgb, 64, 32)
        if same_bytes(tmp, dst):
            os.remove(tmp)
        else:
            os.replace(tmp, dst)
        return f"hdri/{name}"

    def render_world(self):
        """The World however it is built -- a sky texture, gradients, a turned
        image -- rendered by Cycles to an equirectangular panorama laid out as
        Blender's environment images are, which is how Fitzel reads them
        (EnvironmentIBL.cpp). Rendered again only when the World changed."""
        name = f"{self.scene_name}_world.hdr"
        dst = os.path.join(self.project, "hdri", name)
        sig = self.world_signature()
        if self.record_prev.get("world") == sig and os.path.exists(dst):
            self.world_sig = sig
            return f"hdri/{name}"
        res = int(self.s.world_resolution)
        tmp_scene = bpy.data.scenes.new("fitzel_world_bake")
        cam_data = bpy.data.cameras.new("fitzel_world_bake")
        cam = bpy.data.objects.new("fitzel_world_bake", cam_data)
        tmp = dst[:-4] + ".export-tmp.hdr"
        try:
            tmp_scene.world = self.scene.world
            tmp_scene.collection.objects.link(cam)
            tmp_scene.camera = cam
            cam_data.type = "PANO"
            if hasattr(cam_data, "panorama_type"):
                cam_data.panorama_type = "EQUIRECTANGULAR"
            else:
                cam_data.cycles.panorama_type = "EQUIRECTANGULAR"
            cam.rotation_euler = WORLD_CAMERA_EULER
            r = tmp_scene.render
            r.engine = "CYCLES"
            r.resolution_x, r.resolution_y, r.resolution_percentage = res, res // 2, 100
            r.film_transparent = False
            r.image_settings.file_format = "HDR"
            r.filepath = tmp
            tmp_scene.cycles.samples = 16
            tmp_scene.cycles.use_denoising = False
            tmp_scene.cycles.device = "CPU"
            tmp_scene.view_settings.view_transform = "Standard"
            tmp_scene.view_settings.exposure = 0.0
            bpy.ops.render.render(write_still=True, scene=tmp_scene.name)
        finally:
            bpy.data.objects.remove(cam)
            bpy.data.cameras.remove(cam_data)
            bpy.data.scenes.remove(tmp_scene)
        if not os.path.exists(tmp):
            self.warn("The World could not be rendered to a panorama")
            return None
        if same_bytes(tmp, dst):
            os.remove(tmp)
        else:
            os.replace(tmp, dst)
        self.world_sig = sig
        return f"hdri/{name}"

    def world_panorama(self):
        """(project-relative panorama, IBL intensity) for the World, or None."""
        kind = self.world_kind() if self.s.export_world else None
        if kind is None or (self.mode != "MATCH" and kind[0] != "IMAGE"):
            return None
        os.makedirs(os.path.join(self.project, "hdri"), exist_ok=True)
        if kind[0] == "IMAGE":
            rel, strength = self.copy_world_image(kind[1]), kind[2]
        elif kind[0] == "COLOR":
            rel, strength = self.color_world(kind[1]), kind[2]
        else:
            rel, strength = self.render_world(), 1.0   # the strength is in the render
        if rel is None:
            return None
        ensure_meta(os.path.join(self.project, rel), "Texture")
        return rel, strength

    def sun_entity(self):
        comp, _, _ = sun_values(self.sun, self.scene)
        return {"type": T_SUN, "name": self.sun.name if self.sun else "Sun", "id": 0,
                "parent": -1, "active": True, "center": [0.0, 0.0, 0.0],
                "half": [1.0, 1.0, 1.0], "rotation": [0.0, 0.0, 0.0], "components": [comp]}

    def settings(self):
        s = dict(self.old_settings) if self.s.keep_settings else {}
        _, hour, dec = sun_values(self.sun, self.scene)
        s.update({
            # No terrain entity, and none to migrate in from older settings.
            "terrainEntity": True,
            "startMode": START_MODE[self.s.start_mode],
            "timeOfDay": round(hour, 4), "sunLatitude": 0.0,
            "sunDeclination": round(dec, 4), "timeFlows": self.s.time_flows,
            "waterLevel": self.s.water_level if self.s.water else -1000.0,
        })
        if self.s.clean_world:
            s.update({"grassEnabled": False, "treeEnabled": False, "flowerEnabled": False,
                      "birdsEnabled": False, "wildlife": False, "fireflyEnabled": False,
                      "motes": False, "soundscape": False, "farTerrain": False,
                      "herdCount": 0, "showGrid": False})
        cam = self.scene.camera
        if cam is not None:
            fwd = convert.point(cam.matrix_world.to_3x3().normalized() @ Vector((0, 0, -1)))
            yaw, pitch = convert.yaw_pitch(fwd)
            p = convert.point(cam.matrix_world.translation)
            s["editorCamera"] = {"x": round(p.x, 4), "y": round(p.y, 4), "z": round(p.z, 4),
                                 "yaw": round(yaw, 4), "pitch": round(pitch, 4)}
        if self.mode == "MATCH":
            # Blender's picture: its exposure and view transform, and nothing on
            # top that Blender does not do -- no grade, bloom, lens, haze or
            # weather, no exposure of Fitzel's own.
            vs = self.scene.view_settings
            contrast, saturation = convert.look_grade(vs.look)
            s.update({"exposure": round(convert.fitzel_exposure(vs), 5), "autoExposure": False,
                      "tonemapCurve": convert.tonemap_curve(vs.view_transform),
                      "hue": 0.0, "saturation": saturation, "value": 1.0, "warmth": 0.0,
                      "contrast": contrast, "gradeSplit": 0.0, "gradeVibrance": 0.0,
                      "vignette": 0.0, "filmGrain": 0.0, "postBloom": False,
                      "postLens": False, "fogDensity": 0.0, "volFog": False,
                      "weather": 0.0, "autoWeather": False, "cloudShadows": False})
        elif self.record_prev.get("lighting") == "MATCH":
            s.update(FITZEL_LOOK)   # back from "Like Blender": Fitzel's own look again
        pano = self.world_panorama()
        if pano:
            # "Like Blender" lights in absolute units: the World's own
            # brightness, not Fitzel's normalised one (main.cpp, iblAbsolute).
            s.update({"hdri": pano[0], "ibl": True, "iblSkybox": True,
                      "iblIntensity": round(pano[1], 5),
                      "iblAbsolute": self.mode == "MATCH"})
        elif self.s.export_world and s.get("hdri"):
            s.update({"hdri": "", "ibl": False, "iblSkybox": False, "iblAbsolute": False})
        return s

    def write_game_json(self):
        path = os.path.join(self.project, "game.json")
        if os.path.exists(path):
            return
        mode = {"FPS": "fps", "CAMERA": "camera"}.get(self.s.start_mode, "fps")
        write_text(path, json.dumps({
            "productName": os.path.basename(self.project), "startScene": self.scene_name,
            "exportScenes": [self.scene_name], "startMode": mode, "scripting": "lua",
        }, indent=2) + "\n")

    def kept_entities(self, ours):
        """The entities of the file on disk that Fitzel made: not written by
        Blender last time and not claimed by an object now. A child of an
        entity that is gone keeps its place in the world."""
        ids = {e["id"] for e in ours} | {0}
        keep = [e for e in self.old_entities
                if e["id"] not in self.owned_prev and e["id"] not in ids
                and e.get("type") != T_SUN]
        keep_ids = {e["id"] for e in keep}
        world = None
        for e in keep:
            p = e.get("parent", -1)
            if p != -1 and p not in ids and p not in keep_ids:
                if world is None:
                    world = scene_world(self.old_entities)
                c, r = world[e["id"]]
                e["parent"] = -1
                e["center"], e["rotation"] = convert.vec3(c), convert.euler_deg(r)
        return keep

    def write_record(self):
        record = {"note": "Written by the Fitzel Blender exporter; its .glb files are "
                          "replaced on every export, and these are the entities it owns "
                          "in the scene.",
                  "owned": sorted(self.taken), "instances": self.inst_ids,
                  "lighting": self.mode}
        sig = getattr(self, "world_sig", None) or self.record_prev.get("world")
        if sig:
            record["world"] = sig
        write_text(os.path.join(self.models_dir, MARKER), json.dumps(record, indent=1) + "\n")

    def run(self):
        os.makedirs(self.models_dir, exist_ok=True)
        mine = not os.listdir(self.models_dir) or \
            os.path.exists(os.path.join(self.models_dir, MARKER))
        try:
            self.walk()
            self.write_models()
        finally:
            for m in self.models.values():   # an export that failed half way
                if m is not None and m.mesh is not None:
                    bpy.data.meshes.remove(m.mesh)
                    m.mesh = None
        if mine:
            self.write_record()   # before the clean-up below, which asks for it
        self.drop_stale_models()
        self.write_scripts()

        if self.s.start_mode == "FPS" and not self.player_start and self.scene.camera is None:
            self.warn("No Player Start and no scene camera: the player starts at the origin")
        if self.spots > MAX_SPOTS:
            self.warn(f"{self.spots} spot and area lights: Fitzel draws {MAX_SPOTS} of them")
        if self.shadowed > MAX_SHADOWED:
            self.warn(f"{self.shadowed} lights cast shadows: Fitzel shadows the "
                      f"{MAX_SHADOWED} nearest")
        grid = os.path.join(self.project, "lightgrids", self.scene_name + ".fgrid")
        if self.rewritten and os.path.exists(grid):
            self.warn("The geometry changed since the indirect light was baked -- bake again")
        kept = self.kept_entities(self.entities)
        doc = {"version": 3,
               "app": f"Blender {bpy.app.version_string} / fitzel_exporter",
               "entities": [self.sun_entity()] + self.entities + kept,
               "settings": self.settings()}
        if mine:
            self.write_record()   # again: the World render may have signed it
        write_text(self.scene_path, json.dumps(doc, indent=2) + "\n")
        OWN_WRITES[self.scene_path] = os.path.getmtime(self.scene_path)
        self.write_game_json()
        self.kept = len(kept)
        n_models = sum(1 for m in self.models.values() if m is not None)
        return self.scene_path, len(self.entities) + 1, n_models
