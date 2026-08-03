#version 330 core

// Radial speed blur for the chase-cam racer. The world streaks outward from a
// focus point (the craft), the streak growing with distance from the focus so
// the craft stays sharp and the surroundings smear past it -- the classic
// "going fast" look. `uAmount` is driven by the craft's speed in C++, so the
// effect fades in as you accelerate and vanishes at rest. Directional (samples
// along the radial line), so it reads as motion streaks, not a soft defocus.
//
// Two shaping terms keep it from washing the whole frame out (the WipeOut look):
//   * a dead zone around the focus, so the centre -- where the eye rests and the
//     craft sits -- stays crisp and only the outer frame streaks;
//   * a depth falloff, so the distant sky/clouds (no crisp edges to smear, just
//     turns to mush) keep still while the near ground lines carry the motion.

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uImage;
uniform sampler2D uDepth;   // scene depth, for the sky/distance falloff
uniform vec2  uCenter;      // focus point (the craft on screen), UV [0,1]
uniform float uAmount;      // streak length scale (0 = off); grows with speed
uniform int   uSamples;     // taps along each streak
uniform float uNear;        // camera near/far, to linearize depth
uniform float uFar;

float linearDepth(vec2 uv) {
    float d = texture(uDepth, uv).r * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - d * (uFar - uNear));
}

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    if (uAmount < 0.001 || uSamples <= 1) { FragColor = texture(uImage, uv); return; }

    vec2  dir = uv - uCenter;
    float r   = length(dir);

    // Dead zone + ramp: no streak near the focus, then a steep build-up outward so
    // the centre reads sharp and the effect lives at the edges of the frame.
    float ramp = smoothstep(0.16, 0.6, r);
    ramp *= ramp;

    // Distance falloff: fade the streak out for far pixels (the sky/clouds), where
    // radial blur only softens the image instead of selling speed.
    float far01    = clamp((linearDepth(uv) - uFar * 0.35) / (uFar * 0.6), 0.0, 1.0);
    float skyAtten = 1.0 - 0.85 * far01;

    float amt = uAmount * ramp * skyAtten;
    if (amt < 0.0005) { FragColor = texture(uImage, uv); return; }

    // Streak vector: from this pixel back toward the focus -> radial stretch.
    vec2 streak = dir * amt;

    vec3 sum = vec3(0.0);
    for (int i = 0; i < uSamples; ++i) {
        float t = float(i) / float(uSamples - 1); // 0 (here) .. 1 (toward focus)
        sum += texture(uImage, uv - streak * t).rgb;
    }
    FragColor = vec4(sum / float(uSamples), 1.0);
}
