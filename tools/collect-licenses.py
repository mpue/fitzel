#!/usr/bin/env python3
"""Build docs/third-party-licenses.md out of the licences that are actually
on disk, in the versions this build fetched.

A notices file typed by hand is a notices file that quietly stops being true:
someone bumps a GIT_TAG, the versions in the document keep claiming the old
one, and nobody notices because nothing breaks. So nothing here is typed. The
version of every component is read back out of cmake/Dependencies.cmake, and
every licence text is copied verbatim from the fetched source tree.

Two things make it fail loudly rather than produce a plausible-looking lie:

  * a dependency declared in Dependencies.cmake with no entry in COMPONENTS
    below is an error -- add the dependency, and the notices demand a decision
    about its licence in the same breath;
  * a licence file that has moved or vanished upstream is an error too.

Usage:
    python tools/collect-licenses.py [build-dir]      # default: build/release
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class Text:
    """Where a licence text comes from."""

    def __init__(self, path, start=None, end=None, dedent=False, last=False):
        self.path = path      # relative to the dependency's source dir
        self.start = start    # first line containing this (None = file start)
        self.end = end        # line containing this ends it (None = file end)
        self.dedent = dedent  # strip leading comment markers / indent
        # Take the LAST match of `start` instead of the first: several libraries
        # put a "see the notice at the end of this file" line up top and the
        # licence itself at the bottom, and matching the top one swallows the
        # whole source file.
        self.last = last

    def read(self, src_dir):
        f = src_dir / self.path
        if not f.exists():
            raise FileNotFoundError(f)
        lines = f.read_text(encoding="utf-8", errors="replace").splitlines()
        if self.start is not None:
            hits = [i for i, l in enumerate(lines) if self.start in l]
            if not hits:
                raise ValueError(f"start marker {self.start!r} not in {f}")
            lines = lines[hits[-1] if self.last else hits[0]:]
        if self.end is not None:
            for i, l in enumerate(lines):
                if self.end in l:
                    lines = lines[: i + 1]
                    break
            else:
                raise ValueError(f"end marker {self.end!r} not in {f}")
        if self.dedent:
            out = []
            for l in lines:
                l = re.sub(r"^\s*(/\*+|\*+/|\*|//|#)\s?", "", l)
                out.append(l.rstrip())
            lines = out
        # Trim blank lines at both ends.
        while lines and not lines[0].strip():
            lines.pop(0)
        while lines and not lines[-1].strip():
            lines.pop()

        # Two guards on a sliced extract, because both mistakes happened while
        # this file was being written and neither announced itself: a marker
        # that matched too LATE swallowed hundreds of lines of source code, and
        # one that matched too EARLY cut a licence off mid-sentence. Line
        # wrapping is what does it -- "OTHER DEALINGS IN THE SOFTWARE" is one
        # line in some of these files and two in others.
        if self.end is not None:
            if len(lines) > 250:
                raise ValueError(
                    f"{f}: {len(lines)} lines -- end marker {self.end!r} "
                    f"matched too late, this is source code, not a licence")
            if lines and not lines[-1].rstrip().endswith((".", "*/", ")")):
                raise ValueError(
                    f"{f}: extract ends on {lines[-1].strip()!r} -- end marker "
                    f"{self.end!r} matched too early, the licence is cut off")
        return "\n".join(lines)


class Component:
    def __init__(self, dep, name, licence, text, note=None, src=None):
        self.dep = dep          # FetchContent name, or None for a bundled part
        self.name = name
        self.licence = licence
        self.text = text
        self.note = note
        self.src = src          # source dir override (bundled parts)


# Every dependency this project links, plus the third-party code they bundle
# that ends up in the binary. Order is roughly "ours first, theirs after".
COMPONENTS = [
    Component("glfw", "GLFW", "zlib/libpng", Text("LICENSE.md")),
    Component("glm", "OpenGL Mathematics (GLM)", "MIT (of MIT / Happy Bunny)",
              Text("copying.txt"),
              "GLM offers a choice of two licences; this project uses it under "
              "the MIT License."),
    Component("glad", "glad 2", "MIT; generated loader also Khronos "
              "(Apache-2.0 / MIT)", Text("LICENSE"),
              "The generated OpenGL loader contains material derived from the "
              "Khronos specifications, covered by the second half of this text."),
    Component("stb", "stb (stb_image)", "MIT (of MIT / public domain)",
              Text("LICENSE", end="ALTERNATIVE B"),
              "stb offers a choice of two licences; this project uses it under "
              "the MIT License."),
    Component("imgui", "Dear ImGui", "MIT", Text("LICENSE.txt")),
    Component("imguizmo", "ImGuizmo", "MIT", Text("LICENSE")),
    Component("imcolortextedit", "ImGuiColorTextEdit", "MIT", Text("LICENSE")),
    Component("tinyexr", "TinyEXR", "BSD-3-Clause",
              Text("tinyexr.h", start="Copyright (c) 2014", end="POSSIBILITY OF SUCH DAMAGE",
                   dedent=True)),
    Component(None, "miniz (bundled with TinyEXR)", "MIT",
              Text("deps/miniz/miniz.c", start="Copyright 2013-2014 RAD Game Tools",
                   end="****************", dedent=True),
              src="tinyexr"),
    Component("miniaudio", "miniaudio", "MIT-0 (of public domain / MIT-0)",
              Text("LICENSE", start="ALTERNATIVE 2"),
              "miniaudio offers a choice of two licences; this project uses it "
              "under MIT No Attribution."),
    Component("cgltf", "cgltf", "MIT", Text("LICENSE")),
    Component("lua", "Lua", "MIT",
              Text("lua.h", start="Copyright (C) 1994-2024 Lua.org",
                   end="DEALINGS IN THE SOFTWARE", dedent=True, last=True)),
    Component("nlohmann_json", "JSON for Modern C++", "MIT",
              Text("LICENSE.MIT")),
    Component("jolt", "Jolt Physics", "MIT", Text("LICENSE")),
    Component("assimp", "Open Asset Import Library (assimp)", "BSD-3-Clause",
              Text("LICENSE")),
    # --- what assimp brings with it and compiles into the same static library
    Component(None, "zlib (bundled with assimp)", "zlib",
              Text("contrib/zlib/LICENSE"), src="assimp"),
    Component(None, "minizip / unzip (bundled with assimp)", "zlib",
              Text("contrib/unzip/unzip.h", start="Copyright (C) 1998-2010 Gilles Vollant",
                   end="misrepresented as being the original software", dedent=True),
              src="assimp"),
    Component(None, "poly2tri (bundled with assimp)", "BSD-3-Clause",
              Text("contrib/poly2tri/LICENSE"), src="assimp"),
    Component(None, "Clipper (bundled with assimp)", "Boost Software License 1.0",
              Text("contrib/clipper/License.txt"), src="assimp"),
    Component(None, "OpenDDL Parser (bundled with assimp)", "MIT",
              Text("contrib/openddlparser/LICENSE"), src="assimp"),
    Component(None, "Open3DGC (bundled with assimp)", "MIT",
              Text("contrib/Open3DGC/o3dgcCommon.h", start="Copyright (c) 2013 Khaled Mammou",
                   end="THE SOFTWARE.", dedent=True), src="assimp"),
    Component(None, "utf8cpp (bundled with assimp)", "Boost Software License 1.0",
              Text("contrib/utf8cpp/source/utf8.h", start="Copyright 2006 Nemanja Trifunovic",
                   end="DEALINGS IN THE SOFTWARE", dedent=True), src="assimp"),
    Component(None, "pugixml (bundled with assimp)", "MIT",
              Text("contrib/pugixml/src/pugixml.hpp", start="Copyright (c) 2006-2024",
                   end="OTHER DEALINGS IN THE SOFTWARE", dedent=True, last=True),
              src="assimp"),
]

HEADER = """# Third-party licences

