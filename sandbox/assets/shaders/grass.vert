#version 330 core

layout(location = 0) in vec2  aBlade;  // x in [-0.5,0.5], h01 in [0,1]
layout(location = 1) in vec3  iPos;    // blade base (world)
layout(location = 2) in float iRot;    // yaw
layout(location = 3) in float iHeight; // blade height
layout(location = 4) in float iPhase;  // sway phase
layout(location = 5) in float iLush;   // 0 dry .. 1 lush (biome moisture)

uniform mat4  uViewProj;
uniform float uTime;
uniform vec2  uWindDir;
uniform float uWindStrength;

out float vH;
out vec3  vWorldPos;
out vec3  vNormal;
out float vLush;

void main() {
    float h01 = aBlade.y;
    float w   = 0.016 * (1.0 - 0.4 * h01);       // thin blade, tapering to the tip
    vec3  local = vec3(aBlade.x * 2.0 * w, h01 * iHeight, 0.0);

    float c = cos(iRot), s = sin(iRot);
    local = vec3(local.x * c, local.y, local.x * s); // yaw the blade

    // Wind: the tip bends in the wind direction, proportional to height^2.
    float sway = sin(uTime * 1.6 + iPhase + iPos.x * 0.25 + iPos.z * 0.25);
    float bend = uWindStrength * (0.35 + 0.65 * sway) * h01 * h01;
    local.xz += uWindDir * bend;

    vec3 wp = iPos + local;
    vWorldPos = wp;
    vH = h01;
    vLush = iLush;
    vNormal = normalize(vec3(-s, 1.2, c)); // blade facing, biased upward
    gl_Position = uViewProj * vec4(wp, 1.0);
}
