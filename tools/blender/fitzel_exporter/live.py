# The live link: Blender and an open Fitzel editor kept on one scene.
#
#   Ctrl+S in Blender  -> the scene is exported; the Fitzel editor sees the file
#                         change and reloads it (ProjectIO, checkOutsideChange).
#   Ctrl+S in Fitzel   -> a timer here sees the file change and pulls the
#                         lighting back (sync.py).
#
# The exporter remembers the time it left the file with (export.OWN_WRITES), so
# its own write is never mistaken for Fitzel's. The same timer reports a light
# bake running in the background.

import os

import bpy
from bpy.app.handlers import persistent

from . import export, launch, sync

INTERVAL = 1.0
_seen = {}       # scene file -> mtime last looked at
_bake = {}       # the running bake: proc, scene name, grid file, its mtime before


def _redraw():
    wm = bpy.context.window_manager
    for win in (wm.windows if wm else []):
        for area in win.screen.areas:
            if area.type in {"PROPERTIES", "VIEW_3D"}:
                area.tag_redraw()


def _view_layer(scene):
    ctx = bpy.context
    if getattr(ctx, "scene", None) == scene and getattr(ctx, "view_layer", None):
        return ctx.view_layer
    return scene.view_layers[0]


def _scene_path(scene):
    try:
        return export.scene_file(*export.project_paths(scene.fitzel, scene))
    except export.ExportError:
        return None


def start_bake(scene, exe, project, scene_name):
    grid = os.path.join(project, "lightgrids", scene_name + ".fgrid")
    before = os.path.getmtime(grid) if os.path.exists(grid) else None
    _bake.update(proc=launch.bake(exe, project, scene_name, int(scene.fitzel.bake_resolution)),
                 scene=scene.name, grid=grid, before=before, exe=exe)
    scene.fitzel.status = "Baking the indirect light in Fitzel..."


def baking():
    p = _bake.get("proc")
    return p is not None and p.poll() is None


def _check_bake():
    p = _bake.get("proc")
    if p is None or p.poll() is None:
        return
    scene = bpy.data.scenes.get(_bake.get("scene", ""))
    grid = _bake["grid"]
    done = os.path.exists(grid) and os.path.getmtime(grid) != _bake["before"]
    verdict = ""
    try:
        with open(launch.bake_log(_bake["exe"]), "r", encoding="utf-8") as f:
            verdict = f.read().strip()
    except OSError:
        pass
    if scene is not None:
        scene.fitzel.status = (f"Indirect light baked{': ' + verdict if verdict else ''}"
                               if done else
                               f"The bake did not finish{': ' + verdict if verdict else ''}"
                               f" (log: {launch.log_path()})")
    _bake.clear()
    _redraw()


def _poll():
    _check_bake()
    for scene in bpy.data.scenes:
        if not scene.fitzel.live_link:
            continue
        path = _scene_path(scene)
        if not path or not os.path.exists(path):
            continue
        mtime = os.path.getmtime(path)
        last = _seen.get(path)
        _seen[path] = mtime
        if last is None or mtime == last or export.OWN_WRITES.get(path) == mtime:
            continue
        try:
            msg = sync.Pull(scene, _view_layer(scene)).run()
        except export.ExportError as e:
            msg = str(e)
        scene.fitzel.status = msg
        try:
            bpy.ops.ed.undo_push(message="Lighting from Fitzel")
        except RuntimeError:
            pass
        _redraw()
    return INTERVAL


@persistent
def _save_pre(*_args):
    ctx = bpy.context
    scene = getattr(ctx, "scene", None)
    if scene is None or not scene.fitzel.live_link:
        return
    try:
        path, n_ent, n_models = export.Exporter(ctx).run()
        scene.fitzel.status = f"Exported with the save: {n_ent} objects, {n_models} models"
        _seen[path] = export.OWN_WRITES.get(path)
    except export.ExportError as e:
        scene.fitzel.status = str(e)


def register():
    if not bpy.app.timers.is_registered(_poll):
        bpy.app.timers.register(_poll, first_interval=INTERVAL, persistent=True)
    if _save_pre not in bpy.app.handlers.save_pre:
        bpy.app.handlers.save_pre.append(_save_pre)


def unregister():
    if _save_pre in bpy.app.handlers.save_pre:
        bpy.app.handlers.save_pre.remove(_save_pre)
    if bpy.app.timers.is_registered(_poll):
        bpy.app.timers.unregister(_poll)
