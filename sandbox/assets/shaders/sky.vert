#version 330 core

layout(location = 0) in vec3 aPos; // fullscreen quad in clip space (xy in [-1,1])

out vec2 vNdc;

void main() {
    vNdc        = aPos.xy;
    gl_Position = vec4(aPos.xy, 1.0, 1.0); // far plane
}
