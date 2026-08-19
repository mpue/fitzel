#version 330 core

// Upsample the volumetric fog buffer onto the HDR scene.
//
// Four bilinear taps on a tent, which is two things at once: it scales the
// low-resolution march up, and it resolves the per-pixel dither the march left
// behind (the jittered start offset trades banding for noise, and this is where
// the noise is spent). No depth-aware weighting: the fog is a volume, not a
// surface, so it has no silhouette to bleed across -- and the depth buffer it
// would need is attached to the target being drawn into, which is exactly the
// read-write hazard worth not having.
//
// Blending is the caller's (src + dst * srcAlpha), so this only has to hand back
// the marched result: rgb in-scattered, a transmittance.

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uFog;
uniform vec2      uTexel;   // one texel of the fog buffer, in UV

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    vec2 o  = uTexel * 0.75;
    vec4 s = texture(uFog, uv + vec2(-o.x, -o.y))
           + texture(uFog, uv + vec2( o.x, -o.y))
           + texture(uFog, uv + vec2(-o.x,  o.y))
           + texture(uFog, uv + vec2( o.x,  o.y));
    FragColor = s * 0.25;
}
