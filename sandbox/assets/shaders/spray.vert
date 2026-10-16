#version 330 core
// Spray: one GL_POINT per particle. Three kinds share this shader -- airborne
// droplets (aFlat 0), flat surface foam on the water (aFlat 1) and mist (aFlat
// 2). Per-particle size lets foam patches and mist be large while droplets
// stay fine.
layout(location = 0) in vec3  aPos;
layout(location = 1) in float aLife; // 1 = fresh, 0 = dead
layout(location = 2) in float aSize; // per-particle size factor
layout(location = 3) in float aFlat; // 0 = droplet, 1 = surface foam, 2 = mist

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
    float px   = (260.0 / max(dist, 1.0)) * aSize * (aFlat > 1.5 ? 1.0 : uSize);
    gl_PointSize = clamp(px, 1.0, aFlat > 1.5 ? 400.0 : 200.0);
}
