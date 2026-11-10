# Fitzel -> Blender: the lighting as the Fitzel editor left the scene.
#
# The other half of export.py. Every entity Blender wrote carries the id its
# object keeps (Object.fitzel.entity_id), so a lamp moved, recoloured, dimmed
# or retyped in the Fitzel editor is found again and changed here; a lamp made
# in Fitzel becomes a Blender light (and from then on Blender's, like the
# rest); one deleted there is deleted here. The sun, the time of day, the
# exposure and the World's brightness come back the same way.
#
# What counts as "changed in Fitzel" is a value that differs from what an
# export would write now -- so pulling a scene nobody touched changes nothing,
# and a value that went out and came back unchanged is left exactly as it was.

import json
import math
import os
from types import SimpleNamespace

import bpy
from mathutils import Matrix, Vector

from . import convert, export

COLLECTION = "Fitzel Lights"   # where lights made in Fitzel arrive


def _differs(a, b, rel=2e-3, tiny=1e-4):
    return abs(a - b) > max(tiny, rel * max(abs(a), abs(b)))


def _vdiffers(a, b, tol=1e-3):
    return any(abs(x - y) > tol for x, y in zip(a, b))


def _mdiffers(a, b, tol=1e-3):
    return any(abs(a[i][j] - b[i][j]) > tol for i in range(3) for j in range(3))


def _light_comp(e):
    for c in e.get("components", []):
        if isinstance(c, dict) and c.get("type") == "light":
            return c
    return None