Fitzel is built on the open-source components listed below. Every one of them is
under a permissive licence: they may be used in a closed-source, commercially
sold game, and none of them places any condition on this project's own code.

What they DO ask for is this file. MIT, BSD-3-Clause, Apache-2.0 and the Boost
licence all require their copyright notice and licence text to travel with a
binary that contains them, so an exported game ships this document next to its
executable.

Not covered here: the art, audio and models in a game made with the editor. Those
come with whatever terms they were bought or downloaded under, and only the person
who obtained them knows what those are.

This file is GENERATED -- do not edit it by hand. Versions are read back from
`cmake/Dependencies.cmake` and every licence text is copied verbatim out of the
fetched sources, so it cannot drift away from what is actually being built:

    python tools/collect-licenses.py

"""


def read_pins():
    """{dep name: (repository, tag)} straight out of Dependencies.cmake.

    Block extraction walks to the closing line rather than to the next ')' --
    the comments in that file contain parentheses, and matching the first one
    silently cut a declaration in half (Dear ImGui lost its pin that way).
    """
    lines = (ROOT / "cmake" / "Dependencies.cmake").read_text(
        encoding="utf-8").splitlines()
    pins = {}
    i = 0
    while i < len(lines):
        if "FetchContent_Declare(" in lines[i]:
            block = []
            i += 1
            while i < len(lines) and lines[i].strip() != ")":
                block.append(lines[i])
                i += 1
            body = "\n".join(block)
            name = block[0].strip() if block else ""
            repo = re.search(r"GIT_REPOSITORY\s+(\S+)", body)
            tag = re.search(r"GIT_TAG\s+(\S+)", body)
            if re.fullmatch(r"\w+", name) and repo and tag:
                pins[name] = (repo.group(1), tag.group(1))
        i += 1
    return pins


def short_tag(tag):
    return tag[:10] if re.fullmatch(r"[0-9a-f]{40}", tag) else tag


def main(argv):
    build = Path(argv[1]) if len(argv) > 1 else ROOT / "build" / "release"
    deps = build / "_deps"
    if not deps.is_dir():
        print(f"no dependency tree at {deps} -- configure a build first",
              file=sys.stderr)
        return 1

    pins = read_pins()
    covered = {c.dep for c in COMPONENTS if c.dep}
    missing = sorted(set(pins) - covered)
    if missing:
        print("dependencies with no entry in COMPONENTS: " + ", ".join(missing),
              file=sys.stderr)
        print("add them to tools/collect-licenses.py before releasing.",
              file=sys.stderr)
        return 1

    out = [HEADER, "## Overview\n",
           "| Component | Version | Licence |", "| --- | --- | --- |"]
    bodies = []
    for c in COMPONENTS:
        key = c.dep or c.src
        src_dir = deps / f"{key}-src"
        repo, tag = pins.get(c.dep, ("", ""))
        version = short_tag(tag) if c.dep else "(see parent)"
        try:
            text = c.text.read(src_dir)
        except (FileNotFoundError, ValueError) as e:
            print(f"{c.name}: {e}", file=sys.stderr)
            return 1
        out.append(f"| {c.name} | {version} | {c.licence} |")
        body = [f"\n## {c.name}\n"]
        if repo:
            body.append(f"{repo} @ `{tag}`\n")
        body.append(f"Licence: {c.licence}\n")
        if c.note:
            body.append(f"{c.note}\n")
        body.append("```\n" + text + "\n```")
        bodies.append("\n".join(body))

    doc = "\n".join(out) + "\n" + "\n".join(bodies) + "\n"
    dst = ROOT / "docs" / "third-party-licenses.md"
    dst.parent.mkdir(exist_ok=True)
    dst.write_text(doc, encoding="utf-8", newline="\n")
    print(f"wrote {dst.relative_to(ROOT)} -- {len(COMPONENTS)} components")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
