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

// HSV colour grade (applied to the final image).
uniform float uHueShift;    // degrees
uniform float uSaturation;
uniform float uValue;

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

void main() {
    vec2 uv  = vNdc * 0.5 + 0.5;
    vec3 hdr = texture(uHdr, uv).rgb;

    // Ambient occlusion darkens creases/valleys (bilinear-upscaled half-res AO).
    float ao = mix(1.0, texture(uAO, uv).r, uAoStrength);
    hdr *= ao;

    // --- Bloom: gaussian-weighted blur of the bright pass --------------
    vec3  bloom = vec3(0.0);
    float wsum  = 0.0;
    for (int x = -3; x <= 3; ++x) {
        for (int y = -3; y <= 3; ++y) {
            vec2  o = vec2(x, y) * uTexel * 6.0;
            float w = exp(-dot(vec2(x, y), vec2(x, y)) * 0.18);
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
