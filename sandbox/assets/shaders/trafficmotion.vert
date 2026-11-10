#version 330 core

// Motion vectors for the towns' traffic (TownTraffic::drawMotion). The crowd is
// one mesh re-skinned on the CPU every frame under an identity model matrix, so
// the renderer's own motion pass sees nothing move. Each vertex carries where
// it is now (aPos) and where it was a frame ago (aPrev, in the paint slot), and
// both go through their frame's camera -- the same arithmetic as treemotion.

layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 aPrev;

uniform mat4 uViewProj;   // this frame's, jittered (the depth test)
#include "camrel.glsl"
// The motion's two cameras, each without its eye's translation (camrel.glsl), and
// last frame's eye.
uniform mat4 uCurVPRel;   // this frame's, unjittered
uniform mat4 uPrevVPRel;  // last frame's, unjittered
uniform vec3 uPrevCamPos;

out vec4 vCur;
out vec4 vPrev;

void main() {
    vCur  = uCurVPRel * vec4(aPos - uCamPos, 1.0);
    vPrev = uPrevVPRel * vec4(aPrev.xyz - uPrevCamPos, 1.0);
    gl_Position = clipOf(aPos);
}
