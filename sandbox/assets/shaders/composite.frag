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

uniform sampler2D uBloomTex; // pre-blurred bloom pyramid (half res, already
                             // thresholded -- see bloomdown/bloomup.frag)
uniform float uBloom;       // bloom intensity
uniform float uRays;        // god-ray intensity

uniform sampler2D uAO;      // screen-space ambient occlusion (already denoised)
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

// Bright highlights (sun, sky, cloud rims, emissive surfaces) for the rays.
// Read from the bloom pyramid, which is already thresholded *and* blurred: the
// rays used to march the raw HDR, so every bright sub-pixel detail they crossed
// aliased into the streaks. A pre-filtered source cannot alias.
vec3 bright(vec2 uv) { return texture(uBloomTex, uv).rgb; }

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

// Depth-of-field gather. A single ring of 8 taps left the ring itself visible on
// anything with a bright background -- out-of-focus points became little rosettes.
// A golden-angle spiral spreads the same budget (raised to 24) evenly over the
// whole disc, so the blur reads as one soft circle of confusion. Samples are
// weighted by their own CoC so sharp foreground pixels don't bleed backwards.
vec3 dofColor(vec2 uv) {
    float c0 = cocAt(uv);
    if (uDofMax < 0.5 || c0 < 0.02) return texture(uHdr, uv).rgb;
    float radius = c0 * uDofMax;
    vec3  sum  = texture(uHdr, uv).rgb;
    float wsum = 1.0;
    const int   TAPS  = 24;
    const float GOLD  = 2.39996323; // golden angle (radians)
    for (int i = 0; i < TAPS; ++i) {
        float t = (float(i) + 0.5) / float(TAPS);
        float a = float(i) * GOLD;
        vec2  suv = uv + vec2(cos(a), sin(a)) * sqrt(t) * uTexel * radius;
        float w = cocAt(suv);
        sum  += texture(uHdr, suv).rgb * w;
        wsum += w;
    }
    return sum / wsum;
}

void main() {
    vec2 uv  = vNdc * 0.5 + 0.5;
    vec3 hdr = dofColor(uv);

    // Ambient occlusion darkens creases/valleys. Already denoised by the
    // depth-aware blur pass, so one bilinear tap is all it takes here.
    float ao = mix(1.0, texture(uAO, uv).r, uAoStrength);
    hdr *= ao;

    // Bloom: the pyramid pass did the work (threshold, downsample, tent
    // upsample); this is just its result, filtered up to full res.
    vec3 bloom = texture(uBloomTex, uv).rgb;

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
