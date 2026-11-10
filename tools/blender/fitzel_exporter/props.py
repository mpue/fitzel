import bpy
from bpy.props import (BoolProperty, CollectionProperty, EnumProperty, FloatProperty,
                       IntProperty, PointerProperty, StringProperty)


class FitzelScript(bpy.types.PropertyGroup):
    """One Lua script on an object: a text block in this .blend, or a file."""
    text: PointerProperty(
        name="Text", type=bpy.types.Text,
        description="Lua source kept in this .blend (written to the project's scripts/ folder)")
    filepath: StringProperty(
        name="File", subtype="FILE_PATH",
        description="A .lua file on disk, copied into the project's scripts/ folder")


class FitzelObjectSettings(bpy.types.PropertyGroup):
    role: EnumProperty(
        name="Role",
        items=[
            ("AUTO", "Automatic", "What the object is: mesh, light, camera or empty"),
            ("PLAYER_START", "Player Start",
             "Where the walking player starts; it looks along the object's +Y "
             "(a camera: along its view)"),
            ("IGNORE", "Do Not Export", "Leave this object (and its children) out"),
        ],
        default="AUTO")
    physics: EnumProperty(
        name="Collision",
        items=[
            ("AUTO", "Scene Default", "Use the scene's collision setting"),
            ("NONE", "None", "Nothing collides with it"),
            ("STATIC", "Static", "Solid and immovable: its triangles collide"),
            ("DYNAMIC", "Dynamic", "A rigid body that falls and is pushed (convex hull)"),
        ],
        default="AUTO")
    mass: FloatProperty(name="Mass", default=1.0, min=0.01, max=1000.0, unit="MASS")
    scripts: CollectionProperty(type=FitzelScript)
    script_index: IntProperty()
    extra: PointerProperty(
        name="Extra Components", type=bpy.types.Text,
        description="A text block holding a JSON component (or a list of them), "
                    "added to the entity as it is -- e.g. "
                    "{\"type\": \"audio_source\", ...} as the Fitzel editor saves it")
    entity_id: IntProperty(
        name="Fitzel Entity", default=0, min=0,
        description="The id of this object's entity in the Fitzel scene (set by the first "
                    "export; it is how changes made in Fitzel find their way back)")
    light_falloff: EnumProperty(
        name="Falloff",
        items=[
            ("PHYSICAL", "Physical", "Inverse square, as in Blender -- the light is converted "
                                     "from watts exactly"),
            ("CLASSIC", "Classic", "Fitzel's own (1 - d/range)^2: full strength at the bulb, "
                                   "gone at the range; set intensity and range by hand"),
        ],
        default="PHYSICAL")
    light_override: BoolProperty(
        name="Fitzel Light Values", default=False,
        description="Set the light's Fitzel intensity and range by hand instead of "
                    "converting them from watts")
    light_intensity: FloatProperty(name="Intensity", default=8.0, min=0.0, soft_max=30.0)
    light_range: FloatProperty(name="Range", default=12.0, min=0.5, soft_max=60.0, unit="LENGTH")
    light_shadows: BoolProperty(name="Shadows (old)", default=False)   # read by 1.0 files only
    shadow_mode: EnumProperty(
        name="Shadows",
        items=[
            ("BLENDER", "As in Blender", "The light's own Shadow setting (point lights; "
                                         "Fitzel's spots cast none)"),
            ("ON", "On", "Cast shadows in Fitzel (it shadows the 8 nearest at most)"),
            ("OFF", "Off", "No shadows in Fitzel"),
        ],
        default="BLENDER")
    light_baked: BoolProperty(
        name="Baked Only", default=False,
        description="Point lights: only in Fitzel's baked indirect light (Bake Indirect "
                    "Light), not drawn live -- free fill light beyond the 16 live ones")
    camera_main: BoolProperty(
        name="Main Camera", default=False,
        description="Watch through this camera when the game starts, even as the walking "
                    "player (the scene camera is the main camera when starting as Main Camera)")


