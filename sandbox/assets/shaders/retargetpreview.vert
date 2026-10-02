#version 330 core

// The retargeting studio (RetargetPreview.cpp): two figures on a floor, the
// motion's source on the left, the character playing it on the right. Skinned
// on the CPU; this only places, lights and -- for the shadows -- flattens.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;    // the bone overlay's colour (Vertex::paint)

uniform mat4  uViewProj;
uniform mat4  uModel;
uniform int   uFlatten;                 // 1: squash onto the floor along the light
uniform vec3  uLightDir;                // the way the light travels (pointing down)

out vec3 vPos;
out vec3 vNormal;
out vec2 vUv;
out vec4 vColor;

void main() {
    vec3 p = (uModel * vec4(aPos, 1.0)).xyz;
    if (uFlatten == 1) {
        const float floorY = 0.004;
        float k = (p.y - floorY) / max(-uLightDir.y, 0.05);
        p += uLightDir * k;
        p.y = floorY;
    }
    vPos    = p;
    vNormal = mat3(uModel) * aNormal;
    vUv     = aUv;
    vColor  = aColor;
    gl_Position = uViewProj * vec4(p, 1.0);
}