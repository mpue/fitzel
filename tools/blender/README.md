# Fitzel Exporter for Blender

A Blender extension (4.2 and newer) that writes the open Blender scene into a
Fitzel project and, on request, starts it in the engine. One way only: Blender
is where the scene is built, Fitzel plays it.

## Install

```sh
blender --command extension build --source-dir tools/blender/fitzel_exporter --output-dir .
```

Then in Blender: *Edit > Preferences > Get Extensions > ⌄ > Install from Disk*
and pick `fitzel_exporter-1.0.0.zip`. In the add-on's preferences, point
*Fitzel Executable* at `build/linux/bin/sandbox` (or `sandbox.exe`); without
it the add-on tries `FITZEL_EXE`, `/opt/fitzel/sandbox` and `fitzel` on the PATH.

## Use

*Properties > Scene > Fitzel* (also in the 3D view's sidebar, tab *Fitzel*):
choose a project folder, then **Export** or **Export & Play**. Play runs

    sandbox --play <project> --scene <scene> --fullscreen 0

and the game's console output lands in `fitzel-blender-play.log` in the temp
folder. Esc quits the game.

Headless, for a build script:

```sh
blender -b level.blend --python-expr "import bpy; s = bpy.context.scene.fitzel; s.project_dir = '/path/to/MyGame'; bpy.ops.fitzel.export_scene()"
```

## What becomes what

| Blender | Fitzel |
| --- | --- |
| Mesh, curve, text, metaball (modifiers applied) | Model entity; each distinct mesh is one `.glb` in `models/<scene>/` |
| Linked duplicates, collection instances, geometry-node instances | one shared model, one entity per copy (under the instancing object) |
| Point / area light | point light (area lights have no Fitzel equivalent) |
| Spot light | spot light, cone and blend taken over |
| Sun light | the scene's sun: time of day and declination are solved so the sky stands where the lamp shines from |
| Camera | camera; the scene camera is the main camera when the scene starts as *Main Camera* |
| Empty, armature | empty (keeps the hierarchy) |
| World with an Environment Texture | HDRI lighting and sky (`hdri/`) |
| Materials (Principled BSDF) | carried inside the `.glb` as glTF PBR materials |

Per object (*Properties > Object > Fitzel*): **Role** (*Player Start* — the
player looks along the object's +Y; *Do Not Export*), **Collision** (static,
dynamic with mass, none), **Lua scripts** (a text block from the .blend or a
`.lua` file, copied into `scripts/`), Fitzel light intensity and range instead
of the watt conversion, and **Extra Components**: a text block with component
JSON exactly as the Fitzel editor saves it, for anything the panel does not
cover (doors, audio sources, triggers, ...).

## Good to know

- Mirrored or sheared objects get their rotation and scale baked into their own
  model file; Fitzel cannot store such a transform.
- Re-exporting keeps every GUID and rewrites a `.glb` only when its bytes
  changed. Models of deleted objects are removed from `models/<scene>/`.
- The entities are Blender's on every export — anything added to the scene in
  the Fitzel editor is replaced. The scene *settings* (fog, post effects, …)
  tuned there are kept, unless *Keep Engine Settings* is off.
- Animation (armatures, actions, shape-key animation) is not exported: meshes go
  out in the pose they stand in.
- Blender's watts have no exact Fitzel counterpart; the conversion lands a
  1000 W room light at Fitzel's default (intensity 8, range ~12 m).
