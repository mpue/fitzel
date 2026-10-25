#!/usr/bin/env bash
# OpenGL lives in engine/src/rhi/gl and nowhere else, Vulkan in engine/src/rhi/vk
# (see docs/rhi.md). Lists every gl*/vk* call, GL_*/VK_* enum, GL/Vk type and
# loader include outside its backend; exit 1 if there is one. GLSL inside raw
# string literals (gl_Position, ...) does not match: the patterns want a
# capital after "gl" or "GL_".
cd "$(dirname "$0")/.."
src="--include=*.cpp --include=*.hpp --include=*.h engine sandbox/src sandbox/tools"
gl=$(grep -rnE '\bgl[A-Z][A-Za-z0-9]*\(|\bGL_[A-Z0-9_]+\b|\bGL(uint|int|enum|sizei|float|boolean|sync|bitfield|intptr|sizeiptr|uint64|int64|ubyte|char)\b|glad/gl\.h|GLAD_GL_' \
        $src | grep -v '^engine/src/rhi/gl/')
vk=$(grep -rnE '\bvk[A-Z][A-Za-z0-9]*\(|\bVK_[A-Z0-9_]+\b|\bVk[A-Z][A-Za-z0-9]*\b|volk\.h|vk_mem_alloc|vulkan/vulkan' \
        $src | grep -v '^engine/src/rhi/vk/')
status=0
if [ -n "$gl" ]; then
    echo "OpenGL outside engine/src/rhi/gl:"
    echo "$gl"
    status=1
fi
if [ -n "$vk" ]; then
    echo "Vulkan outside engine/src/rhi/vk:"
    echo "$vk"
    status=1
fi
[ $status -eq 0 ] && echo "no OpenGL outside engine/src/rhi/gl, no Vulkan outside engine/src/rhi/vk"
exit $status
