#version 330 core

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uDepth;
uniform mat4  uProjection;
uniform mat4  uInvProjection;
uniform vec3  uKernel[32];
uniform int   uKernelSize;
uniform float uRadius;
uniform float uBias;
uniform float uPower;

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

vec3 viewPos(vec2 uv) {
    float d = texture(uDepth, uv).r;
    vec4 clip = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 v = uInvProjection * clip;
    return v.xyz / v.w;
}

void main() {
    vec2  uv = vNdc * 0.5 + 0.5;
    float d  = texture(uDepth, uv).r;
    if (d >= 1.0) { FragColor = vec4(1.0); return; } // sky: no occlusion

    vec3 P = viewPos(uv);

    // Reconstruct the normal from depth using the *nearer* of each pair of
    // horizontal/vertical neighbours. Plain dFdx/dFdy straddle depth edges and
    // flip the normal there; picking the closer neighbour keeps the basis on the
    // near surface and removes the silhouette halos.
    vec2 texel = 1.0 / vec2(textureSize(uDepth, 0));
    vec3 Pr = viewPos(uv + vec2(texel.x, 0.0));
    vec3 Pl = viewPos(uv - vec2(texel.x, 0.0));
    vec3 Pu = viewPos(uv + vec2(0.0, texel.y));
    vec3 Pd = viewPos(uv - vec2(0.0, texel.y));
    vec3 ddx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
    vec3 ddy = (abs(Pu.z - P.z) < abs(P.z - Pd.z)) ? (Pu - P) : (P - Pd);
    vec3 N = normalize(cross(ddx, ddy));

    // Random per-pixel rotation of the kernel (cheap dither).
    float a = hash12(gl_FragCoord.xy) * 6.2831853;
    vec3  rnd = vec3(cos(a), sin(a), 0.0);
    vec3  T = normalize(rnd - N * dot(rnd, N));
    vec3  B = cross(N, T);
    mat3  TBN = mat3(T, B, N);

    // Bias grows with the local depth slope (fwidth of view Z) so it always
    // clears the depth buffer's quantisation step. Without this, distant,
    // near-horizontal surfaces (terrain toward the horizon) self-occlude in
    // terraced horizontal bands.
    float bias = uBias + fwidth(P.z) * 2.0;

    float occlusion = 0.0;
    for (int i = 0; i < uKernelSize; ++i) {
        vec3 samplePos = P + TBN * uKernel[i] * uRadius;
        vec4 off = uProjection * vec4(samplePos, 1.0);
        off.xyz /= off.w;
        vec2 sUv = off.xy * 0.5 + 0.5;
        if (sUv.x < 0.0 || sUv.x > 1.0 || sUv.y < 0.0 || sUv.y > 1.0) continue;

        float sampleZ = viewPos(sUv).z;
        float range = smoothstep(0.0, 1.0, uRadius / max(abs(P.z - sampleZ), 1e-4));
        occlusion += (sampleZ >= samplePos.z + bias ? 1.0 : 0.0) * range;
    }

    occlusion = 1.0 - occlusion / float(uKernelSize);
    FragColor = vec4(pow(occlusion, uPower));
}
