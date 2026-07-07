#version 330 core

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uHdr;     // linear HDR scene
uniform vec2  uTexel;       // 1 / resolution
uniform float uAspect;
uniform float uExposure;

uniform vec2  uSunUV;       // sun screen position
uniform float uSunOnScreen; // 1 if the sun projects in front of the camera
uniform vec3  uSunColor;    // linear sun tint

uniform float uBloom;       // bloom intensity
uniform float uRays;        // god-ray intensity

uniform sampler2D uAO;      // screen-space ambient occlusion
uniform float uAoStrength;

// Depth of field (distance blur toward the background).
uniform sampler2D uDepth;
uniform float uNear;
uniform float uFar;
uniform float uFocusNear;   // metres: sharp up to here
uniform float uFocusFar;    // metres: fully blurred beyond here
uniform float uDofMax;      // max blur radius in pixels (0 = DOF off)

// HSV colour grade (applied to the final image).
uniform float uHueShift;    // degrees
uniform float uSaturation;
uniform float uValue;
uniform float uWarmth;      // white balance: + warms (golden), - cools (blue)
uniform float uContrast;    // S-curve contrast around mid grey

vec3 rgb2hsv(vec3 c) {
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}
vec3 hsv2rgb(vec3 c) {
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}
vec3 colorGrade(vec3 c) {
    // White balance: warm pushes red up / blue down (golden hour), cool the reverse.
    c *= vec3(1.0 + uWarmth * 0.5, 1.0 + uWarmth * 0.06, 1.0 - uWarmth * 0.45);

    // Contrast: filmic S-curve around mid grey to lift the flat/milky look.
    c = clamp((c - 0.5) * (1.0 + uContrast) + 0.5, 0.0, 1.0);

    vec3 hsv = rgb2hsv(c);
    hsv.x = fract(hsv.x + uHueShift / 360.0);
    hsv.y = clamp(hsv.y * uSaturation, 0.0, 1.0);
    hsv.z = hsv.z * uValue;
    return hsv2rgb(hsv);
}

float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Isolate bright highlights (sun, sky, cloud rims) for bloom / rays.
vec3 bright(vec2 uv) {
    vec3 c = texture(uHdr, uv).rgb;
    return c * smoothstep(1.4, 4.0, luma(c));
}

vec3 aces(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Eye-space distance from the depth buffer.
float linearDepth(vec2 uv) {
    float d = texture(uDepth, uv).r * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - d * (uFar - uNear));
}
// Blur amount 0..1 by distance (sharp foreground, blurred background).
float cocAt(vec2 uv) { return smoothstep(uFocusNear, uFocusFar, linearDepth(uv)); }

// Depth-of-field gather: a two-ring disk scaled by CoC. Samples are weighted by
// their own CoC so sharp foreground pixels don't bleed into blurred background.
vec3 dofColor(vec2 uv) {
    float c0 = cocAt(uv);
    if (uDofMax < 0.5 || c0 < 0.02) return texture(uHdr, uv).rgb;
    float radius = c0 * uDofMax;
    vec3  sum  = texture(uHdr, uv).rgb;
    float wsum = 1.0;
    const int TAPS = 8; // single ring is enough for a soft background blur
    for (int i = 0; i < TAPS; ++i) {
        float a = float(i) / float(TAPS) * 6.2831853;
        vec2  suv = uv + vec2(cos(a), sin(a)) * uTexel * radius;
        float w = cocAt(suv);
        sum  += texture(uHdr, suv).rgb * w;
        wsum += w;
    }
    return sum / wsum;
}

void main() {
    vec2 uv  = vNdc * 0.5 + 0.5;
    vec3 hdr = dofColor(uv);

    // Ambient occlusion darkens creases/valleys. The half-res AO carries the
    // SSAO kernel's per-pixel dither, so denoise it with a 4x4 box blur (step =
    // one AO texel = two full-res texels) before applying -- this is the SSAO
    // blur pass, folded into the composite to avoid a separate target.
    float aoSum = 0.0;
    for (int x = -2; x <= 1; ++x)
        for (int y = -2; y <= 1; ++y)
            aoSum += texture(uAO, uv + vec2(x, y) * uTexel * 2.0).r;
    float ao = mix(1.0, aoSum / 16.0, uAoStrength);
    hdr *= ao;

    // --- Bloom: gaussian-weighted blur of the bright pass (5x5, wider step) --
    vec3  bloom = vec3(0.0);
    float wsum  = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2  o = vec2(x, y) * uTexel * 9.0;
            float w = exp(-dot(vec2(x, y), vec2(x, y)) * 0.30);
            bloom += bright(uv + o) * w;
            wsum  += w;
        }
    }
    bloom /= wsum;

    // How visible the sun actually is (0 when occluded by terrain).
    float occl = smoothstep(1.5, 4.0, luma(texture(uHdr, clamp(uSunUV, 0.0, 1.0)).rgb))
               * uSunOnScreen;

    // --- God rays: radial march from the fragment toward the sun ------
    vec3 rays = vec3(0.0);
    if (uSunOnScreen > 0.5) {
        const int N = 32;
        vec2  delta = (uSunUV - uv) / float(N) * 0.92;
        vec2  s = uv;
        float w = 1.0, decay = 0.95;
        for (int i = 0; i < N; ++i) {
            s += delta;
            rays += bright(s) * w;
            w    *= decay;
        }
        rays = rays / float(N) * uSunColor;
    }

    // --- Lens flare: analytic halo + ghosts, gated by occlusion -------
    vec3 flare = vec3(0.0);
    if (occl > 0.001) {
        vec2  asp = vec2(uAspect, 1.0);
        float d   = length((uv - uSunUV) * asp);
        float halo = exp(-d * 7.0) * 0.7 + exp(-d * 26.0) * 0.5;
        flare += uSunColor * halo;

        vec2 dir = (vec2(0.5) - uSunUV);
        for (int i = 1; i <= 3; ++i) {
            vec2  g  = uSunUV + dir * (float(i) * 0.42);
            float dd = length((uv - g) * asp);
            flare += uSunColor * exp(-dd * 55.0) * (0.5 / float(i));
        }
        flare *= occl;
    }

    vec3 col = hdr + bloom * uBloom + rays * uRays + flare * (0.6 * uRays + 0.4);

    col = aces(col * uExposure);
    col = pow(col, vec3(1.0 / 2.2));
    col = colorGrade(col);
    FragColor = vec4(col, 1.0);
}
