#version 330 core

in vec2 vNdc;
out vec4 FragColor;

uniform mat4 uInvViewProj;
uniform vec3 uCameraPos;

uniform vec3 uSunDir;     // towards the sun
uniform vec3 uSunColor;   // sun radiance (already dimmed at night)
uniform float uTime;

// Cloud controls.
uniform float uCoverage;     // 0..1, higher = more sky covered
uniform float uCloudDensity; // optical density multiplier
uniform float uCloudScale;   // noise frequency
uniform float uCloudSpeed;   // wind speed
uniform float uCloudBottom;  // slab altitudes (world units)
uniform float uCloudTop;

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

// --- 3D value-noise fBm ----------------------------------------------------
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
float fbm3(vec3 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) { s += a * vnoise3(p); p *= 2.02; a *= 0.5; }
    return s;
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

    // Sun disk + tight corona (HDR linear radiance). The soft halo comes from
    // bloom in the composite, so keep the in-sky glow tight to avoid a blowout.
    float sd   = max(dot(dir, uSunDir), 0.0);
    float disk = smoothstep(0.9991, 0.9995, sd);  // the bright disk
    float glow = pow(sd, 200.0) * 0.5;            // tight corona only
    vec3 sunTint = uSunColor * vec3(1.0, 0.9, 0.72);
    col += sunTint * (disk * 9.0 + glow);
    return col;
}

// Cloud density at a world point (0..~1), with height falloff inside the slab.
float cloudDensity(vec3 p) {
    vec3 wind = vec3(uTime * uCloudSpeed, 0.0, uTime * uCloudSpeed * 0.3);
    float n = fbm3((p + wind) * uCloudScale);
    float shape = smoothstep(uCoverage, uCoverage + 0.25, n);

    float t = (p.y - uCloudBottom) / max(uCloudTop - uCloudBottom, 1.0);
    float falloff = smoothstep(0.0, 0.25, t) * smoothstep(1.0, 0.6, t);
    return shape * falloff;
}

// Henyey-Greenstein phase function.
float phaseHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

// Raymarch the cloud slab. Returns premultiplied colour in .rgb and coverage
// (1 - transmittance) in .a.
vec4 renderClouds(vec3 ro, vec3 rd) {
    if (rd.y <= 0.02) return vec4(0.0); // looking at/below the horizon

    float t0 = (uCloudBottom - ro.y) / rd.y;
    float t1 = (uCloudTop    - ro.y) / rd.y;
    t0 = max(t0, 0.0);
    if (t1 <= t0) return vec4(0.0);
    t1 = min(t1, t0 + 1600.0); // cap march length near the horizon

    const int STEPS = 48;
    float dt = (t1 - t0) / float(STEPS);

    float cosA  = dot(rd, uSunDir);
    float phase = mix(phaseHG(cosA, 0.2), phaseHG(cosA, -0.15), 0.5);

    vec3 ambient = pow(mix(vec3(0.10, 0.13, 0.20), vec3(0.55, 0.62, 0.75),
                       smoothstep(-0.1, 0.2, uSunDir.y)), vec3(2.2));

    float T = 1.0;
    vec3  col = vec3(0.0);
    float t = t0 + dt * 0.5;
    for (int i = 0; i < STEPS; ++i) {
        vec3 p = ro + rd * t;
        float d = cloudDensity(p) * uCloudDensity;
        if (d > 0.001) {
            // Light march toward the sun for self-shadowing.
            float ls = 0.0;
            float lstep = (uCloudTop - uCloudBottom) * 0.12;
            for (int j = 1; j <= 5; ++j) {
                vec3 lp = p + uSunDir * (lstep * float(j));
                ls += cloudDensity(lp) * uCloudDensity;
            }
            float sun = exp(-ls * lstep * 0.9);

            vec3 lum = uSunColor * sun * phase * 8.0 + ambient;
            float dens = d * dt;
            float a = 1.0 - exp(-dens);
            col += T * lum * a;
            T   *= exp(-dens);
            if (T < 0.02) break;
        }
        t += dt;
    }
    return vec4(col, 1.0 - T);
}

void main() {
    // Reconstruct the world-space view ray from the NDC position.
    vec4 far = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 world = far.xyz / far.w;
    vec3 dir = normalize(world - uCameraPos);

    vec3 col = skyColor(dir);
    vec4 clouds = renderClouds(uCameraPos, dir);
    col = col * (1.0 - clouds.a) + clouds.rgb;

    FragColor = vec4(toOutput(col), 1.0);
}
