#version 330 core

in vec2 vNdc;
out vec4 FragColor;

uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;

uniform vec3 uSunDir;     // towards the sun
uniform vec3 uSunColor;   // sun radiance (already dimmed at night)
uniform float uTime;

// Cloud controls.
uniform float uCoverage;     // THRESHOLD: lower = more sky covered
uniform float uCloudDensity; // optical density multiplier
uniform float uCloudScale;   // noise frequency
uniform float uCloudSpeed;   // wind speed
uniform float uCloudBottom;  // slab altitudes (world units)
uniform float uCloudTop;

// --- The sheet layers -------------------------------------------------------
// Everything in the sky that is NOT the marched cumulus deck: the low grey
// decks, the mid cellular ones, the ice at the top, and the traffic. They are
// sheets rather than volumes because that is what they physically are -- a
// stratus is two hundred metres of uniform grey and a cirrus has no measurable
// underside at all, so marching either of them spends sixty samples arriving at
// the number the first one already had.
//
// One array rather than a uniform per cloud type: the compositing ORDER is the
// whole point of a layer system, and an order is a list. The CPU sorts by
// height (highest first) and drops the cumulus into that list as kind 0, so a
// stratus deck under a cumulus field occludes it and one above it does not --
// which is exactly the thing two hard-coded layers could never express.
//
// Kinds. Each is a different mask and a different way of taking light, because
// a mackerel sky and a fibrous cirrus are not one function with two thresholds:
const int kCumulus       = 0;   // not a sheet: triggers the raymarch
const int kStratus       = 1;   // low, flat, featureless grey
const int kStratocumulus = 2;   // low, lumpy rolls
const int kAltocumulus   = 3;   // mid, cellular -- a mackerel sky
const int kCirrus        = 4;   // high, combed fibres
const int kCirrocumulus  = 5;   // high, fine grain
const int kContrails     = 6;   // high, straight lines

const int kMaxLayers = 8;
uniform int   uLayerCount;
uniform int   uLayerKind[kMaxLayers];
// x: amount 0..1, y: height (world units), z: feature scale, w: wind speed.
uniform vec4  uLayerA[kMaxLayers];
uniform vec2  uLayerDir[kMaxLayers];   // unit wind heading in XZ

uniform float uExposure;
uniform int   uTonemap;

const float PI = 3.14159265;

// Extinction, PER METRE. It matters that these are per metre and not per sample:
// both marches multiply a density by a step length, and the step lengths follow
// the slab thickness -- so a coefficient tuned by eye at a 180 m slab turns
// every cloud in a 1700 m one solid black. That is exactly what happened when
// the layer was raised, and it is why they are named and written down here
// rather than sitting as bare numbers in the loop.
const float kExtinct = 0.020;   // along the view ray: opaque through ~200 m
const float kSunSig  = 0.0035;  // toward the sun: light reaches ~300 m in

vec3 acesTonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 toOutput(vec3 c) {
    if (uTonemap == 1) {
        c = acesTonemap(c * uExposure);
        c = pow(c, vec3(1.0 / 2.2));
    }
    return c;
}

// --- Noise ------------------------------------------------------------------
float hash11(float p) {
    p = fract(p * 0.1031);
    p *= p + 33.33;
    return fract((p + p) * p);
}
float hash12(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * 0.1031);
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float vnoise3(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i + vec3(0, 0, 0));
    float n100 = hash13(i + vec3(1, 0, 0));
    float n010 = hash13(i + vec3(0, 1, 0));
    float n110 = hash13(i + vec3(1, 1, 0));
    float n001 = hash13(i + vec3(0, 0, 1));
    float n101 = hash13(i + vec3(1, 0, 1));
    float n011 = hash13(i + vec3(0, 1, 1));
    float n111 = hash13(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}
// The 2D one is its own function rather than vnoise3 with z = 0: the high layer
// is sampled once per pixel where the cumulus march is sampled hundreds of
// times, but four hashes instead of eight is still four hashes not spent.
float vnoise2(vec2 x) {
    vec2 i = floor(x);
    vec2 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i + vec2(0, 0)), hash12(i + vec2(1, 0)), f.x),
               mix(hash12(i + vec2(0, 1)), hash12(i + vec2(1, 1)), f.x), f.y);
}
float fbm2(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) { s += a * vnoise2(p); p *= 2.03; a *= 0.5; }
    return s;
}