class Pull:
    def __init__(self, scene, view_layer, report=None):
        self.scene = scene
        self.view_layer = view_layer
        self.s = scene.fitzel
        self.mode = self.s.lighting
        self.report = report or (lambda level, msg: print(f"[Fitzel] {msg}"))
        self.project, self.scene_name = export.project_paths(self.s, scene)
        self.path = export.scene_file(self.project, self.scene_name)
        doc = export.read_json(self.path)
        if not isinstance(doc, dict):
            raise export.ExportError(f"{self.scene_name}.fitzel is not in the project yet "
                                     "-- export first")
        self.doc = doc
        self.entities = [e for e in doc.get("entities", [])
                         if isinstance(e, dict) and isinstance(e.get("id"), int)]
        self.by_id = {e["id"]: e for e in self.entities}
        self.world = export.scene_world(self.entities)
        self.settings = doc.get("settings", {}) if isinstance(doc.get("settings"), dict) else {}
        models_dir = os.path.join(self.project, "models", self.scene_name)
        self.owned, _, _ = export.read_owned(models_dir)
        self.changed, self.added, self.removed = [], [], []
        self.other = []          # sun, exposure, World

    # --- one lamp ---------------------------------------------------------------

    def set_world(self, obj, center, rot, kind, turn):
        """Put the object where Fitzel has it, its scale untouched."""
        loc = convert.from_point(center)
        if turn:
            q = convert.from_rotation(rot, kind).to_quaternion()
        else:
            q = obj.matrix_world.to_quaternion()
        obj.matrix_world = Matrix.LocRotScale(loc, q, obj.matrix_world.to_scale())

    def own_data(self, obj):
        """The light data to change: the object's own (a shared lamp is split off
        rather than changing every object that uses it)."""
        if obj.data.users > 1:
            obj.data = obj.data.copy()
        return obj.data

    def apply_light(self, obj, e, lc_file):
        """Bring one lamp in line with its entity. True if anything changed."""
        fz = obj.fitzel
        world = obj.matrix_world.copy()
        lc_now, kind = export.light_values(obj, world, self.scene)
        c_file, r_file = self.world[e["id"]]
        changed = False

        want_spot = lc_file.get("lightType", 0) == 1
        if want_spot != (lc_now["lightType"] == 1):
            self.own_data(obj).type = "SPOT" if want_spot else "POINT"
            changed = True
            lc_now, kind = export.light_values(obj, world, self.scene)

        # Where it is, and for a cone which way it points.
        c_now = convert.point(world.translation)
        r_now = convert.rotation(world, kind)
        turn = want_spot and _mdiffers(r_now, r_file)
        if _vdiffers(c_now, c_file) or turn:
            self.set_world(obj, c_file, r_file, kind, turn)
            world = obj.matrix_world.copy()
            changed = True

        light = obj.data
        if _vdiffers(lc_now["color"], lc_file.get("color", lc_now["color"]), 1e-4):
            self.own_data(obj).color = [max(0.0, x) for x in lc_file["color"]]
            changed = True

        # How bright and how far: watts for a physical lamp, Fitzel's numbers
        # for one set by hand.
        falloff = lc_file.get("falloff", 0)
        intensity = lc_file.get("intensity", lc_now["intensity"])
        rng = lc_file.get("range", lc_now["range"])
        if falloff == 0 and fz.light_falloff != "CLASSIC":
            fz.light_falloff = "CLASSIC"
            changed = True
        elif falloff == 1 and fz.light_falloff == "CLASSIC":
            fz.light_falloff = "PHYSICAL"
            changed = True
        if fz.light_falloff == "CLASSIC" or fz.light_override:
            if _differs(fz.light_intensity, intensity) or _differs(fz.light_range, rng):
                fz.light_intensity, fz.light_range = intensity, rng
                changed = True
        else:
            now_i = convert.lamp_intensity(light, self.mode, world)
            if _differs(now_i, intensity):
                self.own_data(obj).energy = convert.lamp_energy(intensity, obj.data,
                                                                self.mode, world)
                changed = True
            light = obj.data
            if _differs(convert.lamp_range(light, export.light_threshold(self.scene), world),
                        rng):
                light = self.own_data(obj)
                light.use_custom_distance = True
                light.cutoff_distance = rng
                changed = True

        if light.type == "SPOT":
            angle, blend = convert.spot_cone(light)
            fa = lc_file.get("spotAngle", angle)
            fb = lc_file.get("spotBlend", blend)
            if _differs(angle, fa) or _differs(blend, fb, tiny=1e-3):
                size, b = convert.spot_from_fitzel(fa, fb)
                light = self.own_data(obj)
                light.spot_size, light.spot_blend = size, b
                changed = True
        elif light.type == "AREA" and hasattr(light, "spread"):
            angle, _ = convert.area_cone(light)
            fa = lc_file.get("spotAngle", angle)
            if _differs(angle, fa) and not (angle >= 89.0 and fa >= 89.0):
                self.own_data(obj).spread = math.radians(min(fa, 90.0) * 2.0)
                changed = True

        shadows = bool(lc_file.get("castShadows", lc_now["castShadows"]))
        if not want_spot and shadows != lc_now["castShadows"]:
            if fz.shadow_mode == "BLENDER":
                self.own_data(obj).use_shadow = shadows
            else:
                fz.shadow_mode = "ON" if shadows else "OFF"
            changed = True
        baked = bool(lc_file.get("bakedOnly", False))
        if not want_spot and baked != fz.light_baked:
            fz.light_baked = baked
            changed = True

        active = bool(e.get("active", True))
        if active == obj.hide_render:
            obj.hide_render = not active
            changed = True
        name = e.get("name")
        if isinstance(name, str) and name and name != obj.name:
            obj.name = name
            changed = True
        if self.adopt_components(obj, e):
            changed = True
        return changed

    def adopt_components(self, obj, e):
        """Components Fitzel put on the entity that Blender would not write (a
        script, a sound, ...) go into the object's Extra Components, so the next
        export keeps them."""
        fz = obj.fitzel
        mine = {item.text.name if item.text else os.path.basename(item.filepath)
                for item in fz.scripts}
        extra = []
        if fz.extra:
            try:
                data = json.loads(fz.extra.as_string())
                extra = data if isinstance(data, list) else [data]
            except ValueError:
                return False
        new = []
        for c in e.get("components", []):
            if not isinstance(c, dict) or c.get("type") == "light":
                continue
            if c.get("type") == "script" and c.get("file") in mine:
                continue
            if c in extra:
                continue
            new.append(c)
        if not new:
            return False
        if fz.extra is None:
            fz.extra = bpy.data.texts.new(f"{obj.name}.fitzel.json")
        fz.extra.from_string(json.dumps(extra + new, indent=2) + "\n")
        return True

    def new_light(self, e, lc, placed):
        """A lamp made in Fitzel, as a Blender light -- Blender's from now on."""
        name = e.get("name") or "Light"
        data = bpy.data.lights.new(name, "SPOT" if lc.get("lightType", 0) == 1 else "POINT")
        obj = bpy.data.objects.new(name, data)
        coll = bpy.data.collections.get(COLLECTION)
        if coll is None:
            coll = bpy.data.collections.new(COLLECTION)
        if coll.name not in self.scene.collection.children:
            self.scene.collection.children.link(coll)
        coll.objects.link(obj)
        parent = placed.get(e.get("parent", -1))
        if parent is not None:
            obj.parent = parent
        obj.fitzel.entity_id = e["id"]
        data.use_shadow = bool(lc.get("castShadows", False))
        if lc.get("falloff", 0) == 0:
            # Fitzel's own fall-off: kept as it is, and given a Blender power
            # that lights its middle distance about as brightly.
            obj.fitzel.light_falloff = "CLASSIC"
            obj.fitzel.light_intensity = lc.get("intensity", 8.0)
            obj.fitzel.light_range = lc.get("range", 12.0)
            d = obj.fitzel.light_range / 3.0
            data.energy = convert.lamp_energy(obj.fitzel.light_intensity * (2.0 / 3.0) ** 2
                                              * d * d, data, self.mode)
            data.use_custom_distance = True
            data.cutoff_distance = obj.fitzel.light_range
        c, r = self.world[e["id"]]
        obj.matrix_world = Matrix.LocRotScale(
            convert.from_point(c),
            convert.from_rotation(r, "SPOT" if data.type == "SPOT" else "OBJECT").to_quaternion(),
            Vector((1.0, 1.0, 1.0)))
        self.apply_light(obj, e, lc)   # watts, distance, cone, name from the entity
        return obj

    # --- the sun and the picture ------------------------------------------------

    def pull_sun(self, objs):
        sun_e = next((e for e in self.entities if e.get("type") == export.T_SUN), None)
        comp = next((c for c in (sun_e or {}).get("components", [])
                     if isinstance(c, dict) and c.get("type") == "sun"), None)
        if comp is None:
            return
        st = self.settings
        sun = export.find_sun(objs)
        comp_now, hour_now, dec_now = export.sun_values(sun, self.scene)
        hour = st.get("timeOfDay", hour_now)
        dec = st.get("sunDeclination", dec_now)
        lat = st.get("sunLatitude", 0.0)
        clock = _differs(hour, hour_now, tiny=1e-3) or _differs(dec, dec_now, tiny=1e-3) \
            or abs(lat) > 1e-4
        light = _differs(comp.get("intensity", 1.0), comp_now["intensity"]) or \
            _vdiffers(comp.get("color", comp_now["color"]), comp_now["color"], 1e-4)
        if not (clock or light):
            return
        if sun is None and not light and abs(dec - dec_now) < 1e-3 and abs(lat) < 1e-4:
            self.s.time_of_day = hour % 24.0   # Fitzel's own sun, only a new hour
            self.other.append("time of day")
            return
        if sun is None:   # the sun was Fitzel's: now it is a lamp
            data = bpy.data.lights.new("Sun", "SUN")
            sun = bpy.data.objects.new("Sun", data)
            coll = bpy.data.collections.get(COLLECTION) or bpy.data.collections.new(COLLECTION)
            if coll.name not in self.scene.collection.children:
                self.scene.collection.children.link(coll)
            coll.objects.link(sun)
            clock = light = True
        to_sun = convert.fitzel_sun_dir(hour, lat, dec)
        if clock:
            q = convert.from_point(to_sun).to_track_quat("Z", "Y")
            sun.matrix_world = Matrix.LocRotScale(sun.matrix_world.translation, q,
                                                  sun.matrix_world.to_scale())
        if light:
            if sun.data.users > 1:
                sun.data = sun.data.copy()
            color, strength = convert.sun_from_fitzel(comp.get("color", [1, 1, 1]),
                                                      comp.get("intensity", 1.0),
                                                      to_sun.y, sun.data, self.mode)
            peak = max(max(color), 1e-6)
            if peak > 1.0:   # a colour brighter than white is strength, in Blender
                color, strength = color / peak, strength * peak
            sun.data.color = color
            if sun.fitzel.light_override:
                sun.fitzel.light_intensity = comp.get("intensity", 1.0)
            else:
                sun.data.energy = strength
        self.other.append("sun")

    def pull_picture(self):
        if self.mode != "MATCH":
            return
        st, vs = self.settings, self.scene.view_settings
        curve = st.get("tonemapCurve")
        if isinstance(curve, int) and curve != convert.tonemap_curve(vs.view_transform):
            name = convert.TONEMAP_BACK.get(curve)
            try:
                if name:
                    vs.view_transform = name
                    self.other.append("view transform")
            except TypeError:
                pass
        exposure = st.get("exposure")
        if isinstance(exposure, (int, float)) and exposure > 0 and \
                _differs(convert.fitzel_exposure(vs), exposure):
            vs.exposure = convert.blender_exposure(
                exposure, convert.tonemap_curve(vs.view_transform))
            self.other.append("exposure")
        # The World's brightness: what Fitzel's IBL intensity says over what the
        # export would write, put on the Background's strength.
        ibl = st.get("iblIntensity")
        world = self.scene.world
        if not (isinstance(ibl, (int, float)) and st.get("ibl") and world and world.node_tree):
            return
        ex = export.Exporter(SimpleNamespace(scene=self.scene, view_layer=self.view_layer))
        kind = ex.world_kind()
        now = kind[2] if kind and kind[0] in {"IMAGE", "COLOR"} else 1.0
        if not _differs(now, ibl):
            return
        bg = next((n for n in world.node_tree.nodes if n.type == "BACKGROUND"
                   and not n.inputs["Strength"].is_linked), None)
        if bg is None:
            self.report({"WARNING"}, "The World's brightness changed in Fitzel, but its "
                                     "Background strength is driven by nodes")
            return
        bg.inputs["Strength"].default_value *= ibl / max(now, 1e-6)
        self.other.append("World brightness")

    # --- all of it ----------------------------------------------------------------

    def run(self):
        """Pull the lighting. Returns a one-line summary."""
        if str(self.doc.get("app", "")).startswith("Blender"):
            return "Nothing changed in Fitzel since the last export"
        placed = {}
        for o in self.scene.objects:
            if o.fitzel.entity_id > 0:
                placed.setdefault(o.fitzel.entity_id, o)
        for eid, obj in list(placed.items()):
            if obj.type != "LIGHT" or obj.data.type == "SUN":
                continue
            e = self.by_id.get(eid)
            if e is None:
                if eid in self.owned:   # Blender's, and deleted in Fitzel
                    self.removed.append(obj.name)
                    bpy.data.objects.remove(obj)
                continue
            lc = _light_comp(e)
            if lc is not None and self.apply_light(obj, e, lc):
                self.changed.append(obj.name)
        for e in self.entities:   # made in Fitzel
            lc = _light_comp(e)
            if lc is None or e.get("type") != export.T_LIGHT or e["id"] in self.owned \
                    or e["id"] in placed:
                continue
            placed[e["id"]] = self.new_light(e, lc, placed)
            self.added.append(e.get("name", "Light"))
        # Listed now, not before: a lamp deleted above is gone from it.
        self.pull_sun(export.exported_objects(self.scene, self.view_layer))
        self.pull_picture()
        parts = []
        if self.changed:
            parts.append(f"{len(self.changed)} changed")
        if self.added:
            parts.append(f"{len(self.added)} new")
        if self.removed:
            parts.append(f"{len(self.removed)} deleted")
        if self.other:
            parts.append(", ".join(dict.fromkeys(self.other)))
        return ("From Fitzel: lights " + "; ".join(parts)) if parts \
            else "The lighting in Fitzel is the same as here"
