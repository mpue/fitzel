// The sun's cascaded shadow, for everything that is not lit.frag.
//
// lit.frag has always received the sun's shadow; grass, flowers and trees never
// did. A meadow under a tree stayed in full sun, a forest floor was as bright as
// a clearing, and at sunset the long shadow of a hill fell across the terrain and
// stopped dead at the first blade of grass. This is the lookup those shaders
// share, #included (see fitzel::Shader::readSource), so there is one of it.
//
// The same cascades lit.frag reads, picked the same way -- by view depth -- so a
// shadow edge runs unbroken from the ground into the grass standing on it.
// Cheaper than lit.frag's 25-tap kernel: one bilinear-weighted 2x2, because the
// callers are a hundred thousand blades or a canopy of leaf cards, and both are
// busy enough to hide a harder edge.
//
// Uniforms (set by applySunShadows in FrameRender.hpp):
uniform sampler2DArray uShadowMap;
uniform mat4  uLightSpace[4];
uniform float uCascadeSplits[4];
uniform int   uCascadeCount;    // 0 = this pass has no shadows to give: all lit
uniform vec3  uShadowEye;       // camera position and forward: the cascade is
uniform vec3  uShadowForward;   //   chosen by view depth, as in lit.frag

// ...and the clouds: sunShadow() below includes their shadow, so everything that
// receives the sun's cascades is also dimmed by a passing cloud.
#include "cloudshadow.glsl"

// 0 = in full sun, 1 = in shadow. `L` points towards the sun. `texels` pushes
// the lookup that many shadow texels towards the light: a blade or a leaf card
// has no normal worth offsetting along (lit.frag's fix for acne), and moving
// along the light does the same job for geometry that has none.
float cascadeShadow(vec3 wp, vec3 L, float texels) {
    if (uCascadeCount <= 0) return 0.0;
    float d = dot(wp - uShadowEye, uShadowForward);
    float reach = uCascadeSplits[uCascadeCount - 1];
    if (d > reach) return 0.0;
    int layer = uCascadeCount - 1;
    for (int i = 0; i < uCascadeCount; ++i) {
        if (d < uCascadeSplits[i]) { layer = i; break; }
    }
    float res   = float(textureSize(uShadowMap, 0).x);
    float texel = 2.0 / max(length(uLightSpace[layer][0].xyz) * res, 1e-4);
    vec4  ls = uLightSpace[layer] * vec4(wp + L * (texel * texels), 1.0);
    vec3  p  = ls.xyz / ls.w * 0.5 + 0.5;
    if (p.z > 1.0 || any(lessThan(p.xy, vec2(0.0))) || any(greaterThan(p.xy, vec2(1.0))))
        return 0.0;

    // 2x2 around the sample, weighted by where it falls between them: the
    // cheapest filter whose edge does not stair-step.
    vec2  t  = p.xy * res - 0.5;
    vec2  f  = fract(t);
    vec2  b  = (floor(t) + 0.5) / res;
    float o  = 1.0 / res;
    float z  = p.z - 0.0004 * (1.0 + float(layer));
    float fl = float(layer);
    float s00 = step(texture(uShadowMap, vec3(b,               fl)).r, z);
    float s10 = step(texture(uShadowMap, vec3(b + vec2(o, 0.0), fl)).r, z);
    float s01 = step(texture(uShadowMap, vec3(b + vec2(0.0, o), fl)).r, z);
    float s11 = step(texture(uShadowMap, vec3(b + vec2(o, o),   fl)).r, z);
    float s = mix(mix(s00, s10, f.x), mix(s01, s11, f.x), f.y);
    // Fade out over the last tenth of the shadowed range, where lit.frag's own
    // shadow ends too, so there is no line where the grass lights up again.
    return s * (1.0 - smoothstep(0.9 * reach, reach, d));
}

// The sun's whole shadow at wp: the cascades' and the clouds'.
float sunShadow(vec3 wp, vec3 L, float texels) {
    return 1.0 - (1.0 - cascadeShadow(wp, L, texels)) * cloudLight(wp);
}