// Rounded bulges rather than smooth blobs: the ridge of the noise, inverted, so
// the MAXIMA are domed and the minima are creases. This is the whole difference
// between a cauliflower and a cloud of smoke, and no amount of thresholding
// smooth noise will produce it.
float billow(vec3 p, int octaves) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) {
        if (i >= octaves) break;
        s += a * (1.0 - abs(2.0 * vnoise3(p) - 1.0));
        p *= 2.03;
        a *= 0.5;
    }
    return s;
}

// Sparse procedural starfield (night only).
vec3 starField(vec3 dir) {
    if (dir.y < 0.02) return vec3(0.0);
    vec3 p  = dir * 260.0;
    vec3 id = floor(p);
    vec3 f  = fract(p) - 0.5;
    float h = hash13(id);
    float exists = step(0.986, h);          // ~1.4% of cells
    float point  = exists * smoothstep(0.16, 0.0, length(f));
    float twinkle = 0.5 + 0.5 * hash13(id + 3.17);
    return vec3(0.9, 0.95, 1.0) * point * twinkle * 2.0;
}

// A soft moon disk in a fixed direction.
vec3 moon(vec3 dir) {
    vec3  md = normalize(vec3(0.5, 0.5, 0.72));
    float m  = dot(dir, md);
    float disk = smoothstep(0.9986, 0.9992, m);
    float glow = pow(max(m, 0.0), 250.0) * 0.3;
    return vec3(0.85, 0.9, 1.0) * (disk * 4.0 + glow);
}

// --- Sky gradient + sun ----------------------------------------------------
vec3 skyColor(vec3 dir) {
    float day = smoothstep(-0.12, 0.18, uSunDir.y); // 0 night -> 1 day

    vec3 dayZenith   = vec3(0.20, 0.42, 0.80);
    vec3 dayHorizon  = vec3(0.70, 0.82, 0.95);
    vec3 nightZenith = vec3(0.01, 0.02, 0.06);
    vec3 nightHoriz  = vec3(0.04, 0.06, 0.12);

    float h = clamp(dir.y, 0.0, 1.0);
    vec3 zenith  = mix(nightZenith, dayZenith, day);
    vec3 horizon = mix(nightHoriz,  dayHorizon, day);
    vec3 col = mix(horizon, zenith, pow(h, 0.5));

    // Warm sunset/sunrise tint near the horizon when the sun is low.
    float lowSun = (1.0 - smoothstep(0.0, 0.35, uSunDir.y)) * day;
    float toSun  = max(dot(normalize(vec3(dir.x, 0.0, dir.z)),
                           normalize(vec3(uSunDir.x, 0.0, uSunDir.z))), 0.0);
    col += vec3(0.85, 0.35, 0.10) * lowSun * pow(toSun, 3.0) * (1.0 - h);

    // The gradient colours are authored in sRGB -> linearise for the pipeline.
    col = pow(col, vec3(2.2));

    // Stars and moon fade in at night.
    float night = 1.0 - day;
    col += (starField(dir) + moon(dir)) * night;

    // Sun disk + tight corona (HDR linear radiance). The soft halo comes from
    // bloom in the composite, so keep the in-sky glow tight to avoid a blowout.
    float sd   = max(dot(dir, uSunDir), 0.0);
    float disk = smoothstep(0.9991, 0.9995, sd);  // the bright disk
    float glow = pow(sd, 200.0) * 0.5;            // tight corona only
    vec3 sunTint = uSunColor * vec3(1.0, 0.9, 0.72);
    col += sunTint * (disk * 9.0 + glow);
    return col;
}

// --- Cumulus ----------------------------------------------------------------
// Three things make a shape read as a cumulus rather than as fog with holes in
// it, and the old single-threshold fBm had none of them:
//
//   * BILLOWS -- see billow() above.
//   * A FLAT BASE and a domed top. The slab is one air mass: everything
//     condenses at the same altitude, which is why a real cumulus field looks
//     like it is standing on a sheet of glass. Only the TOPS climb, and they
//     climb further the more of them there are.
//   * EROSION. The bulges are carved back where they are already thin, so the
//     silhouette frays into wisps instead of ending on a smooth contour.
//
// Split in two on purpose: the light march below needs the SHAPE, not the fray,
// and it runs five times per step. Handing it the cheap half is most of what
// pays for the expensive one.

