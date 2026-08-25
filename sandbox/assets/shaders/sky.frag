#version 330 core

in vec2 vNdc;
out vec4 FragColor;

uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;

uniform vec3 uSunDir;     // towards the sun
uniform vec3 uSunColor;   // sun radiance (already dimmed at night)
uniform float uTime;


// The high layer: ice, far above the cumulus and far above the weather.
uniform float uCirrus;       // 0..1 how much of it there is (0 = none)
uniform float uCirrusHeight; // world units -- the tropopause, in effect
uniform float uCirrusSpeed;  // the jet stream, which is not the surface wind
uniform float uContrails;    // 0..1 how much traffic has been through

uniform float uExposure;
uniform int   uTonemap;

const float PI = 3.14159265;


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
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);   // quintic: C2 across cells
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
    f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);   // quintic, as vnoise3
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

// --- Cumulus: not here any more ---------------------------------------------
//
// The low cloud used to be marched right here, from a thresholded billow field.
// It is gone, and not because the code was bad -- because the approach could not
// reach what was being asked of it. A cumulus is DETACHED, has a SHARP OUTLINE
// and is a CAULIFLOWER, and a threshold over a continuous noise field gives
// connected regions with soft edges, always. Several rounds of tuning each fixed
// a real fault (contrast, silhouette, shear, stack depth, march resolution) and
// none of them made it look more like a cloud.
//
// Low cloud is now built as spheres budding off spheres, baked into a volume
// and drawn as one instanced box per cloud -- see CloudShape.hpp and
// CloudField.hpp. It is a separate pass, drawn over this one.
//
// What stays here is everything ABOVE the cumulus: the sky gradient, the sun,
// the stars, and the ice ten kilometres up. Cirrus is a sheet, not a volume --
// from the ground it has no thickness to march -- so the analytic version is
// not a compromise, it is the right shape of answer.

// --- The high layer: cirrus and contrails ------------------------------------
// Not marched. Ice at ten kilometres is a sheet, not a volume: from the ground
// it has no visible thickness, and forty-eight steps through something one
// sample thick would be forty-seven of them wasted. The ray meets the plane
// once, and everything up there is decided in that one place.

// Cirrus: crystals drawn out into streaks by a jet stream that has nothing to do
// with the wind in the cumulus layer -- hence its own speed. The noise is
// squashed hard along one axis and then warped ALONG that axis, which is what
// gives the fibrous, combed look rather than stretched blobs.
float cirrusMask(vec2 q, out float fine) {
    vec2 s = vec2(q.x * 0.10, q.y);        // stretched: streaks, not blobs
    float w = fbm2(s * 0.6);
    float n = fbm2(s + vec2(w * 2.4, 0.0));
    fine = fbm2(s * 3.1 + vec2(w * 1.2, 0.0));
    float amount = clamp(uCirrus, 0.0, 1.0);
    // The threshold walks down as there is more of it, so the slider goes from
    // "a few strands" to "a milky sheet" instead of just turning opacity up.
    return smoothstep(mix(0.72, 0.40, amount), mix(0.95, 0.66, amount), n);
}

