#version 330 core

layout(location = 0) in vec3 aPos;

uniform mat4 uModel;
uniform mat4 uViewProj;

out vec4 vClip;      // clip-space position, for projective texture lookup
out vec3 vWorldPos;

void main() {
    vec4 world  = uModel * vec4(aPos, 1.0);
    vWorldPos   = world.xyz;
    vClip       = uViewProj * world;
    gl_Position = vClip;
}