// How much cloud there can be at height `t` through the slab. `amount` is the
// coverage as a 0..1 quantity (uCoverage is a threshold, so it runs the other
// way): a thin fair-weather field is all base and no build.
float heightShape(float t, float amount) {
    float top = mix(0.34, 1.0, amount);
    return smoothstep(0.0, 0.07, t) * smoothstep(top, top * 0.45, t);
}

// Where in the noise field a world point samples from. The SHEAR is the point:
// the wind is stronger higher up, so a cloud's top is dragged ahead of its base
// and the whole thing leans. One add, and it is most of what stops a field of
// cumulus looking like it was stamped out of a sheet -- a vertical column of
// noise reads as a pillar, a leaning one reads as weather.
vec3 cloudSample(vec3 p, float t) {
    vec3 wind  = vec3(uTime * uCloudSpeed, 0.0, uTime * uCloudSpeed * 0.3);
    vec3 shear = vec3(t * (uCloudTop - uCloudBottom) * 0.5, 0.0, 0.0);
    return (p + wind + shear) * uCloudScale;
}

float cloudBase(vec3 p) {
    float amount = clamp(1.0 - uCoverage, 0.0, 1.0);
    float t = (p.y - uCloudBottom) / max(uCloudTop - uCloudBottom, 1.0);
    float n = billow(cloudSample(p, t), 4);
    return smoothstep(uCoverage, uCoverage + 0.20, n) * heightShape(t, amount);
}

float cloudDensity(vec3 p) {
    float base = cloudBase(p);
    if (base <= 0.0) return 0.0;
    // Finer billows, drifting slightly against the wind so the edges boil rather
    // than slide. Remapped against the base so a thick core survives untouched
    // and only the thin skirts are eaten away -- erode everywhere and the whole
    // field goes translucent instead of getting an edge.
    float t = (p.y - uCloudBottom) / max(uCloudTop - uCloudBottom, 1.0);
    vec3 q = cloudSample(p, t);
    float detail = billow(q * 6.1 - vec3(0.0, uTime * 0.04, 0.0), 3);
    float erode  = (1.0 - detail) * 0.42;
    return clamp((base - erode) / max(1.0 - erode, 1e-3), 0.0, 1.0);
}

// --- The sheet layers -------------------------------------------------------
// Not marched. Everything here is a deck whose thickness is small next to how
// far away it is: a cirrus at ten kilometres has no visible underside, and a
// stratus has one but it is a flat grey one. The ray meets the plane once, and
// everything on that layer is decided in that one place.
//
// What differs between the kinds is the MASK -- how the cloud is cut out of the
// sky -- and how the result takes light. Those are the two things that make a
// mackerel sky read as a mackerel sky, and neither is reachable by putting a
// second threshold on one noise function.

// Worley-ish cell distance: distance to the nearest of a jittered point per
// grid cell. This is what a cellular deck is made of -- altocumulus and
// cirrocumulus are fields of SEPARATE elements with sky between them, and fBm
// thresholded hard gives connected blobs with holes in them instead, which is a
// different picture entirely.
float cellDist(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    float d = 8.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec2 g = vec2(float(x), float(y));
            vec2 o = vec2(hash12(i + g), hash12(i + g + 19.7));
            d = min(d, length(g + o - f));
        }
    }
    return d;
}

// Cirrus: crystals drawn out into streaks by a jet stream that has nothing to do
// with the wind in the cumulus layer. The noise is squashed hard along one axis
// and then warped ALONG that axis, which is what gives the fibrous, combed look
// rather than stretched blobs.
float cirrusMask(vec2 q, float amount, float soft, out float fine) {
    vec2 s = vec2(q.x * 0.10, q.y);        // stretched: streaks, not blobs
    float w = fbm2(s * 0.6);
    float n = fbm2(s + vec2(w * 2.4, 0.0));
    fine = fbm2(s * 3.1 + vec2(w * 1.2, 0.0));
    // The threshold walks down as there is more of it, so the slider goes from
    // "a few strands" to "a milky sheet" instead of just turning opacity up.
    return smoothstep(mix(0.72, 0.40, amount) - soft,
                      mix(0.95, 0.66, amount) + soft, n);
}

