#version 330 core

in vec2 vUv;
uniform sampler2D uTex;
uniform int uAlphaCutout;

void main() {
    if (uAlphaCutout == 1 && texture(uTex, vUv).a < 0.5) discard;
}
