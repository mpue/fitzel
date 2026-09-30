#version 330 core

// The preview's sun shadow map: only the depth matters; leaves cut by alpha.
in vec2 vUv;
out vec4 FragColor;

uniform sampler2D uTex;
uniform int uLeaf;

void main() {
    if (uLeaf == 1 && texture(uTex, vUv).a < 0.5) discard;
    FragColor = vec4(1.0);
}