// Condensation trails. Straightness is the entire read: nothing else in a sky is
// a perfect line, which is why a contrail is recognised at a glance -- and why
// no amount of noise will stand in for one.
//
// Each is drawn older than the last: wider, softer, broken into segments by the
// same shear that spread it, and fading. A sky of four identical fresh lines
// looks like a test pattern.
//
// `spread` ages the whole set at once (the layer's Scale): busy airspace in
// still air keeps its lines sharp for an hour, and one shear aloft turns the
// same four into broad smears. `heading` points the traffic -- lanes run where
// the airways run, so they are roughly parallel, and a fan of four unrelated
// angles is the one arrangement a real sky never has.
float contrails(vec2 q, float amount, float H, float spread, vec2 heading) {
    if (amount <= 0.0) return 0.0;
    float total = 0.0;
    float lane = atan(heading.y, heading.x);
    for (int i = 0; i < 4; ++i) {
        float fi  = float(i);
        // Only the first appears at low amounts, so the slider adds traffic
        // rather than fading four ghosts in together.
        float lit = smoothstep(fi * 0.24, fi * 0.24 + 0.18, amount);
        if (lit <= 0.0) continue;

        // Scattered about the lane, not about the compass: +/- 25 degrees.
        float ang = lane + (hash11(fi * 7.31 + 0.5) - 0.5) * 0.9;
        vec2  n   = vec2(cos(ang), sin(ang));        // across the trail
        vec2  al  = vec2(-n.y, n.x);                 // along it
        // Everything up here is measured in layer heights rather than in
        // metres, so raising the layer does not turn four aircraft into four
        // motorways -- a trail is thin because of how far away it is.
        // Lanes are spread across about four layer-heights of sky, which at a
        // 45-degree look is roughly the width of the view: close enough that
        // more than one is usually in frame, far enough apart that they do not
        // read as a comb.
        float off = (hash11(fi * 3.17 + 1.7) - 0.5) * 4.2 * H;
        float age = clamp(hash11(fi * 5.93 + 4.2) * spread, 0.0, 1.0);

        // Width, as a fraction of the layer's height -- so the ANGLE a trail
        // subtends is the same wherever the layer is put, which is the whole
        // point of measuring up here in heights. The numbers are small because
        // a contrail is: a fresh one is a couple of hundred metres across seen
        // from ten kilometres below, which is under two degrees. Tuned by eye
        // at 1400 m they were five times that, and at a real cruising altitude
        // they came out as searchlight beams.
        float d = abs(dot(q, n) - off);
        float w = mix(0.009, 0.075, age) * H;        // it spreads as it ages
        float core = exp(-(d * d) / (w * w));

        // Along its length: a trail has two ends, and an old one has holes.
        float s    = dot(q, al);
        float ends = smoothstep(1.0, 0.62, abs(s) / (9.3 * H));
        float gaps = mix(1.0, smoothstep(0.30, 0.72,
                                         vnoise2(vec2(s * (1.54 / H) + fi * 17.0, age * 6.0))),
                         age);
        total += core * ends * gaps * mix(0.85, 0.30, age) * lit;
    }
    return clamp(total, 0.0, 1.0);
}

