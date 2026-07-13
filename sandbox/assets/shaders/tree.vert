#version 330 core

layout(location = 0) in vec3  aPos;
layout(location = 1) in vec3  aNormal;
layout(location = 2) in vec2  aUv;
layout(location = 3) in vec3  iPos;   // instance base (world)
layout(location = 4) in float iRot;   // yaw
layout(location = 5) in float iScale;

uniform mat4  uViewProj;
uniform float uTime;
uniform vec2  uWindDir;
uniform float uWindStrength;
uniform float uTreeHeight; // local tree height, for sway weight
uniform vec3  uCamPos;
uniform float uLodMin;     // below this distance this LOD is clipped (finer LOD covers it)
uniform float uLodNear;    // beyond this, the next LOD / billboard takes over (clip the mesh)

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUv;

void main() {
    float c = cos(iRot), s = sin(iRot);
    vec3 p = aPos * iScale;
    p = vec3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
    vec3 n = vec3(aNormal.x * c - aNormal.z * s, aNormal.y, aNormal.x * s + aNormal.z * c);

    vec3 wp = iPos + p;

    // Crown sway: weight by height up the tree.
    float w = clamp(aPos.y / uTreeHeight, 0.0, 1.0);
    w = w * w;
    float sway = sin(uTime * 1.1 + iPos.x * 0.2 + iPos.z * 0.2);
    wp.xz += uWindDir * (uWindStrength * 0.5) * w * (0.5 + 0.5 * sway) * iScale;

    vWorldPos = wp;
    vNormal   = normalize(n);
    vUv       = aUv;
    gl_Position = uViewProj * vec4(wp, 1.0);

    // LOD banding: this mesh level only draws within [uLodMin, uLodNear); the
    // finer level covers nearer trees, the coarser level / billboard covers farther.
    float lodDist = length(iPos.xz - uCamPos.xz);
    if (lodDist < uLodMin || lodDist > uLodNear) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0); // outside the far plane -> clipped
    }
}
