// --- The wind -------------------------------------------------------------------
//
// One air for everything that moves in it: grass, flowers, the trees and their
// impostors. It has a direction that veers slowly, a mean strength, and gusts:
// patches of faster air a few dozen metres across, carried downwind at gust
// speed, so a squall is seen coming -- it runs over the meadow as a darker,
// flattened band, reaches the wood a moment later, and the trees bow in the
// order it reaches them. That travelling structure is most of what makes a
// landscape look alive rather than animated.
//
// Mirrored for the grass path tracer in sandbox/src/Wind.hpp (gust only).

uniform vec2  uWindDir;       // unit, in XZ: where the air is going
uniform float uWindStrength;  // mean strength, 0 calm .. 1 a stiff breeze .. 1.5 storm
uniform float uWindGust;      // 0 steady .. 1 very gusty
uniform float uWindTime;      // seconds: the gust clock

uint windHash(int x, int z, uint seed) {
    uint h = uint(x) * 0x8da6b343u ^ uint(z) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13u;
    h *= 0x5bd1e995u;
    h ^= h >> 15u;
    return h;
}
float windNoise(vec2 p, uint seed) {
    vec2  fl = floor(p);
    int   ix = int(fl.x), iz = int(fl.y);
    vec2  t  = p - fl;
    t = t * t * (3.0 - 2.0 * t);
    const float k = 1.0 / 16777216.0;
    float a = float(windHash(ix,     iz,     seed) & 0xffffffu) * k;
    float b = float(windHash(ix + 1, iz,     seed) & 0xffffffu) * k;
    float c = float(windHash(ix,     iz + 1, seed) & 0xffffffu) * k;
    float d = float(windHash(ix + 1, iz + 1, seed) & 0xffffffu) * k;
    return mix(mix(a, b, t.x), mix(c, d, t.x), t.y);
}

// How much of the mean wind is blowing at xz at time t: ~1 on average, up to
// ~1.5 in a gust, ~0.35 in the lull behind one.
float windGustAt(vec2 xz, float t) {
    const float kSize  = 55.0;   // metres across a gust
    const float kSpeed = 7.0;    // metres per second it travels
    vec2  q = (xz - uWindDir * (t * kSpeed)) / kSize;
    float n = 0.62 * windNoise(q, 0x9e37u)
            + 0.38 * windNoise(q * 2.7 + vec2(3.1, 7.7), 0x7f4au);
    float g = smoothstep(0.30, 0.80, n);
    return mix(1.0, mix(0.35, 1.5, g), uWindGust);
}

float windGust(vec2 xz) { return windGustAt(xz, uWindTime); }
