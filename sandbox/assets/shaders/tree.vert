#version 330 core

layout(location = 0) in vec3  aPos;
layout(location = 1) in vec3  aNormal;
layout(location = 2) in vec2  aUv;
layout(location = 3) in vec3  iPos;   // instance base (world)
layout(location = 4) in float iRot;   // yaw
layout(location = 5) in float iScale;

uniform mat4  uViewProj;
uniform float uTreeHeight; // local tree height, for sway weight
uniform vec3  uCamPos;
uniform float uLodMin;     // below this distance this LOD is clipped (finer LOD covers it)
uniform float uLodNear;    // beyond this, the next LOD / billboard takes over (clip the mesh)
uniform int   uAlphaCutout;// 1 = this draw is foliage (it flutters)
// The handover to the impostors (treedither.glsl): across [uHandover - width,
// uHandover] this mesh gives its pixels up. Width 0 = no impostors, no fade.
uniform float uHandover;
uniform float uHandoverWidth;

#include "wind.glsl"
#include "treewind.glsl"

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;
out float vShare;

void main() {
    float c = cos(iRot), s = sin(iRot);
    vec3 p = aPos * iScale;
    p = vec3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
    vec3 n = vec3(aNormal.x * c - aNormal.z * s, aNormal.y, aNormal.x * s + aNormal.z * c);

    vec3 wp = iPos + p + treeWind(aPos / max(uTreeHeight, 1e-3), iPos, iScale, iRot,
                                  uAlphaCutout == 1);

    vWorldPos = wp;
    vNormal   = normalize(n);
    vUv       = aUv;
    gl_Position = uViewProj * vec4(wp, 1.0);

    // LOD banding: this mesh level only draws within [uLodMin, uLodNear); the
    // finer level covers nearer trees, the coarser level / billboard covers farther.
    float lodDist = length(iPos.xz - uCamPos.xz);
    vShare = (uHandoverWidth > 0.0)
           ? 1.0 - smoothstep(uHandover - uHandoverWidth, uHandover, lodDist) : 1.0;
    if (lodDist < uLodMin || lodDist > uLodNear) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // outside the far plane -> clipped
    }
}
