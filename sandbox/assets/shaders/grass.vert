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

    // Three decorrelated per-blade randoms, derived from the existing instance
    // attributes so no extra vertex data / buffer format is needed. Used to vary
    // width, curvature, stiffness, sway timing and colour independently -- what
    // makes the field read as many individual plants rather than one texture.
    float r1 = fract(sin(iPhase * 91.17 + iRot * 13.30) * 43758.5453);
    float r2 = fract(sin(iPhase * 44.53 + iRot *  7.13 + 2.7) * 24634.6345);
    float r3 = fract(sin(iPos.x * 12.9898 + iPos.z * 78.233) * 43758.5453);

    float h01 = aBlade.y;
    // Blade width varies per plant: some fine, some broad-leaved.
    float widthVar = mix(0.70, 1.65, r2);
    float w   = 0.016 * widthVar * (1.0 - 0.4 * h01); // thin blade, tapering to the tip
    vec3  local = vec3(aBlade.x * 2.0 * w, h01 * iHeight * uHeightScale, 0.0);

    float c = cos(iRot), s = sin(iRot);
    local = vec3(local.x * c, local.y, local.x * s); // yaw the blade

    // Static arch: every blade leans/curves a little in its own direction (not
    // tied to its facing), taller toward the tip. Breaks the "all straight up"
    // look even in dead calm.
    float leanAng = iRot + (r3 - 0.5) * 2.5;
    vec2  leanDir = vec2(cos(leanAng), sin(leanAng));
    local.xz += leanDir * (mix(0.03, 0.17, r1) * h01 * h01);

    // Wind: gusts roll across the field along the wind direction. A low-frequency
    // wave in space+time modulates the local sway amplitude, so some patches lie
    // flat while neighbours still stand and the lull travels downwind -- instead
    // of one uniform shimmer everywhere. A cross-wind octave keeps it irregular.
    float along = dot(iPos.xz, uWindDir);
    float cross = dot(iPos.xz, vec2(-uWindDir.y, uWindDir.x));
    float g1    = sin(along * 0.05 - uTime * 0.85);
    float g2    = sin(along * 0.15 + cross * 0.06 - uTime * 1.70);
    float gust  = clamp(0.45 + 0.42 * g1 + 0.16 * g2, 0.05, 1.15);
    // Per-blade flutter + stiffness: stiff blades bend less and rustle at their
    // own tempo, so the motion isn't lock-stepped.
    float sway  = sin(uTime * (1.35 + 0.6 * r2) + iPhase + along * 0.25);
    float stiff = mix(0.65, 1.25, r1);
    // Shorter blades move less: scale the sway by the blade's true world height
    // (0.35 ~= the default height) so low turf barely stirs while tall stalks
    // whip. Without this every blade bent the same absolute amount.
    float bladeH   = iHeight * uHeightScale;
    float bendGain = clamp(bladeH / 0.35, 0.20, 1.70);
    float bend  = uWindStrength * gust * stiff * (0.30 + 0.70 * sway)
                * h01 * h01 * bendGain;
    local.xz += uWindDir * bend;

    vec3 wp = iPos + local;
    vWorldPos = wp;
    vH = h01;
    vNormal = normalize(vec3(-s, 1.2, c)); // blade facing, biased upward

    // Per-blade colour: large meadow patches + a finer break + a hue shift + per
    // -blade jitter, so the field mixes cool and warm greens, olive and dry straw
    // rather than one flat green. hueN swings tips from blue-green to yellow-green.
    float patch = gnoise(iPos.xz * 0.05);
    float fine  = gnoise(iPos.xz * 0.27 + 11.0);
    float hueN  = gnoise(iPos.xz * 0.11 + 31.0);
    float shade = gnoise(iPos.xz * 0.035 + 5.0);  // broad light/dark zones
    float green = clamp(iLush - (1.0 - patch) * 0.55 + (fine - 0.5) * 0.3
                              - r1 * 0.25, 0.0, 1.0);
    // A minority of blades go strawy even in lush ground (dead/older leaves).
    green = clamp(green - smoothstep(0.86, 0.99, r2) * 0.6, 0.0, 1.0);

    vec3 dryBase   = vec3(0.16, 0.14, 0.06);
    vec3 dryTip    = vec3(0.50, 0.45, 0.22);
    vec3 lushBase  = mix(vec3(0.03, 0.10, 0.03), vec3(0.07, 0.16, 0.05), fine);
    vec3 lushTipC  = vec3(0.14, 0.36, 0.13);  // cool, deep green (dominant)
    vec3 lushTipW  = vec3(0.44, 0.54, 0.16);  // warm yellow-green (the minority)
    vec3 lushTip   = mix(lushTipC, lushTipW, hueN * hueN); // squared -> mostly cool
    // Brightness varies in broad zones (hollows read darker) plus a per-blade
    // jitter and a slight moisture tie -- instead of one uniform bright wash that
    // made the whole field look like fertilised lawn.
    float bright   = (0.55 + 0.55 * shade) * (0.82 + 0.32 * r1)
                   * mix(0.85, 1.05, iLush);
    vBaseCol = mix(dryBase, lushBase, green) * bright;
    vTipCol  = mix(dryTip,  lushTip,  green) * bright;

    gl_Position = uViewProj * vec4(wp, 1.0);
}
