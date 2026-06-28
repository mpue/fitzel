#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uViewProj;

out vec3 vNormal;    // world-space normal
out vec2 vUV;

void main() {
    vNormal     = mat3(transpose(inverse(uModel))) * aNormal;
    vUV         = aUV;
    gl_Position = uViewProj * uModel * vec4(aPos, 1.0);
}
