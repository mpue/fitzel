# Starting the engine: find the Fitzel executable and play a project scene.
#
# Fitzel's player boots from the command line (sandbox/src/Startup.cpp):
#   sandbox --play <project folder> --scene <scene name> [--fullscreen 0|1]
# It opens the project, loads <project>/<scene>.fitzel and starts Play at once.
# Nothing goes back to Blender -- Esc quits the game.

import os
import shutil
import subprocess
import sys
import tempfile


def candidates(chosen):
    if chosen:
        yield chosen
    env = os.environ.get("FITZEL_EXE")
    if env:
        yield env
    if sys.platform == "win32":
        for root in (os.environ.get("ProgramFiles"), os.environ.get("LOCALAPPDATA")):
            if root:
                yield os.path.join(root, "Fitzel", "sandbox.exe")
    else:
        yield "/opt/fitzel/sandbox"   # the .deb (build-deb.sh)
    for name in ("fitzel", "sandbox"):
        found = shutil.which(name)
        if found:
            yield found


def find_engine(chosen=""):
    """The Fitzel executable, or "" when none is found."""
    for c in candidates(chosen):
        c = os.path.abspath(os.path.expanduser(c))
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return ""


def log_path():
    return os.path.join(tempfile.gettempdir(), "fitzel-blender-play.log")


def scene_args(project, scene):
    # The project opens on <folder name>.fitzel when there is one; asking for
    # that same scene again would load it twice.
    stem = os.path.basename(os.path.normpath(project))
    return [] if scene == stem else ["--scene", scene]


def play(exe, project, scene, fullscreen=False):
    """Start the game detached from Blender. Its console output goes to
    log_path(). Returns the process."""
    args = [exe, "--play", project] + scene_args(project, scene) + \
           ["--fullscreen", "1" if fullscreen else "0"]
    log = open(log_path(), "w", encoding="utf-8")
    kw = {}
    if sys.platform == "win32":
        kw["creationflags"] = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kw["start_new_session"] = True
    try:
        return subprocess.Popen(args, cwd=os.path.dirname(exe), stdout=log,
                                stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **kw)
    finally:
        log.close()   # the child has its own handle
