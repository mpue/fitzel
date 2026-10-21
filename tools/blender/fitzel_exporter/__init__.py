# Fitzel Engine Exporter -- Blender scenes into Fitzel projects, and Play.
#
# Install: Edit > Preferences > Get Extensions > Install from Disk, with the
# zip that `blender --command extension build` makes from this folder (or this
# folder zipped). Then: Scene properties > Fitzel > set the project folder >
# Export & Play.

bl_info = {   # read by Blender before 4.2 only; the manifest is the extension's
    "name": "Fitzel Engine Exporter",
    "author": "mpue",
    "version": (1, 0, 0),
    "blender": (4, 2, 0),
    "location": "Properties > Scene > Fitzel, View3D > Sidebar > Fitzel, File > Export",
    "description": "Export the scene to a Fitzel project and play it in the engine",
    "category": "Import-Export",
}

import os

import bpy
from bpy.props import BoolProperty, StringProperty

from . import export, launch, props


def prefs(context):
    addon = context.preferences.addons.get(__package__)
    return addon.preferences if addon else None


def engine_exe(context):
    p = prefs(context)
    return launch.find_engine(bpy.path.abspath(p.engine_path) if p and p.engine_path else "")


class FitzelPreferences(bpy.types.AddonPreferences):
    bl_idname = __package__

    engine_path: StringProperty(
        name="Fitzel Executable", subtype="FILE_PATH",
        description="The sandbox (editor/player) program. Empty: FITZEL_EXE, "
                    "/opt/fitzel/sandbox, or fitzel/sandbox on the PATH")

    def draw(self, context):
        col = self.layout.column()
        col.prop(self, "engine_path")
        found = launch.find_engine(bpy.path.abspath(self.engine_path) if self.engine_path else "")
        col.label(text=f"Using: {found}" if found else "No Fitzel executable found",
                  icon="CHECKMARK" if found else "ERROR")


class FITZEL_OT_export(bpy.types.Operator):
    """Write the scene into the Fitzel project (and start it)"""
    bl_idname = "fitzel.export_scene"
    bl_label = "Export to Fitzel"
    bl_options = {"REGISTER"}

    play: BoolProperty(name="Play", default=False,
                       description="Start the scene in Fitzel after exporting")

    def execute(self, context):
        if context.object and context.object.mode != "OBJECT":
            bpy.ops.object.mode_set(mode="OBJECT")
        exe = ""
        if self.play:
            exe = engine_exe(context)
            if not exe:
                self.report({"ERROR"}, "No Fitzel executable found -- set it in the "
                                       "add-on preferences")
                return {"CANCELLED"}
        try:
            ex = export.Exporter(context, self.report)
            path, n_ent, n_models = ex.run()
        except export.ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        msg = f"Fitzel: {n_ent} objects, {n_models} models -> {path}"
        if ex.warnings:
            msg += f" ({len(ex.warnings)} warnings, see the Info log)"
        self.report({"INFO"}, msg)
        if self.play:
            launch.play(exe, ex.project, ex.scene_name, context.scene.fitzel.fullscreen)
        return {"FINISHED"}


class FITZEL_OT_play(bpy.types.Operator):
    """Start the last exported scene in Fitzel without exporting again"""
    bl_idname = "fitzel.play"
    bl_label = "Play Last Export"

    def execute(self, context):
        try:
            project, scene = export.project_paths(context.scene.fitzel, context.scene)
        except export.ExportError as e:
            self.report({"ERROR"}, str(e))
            return {"CANCELLED"}
        if not os.path.exists(os.path.join(project, scene + ".fitzel")):
            self.report({"ERROR"}, f"{scene}.fitzel is not in the project yet -- export first")
            return {"CANCELLED"}
        exe = engine_exe(context)
        if not exe:
            self.report({"ERROR"}, "No Fitzel executable found -- set it in the add-on "
                                   "preferences")
            return {"CANCELLED"}
        launch.play(exe, project, scene, context.scene.fitzel.fullscreen)
        return {"FINISHED"}


class FITZEL_OT_script_add(bpy.types.Operator):
    """Give the object another Lua script"""
    bl_idname = "fitzel.script_add"
    bl_label = "Add Script"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        fz = context.object.fitzel
        fz.scripts.add()
        fz.script_index = len(fz.scripts) - 1
        return {"FINISHED"}


class FITZEL_OT_script_remove(bpy.types.Operator):
    """Take the selected script off the object"""
    bl_idname = "fitzel.script_remove"
    bl_label = "Remove Script"
    bl_options = {"REGISTER", "UNDO"}

    def execute(self, context):
        fz = context.object.fitzel
        if 0 <= fz.script_index < len(fz.scripts):
            fz.scripts.remove(fz.script_index)
            fz.script_index = max(0, fz.script_index - 1)
        return {"FINISHED"}