// How much sky a layer takes at `q`, and how bright its own fine structure is
// there. `q` arrives already scaled so one unit is one feature, whatever the
// layer's height -- see renderSheet.
// `soft` is a poor man's level of detail: how many features fall inside one
// pixel, roughly. A cirrocumulus is a field of elements a few hundred metres
// across, and toward the horizon dozens of them land on one pixel -- so the
// mask samples one of them at random and the whole band sizzles as the layer
// drifts. Widening every threshold by `soft` blurs the field toward its own
// average exactly where it can no longer be resolved, which is what a mip
// chain would do if any of this were a texture.
float sheetMask(int kind, vec2 q, float amount, vec2 heading, float soft,
                out float fine) {
    soft = min(soft, 0.30);
    fine = 1.0;
    if (kind == kStratus) {
        // A featureless lid. What varies across a stratus is not its shape but
        // its THICKNESS, in patches hundreds of metres across -- so the mask
        // saturates hard and `fine` carries the variation as BRIGHTNESS rather
        // than as holes. That distinction is the whole difference between a
        // stratus and a haze: an overcast ceiling is opaque everywhere and
        // merely darker in places, and a mask that thinned it instead left the
        // blue showing through, which is what the first attempt drew.
        float n = fbm2(q * 0.45);
        fine = 0.62 + 0.38 * fbm2(q * 1.7);
        return smoothstep(mix(0.58, 0.06, amount) - soft,
                          mix(0.72, 0.24, amount) + soft, n);
    }
    if (kind == kStratocumulus) {
        // Rolls. The billow gives the lumps; squashing the sample ALONG the
        // wind lines them up into the parallel bands that separate a
        // stratocumulus from a flat stratus with texture on it.
        //
        // The elements are SMALL relative to the layer's height -- ten or
        // twenty across the visible dome, not three. A flat plane seen from
        // underneath stretches everything radially toward the horizon, and at a
        // low altitude that stretch is severe; features sized for a cirrus at
        // eight kilometres turn into a starburst when the plane is at twelve
        // hundred metres, which is exactly what happened. Hence the x3.
        vec2 al = heading;
        vec2 cr = vec2(-al.y, al.x);
        vec2 r  = vec2(dot(q, al) * 0.58, dot(q, cr)) * 3.0;
        float b = 0.0, a = 0.5;
        vec2 pp = r;
        for (int i = 0; i < 4; ++i) {
            b += a * (1.0 - abs(2.0 * vnoise2(pp) - 1.0));
            pp *= 2.03; a *= 0.5;
        }
        // Like the stratus: a deck, so the variation is brightness and only the
        // edge of the field is a hole.
        fine = 0.55 + 0.45 * b;
        return smoothstep(mix(0.72, 0.34, amount) - soft,
                          mix(0.86, 0.48, amount) + soft, b);
    }
    if (kind == kAltocumulus) {
        // A mackerel sky: separate rounded elements in rows, with sky between
        // them. The cell field gives the elements; a slow fBm decides where
        // there is a field at all, so it comes in patches rather than tiling
        // the whole dome.
        // The elements are not all the same size. cellDist alone gives a field
        // of identical discs on a regular lattice, which reads as a printed
        // pattern rather than as weather -- so a slow fBm swells and shrinks
        // them across the sky, and a second one decides where there is a field
        // at all, so it comes in patches rather than tiling the whole dome.
        // The cells run at 2.6x the base frequency, so the blur does too.
        soft = min(soft * 2.6, 0.32);
        float d = cellDist(q * 2.6);
        float vary = fbm2(q * 0.9) - 0.5;
        float lo = mix(0.30, 0.60, amount) + vary * 0.26;
        float el = smoothstep(lo + soft, lo * 0.45 - soft, d);
        float patch = smoothstep(0.34, 0.66, fbm2(q * 0.22) + amount * 0.28);
        fine = 0.5 + 0.5 * smoothstep(0.5, 0.0, d);   // each element domed
        return el * patch;
    }
    if (kind == kCirrocumulus) {
        // The same cells, far finer and far fainter: the grain a cirrocumulus
        // has, which is the one thing that tells it from a cirrostratus.
        soft = min(soft * 9.0, 0.32);
        float d = cellDist(q * 9.0);
        float vary = fbm2(q * 1.4) - 0.5;
        float lo = mix(0.34, 0.58, amount) + vary * 0.20;
        float el = smoothstep(lo + soft, lo * 0.45 - soft, d);
        float patch = smoothstep(0.30, 0.70, fbm2(q * 0.30) + amount * 0.30);
        fine = 0.7 + 0.3 * smoothstep(0.5, 0.0, d);
        return el * patch * 0.85;
    }
    // kCirrus

    float f = 0.0;
    float m = cirrusMask(q, amount, soft, f);
    fine = mix(0.55, 1.0, f);       // fibres within the sheet
    return m;
}

