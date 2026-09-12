#version 330 core

in vec4 vCur;
in vec4 vPrev;
in vec2 vUv;
out vec4 oMotion;

uniform sampler2D uTex;
uniform int uAlphaCutout;

void main() {
    if (uAlphaCutout == 1 && texture(uTex, vUv).a < 0.5) discard;
    // Renderer's convention: UV units, alpha 1 = measured.
    vec2 cur  = vCur.xy  / vCur.w;
    vec2 prev = vPrev.xy / vPrev.w;
    oMotion = vec4((cur - prev) * 0.5, 0.0, 1.0);
}
