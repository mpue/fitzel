#version 330 core

layout(location = 0) in vec3  aPos;
layout(location = 2) in vec2  aUv;
layout(location = 3) in vec3  iPos;
layout(location = 4) in float iRot;
layout(location = 5) in float iScale;

uniform mat4  uLightSpace;
uniform float uTime;
uniform vec2  uWindDir;
uniform float uWindStrength;
uniform float uTreeHeight;

out vec2 vUv;

void main() {
    float c = cos(iRot), s = sin(iRot);
    vec3 p = aPos * iScale;
    p = vec3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
    vec3 wp = iPos + p;

    float w = clamp(aPos.y / uTreeHeight, 0.0, 1.0);
    w = w * w;
    float sway = sin(uTime * 1.1 + iPos.x * 0.2 + iPos.z * 0.2);
    wp.xz += uWindDir * (uWindStrength * 0.5) * w * (0.5 + 0.5 * sway) * iScale;

    vUv = aUv;
    gl_Position = uLightSpace * vec4(wp, 1.0);
}
