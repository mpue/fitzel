#version 330 core

layout(location = 0) in vec3  aPos;
layout(location = 2) in vec2  aUv;
layout(location = 3) in vec3  iPos;
layout(location = 4) in float iRot;
layout(location = 5) in float iScale;

uniform mat4  uLightSpace;
uniform float uTreeHeight;
uniform int   uAlphaCutout;

#include "wind.glsl"
#include "treewind.glsl"

out vec2 vUv;

void main() {
    float c = cos(iRot), s = sin(iRot);
    vec3 p = aPos * iScale;
    p = vec3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
    // The same motion as tree.vert, so the shade under a tree moves with it.
    vec3 wp = iPos + p + treeWind(aPos / max(uTreeHeight, 1e-3), iPos, iScale, iRot,
                                  uAlphaCutout == 1);
    vUv = aUv;
    gl_Position = uLightSpace * vec4(wp, 1.0);
}
