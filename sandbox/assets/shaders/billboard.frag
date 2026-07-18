#version 330 core

in vec2  vUv;
in vec3  vWorldPos;
in float vCull;
out vec4 FragColor;

uniform sampler2D uTex;
uniform vec3 uViewPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbient;

uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;

// Material colour correction -- kept identical to tree.frag so distant billboards
// track the near meshes.
uniform float uBrightness; // 1 = unchanged (multiplier)
uniform float uContrast;   // 1 = unchanged (pivots around mid-grey)
uniform float uHue;        // 0 = unchanged (radians, rotates about the grey axis)

vec3 hueShift(vec3 col, float a) {
    const vec3 k = vec3(0.57735026919); // normalize(vec3(1))
    float c = cos(a), s = sin(a);
    return col * c + cross(k, col) * s + k * dot(k, col) * (1.0 - c);
}

vec3 correct(vec3 c) {
    c = hueShift(c, uHue);
    c = (c - 0.5) * uContrast + 0.5;
    c *= uBrightness;
    return clamp(c, 0.0, 1.0);
}

vec3 applyFog(vec3 color, vec3 worldPos, vec3 eye, vec3 lightDir) {
    vec3  toFrag = worldPos - eye;
    float dist   = length(toFrag);
    vec3  rd     = toFrag / max(dist, 1e-4);
    float b = uFogHeightFalloff;
    float c = uFogDensity * exp(-(eye.y - uFogHeight) * b);
    float od = (abs(rd.y) > 1e-4)
             ? c * (1.0 - exp(-b * rd.y * dist)) / (b * rd.y)
             : c * dist;
    float fog = 1.0 - exp(-max(od, 0.0));
    float sunAmt = pow(max(dot(rd, normalize(lightDir)), 0.0), 4.0);
    return mix(color, mix(uFogColor, uFogSunColor, sunAmt), clamp(fog, 0.0, 1.0));
}

void main() {
    if (vCull > 0.5) discard;            // near trees are drawn as 3D meshes
    vec4 t = texture(uTex, vUv);
    if (t.a < 0.5) discard;              // alpha cutout

    // The texture is pre-shaded; modulate by scene light so it tracks day/night.
    vec3 albedo = pow(correct(t.rgb), vec3(2.2));
    vec3 lit = albedo * (uAmbient + uLightColor * 0.45);
    lit = applyFog(lit, vWorldPos, uViewPos, uLightDir);
    FragColor = vec4(lit, 1.0);
}
