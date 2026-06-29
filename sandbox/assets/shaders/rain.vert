#version 330 core

layout(location = 0) in vec3  aBase;  // x,z offset in [-half,half]; y seed in [0,H]
layout(location = 1) in float aSpeed; // fall speed
layout(location = 2) in float aTop;   // 0 = bottom of streak, 1 = top

uniform mat4  uViewProj;
uniform vec3  uBoxCenter; // box follows the camera
uniform float uBoxHeight;
uniform float uBoxHalf;
uniform float uStreak;    // streak length
uniform float uTime;
uniform vec3  uWind;      // horizontal slant

out float vFade;

void main() {
    float H = uBoxHeight;
    float y = mod(aBase.y - uTime * aSpeed, H);          // wrapped fall height
    vec3  dir = normalize(vec3(uWind.x, -1.0, uWind.z));  // direction of motion

    vec3 pos = vec3(uBoxCenter.x + aBase.x,
                    uBoxCenter.y - H * 0.5 + y,
                    uBoxCenter.z + aBase.z);
    pos -= dir * (aTop * uStreak);                        // tilt the streak

    vFade = 1.0 - smoothstep(uBoxHalf * 0.55, uBoxHalf, length(aBase.xz));
    gl_Position = uViewProj * vec4(pos, 1.0);
}
