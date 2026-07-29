#version 330 core

layout(location = 0) in vec3  aPos;    // local flower vertex
layout(location = 1) in vec3  aNormal;
layout(location = 2) in float aTint;   // 0 = stem (green), 1 = bloom (iColor)
layout(location = 7) in float aPetal;  // petal index (0..kMaxPetals-1), -1 = stem/centre
layout(location = 3) in vec3  iPos;     // base position (world)
layout(location = 4) in float iYaw;
layout(location = 5) in float iScale;
layout(location = 6) in vec3  iColor;   // bloom colour

uniform mat4  uViewProj;
uniform float uTime;
uniform vec2  uWindDir;
uniform float uWindStrength;

out vec3  vWorldPos;
out vec3  vNormal;
out vec3  vColor;
out float vTint;

// Must match Primitives.hpp (kFlowerMaxPetals / kFlowerStemTop).
const float kMaxPetals = 8.0;
const float kStemTop   = 0.50;
const float TAU        = 6.2831853;

// Per-bloom randomness derived from the world position, so every flower differs
// without carrying extra instance data (and keeps the same look wherever it is
// placed -- the field re-scatters as the camera streams around).
float rnd(vec2 p, float salt) {
    return fract(sin(dot(p, vec2(12.9898, 78.233)) + salt) * 43758.5453);
}

// Rotate v about the Y axis by (c,s) = (cos,sin).
vec3 rotY(vec3 v, float c, float s) {
    return vec3(v.x * c - v.z * s, v.y, v.x * s + v.z * c);
}

void main() {
    float r1 = rnd(iPos.xz, 0.0);
    float r2 = rnd(iPos.xz, 1.7);
    float r3 = rnd(iPos.xz, 3.3);
    float r4 = rnd(iPos.xz, 5.1);

    float petals = floor(5.0 + r1 * 4.0);   // 5..8 petals per bloom
    float tilt   = (r2 - 0.5) * 0.7;        // >0 cups the bloom, <0 opens it flat
    float lean   = r3 * 0.20;               // the whole stem leans a little
    float leanA  = r4 * TAU;                // ...in a random direction

    vec3 lp = aPos;
    vec3 nrm = aNormal;
    if (aPetal >= 0.0) {
        if (aPetal > petals - 0.5) {        // surplus petal: clip it away
            gl_Position = vec4(0.0, 0.0, -2.0, 1.0);
            return;
        }
        // Re-space the remaining petals evenly for the actual count.
        float da = aPetal * (TAU / petals - TAU / kMaxPetals);
        lp  = rotY(lp,  cos(da), sin(da));
        nrm = rotY(nrm, cos(da), sin(da));
        // Then swing the petal open/closed around the rim of the stem top: the
        // (radial, up) offset from the bloom base rotates by `tilt`.
        vec2 rad = lp.xz;
        float rl = length(rad);
        if (rl > 1e-5) {
            vec2  rd = rad / rl;
            vec2  td = vec2(-rd.y, rd.x);        // tangent, unaffected by the tilt
            float dy = lp.y - kStemTop;
            float ct = cos(tilt), st = sin(tilt);
            float r2d = rl * ct - dy * st;
            lp = vec3(rd.x * r2d, kStemTop + rl * st + dy * ct, rd.y * r2d);
            float nr = dot(nrm.xz, rd), nt = dot(nrm.xz, td);
            vec2  nxz = rd * (nr * ct - nrm.y * st) + td * nt;
            nrm = vec3(nxz.x, nr * st + nrm.y * ct, nxz.y);
        }
    }

    // Lean the whole flower (Rodrigues about a horizontal axis).
    vec3  ax = vec3(cos(leanA), 0.0, sin(leanA));
    float cl = cos(lean), sl = sin(lean);
    lp  = lp  * cl + cross(ax, lp)  * sl + ax * dot(ax, lp)  * (1.0 - cl);
    nrm = nrm * cl + cross(ax, nrm) * sl + ax * dot(ax, nrm) * (1.0 - cl);

    float c = cos(iYaw), s = sin(iYaw);
    lp  = rotY(lp,  c, s) * iScale;
    nrm = rotY(nrm, c, s);

    // Gentle sway, stronger toward the bloom (higher local y).
    float sway = sin(uTime * 1.4 + iPos.x * 0.3 + iPos.z * 0.3);
    lp.xz += uWindDir * (uWindStrength * 0.5) * sway * aPos.y * iScale;

    vec3 wp = iPos + lp;
    vWorldPos = wp;
    vNormal   = nrm;
    vColor    = iColor;
    vTint     = aTint;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