// One sheet layer where the view ray crosses its plane. Returns premultiplied
// colour in .rgb and coverage in .a.
vec4 renderSheet(vec3 ro, vec3 rd, int kind, vec4 A, vec2 heading, vec3 behind) {
    float amount = clamp(A.x, 0.0, 1.0);
    if (amount <= 0.0) return vec4(0.0);
    float H = max(A.y, 1.0);
    if (rd.y <= 0.015) return vec4(0.0);
    float t = (H - ro.y) / rd.y;
    if (t <= 0.0) return vec4(0.0);          // the eye is above this layer

    // Where the ray lands, drifted by this layer's OWN wind. The jet stream
    // aloft and the breeze under a stratus are different winds, and a sky where
    // every deck slides at one speed in one direction reads as a single painted
    // dome being panned.
    vec2 q = ro.xz + rd.xz * t;
    q += heading * (uTime * A.w);

    // Scaled by HEIGHT, so a feature is a fixed number of metres across rather
    // than a fixed number of degrees: raise a layer and its elements get
    // smaller, which is most of why a cirrocumulus looks fine-grained and a
    // stratocumulus does not. Scale is the author's multiplier on top of that.
    float freq = 2.24 / (H * max(A.z, 0.05));
    // How far the sample point runs across the noise field from one pixel to
    // the next: the plane's own foreshortening, which goes to infinity at the
    // horizon. Worked out from the geometry rather than taken with dFdx,
    // because the early returns above put this inside divergent control flow
    // and a derivative taken there is undefined -- it happens to work on this
    // driver, which is the worst way for something to work.
    //
    // t/rd.y is the footprint per unit of angle; the constant is one pixel's
    // worth of it at the viewport this engine draws.
    float soft = clamp(freq * (t / max(rd.y, 0.02)) * 0.0016, 0.0, 0.30);
    float fine = 1.0;
    float a;
    if (kind == kContrails) {
        // The one kind that is NOT scaled by freq. Everything about a trail is
        // measured in layer heights already -- how far off the zenith it passes,
        // how wide it is, how long -- because that is what makes a trail look
        // thin from far below instead of turning into a motorway when the layer
        // is raised. Handing it the pre-divided q put every lane tens of
        // thousands of units off the side of the sky, which is a very effective
        // way of drawing nothing at all.
        a = contrails(q, amount, H, A.z, heading);
    } else {
        a = sheetMask(kind, q * freq, amount, heading, soft, fine);
    }
    // What `fine` modulates depends on whether the deck is opaque. On a low
    // deck it is THICKNESS, and a thick lid is darker, not more solid -- so it
    // goes into the colour below. On the thin ones it really is how much cloud
    // is there, so it goes into the alpha. Contrails carry their own brightness
    // along their length and are left alone.
    bool solid = (kind == kStratus || kind == kStratocumulus);
    if (!solid && kind != kContrails) a *= fine;
    // Thinned toward the horizon: at a grazing angle the plane is so far away
    // that atmosphere has eaten it, and without this the whole layer piles up
    // into a hard band where it meets the sky.
    a *= smoothstep(0.015, 0.22, rd.y);
    a = clamp(a, 0.0, 1.0);
    if (a <= 0.001) return vec4(0.0);

    // --- How the kind takes light ------------------------------------------
    // Ice is almost all forward scatter: cirrus is white away from the sun and
    // silver-bright next to it, and it takes the sunset before anything lower
    // does. A low deck is the opposite -- what you are looking at is its SHADED
    // underside, which is why an overcast day is grey and not white. One set of
    // constants for both is why the old shader could only ever draw ice.
    //
    // The scale of these is set by the MARCH, not by taste: uSunColor is around
    // 3.4 at midday, and the cumulus arrives at roughly 0.3 of it after its
    // phase function. A sheet gain near 1 puts every layer far above white, and
    // three of them stacked turn the sky into a sheet of paper -- which is what
    // the first calibration did.
    float sunGain = 0.20;    // how much sun reaches the face you can see
    float fwdGain = 0.85;    // forward-scatter gain toward the sun
    float fwdPow  = 6.0;     // how tight that lobe is
    float shade   = 0.0;     // 0 = a lit sheet, 1 = looking at the dark side
    float opacity = 1.0;
    if (kind == kStratus)            { sunGain = 0.05; fwdGain = 0.10; fwdPow = 3.0; shade = 0.88; }
    else if (kind == kStratocumulus) { sunGain = 0.10; fwdGain = 0.25; fwdPow = 4.0; shade = 0.62; }
    else if (kind == kAltocumulus)   { sunGain = 0.20; fwdGain = 0.55; fwdPow = 5.0; shade = 0.26; opacity = 0.92; }
    else if (kind == kCirrocumulus)  { sunGain = 0.20; fwdGain = 0.80; fwdPow = 6.0; shade = 0.06; opacity = 0.80; }
    else if (kind == kContrails)     { sunGain = 0.22; fwdGain = 1.10; fwdPow = 7.0; shade = 0.02; opacity = 0.90; }

    float dayF  = smoothstep(-0.12, 0.16, uSunDir.y);
    float toSun = max(dot(rd, uSunDir), 0.0);
    float fwd   = pow(toSun, fwdPow);
    vec3  lit   = uSunColor * (sunGain + fwdGain * fwd) * dayF;
    vec3  ambient = pow(mix(vec3(0.06, 0.08, 0.14), vec3(0.52, 0.60, 0.74), dayF),
                        vec3(2.2));
    // The underside. Not a grey tint laid over the lit colour but a darkening
    // of it: a stratus is the sun's own light after two hundred metres of
    // water, so it keeps the sun's HUE and loses its strength.
    vec3 col = mix(lit + ambient, ambient * 0.42, shade);
    // On a solid deck the thickness varies the brightness rather than the coverage.
    if (solid) col *= fine;

    // Aerial perspective, the same law the march uses. A plane intersection at a
    // shallow angle is TENS of kilometres away, and cloud that far off is seen
    // through tens of kilometres of air: it loses its contrast into whatever is
    // behind it. The march has had this from the start; the sheets did not, and
    // it showed the moment more than one of them was switched on -- four decks
    // stayed as white at the horizon as they were overhead and the sky went to
    // paper. It is also what stops a layer ending on a visible line where its
    // plane runs away from the eye.
    // The path length that counts is the HORIZONTAL one. Haze lives in the
    // bottom couple of kilometres, so a ray going steeply up leaves it almost
    // at once however far it travels afterwards, and one going out along the
    // deck stays in it the whole way. Using the slant range instead -- which
    // the march does, and gets away with because a cumulus is never far
    // overhead -- washed a contrail at ten kilometres half into the sky purely
    // for being high up.
    float ground = t * max(length(rd.xz), 0.02);
    col = mix(col, behind, 1.0 - exp(-ground * 2.5e-5));
    return vec4(col * a * opacity, a * opacity);
}

