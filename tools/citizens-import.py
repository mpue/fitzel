"""People for a project's towns, from a Unity crowd pack (Citizens PRO's "People 2.0").

    python3 -I tools/citizens-import.py "<pack folder>" <project folder> [--all] [--only name ...]

For a choice of the pack's figures (or --all of them): a walking .glb in <project>/models/citizens/
(built by Blender, tools/citizens-to-glb.py: the figure, its textures at 1024, and the walk of its
kind -- men, women in flat shoes or heels, boys, girls), its .meta, and a prefab
<project>/prefabs/Citizen <name>-<id>.fprefab: one model entity at the figure's own height, which
the town generator walks on its pavements (cityplan::PersonPrefab; the world generator takes every
"Citizen ..." prefab of the project). The pack itself is not copied -- it stays the user's -- and
re-running replaces what an earlier run made, GUIDs and all.

Why a choice: a town bakes every person look into its walk poses (TownTraffic, some 8 MB a figure
of this size), and twenty-odd faces are a crowd; all eighty-five are three quarters of a gigabyte.
"""
import argparse, hashlib, json, os, struct, subprocess, sys, tempfile
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent

# The choice: (folder under People 2.0, walk). Summer clothes, a few in suits, some in sports
# gear, two grannies and the children -- none of the beach figures.
MAN, WOMAN, HEELS, BOY, GIRL = ("Man/walk.FBX", "Girl no Heel/walk_f.FBX", "Girl with Heel/walk_f.FBX",
                                "Child Man/Walk-child boy.FBX", "Child Girl/Walk-child girl.FBX")
CHOICE = [
    ("Male/casual01_m", MAN), ("Male/casual05_m", MAN), ("Male/casual09_m", MAN),
    ("Male/casual14_m", MAN), ("Male/casual20_m", MAN), ("Male/casual27_m", MAN),
    ("Male/business01_m", MAN), ("Male/business04_m", MAN),
    ("Male/sportive01_m", MAN), ("Male/sportive05_m", MAN), ("Male/child01_m", BOY),
    ("Female/casual01_f", WOMAN), ("Female/casual04_f", WOMAN), ("Female/casual08_f", WOMAN),
    ("Female/casual13_f+", WOMAN), ("Female/casual18_f", WOMAN),
    ("Female/business02_f", HEELS), ("Female/sportive01_f", WOMAN),
    ("Female/granny01", WOMAN), ("Female/granny02", WOMAN),
    ("Female/child01_f", GIRL), ("Female/child02_f", GIRL),
]


def walk_of(folder):
    name = folder.split("/")[-1]
    if "child" in name:
        return GIRL if folder.startswith("Female") else BOY
    if folder.startswith("Female"):
        return HEELS if name.startswith("business") else WOMAN
    return MAN


def files_of(d):
    fs = [f for f in os.listdir(d) if not f.endswith(".meta")]
    model = next((f for f in fs if f.lower().endswith(".fbx")), None)
    pngs = [f for f in fs if f.lower().endswith(".png")]
    hair = next((f for f in fs if f.lower().endswith("_hair.tga")), None)
    normal = next((f for f in pngs if "normal" in f and "hair" not in f), None)
    hairn = next((f for f in pngs if "normal" in f and "hair" in f), None)
    body = next((f for f in pngs if "normal" not in f and "hair" not in f), None)
    return model, body, normal, hair, hairn


def guid(seed):
    """Stable per figure, so a re-run keeps every reference to it."""
    return hashlib.md5(("citizens:" + seed).encode()).hexdigest()


def shrink(src, dst, size, alpha):
    img = Image.open(src)
    img = img.convert("RGBA" if alpha else "RGB")
    if max(img.size) > size:
        img = img.resize((size, size), Image.LANCZOS)
    if alpha:
        img.save(dst)
    else:
        img.save(dst, quality=90)


