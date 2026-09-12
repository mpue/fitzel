#version 330 core

// The cloud-shadow map (CloudShadow.hpp): for each point of a ground plane, the
// sunlight left after the ray towards the sun has crossed the cumulus deck.
// The sky's own density (cumulus.glsl), so the shadow is of the cloud you see.

in vec2 vNdc;
out vec4 FragColor;

uniform float uTime;
uniform float uCoverage;
uniform float uCloudDensity;
uniform float uCloudScale;
uniform float uCloudSpeed;
uniform float uCloudBottom;
uniform float uCloudTop;

uniform vec2  uOrigin;     // world XZ of the map's corner
uniform float uSize;       // world extent
uniform float uRefY;       // the plane the map is for
uniform vec3  uSunDir;

#include "cumulus.glsl"

const float kSunSig = 0.0035;   // sky.frag's extinction towards the sun

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    vec3 L  = normalize(uSunDir);
    if (L.y < 0.03) { FragColor = vec4(1.0); return; }
    vec3 p0 = vec3(uOrigin.x + uv.x * uSize, uRefY, uOrigin.y + uv.y * uSize);
    float s0 = (uCloudBottom - uRefY) / L.y;
    float s1 = (uCloudTop    - uRefY) / L.y;
    const int N = 10;
    float ds = (s1 - s0) / float(N);
    float od = 0.0;
    for (int i = 0; i < N; ++i) {
        vec3 p = p0 + L * (s0 + (float(i) + 0.5) * ds);
        od += cloudBase(p) * uCloudDensity * ds;
    }
    // A cloud is not opaque to the sun from underneath: light scatters in
    // around the edges and through the thin ones, so the deepest shadow keeps
    // a fifth of the sun.
    float T = mix(0.2, 1.0, exp(-od * kSunSig));
    FragColor = vec4(T, T, T, 1.0);
}
