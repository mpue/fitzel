"""Blender side of tools/citizens-import.py: one walking person per job, as a .glb.

    blender -b --python tools/citizens-to-glb.py -- jobs.json

jobs.json: [{"model": FBX, "anim": FBX, "body": PNG, "normal": PNG|null, "hair": TGA|null,
             "hairNormal": PNG|null, "out": GLB, "tex": 1024}, ...]

The figures of a Unity crowd pack (3ds Max biped, "Bip01 ..." bones) are skinned FBX without
motion; the walk is a separate FBX of the same skeleton. Unity plays such a clip as a GENERIC
rig: every bone takes the clip's LOCAL rotation outright. Blender's own route -- hand the walk's
action to the figure -- would replay the walk relative to the figure's rest pose, and the figures'
bind poses are up to nine degrees off the clip's (thighs, clavicles): legs turned out, shoulders
up. So the walk is transferred by hand, frame by frame: each bone the clip's local rotation, on
the figure's own bone lengths; the pelvis the clip's place and turn in the world -- the biped
carries the body's bob and sway on its root node, which Blender imports as the armature object,
and without them the hips rock under a still body like a toddler's -- scaled to the figure's legs.
Then the figure is brought to metres (the pack mixes centimetre and metre files), its body and
hair dressed (the hair cut out at half alpha, which glTF writes as MASK), and only the walk is
exported -- in place, as the pack's walks are, one stride pair of 41 frames at 30 fps.
"""
import json
import sys

import bpy
from mathutils import Matrix, Vector


def arm_of(objs):
    return next(o for o in objs if o.type == 'ARMATURE')


def import_fbx(path):
    before = set(bpy.data.objects)
    bpy.ops.import_scene.fbx(filepath=path)
    return [o for o in bpy.data.objects if o not in before]


def local_of(pb):
    """A pose bone's matrix in its parent's pose space (armature space for a root)."""
    return pb.matrix if pb.parent is None else pb.parent.matrix.inverted() @ pb.matrix


def rest_local(b):
    return b.matrix_local if b.parent is None else b.parent.matrix_local.inverted() @ b.matrix_local


def fcurves_of(action):
    """Every F-curve of an action: the layered (slotted) actions of Blender 4.4+ keep them in
    channel bags, the older ones on the action."""
    if hasattr(action, "layers") and len(action.layers):
        out = []
        for layer in action.layers:
            for strip in layer.strips:
                for bag in strip.channelbags:
                    out.extend(bag.fcurves)
        return out
    return list(action.fcurves)


def image(path, size, fmt):
    img = bpy.data.images.load(path)
    if max(img.size) > size:
        img.scale(size, size)
    img.file_format = fmt
    return img


