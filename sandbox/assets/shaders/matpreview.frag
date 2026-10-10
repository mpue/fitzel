#version 330 core

// The Materials panel's preview. The surface follows lit.frag's rules, so what
// the panel shows is what the scene draws: uReflectivity is metalness (F0 from
// 4% towards the base colour), uRoughness perceptual roughness squared into
// GGX, colours are sRGB and lit linear, the ORM map spreads the sliders' values
// texel by texel, emission is colour * strength (* map). The light is a studio's
// -- a key, a rim and a sky/floor environment -- because a preview has to show
// the material and not a scene. The backdrop is a dark checker so cutouts and
// see-through materials read as holes.
in vec3 vPos;
in vec3 vNormal;
in vec2 vUv;
out vec4 FragColor;

uniform int   uBackdrop;
uniform vec3  uEye;
uniform vec2  uUvScale;

uniform int   uHasTex;
uniform sampler2D uTex;
uniform vec3  uAlbedo;        // sRGB, untextured
uniform vec3  uTint;          // sRGB multiplier on the texture
uniform int   uHasNormal;
uniform sampler2D uNormalMap;
uniform int   uNormalTopDown;
uniform int   uHasOrm;
uniform sampler2D uOrmMap;
uniform vec2  uOrmMean;
uniform float uRoughness;
uniform float uReflectivity;
uniform vec3  uEmission;      // sRGB
uniform float uEmissionStrength;
uniform int   uHasEmissionMap;
uniform sampler2D uEmissionMap;
uniform int   uHasOpacityMap;
uniform sampler2D uOpacityMap;
uniform float uOpacity;
uniform int   uAlphaMode;     // 0 opaque, 1 cutout, 2 blend
uniform float uAlphaCutoff;
uniform int   uGlass;

const float PI = 3.14159265;
const vec3  kKeyDir  = normalize(vec3(-0.55, 0.7, 0.55));
const vec3  kRimDir  = normalize(vec3(0.7, 0.35, -0.6));

vec3 envLight(vec3 d, float rough) {
    // A studio dome: bright soft top, a warm horizon, a dark floor; blurred by
    // roughness toward its average so a rough ball does not mirror the dome.
    float y = d.y;
    vec3 top = vec3(0.85, 0.9, 1.0) * 1.3;
    vec3 hor = vec3(0.55, 0.5, 0.45);
    vec3 flo = vec3(0.08, 0.07, 0.07);
    vec3 sharp = y > 0.0 ? mix(hor, top, smoothstep(0.0, 0.8, y)) : mix(hor, flo, smoothstep(0.0, 0.3, -y));
    // a softbox: a bright window up and to the left
    float box = smoothstep(0.92, 0.97, dot(d, kKeyDir));
    sharp += vec3(4.0) * box;
    vec3 avg = vec3(0.45, 0.46, 0.48);
    return mix(sharp, avg, rough * rough);
}

vec3 applyNormalMap(vec3 N, vec3 p, vec2 uv) {
    vec3 nt = texture(uNormalMap, uv).xyz * 2.0 - 1.0;
    if (uNormalTopDown == 1) nt.y = -nt.y;
    vec3 dp1 = dFdx(p), dp2 = dFdy(p);
    vec2 du1 = dFdx(uv), du2 = dFdy(uv);
    vec3 dp2p = cross(dp2, N), dp1p = cross(N, dp1);
    vec3 T = dp2p * du1.x + dp1p * du2.x;
    vec3 B = dp2p * du1.y + dp1p * du2.y;
    float s = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
    return normalize(mat3(T * s, B * s, N) * nt);
}

float ggx(float NdH, float a) {
    float a2 = a * a;
    float d = NdH * NdH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d + 1e-6);
}

vec3 tonemap(vec3 c) {
    // ACES fit, then the display's gamma
    c = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);
    return pow(clamp(c, 0.0, 1.0), vec3(1.0 / 2.2));
}

