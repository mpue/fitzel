#version 330 core

// A distant tree's shadow (VegetationSystem::drawImpostorShadows): its impostor
// card stood upright and turned square to the sun, drawn into a cascade. What
// the sun sees of it is the tree's silhouette, so what it casts is the tree's
// shadow -- for one quad instead of the mesh.

layout(location = 3) in vec3  iPos;
layout(location = 4) in float iRot;
layout(location = 5) in float iScale;

uniform mat4  uLightSpace;
uniform vec3  uEye;         // the camera (y unused): the band is a ground distance
uniform float uFrom;        // nearer than this the meshes cast
uniform float uTo;          // further than this nothing does
uniform vec3  uAcross;      // horizontal, square to the sun's heading
uniform float uAspect;      // one view's width / height (the tree is unit height)
uniform float uViews;       // side views in the atlas row
uniform float uSideFrac;    // their share of the atlas width

#include "wind.glsl"
#include "treewind.glsl"

out vec2 vUv;

void main() {
    int   id = gl_VertexID;
    vec2  corner = vec2(float(id & 1), float((id >> 1) & 1));
    float d = length(iPos.xz - uEye.xz);
    if (d < uFrom || d > uTo) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }
    float h     = iScale;
    float cardH = 1.04 * h;
    vec3  p = iPos + uAcross * (corner.x - 0.5) * cardH * uAspect
            + vec3(0.0, corner.y * cardH - 0.02 * h, 0.0);
    p += treeWind(vec3(0.0, corner.y * 1.04 - 0.02, 0.0), iPos, iScale, iRot, false);
    // The same view the lit card shows (impostor.vert), so the shadow belongs.
    float view = floor(fract(iRot / 6.2831853) * uViews);
    vUv = vec2((view + corner.x) / uViews * uSideFrac, corner.y);
    gl_Position = uLightSpace * vec4(p, 1.0);
}
