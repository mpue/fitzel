#version 330 core

// The exposure meter: the frame's centre-weighted mean log2 luminance, eased
// towards over time, in one pixel.
//
// Two stages, because one fragment reading the whole frame is one GPU thread
// doing hundreds of dependent fetches from a texture the size of the screen --
// it cost a third of a millisecond to produce a single number.
//
//   uStage 0  a 16x9 target: each fragment reduces its own cell of the HDR
//             frame with an 8x8 grid of taps, in parallel with the others.
//             Out: (sum of weight * log2 L, sum of weight).
//   uStage 1  the 1x1 result: adds up the 144 cells and eases the previous
//             value towards the new reading.
//             Out: R = adapted (what the composite exposes by), G = this
//             frame's raw reading (for the editor's readout).
//
// Log-domain on purpose: a frame that is a tenth sky must not be judged by the
// sky alone, which a plain mean of radiance would do -- the sun is thousands of
// times brighter than the ground. Centre-weighted, because what is in the
// middle of the frame is what is being looked at.

in vec2 vNdc;
out vec4 FragColor;

uniform int   uStage;
uniform sampler2D uSrc;    // stage 0: the HDR frame; stage 1: the 16x9 cells
uniform sampler2D uPrev;   // stage 1: last frame's 1x1 result
uniform float uBlend;      // stage 1: 0..1 of the way to the reading; 1 = snap

const ivec2 CELLS = ivec2(16, 9);
const int   TAPS  = 8;     // per cell, per axis

void main() {
    if (uStage == 0) {
        ivec2 cell = ivec2(gl_FragCoord.xy);
        float sumL = 0.0, sumW = 0.0;
        for (int y = 0; y < TAPS; ++y) {
            for (int x = 0; x < TAPS; ++x) {
                vec2  uv = (vec2(cell) + (vec2(x, y) + 0.5) / float(TAPS)) / vec2(CELLS);
                vec3  c  = textureLod(uSrc, uv, 0.0).rgb;
                float l  = dot(c, vec3(0.2126, 0.7152, 0.0722));
                if (isnan(l) || isinf(l)) continue;
                vec2  d = (uv - 0.5) * vec2(1.6, 1.0);
                float w = exp(-dot(d, d) * 3.0);
                sumL += log2(clamp(l, 1e-4, 6.0e4)) * w;
                sumW += w;
            }
        }
        FragColor = vec4(sumL, sumW, 0.0, 1.0);
        return;
    }

    float sumL = 0.0, sumW = 0.0;
    for (int y = 0; y < CELLS.y; ++y)
        for (int x = 0; x < CELLS.x; ++x) {
            vec2 s = texelFetch(uSrc, ivec2(x, y), 0).rg;
            sumL += s.x;
            sumW += s.y;
        }
    float measured = sumL / max(sumW, 1e-6);
    float prev     = texelFetch(uPrev, ivec2(0), 0).r;
    if (isnan(prev) || isinf(prev)) prev = measured;
    FragColor = vec4(mix(prev, measured, clamp(uBlend, 0.0, 1.0)), measured, 0.0, 1.0);
}