// Condensation trails. Straightness is the entire read: nothing else in a sky is
// a perfect line, which is why a contrail is recognised at a glance -- and why
// no amount of noise will stand in for one.
//
// Each is drawn older than the last: wider, softer, broken into segments by the
// same shear that spread it, and fading. A sky of four identical fresh lines
// looks like a test pattern.
float contrails(vec2 q) {
    float amount = clamp(uContrails, 0.0, 1.0);
    if (amount <= 0.0) return 0.0;
    float total = 0.0;
    for (int i = 0; i < 4; ++i) {
        float fi  = float(i);
        // Only the first appears at low amounts, so the slider adds traffic
        // rather than fading four ghosts in together.
        float lit = smoothstep(fi * 0.24, fi * 0.24 + 0.18, amount);
        if (lit <= 0.0) continue;

        float ang = hash11(fi * 7.31 + 0.5) * PI;
        vec2  n   = vec2(cos(ang), sin(ang));        // across the trail
        vec2  al  = vec2(-n.y, n.x);                 // along it
        // Everything up here is measured in cirrus heights rather than in
        // metres, so raising the layer does not turn four aircraft into four
        // motorways -- a trail is thin because of how far away it is.
        float H   = max(uCirrusHeight, 1.0);
        float off = (hash11(fi * 3.17 + 1.7) - 0.5) * 7.9 * H;
        float age = hash11(fi * 5.93 + 4.2);         // 0 fresh .. 1 spread out

        float d = abs(dot(q, n) - off);
        // Widths in cirrus heights: a fresh trail is a few hundred metres of ice
        // seen from kilometres away, which is a hairline. The old 0.04..0.31 put
        // it at 56..430 m of half-width at a 1400 m layer -- so the "trail"
        // subtended ten degrees of sky and read as a searchlight beam. A quarter
        // of that is still generous for the aged one, which really does spread.
        float w = mix(0.010, 0.072, age) * H;        // it spreads as it ages
        float core = exp(-(d * d) / (w * w));

        // Along its length: a trail has two ends, and an old one has holes.
        float s    = dot(q, al);
        float ends = smoothstep(1.0, 0.62, abs(s) / (9.3 * H));
        float gaps = mix(1.0, smoothstep(0.30, 0.72,
                                         vnoise2(vec2(s * (1.54 / H) + fi * 17.0, age * 6.0))),
                         age);
        // ...and pale with it. A contrail is recognised by being STRAIGHT, not
        // by being bright; at 0.85 coverage a fresh one was as opaque as a
        // cumulus and drew the eye off everything else in the sky.
        total += core * ends * gaps * mix(0.42, 0.11, age) * lit;
    }
    return clamp(total, 0.0, 1.0);
}

// Both of the above, where the view ray crosses the high plane. Returns
// premultiplied colour in .rgb and coverage in .a.
vec4 renderHigh(vec3 ro, vec3 rd) {
    if (uCirrus <= 0.0 && uContrails <= 0.0) return vec4(0.0);
    if (rd.y <= 0.015) return vec4(0.0);
    float t = (uCirrusHeight - ro.y) / rd.y;
    if (t <= 0.0) return vec4(0.0);

    vec2 q = ro.xz + rd.xz * t;
    q += vec2(uTime * uCirrusSpeed, uTime * uCirrusSpeed * 0.22);

    float fine = 0.0;
    float a = cirrusMask(q * (2.24 / max(uCirrusHeight, 1.0)), fine);
    a *= mix(0.55, 1.0, fine);            // fibres within the sheet
    a += contrails(q) * 0.55;
    // Thinned toward the horizon: at a grazing angle the plane is so far away
    // that atmosphere has eaten it, and without this the whole layer piles up
    // into a hard band where it meets the sky.
    a *= smoothstep(0.015, 0.22, rd.y);
    a = clamp(a, 0.0, 1.0);
    if (a <= 0.001) return vec4(0.0);

    // Ice is almost all forward scatter: cirrus is white away from the sun and
    // silver-bright next to it, and it takes the sunset before anything lower
    // does -- it is the last thing up there still in daylight.
    float dayF   = smoothstep(-0.12, 0.16, uSunDir.y);
    float toSun  = max(dot(rd, uSunDir), 0.0);
    float fwd    = pow(toSun, 6.0);
    vec3  lit    = uSunColor * (0.55 + 2.6 * fwd) * dayF;
    vec3  ambient = pow(mix(vec3(0.06, 0.08, 0.14), vec3(0.52, 0.60, 0.74), dayF),
                        vec3(2.2));
    vec3  col = lit + ambient;
    return vec4(col * a, a);
}


void main() {
    // Reconstruct the world-space view ray from the NDC position.
    vec4 far = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 world = far.xyz / far.w;
    vec3 dir = normalize(world - uCameraPos);

    vec3 col = skyColor(dir);
    // The high layer first: it is above the cumulus, so the cumulus draws over
    // it and a cloud passing under a contrail cuts the contrail, as it should.
    vec4 high = renderHigh(uCameraPos, dir);
    col = col * (1.0 - high.a) + high.rgb;
    // The low cloud is a separate pass drawn after this one (CloudField), which
    // is also what puts it in front of the cirrus -- correctly, since it is nine
    // kilometres nearer.

    FragColor = vec4(toOutput(col), 1.0);
}
