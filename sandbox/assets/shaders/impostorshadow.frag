#version 330 core

in vec2 vUv;
uniform sampler2D uAlbedo;

void main() {
    if (texture(uAlbedo, vUv).a < 0.5) discard;
}
