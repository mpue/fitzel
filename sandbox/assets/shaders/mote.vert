#version 330 core

// Motes in the air (Motes.hpp): a camera-facing quad per mote, never smaller
// than a pixel -- a grain of pollen three metres away is a hundredth of one,
// and a quad that small falls between the pixel centres and flickers. Grown
// to a pixel, its light is spread over the larger area, so it stays as bright
// in sum as the grain would be.
//
// The light is all worked out here, per mote: how much the grain scatters
// towards the eye (strongly forward -- the reason pollen is invisible until
// the sun is in front of you), and whether the sun reaches it at all.

layout(location = 0) in vec3 iPos;
layout(location = 1) in vec3 iMote;   // radius (m), kind (0 pollen 1 fluff 2 dust), phase

uniform mat4  uViewProj;
uniform vec3  uViewPos;
uniform vec3  uLightDir;     // towards the sun
uniform vec3  uLightColor;
uniform vec3  uAmbient;
uniform float uPxScale;      // pixels per metre at one metre
uniform float uRadius;       // the box's half-size: faded out before it
uniform float uTime;
uniform float uClear;        // 1 clear air .. 0 rain

#include "sunshadow.glsl"

out vec2 vUV;
out vec3 vCol;

// Henyey-Greenstein, unnormalised (1 for an isotropic scatterer at g = 0).
float hg(float cosT, float g) {
    float g2 = g * g;
    return (1.0 - g2) / pow(1.0 + g2 - 2.0 * g * cosT, 1.5);
}

void main() {
    vec2 c = vec2((gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0,
                  (gl_VertexID >= 2) ? 1.0 : -1.0);
    vec3  toM  = iPos - uViewPos;
    float dist = max(length(toM), 1e-3);
    vec3  rd   = toM / dist;
    vec3  right = normalize(cross(rd, vec3(0.0, 1.0, 0.0)) + vec3(1e-5, 0.0, 0.0));
    vec3  up    = cross(right, rd);

    float size  = iMote.x;
    float drawR = max(size, 1.1 * dist / uPxScale);
    float spread = (size * size) / (drawR * drawR);

    vec3  L    = normalize(uLightDir);
    float cosT = dot(rd, L);
    float kind = iMote.y;
    float sun  = 1.0 - sunShadow(iPos, L, 1.0);
    vec3  tint;
    float scatter;
    if (kind < 0.5) {            // pollen: yellowish, glints as it tumbles
        tint    = vec3(1.0, 0.86, 0.55);
        scatter = 0.9 * hg(cosT, 0.78) + 0.05;
        scatter *= 0.35 + 0.65 * pow(0.5 + 0.5 * sin(uTime * (2.0 + iMote.z) + iMote.z * 7.0), 2.0);
    } else if (kind < 1.5) {     // a seed's parachute: white, lit from any side
        tint    = vec3(1.0);
        scatter = 0.45 * hg(cosT, 0.55) + 0.30;
    } else {                     // dust: only against the light
        tint    = vec3(0.95, 0.92, 0.88);
        scatter = 1.2 * hg(cosT, 0.85);
    }
    float fade = smoothstep(0.35, 1.4, dist) * (1.0 - smoothstep(uRadius * 0.55, uRadius * 0.95, dist));
    vec3 light = uLightColor * sun * scatter + uAmbient * 0.25;
    vCol = tint * light * spread * fade * uClear * 3.0;

    vUV = c;
    vec3 wp = iPos + (right * c.x + up * c.y) * drawR;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
