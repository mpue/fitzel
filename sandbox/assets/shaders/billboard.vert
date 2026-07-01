#version 330 core

// No per-vertex buffer: the quad corner comes from gl_VertexID (triangle strip).
layout(location = 3) in vec3  iPos;
layout(location = 4) in float iRot;
layout(location = 5) in float iScale;

uniform mat4  uViewProj;
uniform vec3  uCamRight;   // camera right vector (world)
uniform vec3  uCamPos;
uniform float uLodNear;    // below this distance the 3D mesh is used instead
uniform float uTreeHeight; // local mesh height (to match scale)
uniform float uAspect;     // texture width / height

out vec2  vUv;
out vec3  vWorldPos;
out float vCull;

void main() {
    vec2 corner = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 0.5 : -0.5,
                       (gl_VertexID >= 2) ? 1.0 : 0.0);

    float bh = uTreeHeight * iScale * 1.05;      // billboard height (~mesh height)
    float bw = bh * uAspect;
    vec3  right = normalize(vec3(uCamRight.x, 0.0, uCamRight.z)); // upright (cylindrical)

    vec3 wp = iPos + right * (corner.x * bw) + vec3(0.0, corner.y * bh, 0.0);

    vUv       = vec2(corner.x + 0.5, corner.y);
    vWorldPos = wp;
    vCull     = (length(iPos.xz - uCamPos.xz) < uLodNear) ? 1.0 : 0.0;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
