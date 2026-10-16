#version 330 core

// The open water's surface: rings around the camera (OceanRenderer.cpp),
// displaced by the spectral ocean's three cascades (Ocean.hpp) and by the
// waves things make (WaveSim.hpp).
layout(location = 0) in vec3 aPos;  // xz: offset from the camera, metres; y: vertex spacing

uniform mat4  uViewProj;
uniform vec3  uGridCentre;          // camera x, still water level, camera z

// The ocean: per cascade, displacement (dx, height, dz, foam) on a repeating
// patch of uPatch[i] metres, uOceanN texels across.
uniform sampler2D uDisp0;
uniform sampler2D uDisp1;
uniform sampler2D uDisp2;
uniform vec3  uPatch;
uniform float uOceanN;
uniform float uWaveHs;              // significant wave height, for the crest factor

// The waves things make: height (and slope) on a patch around the camera.
uniform sampler2D uWaveTex;
uniform vec2  uWaveOrigin;
uniform float uWaveExtent;

// The scene under the water, to calm the sea where it gets shallow.
uniform sampler2D uRefractionDepth;
uniform float uNear;
uniform float uFar;
uniform int   uOrtho;
// 1 when drawn over the far terrain's lakes (see main), in ITS depth range: the
// refraction pass's depth means nothing there, and so much water is open sea.
uniform int   uFarPass;

// The coast (ShoreMap.hpp): depth, distance to the shore (both negative on
// land) and the direction to it, around the eye.
uniform sampler2D uShoreTex;
uniform int   uShoreOn;
uniform vec2  uShoreOrigin;
uniform float uShoreExtent;
uniform vec3  uBreakers;     // height arriving, length in the surf, speed running in
uniform float uTime;

out vec4  vClip;
out vec3  vWorldPos;
out vec2  vRestXZ;   // where this water is at rest: the cascades are read there
out float vCrest;    // height over the significant height: high on crests
out float vCalm;     // 0 in the shallows and on land, 1 in open water
out float vBreak;    // 0..1: this is the face of a breaking wave
out float vSurf;     // 0..1: in the surf zone, where the broken water churns
out vec2  vShoreSlope;   // the breakers' own slope, for the pixel normal
out float vShorePhase;   // 0 just behind a breaker's crest, rising to 1 at the next
out float vColumn;       // water over the ground here, m (-1: not known)

float linearDepth(float d) {
    if (uOrtho == 1) return uNear + d * (uFar - uNear);
    float z = d * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - z * (uFar - uNear));
}

// One cascade at a mip as coarse as the mesh is here: a wave shorter than two
// vertices cannot be drawn by them, and sampled at full detail it would slide
// through the grid as the camera moves -- the "swimming" of a water mesh. The
// pixel normal (water.frag) carries those waves instead.
vec3 cascade(sampler2D t, vec2 xz, float L, float spacing) {
    float lod = max(log2(spacing * uOceanN / L), 0.0);
    return textureLod(t, xz / L + 0.5 / uOceanN, lod).xyz;
}

