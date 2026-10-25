#!/usr/bin/env bash
# OpenGL lives in engine/src/rhi/gl and nowhere else (see docs/rhi.md).
# Lists every gl* call, GL_* enum, GL type or glad include outside it; exit 1
# if there is one. GLSL inside raw string literals (gl_Position, ...) does not
# match: the patterns want a capital after "gl" or "GL_".
cd "$(dirname "$0")/.."
hits=$(grep -rnE '\bgl[A-Z][A-Za-z0-9]*\(|\bGL_[A-Z0-9_]+\b|\bGL(uint|int|enum|sizei|float|boolean|sync|bitfield|intptr|sizeiptr|uint64|int64|ubyte|char)\b|glad/gl\.h|GLAD_GL_' \
        --include='*.cpp' --include='*.hpp' --include='*.h' engine sandbox/src sandbox/tools \
        | grep -v '^engine/src/rhi/gl/')
if [ -n "$hits" ]; then
    echo "OpenGL outside engine/src/rhi/gl:"
    echo "$hits"
    exit 1
fi
echo "no OpenGL outside engine/src/rhi/gl"
