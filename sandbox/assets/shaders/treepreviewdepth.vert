#version 330 core

// The preview's sun shadow map (TreePreview.cpp).
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUv;

uniform mat4 uViewProj;
out vec2 vUv;

void main() {
    vUv = aUv;
    gl_Position = uViewProj * vec4(aPos, 1.0);
}