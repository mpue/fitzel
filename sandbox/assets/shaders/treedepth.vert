#version 330 core

layout(location = 0) in vec3  aPos;
layout(location = 2) in vec2  aUv;
layout(location = 3) in vec3  iPos;
layout(location = 4) in float iRot;
layout(location = 5) in float iScale;
layout(location = 6) in vec4  iTilt;   // as tree.vert
layout(location = 7) in float iStill;
layout(location = 9) in float iBurn;

uniform mat4  uLightSpace;
uniform float uTreeHeight;
uniform int   uAlphaCutout;

#include "wind.glsl"
#include "treewind.glsl"
#include "treeburn.glsl"

out vec2 vUv;
out float vBurn;
flat out float vBurnKey;

void main() {
    float c = cos(iRot), s = sin(iRot);
    vec3 p = aPos * iScale;
    p = vec3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
    // The same motion as tree.vert, so the shade under a tree moves with it.
    vec3 wp = iPos + treeTurn(iTilt, p + (1.0 - iStill) *
              treeWind(aPos / max(uTreeHeight, 1e-3), iPos, iScale, iRot, uAlphaCutout == 1));
    vUv = aUv;
    vBurn    = iBurn;
    vBurnKey = treeBurnKey(aPos / max(uTreeHeight, 1e-3));
    gl_Position = uLightSpace * vec4(wp, 1.0);
}
