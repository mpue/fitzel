#version 330 core

// Bloom, step 2: upsample one pyramid level onto the next larger one with a 3x3
// tent filter, added on top (the caller blends ONE/ONE). Walking the pyramid back
// up this way sums a set of ever-wider blurs, which is both cheaper and far
// smoother than one big-radius gather -- no tap pattern can show through.

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uSrc;
uniform vec2  uSrcTexel; // 1 / source (smaller level) resolution
uniform float uRadius;   // tent radius in source texels (~1 = tight, 2 = soft)

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    vec2 o  = uSrcTexel * uRadius;

    vec3 s = texture(uSrc, uv + vec2(-o.x,  o.y)).rgb
           + texture(uSrc, uv + vec2( 0.0,  o.y)).rgb * 2.0
           + texture(uSrc, uv + vec2( o.x,  o.y)).rgb
           + texture(uSrc, uv + vec2(-o.x,  0.0)).rgb * 2.0
           + texture(uSrc, uv).rgb                    * 4.0
           + texture(uSrc, uv + vec2( o.x,  0.0)).rgb * 2.0
           + texture(uSrc, uv + vec2(-o.x, -o.y)).rgb
           + texture(uSrc, uv + vec2( 0.0, -o.y)).rgb * 2.0
           + texture(uSrc, uv + vec2( o.x, -o.y)).rgb;

    FragColor = vec4(s * (1.0 / 16.0), 1.0);
}
