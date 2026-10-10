#version 330 core

// The Materials panel's preview (MaterialPreview.cpp): one sphere, or a
// full-screen quad for the backdrop (uBackdrop = 1, positions already in clip space).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

uniform mat4 uViewProj;
uniform mat4 uModel;
uniform int  uBackdrop;

out vec3 vPos;
out vec3 vNormal;
out vec2 vUv;

void main() {
    vUv = aUv;
    if (uBackdrop == 1) {
        vPos = aPos;
        vNormal = vec3(0.0, 0.0, 1.0);
        gl_Position = vec4(aPos.xy, 0.999, 1.0);
        return;
    }
    vec4 w = uModel * vec4(aPos, 1.0);
    vPos = w.xyz;
    vNormal = mat3(uModel) * aNormal;
    gl_Position = uViewProj * w;
}