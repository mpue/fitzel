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

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}

void main() {
    float H = uBoxHeight;
    // Where in its fall this drop is, and WHICH fall it is on. The buffer is
    // built once, so without the second number every drop runs down the same
    // column of air for ever: stand still in the rain and the same fourteen
    // thousand streaks cycle past the same fourteen thousand places, about
    // twice a second. That reads as a texture scrolling, not as weather.
    float ph  = (aBase.y - uTime * aSpeed) / H;
    float y   = fract(ph) * H;                            // wrapped fall height
    // Wrapped for the same reason lit.frag wraps its drop counter: at an hour of
    // uptime an unwrapped one is past where fract() of a product still has any
    // digits left to be random with.
    float cyc = mod(floor(ph), 97.0);
    // A fresh column each time round, about one drop-spacing wide -- enough that
    // no two consecutive falls trace the same line, small enough that the box
    // stays evenly full. Both ends of the streak share aBase and aSpeed, so they
    // agree on the cycle and the segment never tears across a wrap.
    vec2 jit = (vec2(hash21(aBase.xz + cyc * 0.53),
                     hash21(aBase.zx * 1.7 + cyc * 0.31)) - 0.5) * 1.8;
    vec3  dir = normalize(vec3(uWind.x, -1.0, uWind.z));  // direction of motion

    vec3 pos = vec3(uBoxCenter.x + aBase.x + jit.x,
                    uBoxCenter.y - H * 0.5 + y,
                    uBoxCenter.z + aBase.z + jit.y);
    pos -= dir * (aTop * uStreak);                        // tilt the streak

    vFade = 1.0 - smoothstep(uBoxHalf * 0.55, uBoxHalf, length(aBase.xz));
    gl_Position = uViewProj * vec4(pos, 1.0);
}
