#!/usr/bin/env python3
"""Compile every shader the way the browser will see it, without a browser.

Each .vert/.frag under sandbox/assets/shaders gets its #includes expanded and
the GLSL 3.30 -> ES 3.00 rewrite of Shader.cpp (translateForES) applied, then
goes through glslangValidator as an ES 3.00 shader. Fragment shaders also get
their ACTIVE samplers counted (glslang's reflection after optimisation drops the
unused ones, as the browser's compiler does): WebGL2 guarantees 16, and a
fragment shader over that compiles and then fails to link at runtime -- which in
the browser is a black frame and a line in the console.

Usage:  python web/shadercheck.py [shader-dir] [--limit 16]
Exit code 1 when anything fails. glslangValidator comes with the Vulkan SDK
(VULKAN_SDK) or from PATH.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

HEAD = """#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;
precision highp sampler3D;
precision highp samplerCube;
precision highp sampler2DArray;
precision highp sampler2DShadow;
precision highp sampler2DArrayShadow;
precision highp samplerCubeShadow;
precision highp isampler2D;
precision highp usampler2D;
precision highp isampler3D;
precision highp usampler3D;
precision highp isampler2DArray;
precision highp usampler2DArray;
#define FITZEL_WEB 1
#line 2
"""


def expand(path, depth=0):
    with open(path, encoding="utf-8") as f:
        src = f.read()
    if depth > 8 or "#include" not in src:
        return src
    out = []
    d = os.path.dirname(path)
    for i, ln in enumerate(src.split("\n")):
        st = ln.lstrip()
        if st.startswith("#include"):
            m = re.search(r'"([^"]+)"', st)
            if m:
                out.append(expand(os.path.join(d, m.group(1)), depth + 1))
                out.append("#line %d" % (i + 2))
                continue
        out.append(ln)
    return "\n".join(out)


def to_es(src, stage):
    v = src.find("#version")
    if v < 0:
        return src
    nl = src.find("\n", v)
    head = HEAD
    if stage == "vert" and "gl_ClipDistance" in src:
        # glslang has no ANGLE extension; check the variant without clipping,
        # which is the one every browser can run.
        src = "\n".join(l for l in src.split("\n") if "gl_ClipDistance" not in l)
        nl = src.find("\n", v)
    return src[:v] + head + src[nl + 1:]


def validator():
    sdk = os.environ.get("VULKAN_SDK")
    cands = []
    if sdk:
        cands.append(os.path.join(sdk, "Bin", "glslangValidator.exe"))
    cands += glob.glob(r"C:\VulkanSDK\*\Bin\glslangValidator.exe")
    for c in cands:
        if os.path.isfile(c):
            return c
    return shutil.which("glslangValidator")


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    limit = 16
    if "--limit" in sys.argv:
        limit = int(sys.argv[sys.argv.index("--limit") + 1])
    root = args[0] if args else os.path.join("sandbox", "assets", "shaders")
    gv = validator()
    if not gv:
        print("glslangValidator not found (install the Vulkan SDK)")
        return 2
    tmp = tempfile.mkdtemp(prefix="fzes_")
    bad = 0
    files = sorted(glob.glob(os.path.join(root, "*.vert")) +
                   glob.glob(os.path.join(root, "*.frag")))
    for path in files:
        stage = path.rsplit(".", 1)[1]
        src = expand(path)
        if "#version 330" not in src:
            continue
        es = to_es(src, stage)
        out = os.path.join(tmp, os.path.basename(path))
        with open(out, "w", encoding="utf-8") as f:
            f.write(es)
        r = subprocess.run([gv, "-S", stage, "-l", "-q", out],
                           capture_output=True, text=True)
        text = r.stdout + r.stderr
        name = os.path.basename(path)
        if r.returncode != 0:
            bad += 1
            errs = [l for l in text.split("\n") if "ERROR" in l][:12]
            print("FAIL  %s" % name)
            for e in errs:
                print("        " + e.replace(out, name))
            continue
        samplers = 0
        if stage == "frag":
            # Reflection: "Uniform reflection:" block lists active uniforms
            # with their GL type (0x8b5e = sampler2D, ...).
            sect = text.split("Uniform reflection:")[1].split("Uniform block")[0] \
                if "Uniform reflection:" in text else ""
            for ln in sect.split("\n"):
                m = re.search(r"type (?:0x)?([0-9a-fA-F]+).*?size (\d+)", ln)
                if not m:
                    continue
                t = int(m.group(1), 16)
                if (0x8B5D <= t <= 0x8B64) or (0x8DC0 <= t <= 0x8DD8) or t in (0x8DC1, 0x8DC4, 0x8DC5):
                    samplers += int(m.group(2))
        flag = ""
        if samplers > limit:
            bad += 1
            flag = "   OVER the WebGL2 limit of %d" % limit
        print("ok    %-28s%s%s" % (name, ("%2d samplers" % samplers) if stage == "frag" else "", flag))
    shutil.rmtree(tmp, ignore_errors=True)
    print("\n%d of %d shaders fail" % (bad, len(files)) if bad else "\nall %d shaders pass" % len(files))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
