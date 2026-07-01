#version 330 core

in float vH;
in vec3  vWorldPos;
in vec3  vNormal;
in vec3  vBaseCol; // per-blade colours (computed in the vertex shader)
in vec3  vTipCol;
out vec4 FragColor;

uniform vec3 uViewPos;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbient;
uniform vec3 uTint;       // grass colour multiplier

uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;

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
    // Per-blade base/tip colours come from the vertex shader. Distance flattening
    // blends toward the tip so far-field blades don't shimmer into noise.
    float dist    = length(vWorldPos - uViewPos);
    float farFade = smoothstep(8.0, 38.0, dist);
    float h       = mix(vH, 0.6, farFade);
    vec3 albedo = pow(mix(vBaseCol, vTipCol, h), vec3(2.2)) * uTint; // -> linear

    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    vec3 V = normalize(uViewPos - vWorldPos);
    float diff = max(dot(N, L), 0.0);
    float back = max(dot(-N, L), 0.0) * 0.3; // light wrapping through the blade

    // Sun translucency: blades glow when the sun is behind them toward the eye,
    // strongest at the tips -> the warm backlit shimmer of a real meadow.
    float trans = pow(max(dot(V, -L), 0.0), 3.0) * (0.35 + 0.65 * vH);
    vec3  glow  = uLightColor * albedo * trans * 1.5;

    vec3 color = albedo * uAmbient
               + uLightColor * albedo * (diff * 0.85 + 0.2 + back)
               + glow;
    color *= mix(mix(0.72, 1.0, vH), 1.0, farFade); // gentle base AO, none at distance

    color = applyFog(color, vWorldPos, uViewPos, uLightDir);
    FragColor = vec4(color, 1.0);
}
