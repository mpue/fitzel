#version 330 core

in vec2 vUv;
in vec4 vColor;
in vec3 vWorld;

uniform sampler2D uTex;
uniform int   uHasTex;
uniform int   uAdditive;
uniform vec3  uCamPos;
uniform vec3  uFogColor;
uniform float uFogDensity;

out vec4 FragColor;

void main() {
    vec4 t;
    if (uHasTex == 1) {
        t = texture(uTex, vUv);
    } else {
        // A soft round dot, generated rather than sampled, so a freshly added
        // emitter looks like something before any sprite has been chosen. The
        // squared falloff keeps the edge from reading as a hard disc.
        float d = length(vUv - 0.5) * 2.0;
        float a = clamp(1.0 - d * d, 0.0, 1.0);
        t = vec4(1.0, 1.0, 1.0, a * a);
    }
    vec4 c = t * vColor;
    if (c.a < 0.004) discard;   // cheaper than blending a fully faded quad

    // Fog. Additive particles cannot fade TOWARD a colour -- they only ever add
    // light, so distance has to take that light away instead. Mixing them toward
    // the fog colour would make far-off sparks brighter than near ones.
    float d = length(vWorld - uCamPos);
    float f = clamp(1.0 - exp(-d * uFogDensity), 0.0, 1.0);
    if (uAdditive == 1) c.rgb *= (1.0 - f);
    else                c.rgb  = mix(c.rgb, uFogColor, f);

    FragColor = c;
}
