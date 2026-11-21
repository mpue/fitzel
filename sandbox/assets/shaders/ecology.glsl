// --- Where the forest is (see sandbox/src/Ecology.hpp) --------------------------
//
// The GPU copy of ecology::sample, operation for operation, so a canopy painted
// on the far terrain or a forest floor under the near one lands exactly where
// the CPU put the trees. uint arithmetic wraps the same on both sides.

uniform int   uEcoOn;          // 0 = no ecology in this scene
uniform float uEcoCover;
uniform float uEcoStandSize;
uniform float uEcoTreeLine;
uniform float uEcoWater;
uniform float uEcoSolitary;
uniform float uEcoSlopeLove;
uniform int   uEcoTowns;       // towns' open land: discs (x, z, radius), see Ecology.hpp
uniform vec4  uEcoTown[8];
// A real place's woods (ecology::Mask): R forest, G settled, B field, A covered.
// uEcoMaskRect = x0, z0 (texel 0's centre), cell, texels per side; w = 0: none.
uniform sampler2D uEcoMask;
uniform vec4      uEcoMaskRect;

vec4 ecoMaskAt(vec2 xz) {
    if (uEcoMaskRect.w < 2.0) return vec4(0.0);
    vec2 f = (xz - uEcoMaskRect.xy) / uEcoMaskRect.z;
    if (f.x < 0.0 || f.y < 0.0 || f.x > uEcoMaskRect.w - 1.0 || f.y > uEcoMaskRect.w - 1.0) return vec4(0.0);
    return textureLod(uEcoMask, (f + 0.5) / uEcoMaskRect.w, 0.0);
}

uint ecoHash(int x, int z, uint seed) {
    uint h = uint(x) * 0x8da6b343u ^ uint(z) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 13u;
    h *= 0x5bd1e995u;
    h ^= h >> 15u;
    return h;
}

float ecoNoise(vec2 p, uint seed) {
    vec2  fl = floor(p);
    int   ix = int(fl.x), iz = int(fl.y);
    vec2  t  = p - fl;
    t = t * t * (3.0 - 2.0 * t);
    const float k = 1.0 / 16777216.0;
    float a = float(ecoHash(ix,     iz,     seed) & 0xffffffu) * k;
    float b = float(ecoHash(ix + 1, iz,     seed) & 0xffffffu) * k;
    float c = float(ecoHash(ix,     iz + 1, seed) & 0xffffffu) * k;
    float d = float(ecoHash(ix + 1, iz + 1, seed) & 0xffffffu) * k;
    float ab = a + (b - a) * t.x;
    float cd = c + (d - c) * t.x;
    return ab + (cd - ab) * t.y;
}

float ecoFbm(vec2 p, int octaves, uint seed) {
    float sum = 0.0, amp = 0.5, norm = 0.0;
    for (int i = 0; i < octaves; ++i) {
        sum  += amp * ecoNoise(p, seed + uint(i) * 101u);
        norm += amp;
        p     = p * 2.03 + vec2(17.3, -9.1);
        amp  *= 0.5;
    }
    return sum / norm;
}

// How much xz lies in a town's open land, 0..1 (ecology::settled).
float ecoSettledDiscs(vec2 xz) {
    if (uEcoTowns <= 0) return 0.0;
    float s   = 0.0;
    float rag = ecoNoise(xz / 23.0, 0x7e11u) - 0.5;
    for (int i = 0; i < 8; ++i) {
        if (i >= uEcoTowns) break;
        vec4 t = uEcoTown[i];
        if (t.z <= 1.0) continue;
        float d = length(xz - t.xy) / t.z + 0.25 * rag;
        s = max(s, 1.0 - smoothstep(0.8, 1.15, d));
    }
    return s;
}

float ecoSettled(vec2 xz) {
    float s = ecoSettledDiscs(xz);
    vec4  m = ecoMaskAt(xz);
    return max(s * (1.0 - m.a), m.g * m.a);
}

// x: trees per cell (0..1), y: inside a stand (0 meadow .. 1 wood), z: edge.
vec3 ecoSample(vec2 xz, float h, float ny) {
    if (uEcoOn == 0) return vec3(0.0);
    float stands = ecoFbm(xz / uEcoStandSize, 4, 0x51edu);
    float ragged = ecoFbm(xz / (uEcoStandSize * 0.18), 2, 0x3a7cu);
    float f      = stands + 0.22 * (ragged - 0.5) + uEcoSlopeLove * (1.0 - ny);
    float cut    = 0.5 + (0.5 - uEcoCover) * 0.45;
    float forest = smoothstep(cut - 0.035, cut + 0.035, f) * (1.0 - ecoSettled(xz));
    float lone   = uEcoSolitary;
    vec4  m      = ecoMaskAt(xz);
    if (m.a > 0.0) {
        float mf = smoothstep(0.30, 0.70, m.r + 0.18 * (ragged - 0.5));
        forest = mix(forest, mf * (1.0 - m.g), m.a);
        lone   = mix(lone, uEcoSolitary * (1.0 + 1.5 * m.g) * (1.0 - m.b), m.a);
    }
    float edge   = 4.0 * forest * (1.0 - forest);
    float d      = max(forest, lone);
    d *= smoothstep(0.62, 0.72, ny);
    float tl = uEcoTreeLine + (ragged - 0.5) * 180.0;
    d *= 1.0 - smoothstep(tl - 140.0, tl + 30.0, h);
    d *= smoothstep(uEcoWater + 0.6, uEcoWater + 1.6, h);
    return vec3(d, forest, edge);
}
