# The scene walk: Blender objects in, a Fitzel scene (.fitzel) and its models out.
#
# What lands in the project folder:
#   <project>/<scene>.fitzel         the scene (JSON, schema v3)
#   <project>/models/<scene>/*.glb   one model per distinct mesh (+ .meta GUIDs)
#   <project>/scripts/*.lua          the objects' Lua scripts
#   <project>/hdri/*                 the World's environment texture, if any
#   <project>/game.json              only when the project has none yet
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
        self.next_id = 1         # 0 is the Sun
        self.scripts = {}        # file name in scripts/ -> source text
        self.bake_count = 0
        self.player_start = False
        self.main_camera = False
        self.warnings = []
        self.sun = None          # the Blender sun lamp object driving the sky
        self.depsgraph = None

    # --- entities -------------------------------------------------------------

    def add_entity(self, name, etype, center, rot, half, comps, parent):
        """Append one entity. `center`/`rot` are Fitzel world values; the file
        stores them relative to the parent (its world position and rotation --
        scale is not part of Fitzel's hierarchy, SceneGraph.hpp)."""
        eid = self.next_id
        self.next_id += 1
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

    def add_model_entity(self, name, world, model, baked, comps, parent):
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
        return self.add_entity(name, T_MODEL, center, rot, half, [mc] + comps, parent)

    def place_geometry(self, name, world, eval_obj, orig, behaviour, parent, instancer=None):
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
        return self.add_model_entity(name, world, model, bake is not None,
                                     self.physics_of(orig) + behaviour, parent)

    @staticmethod
    def base_name(key, orig):
        if key[0] == "MESH":
            return orig.data.name
        return orig.name

    # --- the other kinds ------------------------------------------------------

    def place_light(self, name, world, obj, comps, parent):
        light = obj.data
        fz = obj.fitzel
        intensity = fz.light_intensity if fz.light_override else convert.light_intensity(light)
        rng = fz.light_range if fz.light_override else convert.light_range(light)
        spot = light.type == "SPOT"
        lc = {"type": "light", "lightType": 1 if spot else 0,
              "color": convert.vec3(light.color),
              "intensity": round(intensity, 4), "range": round(rng, 4),
              "spotAngle": round(min(max(
                  math.degrees(light.spot_size) * 0.5, 5.0), 80.0), 3)
              if spot else 32.0,
              "spotBlend": round(light.spot_blend, 4) if spot else 0.2,
              "castShadows": bool(fz.light_shadows and not spot),
              "shadowBias": 0.003, "bakedOnly": False}
        if light.type == "AREA":
            self.warn(f"{name}: area lights become point lights")
        rot = convert.rotation(world, "SPOT" if spot else "OBJECT")
        return self.add_entity(name, T_LIGHT, convert.point(world.translation), rot,
                               Vector((0.15, 0.15, 0.15)), [lc] + comps, parent)

    def place_camera(self, name, world, obj, comps, parent):
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
        return self.add_entity(name, T_EMPTY, convert.point(world.translation),
                               convert.rotation(world, "VIEW"), Vector((0.5, 0.5, 0.5)),
                               [cc] + comps, parent)

    def place_player_start(self, name, world, obj, comps, parent):
        if self.player_start:
            self.warn(f"{name}: there is already a Player Start; this one is an empty")
            return self.place_empty(name, world, comps, parent)
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
        e = {"type": T_EMPTY, "name": name, "id": self.next_id, "parent": -1, "active": True,
             "center": convert.vec3(convert.point(world.translation)),
             "rotation": [0.0, round(yaw, 4), 0.0], "half": [0.3, 0.9, 0.3],
             "components": [ps] + comps}
        self.next_id += 1
        self.entities.append(e)
        return e["id"], convert.point(world.translation), rot

    def place_empty(self, name, world, comps, parent):
        return self.add_entity(name, T_EMPTY, convert.point(world.translation),
                               convert.rotation(world), Vector((0.5, 0.5, 0.5)), comps, parent)

    # --- the walk -------------------------------------------------------------

    def exported(self, obj, view_layer):
        o = obj
        while o is not None:   # a Do Not Export parent takes its children along
            if o.fitzel.role == "IGNORE":
                return False
            o = o.parent
        return not self.s.only_visible or obj.visible_get(view_layer=view_layer)

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
        objs = [o for o in view_layer.objects if self.exported(o, view_layer)]

        def depth(o):
            d = 0
            while o.parent is not None:
                d, o = d + 1, o.parent
            return d
        objs.sort(key=lambda o: (depth(o), o.name))

        for obj in objs:
            if obj.type == "LIGHT" and obj.data.type == "SUN" and obj.fitzel.role == "AUTO":
                if self.sun is None:
                    self.sun = obj
                else:
                    self.warn(f"{obj.name}: Fitzel has one sun; {self.sun.name} drives it")
                continue
            world = obj.matrix_world.copy()
            parent = self.parent_of(obj)
            comps = self.behaviour_of(obj)
            placed = None
            if obj.fitzel.role == "PLAYER_START":
                placed = self.place_player_start(obj.name, world, obj, comps, parent)
            elif obj.type in GEOMETRY and not (obj.is_instancer and
                                                not obj.show_instancer_for_viewport):
                placed = self.place_geometry(obj.name, world, obj.evaluated_get(depsgraph),
                                             obj, comps, parent)
            elif obj.type == "LIGHT":
                placed = self.place_light(obj.name, world, obj, comps, parent)
            elif obj.type == "CAMERA":
                placed = self.place_camera(obj.name, world, obj, comps, parent)
            if placed is None:   # empties, armatures, faceless curves: keep the hierarchy
                placed = self.place_empty(obj.name, world, comps, parent)
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
                self.place_geometry(name, world, ob, orig, [], parent, instancer=host)
            elif ob.type == "LIGHT" and ob.data.type != "SUN":
                self.place_light(name, world, orig, [], parent)

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
                if same_bytes(tmp, path):
                    os.remove(tmp)      # unchanged: keep the file and its date
                else:
                    os.replace(tmp, path)
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

    def world_hdri(self):
        """The World's environment texture as (project-relative path, strength),
        copied into hdri/. None when the World has none."""
        world = self.scene.world
        if not (self.s.export_world and world and world.use_nodes and world.node_tree):
            return None
        env = next((n for n in world.node_tree.nodes
                    if n.type == "TEX_ENVIRONMENT" and n.image), None)
        if env is None:
            return None
        strength = 1.0
        bg = next((n for n in world.node_tree.nodes if n.type == "BACKGROUND"), None)
        if bg is not None:
            strength = bg.inputs["Strength"].default_value
        img = env.image
        src = bpy.path.abspath(img.filepath, library=img.library) if img.filepath else ""
        name = safe_name(os.path.basename(src) if src else img.name)
        if "." not in name:
            name += ".hdr" if img.file_format == "HDR" else ".exr"
        d = os.path.join(self.project, "hdri")
        os.makedirs(d, exist_ok=True)
        dst = os.path.join(d, name)
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
        ensure_meta(dst, "Texture")
        return f"hdri/{name}", strength

    def sun_entity(self):
        sun = self.sun
        if sun is not None:
            fz = sun.fitzel
            color = sun.data.color
            intensity = fz.light_intensity if fz.light_override else convert.light_intensity(sun.data)
        else:
            color, intensity = (1.0, 0.97, 0.9), 1.0
        return {"type": T_SUN, "name": sun.name if sun else "Sun", "id": 0, "parent": -1,
                "active": True, "center": [0.0, 0.0, 0.0], "half": [1.0, 1.0, 1.0],
                "rotation": [0.0, 0.0, 0.0],
                "components": [{"type": "sun", "color": convert.vec3(color),
                                "intensity": round(intensity, 4)}]}

    def settings(self, scene_path):
        s = {}
        if self.s.keep_settings and os.path.exists(scene_path):
            try:
                with open(scene_path, "r", encoding="utf-8") as f:
                    old = json.load(f).get("settings", {})
                if isinstance(old, dict):
                    s = old
            except (OSError, ValueError):
                pass
        if self.sun is not None:
            hour, dec = convert.sun_clock(self.sun.matrix_world)
        else:
            hour, dec = self.s.time_of_day, -10.4
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
        hdri = self.world_hdri()
        if hdri:
            s.update({"hdri": hdri[0], "ibl": True, "iblSkybox": True,
                      "iblIntensity": round(hdri[1], 4)})
        elif self.s.export_world and "hdri" in s:
            s.update({"hdri": "", "ibl": False, "iblSkybox": False})
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

    def run(self):
        os.makedirs(self.models_dir, exist_ok=True)
        marker = os.path.join(self.models_dir, MARKER)
        if not os.listdir(self.models_dir) or os.path.exists(marker):
            write_text(marker, "Written by the Fitzel Blender exporter; its .glb files "
                               "are replaced on every export.\n")
        try:
            self.walk()
            self.write_models()
        finally:
            for m in self.models.values():   # an export that failed half way
                if m is not None and m.mesh is not None:
                    bpy.data.meshes.remove(m.mesh)
                    m.mesh = None
        self.drop_stale_models()
        self.write_scripts()

        if self.s.start_mode == "FPS" and not self.player_start and self.scene.camera is None:
            self.warn("No Player Start and no scene camera: the player starts at the origin")
        scene_path = os.path.join(self.project, self.scene_name + ".fitzel")
        doc = {"version": 3,
               "app": f"Blender {bpy.app.version_string} / fitzel_exporter",
               "entities": [self.sun_entity()] + self.entities,
               "settings": self.settings(scene_path)}
        write_text(scene_path, json.dumps(doc, indent=2) + "\n")
        self.write_game_json()
        n_models = sum(1 for m in self.models.values() if m is not None)
        return scene_path, len(self.entities) + 1, n_models
