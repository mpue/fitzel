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
    vec3 N = normalize(cross(dFdx(P), dFdy(P))); // normal from depth

    // Random per-pixel rotation of the kernel (cheap dither).
    float a = hash12(gl_FragCoord.xy) * 6.2831853;
    vec3  rnd = vec3(cos(a), sin(a), 0.0);
    vec3  T = normalize(rnd - N * dot(rnd, N));
    vec3  B = cross(N, T);
    mat3  TBN = mat3(T, B, N);

    float occlusion = 0.0;
    for (int i = 0; i < uKernelSize; ++i) {
        vec3 samplePos = P + TBN * uKernel[i] * uRadius;
        vec4 off = uProjection * vec4(samplePos, 1.0);
        off.xyz /= off.w;
        vec2 sUv = off.xy * 0.5 + 0.5;
        if (sUv.x < 0.0 || sUv.x > 1.0 || sUv.y < 0.0 || sUv.y > 1.0) continue;

        float sampleZ = viewPos(sUv).z;
        float range = smoothstep(0.0, 1.0, uRadius / max(abs(P.z - sampleZ), 1e-4));
        occlusion += (sampleZ >= samplePos.z + uBias ? 1.0 : 0.0) * range;
    }

    occlusion = 1.0 - occlusion / float(uKernelSize);
    FragColor = vec4(pow(occlusion, uPower));
}
