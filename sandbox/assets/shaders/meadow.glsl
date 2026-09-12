// --- The meadow, seen from too far away to see a blade ------------------------
//
// The grass is real geometry out to its streamed radius and nothing past it, so
// the ground beyond is whatever the terrain layers are: a soil texture with a
// few green flecks in it, which at a hundred metres reads as bare earth. From
// the air the whole valley turned into desert that way, with a green disc
// around the camera.
//
// Real meadows are green from any distance because the blades cover the soil.
// This is the colour they cover it with: grass.vert's own per-blade palette,
// averaged over what a blade's randoms would do, keyed by the same value noise
// at the same frequencies -- so the broad light and dark patches, the cool and
// warm swings of the field, land in the same places as the blades themselves
// and the tint continues the field instead of replacing it.
//
// Returned in grass.vert's space (display-referred); the caller linearises.

float meadowHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float meadowNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = meadowHash(i), b = meadowHash(i + vec2(1, 0));
    float c = meadowHash(i + vec2(0, 1)), d = meadowHash(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// `px` = world metres per pixel: noise finer than a pixel falls back to its mean.
vec3 meadowColour(vec2 xz, float lush, float px) {
    float fadeHue  = 1.0 - smoothstep(3.0, 9.0, px);      // 9 m patches
    float meadow   = meadowNoise(xz * 0.05);
    float hueN     = mix(0.5, meadowNoise(xz * 0.11 + 31.0), fadeHue);
    float shade    = meadowNoise(xz * 0.035 + 5.0);
    // grass.vert: green = lush - (1-meadow)*0.55 + (fine-.5)*.3 - r1*.25, minus
    // the strawy minority; fine, r1 and r2 at their means.
    float green    = clamp(lush - (1.0 - meadow) * 0.55 - 0.125 - 0.04, 0.0, 1.0);
    vec3 dryBase   = vec3(0.16, 0.14, 0.06);
    vec3 dryTip    = vec3(0.50, 0.45, 0.22);
    vec3 lushBase  = vec3(0.05, 0.13, 0.04);
    vec3 lushTip   = mix(vec3(0.14, 0.36, 0.13), vec3(0.44, 0.54, 0.16), hueN * hueN);
    float bright   = (0.55 + 0.55 * shade) * 0.98 * mix(0.85, 1.05, lush);
    vec3 base = mix(dryBase, lushBase, green) * bright;
    vec3 tip  = mix(dryTip,  lushTip,  green) * bright;
    return mix(base, tip, 0.6);   // grass.frag's far-field blend point
}

// How much of the ground at a point carries grass at all -- grass.cpp's
// placement rules as a soft mask: not too steep, above the water, below the
// snow, and the broad bare patches left open.
float meadowCover(vec2 xz, float h, float ny, float waterLevel, float top, float px) {
    float c = smoothstep(0.80, 0.86, ny)
            * smoothstep(waterLevel + 0.3, waterLevel + 0.9, h)
            * (1.0 - smoothstep(top - 3.0, top, h));
    // The bare patches (placement drops blades where this noise < 0.26):
    // 8 m features, so at a distance only their share survives. Half-covered
    // even at their barest -- the neighbouring blades lean over a gap, and
    // from above a meadow's holes read as darker grass, not as a sand pit.
    float bare = meadowNoise(xz * 0.13 + vec2(19.0, 7.0));
    float open = mix(mix(0.5, 1.0, smoothstep(0.20, 0.32, bare)), 0.88,
                     smoothstep(4.0, 12.0, px));
    return c * open;
}