// Henyey-Greenstein phase function.
float phaseHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

// Raymarch the cloud slab. Returns premultiplied colour in .rgb and coverage
// (1 - transmittance) in .a.
vec4 renderClouds(vec3 ro, vec3 rd, vec3 behind) {
    // A soft floor rather than a hard cutoff. At a grazing angle the slab runs
    // for hundreds of kilometres and no march can pay for that, so the layer is
    // faded into the haze instead of ending on a line -- which is exactly what
    // the old 1600 m cap drew across the horizon.
    float horizonFade = smoothstep(0.0, 0.10, rd.y);
    if (horizonFade <= 0.0) return vec4(0.0);

    float thick = max(uCloudTop - uCloudBottom, 1.0);
    float t0 = (uCloudBottom - ro.y) / rd.y;
    float t1 = (uCloudTop    - ro.y) / rd.y;
    t0 = max(t0, 0.0);
    if (t1 <= t0) return vec4(0.0);
    // Reach measured in slab thicknesses, so raising the layer lengthens the
    // view along it by the same factor and the field still runs to the horizon.
    t1 = min(t1, t0 + thick * 14.0);

    const int STEPS = 64;
    // Steps that GROW with distance. A cloud twenty kilometres away is a few
    // pixels wide and does not need the sampling one overhead does; a constant
    // step long enough to reach it would band everything close up. Geometric
    // growth keeps the near end fine and still gets there in 64 steps.
    const float GROW = 1.035;
    float span = (pow(GROW, float(STEPS)) - 1.0) / (GROW - 1.0);
    float dt = (t1 - t0) / span;

    float cosA  = dot(rd, uSunDir);
    float phase = mix(phaseHG(cosA, 0.2), phaseHG(cosA, -0.15), 0.5);
    float dayF  = smoothstep(-0.1, 0.2, uSunDir.y); // fade sun lighting at night

    vec3 ambient = pow(mix(vec3(0.10, 0.13, 0.20), vec3(0.55, 0.62, 0.75),
                       smoothstep(-0.1, 0.2, uSunDir.y)), vec3(2.2));

    float T = 1.0;
    vec3  col = vec3(0.0);
    // Per-pixel dither on the start offset breaks the raymarch banding.
    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float t = t0 + dt * dither;
    for (int i = 0; i < STEPS; ++i) {
        vec3 p = ro + rd * t;
        float d = cloudDensity(p) * uCloudDensity;
        if (d > 0.001) {
            // Light march toward the sun for self-shadowing. cloudBase, not
            // cloudDensity: what casts the shadow inside a cumulus is its bulk,
            // and the fray on its edges is not worth five samples a step.
            float ls = 0.0;
            float lstep = (uCloudTop - uCloudBottom) * 0.12;
            for (int j = 1; j <= 5; ++j) {
                vec3 lp = p + uSunDir * (lstep * float(j));
                ls += cloudBase(lp) * uCloudDensity;
            }
            float sun = exp(-ls * lstep * kSunSig);

            // Silver lining: light that took a short path through the thin edge
            // of a bulge and came out the other side, which is the brightest
            // thing in a real cumulus field and was missing entirely.
            float rim = pow(1.0 - clamp(d, 0.0, 1.0), 3.0) * pow(max(cosA, 0.0), 8.0);

            vec3 lum = uSunColor * (sun * phase * 3.5 + rim * 1.4) * dayF + ambient;
            // Aerial perspective. Cloud twenty kilometres away is seen through
            // twenty kilometres of air and loses its contrast into the sky
            // behind it -- which is the entire reason a real cumulus field reads
            // as going somewhere. Without it the far edge stays as white as the
            // one overhead and the layer is a band pasted along the horizon,
            // which is what the first attempt looked like.
            lum = mix(lum, behind, 1.0 - exp(-t * 7.0e-5));
            float dens = d * dt * kExtinct;
            float a = 1.0 - exp(-dens);
            col += T * lum * a;
            T   *= exp(-dens);
            if (T < 0.02) break;
        }
        t  += dt;
        dt *= GROW;
    }
    return vec4(col * horizonFade, (1.0 - T) * horizonFade);
}

