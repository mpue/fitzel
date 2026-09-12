#version 330 core

// Motion vectors for the swaying trees (VegetationSystem::drawTreeMotion).
// TAA reprojects everything without a vector from depth and the camera alone,
// which is exact for a world that stands still -- and a smear for a crown in the
// wind: last frame's leaves are dragged over this frame's. Here each vertex is
// posed twice, now and one frame ago, through exactly tree.vert's arithmetic,
// so the depth test finds the fragments the lit pass wrote.

layout(location = 0) in vec3  aPos;
layout(location = 2) in vec2  aUv;
layout(location = 3) in vec3  iPos;
layout(location = 4) in float iRot;
layout(location = 5) in float iScale;

uniform mat4  uViewProj;      // this frame's, jittered (the depth test)
uniform mat4  uCurVP;         // this frame's, unjittered
uniform mat4  uPrevVP;        // last frame's, unjittered
uniform float uPrevWindTime;  // the wind clock one frame ago
uniform float uTreeHeight;
uniform int   uAlphaCutout;
uniform vec3  uCamPos;
uniform float uLodMin;
uniform float uLodNear;

#include "wind.glsl"
#include "treewind.glsl"

out vec4 vCur;
out vec4 vPrev;
out vec2 vUv;

void main() {
    float c = cos(iRot), s = sin(iRot);
    vec3 p = aPos * iScale;
    p = vec3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
    vec3 lp   = aPos / max(uTreeHeight, 1e-3);
    bool leaf = uAlphaCutout == 1;
    vec3 now  = iPos + p + treeWindAt(lp, iPos, iScale, iRot, leaf, uWindTime);
    vec3 then = iPos + p + treeWindAt(lp, iPos, iScale, iRot, leaf, uPrevWindTime);
    vCur  = uCurVP * vec4(now, 1.0);
    vPrev = uPrevVP * vec4(then, 1.0);
    vUv   = aUv;
    gl_Position = uViewProj * vec4(now, 1.0);
    float d = length(iPos.xz - uCamPos.xz);
    if (d < uLodMin || d > uLodNear) gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
}
