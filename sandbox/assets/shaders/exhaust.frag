#version 430 core

// Rocket exhaust: the clouds and the flames, marched against the scene depth
// (ExhaustFx::render).
//
// THE CLOUD is the density grid the splat pass built, carved at its surface by
// three octaves of inverted worley noise addressed in each puff's own frame (so
// the billows are as big as the puff they sit on and travel with it). Lit by:
//   - the sun, through the cloud's own optical depth towards it, in three
//     scattering orders with a forward-and-back phase -- the bright rims when
//     the sun is behind a cloud, the soft grey of its shaded side;
//   - the flames, as line lights, through the depth towards them -- the orange
//     that soaks the bottom of a launch cloud;
//   - the sky from above, dimmed in the creases between billows;
//   - and its own glow where the exhaust is still hot.
//
// THE FLAMES are integrated first, each in its own short fine march along the
// piece of ray inside its cone, emitting as hot gas does -- as much as it
// absorbs -- so a solid-motor plume is a blinding opaque white and a hydrogen
// flame a faint translucent one. The result is dropped into the cloud march at
// the depth of the flame, so smoke in front of it hides it.
//
// Out: rgb light towards the eye, a what survives of the scene behind. The
// host blends that onto the HDR buffer as src + dst * srcAlpha.

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uDepth;
// The grids: 0 over the pad, 1 coarse over everything, 2 travelling with the
// vehicle (ExhaustFx). A point takes the pad's, then the vehicle's, then the
// coarse one.
uniform sampler3D uA0, uB0, uL0, uOcc0;
uniform sampler3D uA1, uB1, uL1, uOcc1;
uniform sampler3D uA2, uB2, uL2, uOcc2;
uniform sampler3D uNoise;
// The puffs themselves (3 vec4 each: centre + radius, mass/heat/seed/opacity,
// merge): the
// billows are evaluated in each one's own frame, by the indices in uB.
layout(std430, binding = 0) readonly buffer Puffs { vec4 P[]; };
uniform float uNoiseK;

uniform vec3  uO0, uE0;  uniform float uV0;  uniform int uHave0;
uniform vec3  uO1, uE1;  uniform float uV1;  uniform int uHave1;
uniform vec3  uO2, uE2;  uniform float uV2;  uniform int uHave2;

uniform mat4  uInvViewProjRel;
uniform vec3  uCamPos;
uniform float uTime;
uniform float uJitter;
uniform int   uMaxSteps;
uniform int   uDebug;      // EXHAUST_DEBUG: 1 = no empty-space skipping, 3 = coarse grid only
uniform int   uStable;     // EXHAUST_STABLE bits: 1 nearest puff list, 2 jitter fixed per pixel

uniform float uSigma, uThresh, uSoft, uErode, uAlbedo;
uniform vec3  uTint;
uniform vec3  uSunDir, uSunColor, uSkyColor, uGroundColor;
uniform float uSunI, uSkyI, uFlameI, uHeatI;

#define MAX_FLAMES 8
uniform int  uFlameCount;
uniform vec3 uFA[MAX_FLAMES];   // nozzle
uniform vec3 uFD[MAX_FLAMES];   // axis, unit
uniform vec4 uFP[MAX_FLAMES];   // length, r0, r1, brightness
uniform vec4 uFQ[MAX_FLAMES];   // core, diamonds, seed, light
uniform vec3 uFC[MAX_FLAMES];   // tint

const float PI = 3.14159265;

float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

