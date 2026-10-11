#version 330 core

in vec2 vUv;
in float vBurn;
flat in float vBurnKey;
uniform sampler2D uTex;
uniform int uAlphaCutout;
uniform float uAlphaCutoff;

#include "treeburn.glsl"

void main() {
    if (uAlphaCutout == 1 && texture(uTex, vUv).a < uAlphaCutoff) discard;
    if (uAlphaCutout == 1 && treeLeafBurnt(vBurnKey, vBurn)) discard;
}
