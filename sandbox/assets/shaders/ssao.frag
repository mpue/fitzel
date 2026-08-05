#version 330 core

// Horizon-based ambient occlusion from the depth buffer (half res).
//
// The previous kernel-in-a-hemisphere version needed a grazing-angle fade to hide
// its own self-occlusion on near-flat ground -- and that fade *was* the dark band
// that swept across the terrain as the camera turned: AO switched off over a
// narrow range of view angles and the transition read as a stripe. Horizon-based
// AO doesn't need it. It marches a few screen-space directions, tracks the
// highest elevation angle it can see along each, and turns that horizon into
// occlusion. Flat ground has no horizon above its own tangent plane at any view
// angle, so it stays unoccluded by construction -- nothing to fade, no band.

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uDepth;
uniform mat4  uProjection;
uniform mat4  uInvProjection;
uniform float uRadius;    // occlusion radius in view-space metres
uniform float uBias;      // horizon starts at this elevation (radians): self-shadow guard
uniform float uPower;     // contrast curve on the result
uniform float uFadeStart; // view distance where AO begins to fade out
uniform float uFadeEnd;   // ..and where it reaches zero

const int   kDirections = 6; // screen-space marching directions
const int   kSteps      = 5; // taps along each direction
const float kPi         = 3.14159265;

vec3 viewPos(vec2 uv) {
    float d = texture(uDepth, uv).r;
    vec4 clip = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    vec4 v = uInvProjection * clip;
    return v.xyz / v.w;
}

// Interleaved gradient noise: a fine, evenly spread dither that the bilateral
// blur resolves cleanly (a plain hash clumps, and the blur turns clumps into
// blotches that crawl).
float ign(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec2  uv = vNdc * 0.5 + 0.5;
    float d  = texture(uDepth, uv).r;
    if (d >= 1.0) { FragColor = vec4(1.0); return; } // sky: no occlusion

    vec3 P = viewPos(uv);

    // Surface normal from depth, taking the *nearer* neighbour of each pair so the
    // basis stays on the near surface at silhouettes (no edge halos).
    vec2 size  = vec2(textureSize(uDepth, 0));
    vec2 texel = 1.0 / size;
    vec3 Pr = viewPos(uv + vec2(texel.x, 0.0));
    vec3 Pl = viewPos(uv - vec2(texel.x, 0.0));
    vec3 Pu = viewPos(uv + vec2(0.0, texel.y));
    vec3 Pd = viewPos(uv - vec2(0.0, texel.y));
    vec3 ddx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
    vec3 ddy = (abs(Pu.z - P.z) < abs(P.z - Pd.z)) ? (Pu - P) : (P - Pd);
    vec3 N = normalize(cross(ddx, ddy));
    if (dot(N, -P) < 0.0) N = -N; // always face the camera

    // World radius -> UV radius at this depth (vertical; x is corrected by aspect
    // below so the march stays a circle). Clamped so close geometry doesn't march
    // across half the screen and distant geometry still gets a few texels.
    float projScale = 0.5 * uProjection[1][1] / max(-P.z, 1.0e-3);
    float radiusUv  = clamp(uRadius * projScale, 2.0 * texel.y, 0.08);
    vec2  aspectFix = vec2(size.y / size.x, 1.0);

    float noise = ign(gl_FragCoord.xy);
    float sinBias = sin(uBias);
    float aoSum = 0.0;

    for (int dir = 0; dir < kDirections; ++dir) {
        // Rotate the fan per pixel and jitter the step start, so 6x5 taps cover
        // the disc as evenly as a much larger fixed kernel would.
        float ang   = (float(dir) + noise) * (kPi / float(kDirections));
        vec2  dirUv = vec2(cos(ang), sin(ang)) * aspectFix;

        float sinH  = sinBias; // highest horizon seen so far along this direction
        float dirAo = 0.0;
        for (int s = 1; s <= kSteps; ++s) {
            float t   = (float(s) - 0.5 + noise) / float(kSteps);
            vec2  suv = uv + dirUv * (t * radiusUv);
            if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) break;

            vec3  S   = viewPos(suv) - P;
            float len = length(S);
            if (len < 1.0e-4) continue;

            float sinS = dot(N, S) / len;   // elevation over the tangent plane
            if (sinS > sinH) {
                // Attenuate by range so an occluder entering the radius fades in
                // instead of popping, and accumulate the horizon increment (the
                // total per direction stays <= 1).
                float att = clamp(1.0 - (len * len) / (uRadius * uRadius), 0.0, 1.0);
                dirAo += (sinS - sinH) * att;
                sinH   = sinS;
            }
        }
        aoSum += dirAo;
    }

    float ao = 1.0 - aoSum / float(kDirections);

    // Distance fade only -- no angle-dependent term, that was the sweeping band.
    float distFade = 1.0 - smoothstep(uFadeStart, uFadeEnd, -P.z);
    ao = mix(1.0, clamp(ao, 0.0, 1.0), distFade);

    FragColor = vec4(pow(ao, uPower));
}