float hg(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

vec2 boxHit(vec3 ro, vec3 rd, vec3 lo, vec3 hi) {
    vec3 inv = 1.0 / rd;
    vec3 a = (lo - ro) * inv, b = (hi - ro) * inv;
    vec3 mn = min(a, b), mx = max(a, b);
    return vec2(max(max(mn.x, mn.y), mn.z), min(min(mx.x, mx.y), mx.z));
}

bool inside01(vec3 u) {
    return all(greaterThanEqual(u, vec3(0.0))) && all(lessThanEqual(u, vec3(1.0)));
}

// The grid p is in (-1: none), and p in its 0..1 coordinates.
int gridOf(vec3 p, out vec3 u) {
    if (uDebug != 3) {
        u = (p - uO0) / uE0;
        if (uHave0 != 0 && inside01(u)) return 0;
        u = (p - uO2) / uE2;
        if (uHave2 != 0 && inside01(u)) return 2;
    }
    u = (p - uO1) / uE1;
    if (uHave1 != 0 && inside01(u)) return 1;
    return -1;
}

float gridVoxel(int g) { return g == 0 ? uV0 : g == 2 ? uV2 : uV1; }

vec4 gridA(int g, vec3 u) {
    if (g == 0) return texture(uA0, u);
    if (g == 2) return texture(uA2, u);
    return texture(uA1, u);
}

vec4 gridL(int g, vec3 u) {
    if (g == 0) return texture(uL0, u);
    if (g == 2) return texture(uL2, u);
    return texture(uL1, u);
}

// Where the ray from p enters the box ahead of it (a long way if it does not).
float entryAt(vec3 p, vec3 rd, vec3 lo, vec3 hi) {
    vec2 b = boxHit(p, rd, lo, hi);
    return (b.y > b.x && b.x > 0.0) ? b.x : 1e9;
}

// Distance to leave the empty brick p is in, 0 if it holds cloud. `vox` gets
// the voxel edge of the grid p is in (or a large step outside all of them).
// A coarser grid leaves out the puffs well inside a finer one, so its bricks
// there say empty: a skip ends where the ray enters a finer grid, and one
// outside every grid ends where it enters the next.
float emptySkip(vec3 p, vec3 rd, out float vox) {
    vec3 u;
    int g = gridOf(p, u);
    bool fine = uDebug != 3;
    float enter0 = (fine && uHave0 != 0) ? entryAt(p, rd, uO0, uO0 + uE0) : 1e9;
    float enter2 = (fine && uHave2 != 0) ? entryAt(p, rd, uO2, uO2 + uE2) : 1e9;
    if (g < 0) {
        vox = 8.0;
        float enter1 = uHave1 != 0 ? entryAt(p, rd, uO1, uO1 + uE1) : 1e9;
        return min(min(enter0, enter2), enter1);
    }
    vox = gridVoxel(g);
    float occ = g == 0 ? texture(uOcc0, u).r : g == 2 ? texture(uOcc2, u).r : texture(uOcc1, u).r;
    if (occ > 0.0) return 0.0;
    vec3 o  = g == 0 ? uO0 : g == 2 ? uO2 : uO1;
    vec3 bs = vec3(vox * 8.0);
    vec3 lo = o + floor((p - o) / bs) * bs;
    vec3 t  = (mix(lo, lo + bs, step(0.0, rd)) - p) / rd;
    float skip = min(min(t.x, t.y), t.z);
    if (g != 0) skip = min(skip, enter0);
    if (g == 1) skip = min(skip, enter2);
    return skip;
}

vec3 seedVec(float s) {
    return fract(vec3(s, s * 7.1317 + 0.31, s * 13.7731 + 0.67));
}

// What the voxel's four strongest puffs say about p, exactly, each asked in
// its own frame -- (p - centre) / radius:
//   F       the cloud's shape: the strongest ball plus a share of the rest (the
//           splat's rule), from the real spheres, so a surface is as round as
//           its puff whatever the size of the voxel it falls in;
//   op      how thick the stuff is (vapour is a veil, exhaust is a wall);
//   billow  the inverted-worley billows, blended by how deep inside each puff
//           p is -- exact where one rules, a soft crease where two meet;
//   fbm     the slow noise, the same way.
struct PuffSample { float F; float op; float billow; float fbm; };
PuffSample puffsAt(vec3 p, vec4 idx) {
    PuffSample r;
    r.F = 0.0; r.op = 1.0; r.billow = 0.5; r.fbm = 0.5;
    float best = 0.0, sum = 0.0, opw = 0.0, wsum = 0.0, mg = 0.0;
    vec2  acc = vec2(0.0);
    for (int k = 0; k < 4; ++k) {
        float fi = idx[k];
        if (fi < 0.0) break;
        uint  i = uint(fi + 0.5);
        vec4  a = P[3u * i];
        vec4  c = P[3u * i + 1u];
        vec3  d = (p - a.xyz) / a.w;
        float k1 = 1.0 - dot(d, d);
        if (k1 <= 0.0) continue;
        float k2 = k1 * k1;
        float w  = k2 * c.x;
        best = max(best, w);
        sum += w;
        opw += w * c.w;
        mg  += w * P[3u * i + 2u].x;
        // Steep: one puff owns a point, the hand-over is short. By weight, so
        // a puff fading in takes its billows over gradually, not at birth.
        float wn = w * w;
        wn *= wn;
        vec4 n = texture(uNoise, d * uNoiseK + seedVec(c.z));
        acc  += wn * vec2(n.g * 0.55 + n.b * 0.30 + n.a * 0.15, n.r);
        wsum += wn;
    }
    r.F = best + mix(0.15, 1.0, sum > 0.0 ? mg / sum : 0.0) * (sum - best);
    if (sum > 0.0) r.op = opw / sum;
    if (wsum > 0.0) { r.billow = acc.x / wsum; r.fbm = acc.y / wsum; }
    return r;
}

// The puff list for p, from a voxel picked at random among p's neighbours
// with the trilinear weights (stochastic filtering): a list changes from one
// voxel to the next, and picked by the nearest voxel that change is a step
// you can see; picked like this it is noise, which the temporal AA averages
// into the blend the hardware cannot do for indices.
vec4 idxAt(sampler3D tex, vec3 u, vec3 dims, vec3 jit) {
    return texture(tex, u + (jit - 0.5) / dims);
}

vec4 gridIdx(int g, vec3 u, vec3 jit) {
    if (g == 0) return idxAt(uB0, u, uE0 / uV0, jit);
    if (g == 2) return idxAt(uB2, u, uE2 / uV2, jit);
    return idxAt(uB1, u, uE1 / uV1, jit);
}

// How sharply the cloud turns solid past its surface, in density-field units,
// for puffs of radius r. The field falls off over a puff's whole radius, so a
// fixed band of it is a few metres deep on a young puff and tens of metres on
// an old one -- and an old cloud would read as a translucent blob. Scaled, the
// surface is about as crisp in metres whatever the size of what it bounds.
float softFor(float r) {
    return uSoft * clamp(7.0 / max(r, 1.0), 0.12, 1.0);
}

// The carved cloud's extinction at p -- what the march sees, used for the
// short shadow taps that give each billow its own lit and shaded side (the grid
// is too coarse to know about anything the noise carved).
float sigmaDetail(vec3 p, vec3 jit) {
    vec3 u;
    int g = gridOf(p, u);
    if (g < 0) return 0.0;
    vec4 A = gridA(g, u);
    if (A.r <= uThresh * 0.1) return 0.0;
    PuffSample ps = puffsAt(p, gridIdx(g, u, jit));
    float F = max(ps.F, A.r * 0.8);
    return uSigma * ps.op *
           smoothstep(0.0, softFor(A.b), F - uThresh - uErode * (1.0 - ps.billow) + (ps.fbm - 0.5) * 0.2);
}

// Glow of hot exhaust: dull red through orange to yellow, radiance per unit of
// extinction.
vec3 heatColor(float h) {
    h = clamp(h, 0.0, 1.5);
    vec3 c = vec3(1.0, 0.22, 0.03) * smoothstep(0.02, 0.35, h)
           + vec3(0.0, 0.30, 0.08) * smoothstep(0.30, 0.75, h)
           + vec3(0.0, 0.22, 0.30) * smoothstep(0.70, 1.20, h);
    return c * h * h * 26.0;
}

vec3 flameColor(float hot, vec3 tint) {
    vec3 white = vec3(1.0, 0.93, 0.80);
    vec3 deep  = tint * vec3(1.0, 0.55, 0.40);
    return mix(deep, mix(tint, white, smoothstep(0.35, 0.85, hot)), smoothstep(0.0, 0.3, hot));
}

// Ray against a finite cylinder (the flame's bounds): entry and exit t.
vec2 rayCyl(vec3 ro, vec3 rd, vec3 a, vec3 ax, float len, float rad) {
    vec3 oc = ro - a;
    float card = dot(ax, rd), caoc = dot(ax, oc);
    float A = 1.0 - card * card;
    float B = dot(oc, rd) - caoc * card;
    float C = dot(oc, oc) - caoc * caoc - rad * rad;
    vec2 tc;
    if (A < 1e-6) {
        if (C > 0.0) return vec2(1e9, -1e9);
        tc = vec2(-1e9, 1e9);
    } else {
        float h = B * B - A * C;
        if (h < 0.0) return vec2(1e9, -1e9);
        h = sqrt(h);
        tc = vec2((-B - h) / A, (-B + h) / A);
    }
    vec2 ts;
    if (abs(card) < 1e-6) {
        if (caoc < 0.0 || caoc > len) return vec2(1e9, -1e9);
        ts = vec2(-1e9, 1e9);
    } else {
        float s0 = -caoc / card, s1 = (len - caoc) / card;
        ts = vec2(min(s0, s1), max(s0, s1));
    }
    return vec2(max(tc.x, ts.x), min(tc.y, ts.y));
}

// One flame along this ray: emitted light, what it lets through, and where.
void flameMarch(int f, vec3 ro, vec3 rd, float tScene, float jit,
                out vec3 E, out float Tf, out float at) {
    E = vec3(0.0);
    Tf = 1.0;
    at = -1.0;
    vec3  a    = uFA[f];
    vec3  ax   = uFD[f];
    float len  = uFP[f].x, r0 = uFP[f].y, r1 = uFP[f].z, bright = uFP[f].w;
    float core = uFQ[f].x, diamonds = uFQ[f].y, seed = uFQ[f].z;
    vec2 h = rayCyl(ro, rd, a, ax, len, r1 * 1.35 + 0.5);
    h.x = max(h.x, 0.0);
    h.y = min(h.y, tScene);
    if (h.y <= h.x) return;
    const int N = 32;
    float dt = (h.y - h.x) / float(N);
    float t  = h.x + dt * jit;
    float kCore = mix(0.025, 0.9, core);
    // A breath, not a strobe: the turbulence below already moves the flame, and
    // anything faster than a few hertz here shows as the picture pumping.
    float flick = 0.97 + 0.03 * sin(uTime * 9.0 + seed * 40.0) * sin(uTime * 5.7 + seed * 11.0);
    for (int k = 0; k < N; ++k, t += dt) {
        vec3  w   = ro + rd * t - a;
        float s   = dot(w, ax);
        if (s < 0.0 || s > len) continue;
        float rho = length(w - ax * s);
        float sn  = s / len;
        // A plume opens fast just past the nozzle, then slower.
        float R   = r0 + (r1 - r0) * (0.35 * sqrt(sn) + 0.65 * sn);
        // Turbulence riding down the plume with the gas.
        vec3  nc  = (w - ax * (uTime * 55.0)) * (0.9 / max(r0, 0.3)) * 0.06
                    + vec3(seed, seed * 1.7, seed * 2.3);
        float n   = texture(uNoise, nc).r;
        float n2  = texture(uNoise, nc * 2.7 + 0.31).g;
        float rr  = rho / (R * (0.7 + 0.45 * n + 0.2 * n2 * sn));
        float radial = exp(-rr * rr * 2.2);
        float axial  = smoothstep(0.0, 0.025, sn) * (1.0 - smoothstep(0.5, 1.0, sn));
        float hot    = (1.0 - sn) * radial;
        float lam    = 2.6 * r0 * (1.0 + 0.5 * sn);
        float dia    = diamonds * pow(0.5 + 0.5 * cos(2.0 * PI * s / lam), 10.0)
                     * exp(-sn * 3.5) * exp(-rr * rr * 5.0);
        float kAbs   = kCore * radial * axial;
        vec3  col    = flameColor(hot, uFC[f]);
        float B      = bright * flick * (0.3 + 0.7 * (1.0 - sn)) * (0.8 + 0.4 * n);
        E  += Tf * col * B * (kAbs + dia * 0.6 * axial) * dt;
        Tf *= exp(-kAbs * dt * 0.8);
    }
    at = 0.5 * (h.x + h.y);
}

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    vec4 farH = uInvViewProjRel * vec4(vNdc, 1.0, 1.0);
    vec3 rd = normalize(farH.xyz / farH.w);
    vec3 ro = uCamPos;
    float d = texture(uDepth, uv).r;
    float tScene = 1e7;
    if (d < 1.0) {
        vec4 hp = uInvViewProjRel * vec4(vNdc, d * 2.0 - 1.0, 1.0);
        tScene = length(hp.xyz / hp.w);
    }
    float jit = fract(ign(gl_FragCoord.xy) + ((uStable & 2) != 0 ? 0.0 : uJitter));
    vec3 jit3 = fract(vec3(jit, ign(gl_FragCoord.yx + 17.0) + uJitter * 1.7, ign(gl_FragCoord.xy * 1.3 + 41.0) + uJitter * 2.3));

    // --- Flames first, each on its own -----------------------------------
    vec3  fE[MAX_FLAMES];
    float fT[MAX_FLAMES];
    float fAt[MAX_FLAMES];
    for (int f = 0; f < MAX_FLAMES; ++f) {
        fE[f] = vec3(0.0); fT[f] = 1.0; fAt[f] = -1.0;
        if (f < uFlameCount) flameMarch(f, ro, rd, tScene, jit, fE[f], fT[f], fAt[f]);
    }

    // --- The cloud -------------------------------------------------------
    vec2 span = vec2(1e9, -1e9);
    if (uHave0 != 0) {
        vec2 b = boxHit(ro, rd, uO0, uO0 + uE0);
        if (b.y > b.x) span = vec2(min(span.x, b.x), max(span.y, b.y));
    }
    if (uHave1 != 0) {
        vec2 b = boxHit(ro, rd, uO1, uO1 + uE1);
        if (b.y > b.x) span = vec2(min(span.x, b.x), max(span.y, b.y));
    }
    if (uHave2 != 0) {
        vec2 b = boxHit(ro, rd, uO2, uO2 + uE2);
        if (b.y > b.x) span = vec2(min(span.x, b.x), max(span.y, b.y));
    }
    float t0 = max(span.x, 0.0), t1 = min(span.y, tScene);

    float T = 1.0;
    vec3  L = vec3(0.0);
    float cosT = dot(rd, uSunDir);
    float phSun = mix(hg(cosT, 0.62), hg(cosT, -0.22), 0.35);
    float iso = 1.0 / (4.0 * PI);
    bool added[MAX_FLAMES];
    for (int f = 0; f < MAX_FLAMES; ++f) added[f] = fAt[f] < 0.0;

    if (t1 > t0) {
        float t = t0 + jit * max(uV0 * 0.55, t0 * 0.0025);
        for (int i = 0; i < uMaxSteps; ++i) {
            if (t >= t1 || T < 0.004) break;
            vec3 p = ro + rd * t;
            float vox;
            float skip = emptySkip(p, rd, vox);
            if (uDebug == 1 && skip < 1e8) skip = 0.0;
            // Never so fine that the budget runs out before the ray has crossed
            // the volume: a ray cut short draws the clouds behind it cut off.
            float ds = max(max(vox * 0.55, t * 0.0025), (t1 - t0) / float(uMaxSteps / 2));
            float tNext = t + (skip > 0.0 ? max(skip + 0.05, ds) : ds);
            if (skip <= 0.0) {
                vec3 u;
                int g = gridOf(p, u);
                vec4 A = gridA(g, u);
                if (A.r > uThresh * 0.1) {
                    vec3 jit = (uStable & 1) != 0 ? vec3(0.5)
                             : fract(vec3(jit3.x + float(i) * 0.6180339, jit3.y + float(i) * 0.7548777,
                                          jit3.z + float(i) * 0.5698403));
                    vec4 idx = gridIdx(g, u, jit);
                    vec3 Lt  = gridL(g, u).xyz;
                    PuffSample ps = puffsAt(p, idx);
                    float F = max(ps.F, A.r * 0.8);
                    float billow = ps.billow;
                    float dd  = F - uThresh - uErode * (1.0 - billow) + (ps.fbm - 0.5) * 0.2;
                    float sig = uSigma * ps.op * smoothstep(0.0, softFor(A.b), dd);
                    if (sig > 1e-5) {
                        // Two taps towards the sun through the carved cloud, on top
                        // of what the grid says about the rest of the way.
                        float sh = vox * 0.7;
                        float ts = Lt.x + (sigmaDetail(p + uSunDir * sh, jit) * sh +
                                           sigmaDetail(p + uSunDir * sh * 2.6, jit) * sh * 1.6) * 0.8;
                        // Single scattering with the phase, then the light that has
                        // bounced around inside: in a cloud this thick that is most
                        // of it, and it is what makes the sunlit side read as a
                        // white surface rather than a faint haze.
                        // The 0.22 is the light a thick cloud sends back from the
                        // side the sun is on whatever the angle (what single
                        // scattering with a phase function alone gets far too dark,
                        // because it ignores the bounces just under the surface); it
                        // shares the direct term's shadow, which is what gives every
                        // billow a lit and a shaded side.
                        vec3 sunL = uSunColor * uSunI *
                                    ((phSun + 0.22) * exp(-ts) + iso * (1.3 * exp(-ts * 0.35) + 0.6 * exp(-ts * 0.1)));
                        vec3 fl = vec3(0.0);
                        for (int f = 0; f < uFlameCount; ++f) {
                            vec3  ab = uFD[f] * uFP[f].x;
                            float hh = clamp(dot(p - uFA[f], ab) / max(dot(ab, ab), 1e-4), 0.0, 1.0);
                            vec3  dv = p - (uFA[f] + ab * hh);
                            // Never closer than ~25 m in the falloff: the flame is a
                            // body of light tens of metres long, not a point, and the
                            // steam right beside it would otherwise burn out to white.
                            float rr = max(uFP[f].z * 2.0 + 2.0, 25.0);
                            fl += uFC[f] * uFQ[f].w / (dot(dv, dv) + rr * rr);
                        }
                        float tf = Lt.y;
                        fl *= uFlameI * iso * (exp(-tf) + 1.2 * exp(-tf * 0.3) + 0.8 * exp(-tf * 0.08));
                        float crease = mix(0.45, 1.0, billow);
                        vec3 amb = (uSkyColor * uSkyI * (0.25 + 0.75 * exp(-Lt.z * 0.5))
                                    + uGroundColor * 0.35) * crease;
                        vec3 em  = heatColor(A.g) * uHeatI;
                        // Times pi: the surfaces around are lit as albedo * light *
                        // cos (lit.frag, no 1/pi), so a cloud lit by the same sun has
                        // to be pi brighter than the radiometric answer to sit beside
                        // a white wall at the same brightness.
                        vec3 Sin = (sunL + fl + amb) * (PI * uAlbedo) * uTint + em;
                        float ext = exp(-sig * (tNext - t));
                        L += T * Sin * (1.0 - ext);
                        T *= ext;
                    }
                }
            }
            for (int f = 0; f < uFlameCount; ++f) {
                if (!added[f] && fAt[f] <= tNext) {
                    L += T * fE[f];
                    T *= fT[f];
                    added[f] = true;
                }
            }
            t = tNext;
        }
    }
    // Flames beyond (or without) any cloud.
    for (int f = 0; f < uFlameCount; ++f) {
        if (!added[f]) {
            L += T * fE[f];
            T *= fT[f];
        }
    }
    FragColor = vec4(L, T);
}
