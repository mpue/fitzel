#version 330 core

// No base VBO: the quad corner comes from gl_VertexID (triangle strip).
layout(location = 0) in vec3  iPos;   // world position
layout(location = 1) in float iPhase; // blink phase

uniform mat4  uViewProj;
uniform vec3  uCamRight;
uniform vec3  uCamUp;
uniform float uSize;

out vec2  vUV;
out float vPhase;

void main() {
    vec2 c = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0,
                  (gl_VertexID >= 2) ? 1.0 : -1.0);
    vec3 wp = iPos + uCamRight * (c.x * uSize) + uCamUp * (c.y * uSize);
    vUV    = c;
    vPhase = iPhase;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
