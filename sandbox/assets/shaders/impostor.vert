#version 330 core

// A tree past the mesh range, as one card of its baked impostor (see
// VegetationSystem::bakeImpostor). The card turns about the trunk to face the
// eye and leans back towards it the more the eye looks down, so the forest
// still has crowns from a hilltop rather than a field of edge-on cards.

layout(location = 3) in vec3  iPos;
layout(location = 4) in float iRot;
layout(location = 5) in float iScale;

uniform mat4  uViewProj;
uniform vec3  uCamPos;
uniform float uAspect;      // one view's width / height (the tree is unit height)
uniform float uViews;       // views in the atlas row
uniform float uStart;       // impostors take over here...
uniform float uFadeWidth;   // ...crossfading with the meshes over this band
uniform float uEnd;         // and fade out at the edge of the field
uniform float uTime;
uniform vec2  uWindDir;
uniform float uWindStrength;

out vec2  vUv;
out vec3  vWorldPos;
out vec3  vRight;
out vec3  vUp;
out vec3  vFwd;
out float vFade;
out float vFlip;

void main() {
    int   id = gl_VertexID;
    vec2  corner = vec2(float(id & 1), float((id >> 1) & 1));
    vec3  toCam  = uCamPos - iPos;
    float d      = length(toCam.xz);
    // 0 inside the mesh range, rising to 1 across the crossfade; out again at
    // the far edge so the field ends in a fade, not a line.
    vFade = smoothstep(uStart - uFadeWidth, uStart, d)
          * (1.0 - smoothstep(uEnd * 0.92, uEnd, d));
    if (vFade <= 0.0) { gl_Position = vec4(0.0, 0.0, 2.0, 1.0); return; }

    vec3  fwd   = (d > 1e-3) ? vec3(toCam.x, 0.0, toCam.z) / d : vec3(0.0, 0.0, 1.0);
    vec3  right = vec3(fwd.z, 0.0, -fwd.x);
    float elev  = atan(max(toCam.y, 0.0), max(d, 1e-3));
    float tilt  = clamp(elev, 0.0, 1.2) * 0.65;
    vec3  up    = cos(tilt) * vec3(0.0, 1.0, 0.0) + sin(tilt) * fwd;
    vec3  nrm   = cos(tilt) * fwd - sin(tilt) * vec3(0.0, 1.0, 0.0);

    // The bake framed the unit-height tree from -0.02 to 1.02 (a margin for
    // the crown's top): the card is that frame at this tree's size.
    float h     = iScale;
    float cardH = 1.04 * h;
    float w     = cardH * uAspect;
    vec3  p = iPos + right * (corner.x - 0.5) * w + up * (corner.y * cardH - 0.02 * h);
    // The crown sways like the mesh does (tree.vert), so the switch between
    // the two does not stop the wind.
    float sway = sin(uTime * 1.1 + iPos.x * 0.2 + iPos.z * 0.2);
    p.xz += uWindDir * (uWindStrength * 0.5) * corner.y * corner.y
          * (0.5 + 0.5 * sway) * iScale;

    // Which of the baked views this tree shows, and whether mirrored: its yaw
    // decides, so neighbours differ and a tree never changes view as you move.
    float r    = fract(iRot / 6.2831853);
    float view = floor(r * uViews);
    vFlip      = (fract(r * 7.31) > 0.5) ? 1.0 : 0.0;
    float u    = (vFlip > 0.5) ? 1.0 - corner.x : corner.x;
    vUv   = vec2((view + u) / uViews, corner.y);

    vRight    = right;
    vUp       = up;
    vFwd      = nrm;
    vWorldPos = p;
    gl_Position = uViewProj * vec4(p, 1.0);
}
