#version 330 core

// Motion vectors for the towns' traffic (TownTraffic::drawMotion). The crowd is
// one mesh re-skinned on the CPU every frame under an identity model matrix, so
// the renderer's own motion pass sees nothing move. Each vertex carries where
// it is now (aPos) and where it was a frame ago (aPrev, in the paint slot), and
// both go through their frame's camera -- the same arithmetic as treemotion.

layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 aPrev;

uniform mat4 uViewProj;   // this frame's, jittered (the depth test)
uniform mat4 uCurVP;      // this frame's, unjittered
uniform mat4 uPrevVP;     // last frame's, unjittered

out vec4 vCur;
out vec4 vPrev;

void main() {
    vCur  = uCurVP * vec4(aPos, 1.0);
    vPrev = uPrevVP * vec4(aPrev.xyz, 1.0);
    gl_Position = uViewProj * vec4(aPos, 1.0);
}
