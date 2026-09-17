#version 330 core

in vec3 vNormal;
in vec2 vUv;
layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;

uniform sampler2D uTex;
uniform int  uHasTex;
uniform int  uAlphaCutout;
uniform float uAlphaCutoff;
uniform vec3 uBaseColor;   // an untextured part's glTF colour (sRGB)
uniform vec3 uTint;        // the author's tint, as tree.frag applies it

void main() {
    vec4 t = (uHasTex == 1) ? texture(uTex, vUv) : vec4(uBaseColor, 1.0);
    if (uAlphaCutout == 1 && t.a < uAlphaCutoff) discard;
    t.rgb = clamp(t.rgb * uTint, 0.0, 1.0);
    vec3 n = normalize(vNormal);
    // A leaf card is seen from both sides; the side facing away from the
    // camera would bake as a normal pointing into the crown.
    if (!gl_FrontFacing && uAlphaCutout == 1) n = -n;
    outAlbedo = vec4(t.rgb, 1.0);
    // .a = 1 for foliage, 0.5 for bark: the draw treats the two differently.
    outNormal = vec4(n * 0.5 + 0.5, uAlphaCutout == 1 ? 1.0 : 0.5);
}
