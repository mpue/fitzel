#version 330 core

layout(location = 0) in vec2  aBlade;  // x in [-0.5,0.5], h01 in [0,1]
layout(location = 1) in vec3  iPos;    // blade base (world)
layout(location = 2) in float iRot;    // yaw
layout(location = 3) in float iHeight; // blade height
layout(location = 4) in float iPhase;  // sway phase
layout(location = 5) in float iLush;   // 0 dry .. 1 lush (biome moisture)

uniform mat4  uViewProj;
uniform float uTime;
uniform vec2  uWindDir;
uniform float uWindStrength;
uniform float uHeightScale; // global blade-height multiplier (1 = as baked)

out float vH;
out vec3  vWorldPos;
out vec3  vNormal;
out vec3  vBaseCol; // per-blade base/tip colour (computed here, not per fragment)
out vec3  vTipCol;

// Value noise for large-scale colour patches (per blade, from its base position).
float ghash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float gnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = ghash(i), b = ghash(i + vec2(1, 0));
    float c = ghash(i + vec2(0, 1)), d = ghash(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    // Frustum cull per blade: project the base and drop blades that are behind
    // the camera / well off-screen / beyond far. Generous margins (blades have
    // height + wind) so nothing pops at the edges. Early-out also skips the
    // per-blade colour work below. Every vertex of a blade shares iPos, so the
    // whole blade is culled consistently.
    vec4 bc = uViewProj * vec4(iPos, 1.0);
    if (bc.w <= 0.02 ||
        abs(bc.x) > bc.w * 1.3 ||
        bc.y < -bc.w * 2.0 || bc.y > bc.w * 2.0 ||
        bc.z > bc.w) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // offscreen degenerate -> clipped
        return;
    }

    float h01 = aBlade.y;
    float w   = 0.016 * (1.0 - 0.4 * h01);       // thin blade, tapering to the tip
    vec3  local = vec3(aBlade.x * 2.0 * w, h01 * iHeight * uHeightScale, 0.0);

    float c = cos(iRot), s = sin(iRot);
    local = vec3(local.x * c, local.y, local.x * s); // yaw the blade

    // Wind: the tip bends in the wind direction, proportional to height^2.
    float sway = sin(uTime * 1.6 + iPhase + iPos.x * 0.25 + iPos.z * 0.25);
    float bend = uWindStrength * (0.35 + 0.65 * sway) * h01 * h01;
    local.xz += uWindDir * bend;

    vec3 wp = iPos + local;
    vWorldPos = wp;
    vH = h01;
    vNormal = normalize(vec3(-s, 1.2, c)); // blade facing, biased upward

    // Per-blade colour: large meadow patches + a finer break + per-blade jitter,
    // so the field mixes lush green, olive and dry straw (was per-fragment).
    float rnd   = fract(sin(iPhase * 91.17 + iRot * 13.3) * 43758.5453);
    float patch = gnoise(iPos.xz * 0.05);
    float fine  = gnoise(iPos.xz * 0.27 + 11.0);
    float green = clamp(iLush - (1.0 - patch) * 0.55 + (fine - 0.5) * 0.3
                              - rnd * 0.25, 0.0, 1.0);
    vec3 dryBase  = vec3(0.17, 0.15, 0.07);
    vec3 dryTip   = vec3(0.55, 0.48, 0.24);
    vec3 lushBase = mix(vec3(0.05, 0.14, 0.03), vec3(0.10, 0.20, 0.05), fine);
    vec3 lushTip  = mix(vec3(0.26, 0.44, 0.11), vec3(0.54, 0.62, 0.20), fine);
    float bright  = 0.80 + 0.40 * rnd;
    vBaseCol = mix(dryBase, lushBase, green) * bright;
    vTipCol  = mix(dryTip,  lushTip,  green) * bright;

    gl_Position = uViewProj * vec4(wp, 1.0);
}
