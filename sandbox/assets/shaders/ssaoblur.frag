#version 330 core

// Denoise the half-res AO with a depth-aware (bilateral) box blur.
//
// The per-pixel rotation in the AO pass trades banding for noise, and that noise
// has to go somewhere. The composite used to do a plain 4x4 box, which also
// averages across depth discontinuities -- that is what smeared AO off silhouettes
// as a halo. Weighting each tap by how close its depth is to the centre's keeps
// the blur inside one surface.

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uAO;
uniform sampler2D uDepth;  // full-res scene depth (same UVs)
uniform vec2  uTexel;      // 1 / AO resolution
uniform float uNear;
uniform float uFar;
uniform float uDepthSigma; // metres of depth difference the blur tolerates

float linearDepth(vec2 uv) {
    float d = texture(uDepth, uv).r * 2.0 - 1.0;
    return (2.0 * uNear * uFar) / (uFar + uNear - d * (uFar - uNear));
}

void main() {
    vec2  uv = vNdc * 0.5 + 0.5;
    float z0 = linearDepth(uv);

    float sum = 0.0, wsum = 0.0;
    for (int x = -2; x <= 2; ++x) {
        for (int y = -2; y <= 2; ++y) {
            vec2  o  = vec2(x, y) * uTexel;
            float zs = linearDepth(uv + o);
            // Gaussian in space, gaussian in depth. The depth term is scaled by
            // distance so a tolerance that works up close doesn't reject every
            // tap far away (where one texel already spans metres).
            float dz = (zs - z0) / max(uDepthSigma * max(1.0, z0 * 0.05), 1.0e-3);
            float w  = exp(-dot(vec2(x, y), vec2(x, y)) * 0.25 - dz * dz);
            sum  += texture(uAO, uv + o).r * w;
            wsum += w;
        }
    }
    FragColor = vec4(sum / max(wsum, 1.0e-4));
}
