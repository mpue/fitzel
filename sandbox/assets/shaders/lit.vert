#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uViewProj;
uniform vec4 uClipPlane; // world-space plane; fragments with dot < 0 are clipped

out vec3  vWorldPos;
out vec3  vNormal;
out vec2  vUV;
out float vViewDepth; // positive distance along the view direction

void main() {
    vec4 world  = uModel * vec4(aPos, 1.0);
    vWorldPos   = world.xyz;
    vNormal     = mat3(transpose(inverse(uModel))) * aNormal;
    vUV         = aUV;
    vViewDepth  = -(uView * world).z;

    gl_ClipDistance[0] = dot(world, uClipPlane);
    gl_Position = uViewProj * world;
}