void main() {
    // Reconstruct the world-space view ray from the NDC position.
    vec4 far = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 world = far.xyz / far.w;
    vec3 dir = normalize(world - uCameraPos);

    vec3 col = skyColor(dir);

    // The layers, back to front. The CPU has already sorted them by height with
    // the highest first, so this loop is the whole of the compositing order: a
    // deck draws over everything above it and under everything below it, and a
    // cumulus passing beneath a contrail cuts the contrail exactly as it should.
    //
    // The cumulus is one entry in this list rather than a step after it. That is
    // the difference between a layer system and two hard-coded layers: with the
    // march sitting in the same order as the sheets, a stratus deck UNDER a
    // cumulus field hides it, which is what a low overcast does and what the old
    // fixed "high plane, then clouds" order could never draw.
    for (int i = 0; i < uLayerCount; ++i) {
        if (i >= kMaxLayers) break;
        vec4 l;
        if (uLayerKind[i] == kCumulus) {
            // `col` and not the bare sky: the aerial perspective inside the
            // march fades distant cloud into whatever is actually behind it,
            // which now includes any sheet already drawn.
            l = renderClouds(uCameraPos, dir, col);
        } else {
            // `col` and not the bare sky, for the same reason the march gets
            // it: what a distant deck fades INTO is everything already drawn
            // behind it, which includes the layers above this one.
            l = renderSheet(uCameraPos, dir, uLayerKind[i], uLayerA[i],
                            uLayerDir[i], col);
        }
        col = col * (1.0 - l.a) + l.rgb;
    }

    FragColor = vec4(toOutput(col), 1.0);
}
