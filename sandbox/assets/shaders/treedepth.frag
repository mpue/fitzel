#version 330 core

in vec2 vUv;
uniform sampler2D uTex;
uniform int uAlphaCutout;
uniform float uAlphaCutoff;

void main() {
    if (uAlphaCutout == 1 && texture(uTex, vUv).a < uAlphaCutoff) discard;
}