void main() {
    vec2  rest    = uGridCentre.xz + aPos.xz;
    float spacing = aPos.y;
    vec3  d = cascade(uDisp0, rest, uPatch.x, spacing)
            + cascade(uDisp1, rest, uPatch.y, spacing)
            + cascade(uDisp2, rest, uPatch.z, spacing);

    // The sea calms where the bed comes up. Read off the refraction pass's
    // depth at this vertex: the water column along the view, which near the
    // shore is short -- and negative over a beach, which that pass keeps up to
    // a wave's height above the still water (main) so a crest knows not to
    // stand up through it. Nothing there at all is open, deep water: a scene
    // with no ground under its sea is still a sea.
    vCalm = 1.0;
    vec4 c0 = uViewProj * vec4(rest.x, uGridCentre.y, rest.y, 1.0);
    if (uFarPass == 0 && c0.w > 0.0) {
        vec2 uv = c0.xy / c0.w * 0.5 + 0.5;
        if (all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) {
            float raw = textureLod(uRefractionDepth, uv, 0.0).r;
            float waterZ = linearDepth(c0.z / c0.w * 0.5 + 0.5);
            if (raw < 0.99999)
                vCalm = smoothstep(0.0, 4.0, linearDepth(raw) - waterZ);
        }
    }
    // --- The breakers ---------------------------------------------------------
    // Waves that feel the bottom. Rows of them run in square to the beach --
    // the direction the shore map gives -- and as the water shallows each one
    // stands taller (shoaling) and steeper, until it is too tall for the water
    // under it (about 0.8 of the depth), tips over and goes on as a lower,
    // churning bore of foam that runs up the sand and slides back.
    vBreak = 0.0;
    vSurf  = 0.0;
    vShoreSlope = vec2(0.0);
    vShorePhase = 0.5;
    vColumn = -1.0;
    vec3 surf = vec3(0.0);
    bool land = false;
    if (uShoreOn == 1) {
        vec2 suv = (rest - uShoreOrigin) / uShoreExtent;
        float inMap = smoothstep(0.0, 0.06, min(min(suv.x, suv.y), min(1.0 - suv.x, 1.0 - suv.y)));
        if (inMap > 0.0) {
            vec4  sh    = textureLod(uShoreTex, suv, 0.0);
            float depth = sh.x, dist = sh.y;
            vec2  dir   = sh.zw;
            // The map knows the depth far better than the refraction pass's
            // view along a ray does; it decides how calm the sea is here.
            vCalm = mix(vCalm, smoothstep(0.0, 4.0, depth), inMap);
            land  = depth <= 0.0 && inMap > 0.5;
            float H0 = uBreakers.x, L = max(uBreakers.y, 2.0);
            if (H0 > 0.0 && dot(dir, dir) > 0.01 && dist < 6.0 * L && dist > -0.6 * L) {
                float k     = 6.2831853 / L;
                float omega = k * uBreakers.z;
                // Gerstner's phase for a wave running along `dir` (dist falls
                // that way): crests at sin = 1.
                float theta = -(k * dist + omega * uTime);
                // Sets: a few big ones, then a lull -- and not the same all
                // along the beach.
                vec2  along = vec2(-dir.y, dir.x);
                float set   = 0.55 + 0.45 * sin(theta * 0.17 + dot(rest, along) * 0.021);
                // Waves only feel the bottom within half a wavelength of it.
                float feel  = 1.0 - smoothstep(0.25 * L, 0.6 * L, depth);
                float dpos  = max(depth, 0.0);
                float H     = H0 * set * clamp(pow(0.25 * L / max(dpos, 0.25), 0.25), 1.0, 1.9);
                float ratio = H / max(dpos, 0.05);
                float brk   = smoothstep(0.55, 0.9, ratio);
                // Broken, it is a bore no taller than the water lets it be.
                H = mix(H, min(H, 0.78 * dpos + 0.08), brk);
                float c     = 0.5 + 0.5 * sin(theta);
                float sharp = mix(1.2, 3.5, max(brk, feel * 0.4));
                float y     = H * (pow(c, sharp) - 0.32);
                // Up the sand: the surge after each breaker, thinning inland.
                // A third of the breaker's height or so: what runs up a beach
                // is a thin sheet, and only the biggest of a set gets far.
                if (depth <= 0.0) {
                    float run = 0.35 * H0 * set * set;
                    y = run * smoothstep(-0.35 * L, 0.0, dist) * max(sin(theta) * 0.5 + 0.5, 0.0) - 0.05;
                }
                float q = mix(0.25, 0.9, brk);   // a breaking crest leans forward
                surf = vec3(dir.x, 0.0, dir.y) * (q * H * 0.5 * cos(theta)) + vec3(0.0, y, 0.0);
                surf *= feel * inMap;
                // Its slope along the run, for the pixel normal: dy/d(dist),
                // turned into the slope toward the shore.
                float dydd = H * sharp * pow(max(c, 1e-3), sharp - 1.0) * 0.5 * cos(theta) * (-k);
                vShoreSlope = -dir * dydd * feel * inMap;
                // The white roll: the top of a breaking crest and the face it
                // is falling down -- shoreward of the crest, where cos < 0.
                vBreak = brk * smoothstep(0.72, 0.97, c) * (0.55 + 0.45 * smoothstep(0.3, -0.3, cos(theta)))
                       * feel * inMap;
                // Where this point is in the train: the crest is at sin = 1
                // (theta = pi/2), and the water behind it was passed by it a
                // fraction of a period ago.
                vShorePhase = fract((theta - 1.5707963) / 6.2831853);
                vColumn = max(depth + y * feel, 0.0);
                vSurf  = feel * inMap * (1.0 - smoothstep(0.0, max(H0 * 2.2, 0.4), depth)) *
                         step(-0.05, depth + y);
            }
        }
    }
    // On land the sea itself is flat (and under the ground); only the surge
    // the breakers send up the beach stands over it.
    d *= land ? 0.0 : mix(0.08, 1.0, vCalm);
    d += surf;

    // The waves things make, fading out at the edge of their patch.
    vec2 wuv = (rest - uWaveOrigin) / uWaveExtent;
    float edge = smoothstep(0.0, 0.08, min(min(wuv.x, wuv.y), min(1.0 - wuv.x, 1.0 - wuv.y)));
    if (edge > 0.0) d.y += textureLod(uWaveTex, wuv, 0.0).r * edge;

    vec3 world = vec3(rest.x + d.x, uGridCentre.y + d.y, rest.y + d.z);
    vWorldPos = world;
    vRestXZ   = rest;
    vCrest    = d.y / max(uWaveHs * 0.5, 0.05);
    vClip     = uViewProj * vec4(world, 1.0);
    gl_Position = vClip;
}