def dress(mesh, job):
    """Body (colour + normal) and, where the figure has hair, the hair cut out at half alpha."""
    for slot in mesh.material_slots:
        hair = "hair" in (slot.material.name if slot.material else "").lower()
        if hair and not job.get("hair"):
            continue
        mat = bpy.data.materials.new(("hair_" if hair else "body_") + mesh.name)
        mat.use_nodes = True
        nt = mat.node_tree
        bsdf = next(n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED')
        bsdf.inputs["Roughness"].default_value = 0.85
        tex = nt.nodes.new("ShaderNodeTexImage")
        tex.image = image(job["hair"] if hair else job["body"], job["tex"], 'PNG' if hair else 'JPEG')
        nt.links.new(tex.outputs["Color"], bsdf.inputs["Base Color"])
        nrm_path = job.get("hairNormal") if hair else job.get("normal")
        if nrm_path:
            nt_img = nt.nodes.new("ShaderNodeTexImage")
            nt_img.image = image(nrm_path, job["tex"], 'JPEG')
            nt_img.image.colorspace_settings.name = 'Non-Color'
            nmap = nt.nodes.new("ShaderNodeNormalMap")
            nt.links.new(nt_img.outputs["Color"], nmap.inputs["Color"])
            nt.links.new(nmap.outputs["Normal"], bsdf.inputs["Normal"])
        if hair:
            cut = nt.nodes.new("ShaderNodeMath")
            cut.operation = 'GREATER_THAN'
            cut.inputs[1].default_value = 0.5
            nt.links.new(tex.outputs["Alpha"], cut.inputs[0])
            nt.links.new(cut.outputs[0], bsdf.inputs["Alpha"])
        slot.material = mat


def convert(job):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.fps = 30

    figure = import_fbx(job["model"])
    fig = arm_of(figure)
    mesh = next(o for o in figure if o.type == 'MESH')
    for o in figure:
        if o.type not in ('ARMATURE', 'MESH'):
            bpy.data.objects.remove(o)
    walk = import_fbx(job["anim"])
    clip = arm_of(walk)
    act = clip.animation_data.action
    f0, f1 = int(act.frame_range[0]), int(act.frame_range[1])

    # The biped's body moves on its root NODE, Bip01 -- the bob, the sway, the turn of the hips
    # into each step -- which Blender makes the armature OBJECT, its pelvis bone all but still
    # under it. So the pelvis is carried over in the world (both files face -Y), the object's
    # motion included, its height scaled by the two legs' lengths (the files' units differ).
    root = next(b for b in fig.data.bones if b.parent is None)
    def leg(arm):
        sc_ = arm.matrix_world.to_scale().x
        return sum(arm.data.bones[n].length for n in ("Bip01 L Thigh", "Bip01 L Calf")) * sc_
    ratio = leg(fig) / max(leg(clip), 1e-9)
    W = fig.matrix_world.copy()
    W_inv, W_rot = W.inverted(), W.to_quaternion()

    if fig.animation_data:
        fig.animation_data.action = None
    order = []                       # parents before children
    def walk_tree(b):
        order.append(b)
        for c in b.children:
            walk_tree(c)
    walk_tree(root)
    for pb in fig.pose.bones:
        pb.rotation_mode = 'QUATERNION'

    for f in range(f0, f1 + 1):
        sc.frame_set(f)
        want = {}
        for b in order:
            src = clip.pose.bones.get(b.name)
            rest = rest_local(b)
            if src is None:
                local = rest
            elif b.parent is None:
                Pw = clip.matrix_world @ src.matrix                  # the clip's pelvis, in the world
                pos = W_inv @ (Pw.translation * ratio)               # ...at the figure's size, in its armature
                rot = W_rot.inverted() @ Pw.to_quaternion()
                local = Matrix.Translation(pos) @ rot.to_matrix().to_4x4()
            else:
                local = Matrix.Translation(rest.translation) @ local_of(src).to_quaternion().to_matrix().to_4x4()
            want[b.name] = local if b.parent is None else want[b.parent.name] @ local
        for b in order:
            pb = fig.pose.bones[b.name]
            parent = want[b.parent.name] if b.parent else Matrix.Identity(4)
            pb.matrix_basis = rest_local(b).inverted() @ (parent.inverted() @ want[b.name])
            pb.keyframe_insert("location", frame=f)
            pb.keyframe_insert("rotation_quaternion", frame=f)
    for o in walk:
        bpy.data.objects.remove(o)
    for a in list(bpy.data.actions):
        if a != (fig.animation_data.action if fig.animation_data else None):
            bpy.data.actions.remove(a)

    # Metres: whatever scale the file's objects carry goes into the data -- and into the walk's
    # positions, which are in the armature's own units.
    def height():
        zs = [(mesh.matrix_world @ Vector(c)).z for c in mesh.bound_box]
        return max(zs) - min(zs)
    if height() > 10.0:                       # a centimetre file without its 0.01 -- its
        fig.scale = fig.scale * 0.01          # place is centimetres too
        fig.location = fig.location * 0.01
        bpy.context.view_layer.update()
    s = fig.matrix_world.to_scale().x         # the armature's units, in metres
    bpy.ops.object.select_all(action='DESELECT')
    fig.select_set(True)
    mesh.select_set(True)
    bpy.context.view_layer.objects.active = fig
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)
    for fc in fcurves_of(fig.animation_data.action):
        if fc.data_path.endswith(".location"):
            for kp in fc.keyframe_points:
                kp.co.y *= s
                kp.handle_left.y *= s
                kp.handle_right.y *= s
    dress(mesh, job)
    fig.animation_data.action.name = "walk"
    sc.frame_start, sc.frame_end = f0, f1

    bpy.ops.export_scene.gltf(filepath=job["out"], export_format='GLB', export_animations=True,
                              export_animation_mode='ACTIONS', export_image_format='AUTO',
                              export_yup=True, export_apply=False)
    print(f"citizen {job['out']} {height():.2f} m", flush=True)


def main():
    jobs = json.loads(open(sys.argv[sys.argv.index("--") + 1]).read())
    for job in jobs:
        try:
            convert(job)
        except Exception as e:   # one bad figure is not the crowd
            print(f"citizen FAILED {job['out']}: {e}", flush=True)


main()
