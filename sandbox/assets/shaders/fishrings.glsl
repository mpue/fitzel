// --- Rings where a fish rose or fell back in (Wildlife.hpp) ---------------------
// Shared by the lake (water.frag) and the brooks and rivers (river.frag): the
// host hands both the same list, and each surface shows the rings on it.
//
// A packet of waves running out from the spot: crests behind a front that
// travels at a walking pace, the wavelength stretching as it spreads (the
// short waves are slower, the train sorts itself out), fading as the ring
// grows and the energy thins. `px` is the SMALLER of the pixel's two
// footprints: on water seen at a glancing angle a pixel is a metre deep but
// a few centimetres wide, and a ring is still read along its sides.
uniform int  uFishRingCount;
uniform vec4 uFishRing[6];      // xz, age (s), strength

vec3 fishRings(vec3 N, vec2 wp, float px, float amount) {
    vec2 tilt = vec2(0.0);
    for (int i = 0; i < 6; ++i) {
        if (i >= uFishRingCount) break;
        vec2  d     = wp - uFishRing[i].xy;
        float r     = length(d);
        float age   = uFishRing[i].z;
        float front = 0.04 + age * 0.45;
        if (r > front + 0.05) continue;
        float lambda = 0.07 + age * 0.05;
        float k      = 6.2831853 / lambda;
        float env    = exp(-(front - r) / (0.12 + age * 0.18))
                     * smoothstep(front + 0.05, front - 0.03, r);
        float amp    = uFishRing[i].w * env * exp(-age * 0.55) / sqrt(1.0 + r * 3.0);
        amp *= 1.0 - smoothstep(lambda * 0.35, lambda * 1.3, px);
        tilt += (d / max(r, 1e-3)) * cos((r - front) * k) * amp;
    }
    N.xz += tilt * 0.7 * amount;
    return normalize(N);
}
