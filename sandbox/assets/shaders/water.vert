#version 330 core

layout(location = 0) in vec3 aPos; // grid in [-0.5,0.5], scaled by uModel

uniform mat4  uModel;
uniform mat4  uViewProj;
uniform float uTime;
uniform float uWaveHeight; // master swell amplitude
uniform float uChoppy;     // 0..1 Gerstner steepness

out vec4  vClip;
out vec3  vWorldPos;
out vec3  vWaveNormal;
out float vCrest;          // -1..1, high on wave crests (for whitecaps)

const float TWO_PI = 6.2831853;
const int   NWAVES = 4;

// Sum a few Gerstner waves: vertical + horizontal displacement, plus the
// analytic surface normal (GPU Gems formulation) and a crest factor.
void gerstner(vec2 pos, out vec3 disp, out vec3 nrm, out float crest) {
    vec2  dir[4]; float wl[4]; float amp[4];
    dir[0] = normalize(vec2( 1.0,  0.30)); wl[0] = 48.0; amp[0] = 1.00;
    dir[1] = normalize(vec2(-0.7,  0.70)); wl[1] = 31.0; amp[1] = 0.55;
    dir[2] = normalize(vec2( 0.2, -1.00)); wl[2] = 19.0; amp[2] = 0.32;
    dir[3] = normalize(vec2(-0.6, -0.45)); wl[3] = 12.0; amp[3] = 0.18;

    disp = vec3(0.0);
    float nx = 0.0, ny = 0.0, nz = 0.0;
    crest = 0.0;
    float ampSum = 0.0;

    for (int i = 0; i < NWAVES; ++i) {
        float k = TWO_PI / wl[i];
        float w = sqrt(9.8 * k);          // deep-water dispersion
        float a = amp[i] * uWaveHeight;
        float q = uChoppy / (k * a * float(NWAVES) + 1e-3); // keep loops away
        q = min(q, 1.0);

        float ph = k * dot(dir[i], pos) - w * uTime;
        float c = cos(ph), s = sin(ph);

        disp.xz += q * a * dir[i] * c;
        disp.y  += a * s;

        float wa = k * a;
        nx += dir[i].x * wa * c;
        nz += dir[i].y * wa * c;
        ny += q * wa * s;

        crest  += amp[i] * s;
        ampSum += amp[i];
    }

    nrm   = normalize(vec3(-nx, 1.0 - ny, -nz));
    crest = crest / max(ampSum, 1e-3);
}

void main() {
    vec4 world = uModel * vec4(aPos, 1.0);
    vec3 disp, nrm; float crest;
    gerstner(world.xz, disp, nrm, crest);
    world.xyz += disp;

    vWorldPos   = world.xyz;
    vWaveNormal = nrm;
    vCrest      = crest;
    vClip       = uViewProj * world;
    gl_Position = vClip;
}
