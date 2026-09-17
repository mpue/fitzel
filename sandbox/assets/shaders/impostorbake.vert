#version 330 core

// Baking a tree's impostor (see VegetationSystem::bakeImpostor): the unit-height
// mesh, turned to one of the atlas views, through an orthographic camera.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;

uniform mat4  uProj;
uniform float uYaw;
uniform int   uTop;     // 1: the view from straight above (x right, -z up the cell)

out vec3 vNormal;   // in the view's frame: x right, y up, z towards the camera
out vec2 vUv;

void main() {
    float c = cos(uYaw), s = sin(uYaw);
    vec3 p = vec3(aPos.x * c - aPos.z * s, aPos.y, aPos.x * s + aPos.z * c);
    vec3 n = vec3(aNormal.x * c - aNormal.z * s, aNormal.y, aNormal.x * s + aNormal.z * c);
    if (uTop == 1) {
        // Laid on its back: height becomes the depth towards this camera.
        p = vec3(aPos.x, -aPos.z, aPos.y - 0.5);
        n = vec3(aNormal.x, -aNormal.z, aNormal.y);
    }
    vNormal = n;
    vUv = aUv;
    gl_Position = uProj * vec4(p, 1.0);
}
