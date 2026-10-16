#version 330 core

// One ring of the far terrain (see FarTerrain.hpp): a unit grid laid over the
// ring's square and lifted by the heightfield the worker computed for it.

layout(location = 0) in vec2 aGrid;   // 0..1 across the ring

uniform mat4      uViewProj;
uniform sampler2D uHeight;     // r = height (m), g = moisture 0..1
uniform vec2      uOrigin;     // world XZ of the ring's first sample
uniform float     uSize;       // world extent of the ring
uniform float     uGrid;       // samples per side
uniform vec4      uHole;       // xz min, xz max: what a finer surface covers
uniform float     uSink;       // how far to drop the part of the ring under it
uniform float     uWaterLevel;
uniform vec3      uOceanCut;   // eye x, eye z, reach: the open water draws the lakes in here

out vec3  vWorldPos;
out float vSunk;               // 1 = under the finer surface

void main() {
    vec2  xz  = uOrigin + aGrid * uSize;
    // Vertex (i, j) sits exactly on texel (i, j): texel centres, not edges.
    vec2  uv  = (aGrid * (uGrid - 1.0) + 0.5) / uGrid;
    float h   = textureLod(uHeight, uv, 0.0).r;
    float y   = max(h, uWaterLevel);           // a lake's surface is flat
    // Within the open water's reach its own surface (water.vert) is drawn over
    // the lakes, waves and all, in this depth range -- so here they are left as
    // the bed, well under the troughs, for it to cover.
    if (h < uWaterLevel && distance(xz, uOceanCut.xy) < uOceanCut.z)
        y = min(h, uWaterLevel - 15.0);
    // Under the finer surface: out of sight, but still a surface, so a chunk
    // that has not streamed in yet shows a dip rather than the sky.
    vSunk = 0.0;
    if (xz.x > uHole.x && xz.y > uHole.y && xz.x < uHole.z && xz.y < uHole.w) {
        y -= uSink;
        vSunk = 1.0;
    }
    vWorldPos = vec3(xz.x, y, xz.y);
    gl_Position = uViewProj * vec4(vWorldPos, 1.0);
}
