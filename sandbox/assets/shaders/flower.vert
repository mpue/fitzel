#version 330 core

layout(location = 0) in vec3  aPos;    // local flower vertex
layout(location = 1) in vec3  aNormal;
layout(location = 2) in float aTint;   // 0 = stem (green), 1 = bloom (iColor)
layout(location = 3) in vec3  iPos;     // base position (world)
layout(location = 4) in float iYaw;
layout(location = 5) in float iScale;
layout(location = 6) in vec3  iColor;   // bloom colour

uniform mat4  uViewProj;
uniform float uTime;
uniform vec2  uWindDir;
uniform float uWindStrength;

out vec3  vWorldPos;
out vec3  vNormal;
out vec3  vColor;
out float vTint;

void main() {
    float c = cos(iYaw), s = sin(iYaw);
    vec3 lp = aPos * iScale;
    lp = vec3(lp.x * c - lp.z * s, lp.y, lp.x * s + lp.z * c);

    // Gentle sway, stronger toward the bloom (higher local y).
    float sway = sin(uTime * 1.4 + iPos.x * 0.3 + iPos.z * 0.3);
    lp.xz += uWindDir * (uWindStrength * 0.5) * sway * aPos.y * iScale;

    vec3 wp = iPos + lp;
    vec3 n  = vec3(aNormal.x * c - aNormal.z * s, aNormal.y, aNormal.x * s + aNormal.z * c);
    vWorldPos = wp;
    vNormal   = n;
    vColor    = iColor;
    vTint     = aTint;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
