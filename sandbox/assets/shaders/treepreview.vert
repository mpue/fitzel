#version 330 core

// The tree generator's preview (TreePreview.cpp): the tree in its own frame,
// metres, no instancing, no wind.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

uniform mat4 uViewProj;

out vec3 vPos;
out vec3 vNormal;
out vec2 vUv;

void main() {
    vPos    = aPos;
    vNormal = aNormal;
    vUv     = aUv;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}