class FitzelSceneSettings(bpy.types.PropertyGroup):
    lighting: EnumProperty(
        name="Lighting",
        items=[
            ("MATCH", "Like Blender", "Fitzel lights the scene as Blender renders it: lamps in "
                                      "physical units, the World as sky and ambient light, "
                                      "Blender's exposure and view transform, no Fitzel grade"),
            ("FITZEL", "Fitzel's Look", "Fitzel's own sky, sun, auto exposure and grade; the "
                                        "lamps keep their proportion to the sun"),
        ],
        default="MATCH")
    world_resolution: EnumProperty(
        name="World Bake",
        items=[("512", "512 x 256", ""), ("1024", "1024 x 512", ""),
               ("2048", "2048 x 1024", ""), ("4096", "4096 x 2048", "")],
        default="2048",
        description="Size of the panorama a World made of nodes is rendered to "
                    "(an image World is copied as it is)")
    live_link: BoolProperty(
        name="Live Link", default=False,
        description="Saving the .blend exports the scene (an open Fitzel editor reloads it); "
                    "saving the scene in Fitzel brings its lighting back here")
    bake_resolution: EnumProperty(
        name="Probes",
        items=[("16", "Coarse (16)", ""), ("24", "Medium (24)", ""),
               ("32", "Fine (32)", ""), ("48", "Very Fine (48)", "")],
        default="24",
        description="Probes along the scene's longest side for Fitzel's baked indirect light")
    status: StringProperty(name="Status", default="")
    project_dir: StringProperty(
        name="Project", subtype="DIR_PATH",
        description="The Fitzel project folder the scene is written into "
                    "(created if it does not exist)")
    scene_name: StringProperty(
        name="Scene", default="",
        description="File name of the scene in the project (empty: this Blender scene's name)")
    start_mode: EnumProperty(
        name="Start As",
        items=[
            ("FPS", "Walking Player", "On foot at the Player Start (or the scene camera)"),
            ("CAMERA", "Main Camera", "Watching through the scene camera"),
            ("GAME", "Project Setting", "Whatever the project's game.json says"),
        ],
        default="FPS")
    collision: EnumProperty(
        name="Collision",
        items=[
            ("STATIC", "Static Meshes", "Every mesh is solid -- the level can be walked"),
            ("NONE", "None", "Only objects set to collide do"),
        ],
        default="STATIC")
    only_visible: BoolProperty(
        name="Visible Only", default=True,
        description="Leave out objects hidden in the viewport")
    clean_world: BoolProperty(
        name="Empty World", default=True,
        description="No terrain, grass, trees, animals or water: only what the Blender scene has")
    water: BoolProperty(name="Water", default=False,
                        description="Keep Fitzel's sea, at the given height")
    water_level: FloatProperty(name="Water Level", default=0.0, unit="LENGTH")
    export_world: BoolProperty(
        name="World as Sky", default=True,
        description="Light the scene with the World and show it as the sky (an image World "
                    "always; with Fitzel's Look only then)")
    time_of_day: FloatProperty(
        name="Time of Day", default=12.0, min=0.0, max=24.0,
        description="Hour of the day when the scene has no Sun lamp")
    time_flows: BoolProperty(name="Time Flows", default=False,
                             description="Let the day run while playing")
    keep_settings: BoolProperty(
        name="Keep Engine Settings", default=True,
        description="Re-exporting keeps the look tuned in the Fitzel editor (fog, post "
                    "effects, ...); only what Blender decides is written over")
    move_speed: FloatProperty(name="Walk Speed", default=4.0, min=0.1, soft_max=30.0)
    run_speed: FloatProperty(name="Run Speed", default=8.0, min=0.0, soft_max=40.0)
    step_height: FloatProperty(name="Step Height", default=0.4, min=0.0, soft_max=2.0,
                               unit="LENGTH")
    fullscreen: BoolProperty(name="Fullscreen", default=False,
                             description="Play fullscreen instead of in a window")


classes = (FitzelScript, FitzelObjectSettings, FitzelSceneSettings)


def register():
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.Scene.fitzel = PointerProperty(type=FitzelSceneSettings)
    bpy.types.Object.fitzel = PointerProperty(type=FitzelObjectSettings)


def unregister():
    del bpy.types.Object.fitzel
    del bpy.types.Scene.fitzel
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