class FITZEL_UL_scripts(bpy.types.UIList):
    def draw_item(self, context, layout, data, item, icon, active_data, active_prop):
        row = layout.row(align=True)
        row.prop(item, "text", text="", icon="TEXT")
        row.prop(item, "filepath", text="")


def draw_export_buttons(layout, context):
    s = context.scene.fitzel
    col = layout.column(align=True)
    col.prop(s, "project_dir")
    col.prop(s, "scene_name")
    row = layout.row(align=True)
    row.scale_y = 1.4
    row.operator(FITZEL_OT_export.bl_idname, text="Export", icon="EXPORT").play = False
    row.operator(FITZEL_OT_export.bl_idname, text="Export & Play", icon="PLAY").play = True
    layout.operator(FITZEL_OT_play.bl_idname, icon="FILE_REFRESH")
    if not engine_exe(context):
        layout.label(text="Set the Fitzel executable in the preferences", icon="ERROR")


class FITZEL_PT_scene(bpy.types.Panel):
    bl_label = "Fitzel"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "scene"

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        s = context.scene.fitzel
        draw_export_buttons(layout, context)

        box = layout.box()
        box.label(text="Play", icon="PLAY")
        box.prop(s, "start_mode")
        box.prop(s, "fullscreen")
        if s.start_mode == "FPS":
            box.prop(s, "move_speed")
            box.prop(s, "run_speed")
            box.prop(s, "step_height")

        box = layout.box()
        box.label(text="World", icon="WORLD")
        box.prop(s, "collision")
        box.prop(s, "clean_world")
        row = box.row(heading="Water")
        row.prop(s, "water", text="")
        sub = row.row()
        sub.active = s.water
        sub.prop(s, "water_level", text="")
        box.prop(s, "export_world")
        box.prop(s, "time_of_day")
        box.prop(s, "time_flows")

        box = layout.box()
        box.label(text="Export", icon="SETTINGS")
        box.prop(s, "only_visible")
        box.prop(s, "keep_settings")


class FITZEL_PT_sidebar(bpy.types.Panel):
    bl_label = "Fitzel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Fitzel"

    def draw(self, context):
        self.layout.use_property_split = False
        draw_export_buttons(self.layout, context)
        self.layout.prop(context.scene.fitzel, "start_mode")


class FITZEL_PT_object(bpy.types.Panel):
    bl_label = "Fitzel"
    bl_space_type = "PROPERTIES"
    bl_region_type = "WINDOW"
    bl_context = "object"

    @classmethod
    def poll(cls, context):
        return context.object is not None

    def draw(self, context):
        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        obj = context.object
        fz = obj.fitzel
        layout.prop(fz, "role")
        if fz.role == "IGNORE":
            return
        if obj.type in export.GEOMETRY and fz.role == "AUTO":
            layout.prop(fz, "physics")
            if fz.physics == "DYNAMIC":
                layout.prop(fz, "mass")
        if obj.type == "LIGHT":
            layout.prop(fz, "light_override")
            col = layout.column()
            col.active = fz.light_override
            col.prop(fz, "light_intensity")
            if obj.data.type != "SUN":
                col.prop(fz, "light_range")
            if obj.data.type == "POINT":
                layout.prop(fz, "light_shadows")
        if obj.type == "CAMERA" and fz.role == "AUTO":
            layout.prop(fz, "camera_main")

        layout.label(text="Lua Scripts")
        row = layout.row()
        row.template_list("FITZEL_UL_scripts", "", fz, "scripts", fz, "script_index", rows=2)
        col = row.column(align=True)
        col.operator(FITZEL_OT_script_add.bl_idname, text="", icon="ADD")
        col.operator(FITZEL_OT_script_remove.bl_idname, text="", icon="REMOVE")
        layout.prop(fz, "extra")


def menu_export(self, context):
    self.layout.operator(FITZEL_OT_export.bl_idname, text="Fitzel Scene (.fitzel)").play = False


classes = (FitzelPreferences, FITZEL_OT_export, FITZEL_OT_play, FITZEL_OT_script_add,
           FITZEL_OT_script_remove, FITZEL_UL_scripts, FITZEL_PT_scene, FITZEL_PT_sidebar,
           FITZEL_PT_object)


def register():
    props.register()
    for c in classes:
        bpy.utils.register_class(c)
    bpy.types.TOPBAR_MT_file_export.append(menu_export)


def unregister():
    bpy.types.TOPBAR_MT_file_export.remove(menu_export)
    for c in reversed(classes):
        bpy.utils.unregister_class(c)
    props.unregister()
