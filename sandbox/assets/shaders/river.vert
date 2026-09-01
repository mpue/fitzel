#version 330 core

// The water surface of a brook, river or canal. The strip arrives in WORLD space
// (RiverSystem draws it with no model matrix), and everything the fragment stage
// needs about the channel rides in the attributes every vertex already has:
//   aUV   = (metres across the channel, metres along it from the source)
//   aData = (water depth in metres, whitewater, half-width in metres, how far
//           this water stands clear of the ground -- see Course::air)
// See RiverGen.hpp for why that packing exists rather than a vertex format of
// this shader's own.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aData;

uniform mat4 uViewProj;
uniform vec4 uClipPlane; // world-space plane; fragments with dot < 0 are clipped

out vec3 vWorldPos;
out vec3 vNormal;
out vec2 vUV;
out vec4 vData;

void main() {
    vWorldPos = aPos;
    vNormal   = aNormal;
    vUV       = aUV;
    vData     = aData;

    gl_ClipDistance[0] = dot(vec4(aPos, 1.0), uClipPlane);
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
