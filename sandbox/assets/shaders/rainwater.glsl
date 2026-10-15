// --- Rain on open water ---------------------------------------------------------
// Shared by the lake (water.frag) and the brooks and rivers (river.frag). The
// host defines hash21() and vnoise() before including this.
//
// Rain on water is three things at three distances, and a surface that only
// does the first does nothing at all from where the editor usually looks:
//
//   rainRings     a drop you can SEE -- a ring spreading from where it struck,
//                 and the little crown it throws up as it lands. Metres away.
//   rainDimples   drops too small to show their ring, still big enough to
//                 stipple the surface. Tens of metres.
//   rainRoughness the rain as a texture of the water rather than as drops: the
//                 lake goes matt, its mirror blurs, its sun glint spreads into a
//                 sheen. As far as the lake goes -- this is what a shower on a
//                 lake looks like from a hillside.
//
// Each hands over to the next as the pixel grows past it, so the effect never
// switches off with distance; it only stops being individual drops.
//
// Drops: the same trick lit.frag plays on wet tarmac, and deliberately the same
// numbers -- a stream running across a wet road has to be taking the same rain
// as the road, or the two surfaces read as two weathers. World space is cut into
// cells, each cell drips once per cycle staggered by its own hash, and the
// wave's radial slope goes straight into the normal. Nothing is stored between
// frames -- a drop's whole life is a function of the clock and its cell.
//
// `px` is how many metres of world one pixel covers. `density` is how many drops
// land: the fraction of cells that drip at all in a given cycle -- thinning the
// RATE is what less rain looks like; fading the same drops towards flat makes a
// drizzle read as a downpour seen through frosted glass. `amount` is how hard a
// drop hits, the surface's own dial.

// The visible drops. `splash` collects the crowns: a bright fleck for the first
// tenth of a drop's life, where it struck.
vec3 rainRings(vec3 N, vec2 wp, float amount, float density, float px, float time,
               inout float splash) {
    const float kCell = 0.5;    // metres between drops
    const float kFreq = 34.0;   // ring wavelength
    const float kRate = 1.9;    // drops per second per cell
    const float kTilt = 0.5;    // tilt at full strength
    amount *= 1.0 - smoothstep(kCell * 0.3, kCell * 1.1, px);
    if (amount <= 0.002) return N;  // too far off to resolve: rainDimples' turn
    // A crown is a couple of centimetres across: only worth drawing up close.
    float crown = amount * (1.0 - smoothstep(0.008, 0.035, px));
    vec2 cell = floor(wp / kCell);
    vec2 tilt = vec2(0.0);
    // 3x3 neighbourhood: a ring reaches half a cell, so a drop can never expand in
    // from further out than its neighbours -- no popping at the cell borders.
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2  c     = cell + vec2(x, y);
            float phase = time * kRate + hash21(c);     // its own schedule
            float age   = fract(phase);
            float cyc   = mod(floor(phase), 89.0);      // wrapped: see lit.frag
            // Not every cell drips every cycle when the rain is light. Its own
            // hash, so which drops survive is stable per cell per cycle rather
            // than flickering; and taken on the CYCLE so a thinned-out rain is
            // still rain everywhere rather than the same sparse pattern.
            if (hash21(c * 1.31 + cyc + 11.0) > density) continue;
            // A new landing spot every drop, not one spot per cell for ever.
            vec2  at  = (c + vec2(hash21(c + vec2(7.1, 1.3) + cyc * 0.37),
                                  hash21(c + vec2(3.7, 9.2) + cyc * 0.71))) * kCell;
            vec2  d   = wp - at;
            float r   = length(d);
            if (crown > 0.0 && age < 0.1)
                splash += crown * (1.0 - age * 10.0) * (1.0 - smoothstep(0.004, 0.022, r));
            float f   = age * kCell * 0.5;              // the expanding front
            if (r > f) continue;                         // ahead of it: still flat
            float w = cos((r - f) * kFreq) * exp(-4.0 * r) * (1.0 - age);
            tilt += (d / max(r, 1e-4)) * w;
        }
    }
    N.xz += tilt * amount * kTilt;
    return normalize(N);
}

// Drops as dimples: a short-lived bump where each one landed, no ring, at a
// smaller cell than the rings so the stipple is dense. Fades in as the rings
// fade out and out again once a dimple is under a pixel.
vec3 rainDimples(vec3 N, vec2 wp, float amount, float density, float px, float time) {
    const float kCell = 0.25;
    const float kRate = 2.6;
    const float kSize = 0.07;   // a dimple's radius
    amount *= smoothstep(0.03, 0.12, px) * (1.0 - smoothstep(0.14, 0.40, px));
    if (amount <= 0.002) return N;
    vec2 cell = floor(wp / kCell);
    vec2 tilt = vec2(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2  c     = cell + vec2(x, y);
            float phase = time * kRate + hash21(c + 5.3);
            float age   = fract(phase);
            float cyc   = mod(floor(phase), 89.0);
            if (hash21(c * 1.73 + cyc + 3.0) > density) continue;
            vec2  at = (c + vec2(hash21(c + vec2(2.3, 8.1) + cyc * 0.53),
                                 hash21(c + vec2(6.9, 4.4) + cyc * 0.29))) * kCell;
            vec2  d  = wp - at;
            float r2 = dot(d, d) / (kSize * kSize);
            if (r2 > 4.0) continue;
            // The slope of a gaussian bump, swelling and settling over its life.
            tilt += d / kSize * exp(-r2) * sin(3.14159 * age) * 2.0;
        }
    }
    N.xz += tilt * amount * 0.35;
    return normalize(N);
}

// The rain as a property of the surface, 0..1: how matt it has made the water.
// Smooth in space, so it can be used however big the pixel is. Showers cross a
// lake in bands -- where the wind carries more of it the water goes dull, between
// them it still shines -- and that drifting pattern is the one thing about rain
// on water you can see from a kilometre off.
float rainRoughness(vec2 wp, float amount, float time) {
    if (amount <= 0.002) return 0.0;
    float band = 0.65 * vnoise(wp * 0.018 + vec2(time * 0.045, time * 0.020))
               + 0.35 * vnoise(wp * 0.055 - vec2(time * 0.030, -time * 0.050));
    return clamp(amount * mix(0.45, 1.35, band), 0.0, 1.0);
}
