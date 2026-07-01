#version 330 core

layout(location = 0) in vec3  aPos;   // local bird vertex (+Z forward, wings on X)
layout(location = 1) in float aFlap;  // 1 at wing tips, 0 at body
layout(location = 2) in vec3  iPos;    // world position
layout(location = 3) in float iYaw;    // heading
layout(location = 4) in float iPhase;  // wingbeat phase

uniform mat4  uViewProj;
uniform float uTime;
uniform float uSize;

out float vShade;

void main() {
    vec3 p = aPos * uSize;
    // Flap the wing tips up and down.
    p.y += sin(uTime * 6.0 + iPhase) * aFlap * uSize * 0.9;

    float c = cos(iYaw), s = sin(iYaw);
    p = vec3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);

    vec3 wp = iPos + p;
    vShade = 0.75 + 0.25 * aFlap; // wing tips a touch lighter
    gl_Position = uViewProj * vec4(wp, 1.0);
}
