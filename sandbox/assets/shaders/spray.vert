#version 330 core
// Boat foam: one GL_POINT per particle. Two kinds share this shader -- airborne
// droplets (aFlat 0) and flat surface foam on the water (aFlat 1). Per-particle
// size lets foam patches be large while droplets stay fine.
layout(location = 0) in vec3  aPos;
layout(location = 1) in float aLife; // 1 = fresh, 0 = dead
layout(location = 2) in float aSize; // per-particle size factor
layout(location = 3) in float aFlat; // 0 = droplet, 1 = surface foam

uniform mat4  uViewProj;
uniform vec3  uCam;
uniform float uSize;   // global droplet size scale (inspector-tunable)

out float vLife;
out float vFlat;

void main() {
    vLife = aLife;
    vFlat = aFlat;
    gl_Position = uViewProj * vec4(aPos, 1.0);
    float dist = length(aPos - uCam);
    float px   = (260.0 / max(dist, 1.0)) * aSize * uSize;
    gl_PointSize = clamp(px, 1.0, 200.0);
}
