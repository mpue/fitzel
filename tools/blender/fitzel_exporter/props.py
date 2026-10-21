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
    light_override: BoolProperty(
        name="Fitzel Light Values", default=False,
        description="Set the light's Fitzel intensity and range by hand instead of "
                    "converting them from watts")
    light_intensity: FloatProperty(name="Intensity", default=8.0, min=0.0, soft_max=30.0)
    light_range: FloatProperty(name="Range", default=12.0, min=0.5, soft_max=60.0, unit="LENGTH")
    light_shadows: BoolProperty(
        name="Shadows", default=False,
        description="Point lights only: cast shadows (costly -- Fitzel shadows a few at most)")
    camera_main: BoolProperty(
        name="Main Camera", default=False,
        description="Watch through this camera when the game starts, even as the walking "
                    "player (the scene camera is the main camera when starting as Main Camera)")


class FitzelSceneSettings(bpy.types.PropertyGroup):
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
        name="World HDRI", default=True,
        description="Light the scene with the World's environment texture, when it has one")
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
