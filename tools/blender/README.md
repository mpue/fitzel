# Fitzel Exporter for Blender

A Blender extension (4.2 and newer) that writes the open Blender scene into a
Fitzel project, starts it in the engine, and brings the lighting changed in the
Fitzel editor back. Blender is where the scene is built; the lights can be
tuned on either side.

## Install

```sh
blender --command extension build --source-dir tools/blender/fitzel_exporter --output-dir tools/blender
```

Then in Blender: *Edit > Preferences > Get Extensions > ⌄ > Install from Disk*
and pick `fitzel_exporter-1.1.0.zip`. In the add-on's preferences, point
*Fitzel Executable* at `build/linux/bin/sandbox` (or `sandbox.exe`); without
it the add-on tries `FITZEL_EXE`, `/opt/fitzel/sandbox` and `fitzel` on the PATH.
Needs a Fitzel built from this tree or later: the physical light fall-off,
*AgX (Blender)*, absolute HDRI brightness and the editor's reload are engine
features.

## Use

*Properties > Scene > Fitzel* (also in the 3D view's sidebar, tab *Fitzel*):
choose a project folder, then

- **Export** / **Export & Play** -- Play runs
  `sandbox --play <project> --scene <scene> --fullscreen 0`; the game's output
  lands in `fitzel-blender-play.log` in the temp folder. Esc quits.
- **Open in Editor** -- exports and opens the scene in the Fitzel editor. The
  editor reloads the scene by itself whenever an export writes it (keeping its
  view); if the scene has unsaved work there, it asks first.
- **Lighting from Fitzel** -- brings back what was done to the lights in the
  Fitzel editor (after saving there): lamps moved, turned, recoloured, dimmed,
  given another range, cone or type, switched off, made, deleted; the sun and
  the time of day; the exposure and view transform; the World's brightness.
  Undoable. Pulling twice changes nothing.
- **Live Link** -- both at once: saving the .blend exports, saving in Fitzel
  pulls the lighting back, within a second.
- **Bake Indirect Light** -- exports, then lets Fitzel path-trace the bounced
  light into its probe grid (`lightgrids/<scene>.fgrid`). Fitzel opens, bakes
  and closes; the result shows in the panel. Bake again after the geometry or
  the lights changed -- the export warns when the geometry changed since.

Headless, for a build script:

```sh
blender -b level.blend --python-expr "import bpy; s = bpy.context.scene.fitzel; s.project_dir = '/path/to/MyGame'; bpy.ops.fitzel.export_scene()"
```

## Lighting

*Lighting: Like Blender* (the default) makes Fitzel show the picture Blender
renders. Measured against Cycles on a test scene with point, spot and area
lamps, a sun and a coloured World: the surface brightness agrees to 1-3 %, the
whole picture to a median of 4 % (with the indirect light baked, against Cycles
with all its bounces, also 4 %).

- Lamps get Fitzel's physical fall-off (1/d², faded out at the range), and their
  watts are converted exactly: a point or spot of P watts radiates P/4π W/sr,
  an area lamp P/π straight ahead, a sun S W/m²; Fitzel shades without
  Blender's 1/π, so the irradiance is divided by π. The range is the lamp's
  custom distance, else where EEVEE's light threshold drops it.
- Area lamps become one-sided spot lamps as wide as their spread.
- The sun's direction is solved into Fitzel's clock; its colour and strength
  take back the dimming and the sunset tint Fitzel's sky puts on it. A scene
  without a sun lamp has no sun.
- The World -- a colour, an image or any node setup (rendered by Cycles to a
  panorama, again only when it changed) -- is the sky and the ambient light,
  at its own brightness (Fitzel otherwise brings every panorama to one).
- Blender's exposure and view transform: AgX and Filmic become *AgX (Blender)*,
  PBR Neutral and Standard Fitzel's *Neutral*, its built-in lift taken out of
  the exposure. Looks map to contrast and saturation. Fitzel's grade, bloom,
  vignette, haze, weather and auto exposure are switched off.
- Materials: the glTF base colour is written the way Fitzel reads it (as sRGB),
  so a 0.5 grey stays 0.5; glows are set back by the boost Fitzel's import
  gives them.

*Lighting: Fitzel's Look* keeps Fitzel's sky, sun, auto exposure and grade;
the lamps keep their proportion to the sun (a clear-day sun of 3 W/m² is
Fitzel's intensity 1). Switching back from *Like Blender* restores Fitzel's look.

Per lamp (*Properties > Object > Fitzel*): **Falloff** *Physical* or *Classic*
(Fitzel's own (1 - d/r)², numbers by hand), **Fitzel Light Values** to set the
intensity and range by hand, **Shadows** (as in Blender, on, off), **Baked
Only** (fill light that only the bake sees). The panel shows the values Fitzel
gets.

## What becomes what

| Blender | Fitzel |
| --- | --- |
| Mesh, curve, text, metaball (modifiers applied) | Model entity; each distinct mesh is one `.glb` in `models/<scene>/` |
| Linked duplicates, collection instances, geometry-node instances | one shared model, one entity per copy (under the instancing object) |
| Point light | point light |
| Spot light | spot light, cone and blend taken over |
| Area light | one-sided spot light |
| Sun light | the scene's sun (direction, colour, strength) |
| Camera | camera; the scene camera is the main camera when the scene starts as *Main Camera* |
| Empty, armature | empty (keeps the hierarchy) |
| World | sky and ambient light (`hdri/`) |
| Materials (Principled BSDF) | carried inside the `.glb` as glTF PBR materials |
| *Disable in Renders* | the entity is there, switched off |

Per object: **Role** (*Player Start* -- the player looks along the object's +Y;
*Do Not Export*), **Collision** (static, dynamic with mass, none), **Lua
scripts** (a text block from the .blend or a `.lua` file, copied into
`scripts/`), and **Extra Components**: a text block with component JSON exactly
as the Fitzel editor saves it, for anything the panel does not cover (doors,
audio sources, triggers, ...).

## Good to know

- Every object keeps the id of its entity (shown in its Fitzel panel), and
  `models/<scene>/.fitzel-blender` lists the entities Blender wrote. An entity
  made in the Fitzel editor is kept on every export -- a lamp made there
  becomes a Blender light when the lighting is pulled, with whatever else it
  carried (a script, a sound) in its Extra Components.
- What Blender exported is Blender's: changes made to it in Fitzel other than
  the lighting are replaced by the next export. The scene *settings* (fog,
  post effects, ...) tuned there are kept, unless *Keep Engine Settings* is off
  -- except the ones *Like Blender* decides.
- Fitzel draws 8 spot lamps and shadows the 8 nearest point lamps; its spots
  cast no shadows. The export warns when a scene asks for more.
- Mirrored or sheared objects get their rotation and scale baked into their own
  model file; Fitzel cannot store such a transform.
- Re-exporting keeps every GUID and rewrites a `.glb` only when its bytes
  changed. Models of deleted objects are removed from `models/<scene>/`.
- Animation (armatures, actions, shape-key animation) is not exported: meshes go
  out in the pose they stand in.
