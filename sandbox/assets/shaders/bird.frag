#version 330 core

in float vShade;
out vec4 FragColor;

uniform vec3 uColor; // dark silhouette (linear)

void main() {
    FragColor = vec4(uColor * vShade, 1.0);
}
