#version 330 core

// The retargeting studio's one material (see retargetpreview.vert). uMode:
// 0 a figure (its base colour map, a key light and a sky/ground fill),
// 1 the floor (a metre grid that fades out), 2 a shadow on that floor,
// 3 the bone overlay (flat vertex colour).
in vec3 vPos;
in vec3 vNormal;
in vec2 vUv;
in vec4 vColor;

uniform int       uMode;
uniform sampler2D uTex;
uniform int       uHasTex;
uniform int       uCutout;
uniform vec4      uColor;       // base colour factor (linear)
uniform vec3      uLightDir;
uniform vec3      uEye;
uniform vec3      uBackdrop;    // the clear colour: the floor fades into it
uniform float     uFloorR;      // where the fade ends

out vec4 fragColor;

float gridLine(vec2 p, float perMetre) {
    vec2 q = p * perMetre;
    vec2 g = abs(fract(q + 0.5) - 0.5) / max(fwidth(q), vec2(1e-4));
    return 1.0 - min(min(g.x, g.y), 1.0);
}

vec3 floorColour(vec3 p) {
    vec3 c = vec3(0.60, 0.61, 0.62);
    c = mix(c, vec3(0.52, 0.53, 0.55), gridLine(p.xz, 4.0) * 0.35);   // 25 cm
    c = mix(c, vec3(0.42, 0.43, 0.46), gridLine(p.xz, 1.0) * 0.75);   // 1 m
    return c;
}

void main() {
    if (uMode == 3) { fragColor = vColor; return; }
    if (uMode == 1 || uMode == 2) {
        vec3 c = floorColour(vPos);
        if (uMode == 2) c *= 0.64;
        float fade = smoothstep(uFloorR * 0.55, uFloorR, length(vPos.xz));
        fragColor = vec4(mix(c, uBackdrop, fade), 1.0);
        return;
    }
    vec4 base = uColor;
    if (uHasTex == 1) {
        vec4 t = texture(uTex, vUv);
        if (uCutout == 1 && t.a < 0.5) discard;
        base *= vec4(pow(t.rgb, vec3(2.2)), t.a);
    }
    vec3 n = normalize(vNormal);
    if (!gl_FrontFacing) n = -n;
    float key  = max(dot(n, -uLightDir), 0.0);
    float hemi = 0.5 + 0.5 * n.y;
    vec3 light = vec3(1.0, 0.97, 0.92) * key * 0.9 +
                 mix(vec3(0.16, 0.15, 0.14), vec3(0.40, 0.44, 0.50), hemi);
    vec3 col = base.rgb * light;
    vec3 v = normalize(uEye - vPos);
    col += vec3(0.05) * pow(1.0 - max(dot(n, v), 0.0), 3.0);   // a rim, so the outline reads
    fragColor = vec4(pow(col, vec3(1.0 / 2.2)), 1.0);
}