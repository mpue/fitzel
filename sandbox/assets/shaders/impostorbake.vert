#version 330 core

// Baking a tree's impostor (see VegetationSystem::bakeImpostor): the unit-height
// mesh, turned to one of the atlas views, through an orthographic camera.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

uniform mat4  uProj;
uniform float uYaw;

out vec3 vNormal;   // in the view's frame: x right, y up, z towards the camera
out vec2 vUv;

void main() {
    float c = cos(uYaw), s = sin(uYaw);
    vec3 p = vec3(aPos.x * c - aPos.z * s, aPos.y, aPos.x * s + aPos.z * c);
    vec3 n = vec3(aNormal.x * c - aNormal.z * s, aNormal.y, aNormal.x * s + aNormal.z * c);
    vNormal = n;
    vUv = aUv;
    gl_Position = uProj * vec4(p, 1.0);
}
