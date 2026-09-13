#version 330 core

// The mountains' shadow on the valley (FarTerrain::renderSunShadow): for every
// point of an 8 km square around the eye, the height a thing standing there
// must reach to see the sun over the ranges -- the highest the sun's line of
// sight has to climb anywhere along its way. The cascades shadow what is near;
// this is what makes a valley go into shade while its summits still burn.
//
// Marched over the far terrain's own heightfields (the finest ring that holds
// each step), so the shadow falls from the mountains you see.

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uL0;
uniform sampler2D uL1;
uniform sampler2D uL2;
uniform sampler2D uL3;
uniform sampler2D uL4;
uniform vec4  uR0, uR1, uR2, uR3, uR4;   // origin xz, size, 1 = valid
uniform float uGrid;

uniform vec2  uOrigin;       // world XZ of the map's corner
uniform float uSize;
uniform vec3  uSunDir;

vec2 ringUv(vec2 xz, vec4 r) {
    float cell = r.z / (uGrid - 1.0);
    return ((xz - r.xy) / cell + 0.5) / uGrid;
}
bool inside(vec2 uv) { return all(greaterThan(uv, vec2(0.003))) && all(lessThan(uv, vec2(0.997))); }

float groundAt(vec2 xz) {
    vec2 uv;
    uv = ringUv(xz, uR0); if (uR0.w > 0.5 && inside(uv)) return textureLod(uL0, uv, 0.0).r;
    uv = ringUv(xz, uR1); if (uR1.w > 0.5 && inside(uv)) return textureLod(uL1, uv, 0.0).r;
    uv = ringUv(xz, uR2); if (uR2.w > 0.5 && inside(uv)) return textureLod(uL2, uv, 0.0).r;
    uv = ringUv(xz, uR3); if (uR3.w > 0.5 && inside(uv)) return textureLod(uL3, uv, 0.0).r;
    uv = ringUv(xz, uR4); if (uR4.w > 0.5 && inside(uv)) return textureLod(uL4, uv, 0.0).r;
    return -1.0e4;                               // off every map: nothing there
}

void main() {
    vec2  xz = uOrigin + (vNdc * 0.5 + 0.5) * uSize;
    vec3  L  = normalize(uSunDir);
    float horiz = length(L.xz);
    if (L.y <= 0.0 || horiz < 1e-4) { FragColor = vec4(-1.0e4); return; }
    float tanE = L.y / horiz;
    vec2  d    = L.xz / horiz;
    // From a few dozen metres out -- the ground right here is the cascades'
    // business and the slope's own light -- to the far side of the ranges.
    float need = -1.0e4;
    float t = 40.0;
    for (int i = 0; i < 120; ++i) {
        float g = groundAt(xz + d * t);
        need = max(need, g - t * tanE);
        t *= 1.065;
        // No summit reaches 5 km: past where even that could matter, stop.
        if (t > 45000.0 || 5000.0 - t * tanE < need) break;
    }
    FragColor = vec4(need);
}
