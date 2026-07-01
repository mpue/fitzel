#version 330 core

in vec2  vUV;
in float vPhase;
out vec4 FragColor;

uniform float uTime;
uniform float uNight;  // 0 day .. 1 night (fades the fireflies in)
uniform vec3  uColor;  // warm yellow-green glow (HDR)

void main() {
    float d = length(vUV);
    if (d > 1.0) discard;
    // Soft round core with a falloff so it reads as a glowing point.
    float glow = smoothstep(1.0, 0.0, d);
    glow *= glow;

    // Blink: sharp pulses, each firefly on its own phase, sometimes dark.
    float pulse = 0.5 + 0.5 * sin(uTime * 2.5 + vPhase * 6.2831853);
    pulse = pow(pulse, 3.0);

    float b = uNight * pulse;
    // Additive (blend ONE, ONE): emit HDR so bloom picks up a halo.
    FragColor = vec4(uColor * glow * b * 3.0, 1.0);
}