void main() {
    if (uBackdrop == 1) {
        vec2 q = floor(vUv * vec2(12.0, 12.0));
        float chk = mod(q.x + q.y, 2.0);
        vec3 c = mix(vec3(0.16), vec3(0.22), chk);
        c *= 1.0 - 0.35 * length(vUv - 0.5);
        FragColor = vec4(c, 1.0);
        return;
    }
    vec2 uv = vUv * uUvScale;
    vec3 base;
    float alpha = 1.0;
    if (uHasTex == 1) {
        vec4 t = texture(uTex, uv);
        base = t.rgb * uTint;
        alpha = t.a;
    } else {
        base = uAlbedo;
    }
    if (uHasOpacityMap == 1) alpha = texture(uOpacityMap, uv).r;
    if (uAlphaMode == 1 && alpha < uAlphaCutoff) discard;
    float outA = uAlphaMode == 2 ? alpha * uOpacity : 1.0;
    base = pow(max(base, vec3(0.0)), vec3(2.2));

    vec3 N = normalize(vNormal);
    if (!gl_FrontFacing) N = -N;
    if (uHasNormal == 1) N = applyNormalMap(N, vPos, uv);
    vec3 V = normalize(uEye - vPos);

    float metal = clamp(uReflectivity, 0.0, 1.0);
    float rough = clamp(uRoughness, 0.03, 1.0);
    float occ = 1.0;
    if (uHasOrm == 1) {
        vec3 orm = texture(uOrmMap, uv).rgb;
        occ = orm.r;
        if (uOrmMean.x > 0.004) rough = clamp(rough * orm.g / uOrmMean.x, 0.03, 1.0);
        if (uOrmMean.y > 0.004) metal = clamp(metal * orm.b / uOrmMean.y, 0.0, 1.0);
    }
    vec3 F0 = mix(vec3(0.04), base, metal);
    vec3 diffCol = base * (1.0 - metal);
    float a = rough * rough;
    float NdV = max(dot(N, V), 1e-3);

    vec3 col = vec3(0.0);
    // key + rim
    vec3 L[2] = vec3[2](kKeyDir, kRimDir);
    vec3 C[2] = vec3[2](vec3(3.2, 3.05, 2.85), vec3(1.2, 1.3, 1.6));
    for (int i = 0; i < 2; ++i) {
        vec3 H = normalize(L[i] + V);
        float NdL = max(dot(N, L[i]), 0.0);
        float NdH = max(dot(N, H), 0.0);
        vec3 F = F0 + (1.0 - F0) * pow(1.0 - max(dot(H, V), 0.0), 5.0);
        float k = (rough + 1.0) * (rough + 1.0) / 8.0;
        float G = (NdL / (NdL * (1.0 - k) + k)) * (NdV / (NdV * (1.0 - k) + k));
        vec3 spec = ggx(NdH, a) * G * F / max(4.0 * NdL * NdV, 1e-3);
        col += (diffCol / PI * (1.0 - F) + spec) * C[i] * NdL;
    }
    // the environment: diffuse from the normal, specular from the reflection
    vec3 Fv = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - NdV, 5.0);
    col += diffCol * envLight(N, 1.0) * 0.6 * occ;
    col += Fv * envLight(reflect(-V, N), rough) * occ;

    if (uGlass == 1) {
        // see-through, with the reflection kept on top
        float fr = Fv.r;
        col = mix(vec3(0.0), col, 0.35) + Fv * envLight(reflect(-V, N), rough);
        outA = clamp(0.18 + fr * 0.8, 0.0, 1.0);
    }

    vec3 em = pow(max(uEmission, vec3(0.0)), vec3(2.2)) * uEmissionStrength;
    if (uHasEmissionMap == 1) em *= pow(texture(uEmissionMap, uv).rgb, vec3(2.2));
    col += em;
    FragColor = vec4(tonemap(col), outA);
}