def glb_bounds(path):
    data = path.read_bytes()
    n = struct.unpack_from("<I", data, 12)[0]
    j = json.loads(data[20:20 + n])
    lo, hi = [1e9] * 3, [-1e9] * 3
    for m in j["meshes"]:
        for p in m["primitives"]:
            a = j["accessors"][p["attributes"]["POSITION"]]
            lo = [min(x, y) for x, y in zip(lo, a["min"])]
            hi = [max(x, y) for x, y in zip(hi, a["max"])]
    return lo, hi


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("pack")
    ap.add_argument("project")
    ap.add_argument("--all", action="store_true", help="every figure but the beach ones")
    ap.add_argument("--only", nargs="*", default=[])
    ap.add_argument("--blender", default="blender")
    a = ap.parse_args()
    pack, project = Path(a.pack), Path(a.project)
    people = pack / "Models" / "People 2.0"
    anims = pack / "Animations"
    if a.all:
        choice = [(f"{sex}/{d}", walk_of(f"{sex}/{d}")) for sex in ("Male", "Female")
                  for d in sorted(os.listdir(people / sex))
                  if (people / sex / d).is_dir() and not d.startswith(("nude", "player"))]
    else:
        choice = CHOICE
    if a.only:
        choice = [c for c in choice if c[0].split("/")[-1] in a.only]

    models = project / "models" / "citizens"
    prefabs = project / "prefabs"
    models.mkdir(parents=True, exist_ok=True)
    prefabs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        jobs = []
        for folder, walk in choice:
            d = people / folder
            model, body, normal, hair, hairn = files_of(d)
            if not model or not body:
                print("skipped", folder, "(no model or texture)")
                continue
            name = folder.split("/")[-1].rstrip("+")
            tex = {}
            for key, f, alpha in (("body", body, False), ("normal", normal, False),
                                  ("hair", hair, True), ("hairNormal", hairn, False)):
                if f:
                    dst = tmp / f"{name}_{key}.{'png' if alpha else 'jpg'}"
                    shrink(d / f, dst, 1024, alpha)
                    tex[key] = str(dst)
                else:
                    tex[key] = None
            jobs.append({"model": str(d / model), "anim": str(anims / walk), **tex,
                         "out": str(models / f"{name}.glb"), "tex": 1024, "name": name})
        (tmp / "jobs.json").write_text(json.dumps(jobs, indent=1))
        r = subprocess.run([a.blender, "-b", "--python", str(HERE / "citizens-to-glb.py"), "--",
                            str(tmp / "jobs.json")], capture_output=True, text=True)
        for line in r.stdout.splitlines():
            if line.startswith("citizen"):
                print(line)

    made = 0
    for job in jobs:
        glb = Path(job["out"])
        if not glb.exists():
            continue
        name = job["name"]
        mguid = guid("model:" + name)
        glb.with_name(glb.name + ".meta").write_text(json.dumps({"guid": mguid, "type": "Model"}, indent=2) + "\n")
        lo, hi = glb_bounds(glb)
        size = [b - c for b, c in zip(hi, lo)]
        # The figure's own size: the engine fits a model to its entity's half extents.
        half = [round(s * 0.5, 4) for s in size]
        pguid = guid("prefab:" + name)
        title = "Citizen " + name
        for old in prefabs.glob(f"{title}-*.fprefab"):
            old.unlink()
        doc = {"app": "tools/citizens-import.py", "version": 1, "prefab": {
            "guid": pguid, "name": title,
            "entities": [{"active": True, "center": [0.0, 0.0, 0.0], "half": half, "id": 0,
                          "name": title, "parent": -1, "rotation": [0.0, 0.0, 0.0], "type": 6,
                          "components": [{"model": mguid, "modelFile": glb.name,
                                          "scale": 1.0, "type": "model"}]}]}}
        (prefabs / f"{title}-{pguid[:8]}.fprefab").write_text(json.dumps(doc, indent=1) + "\n")
        made += 1
    print(f"{made} citizens in {models}")


if __name__ == "__main__":
    main()
