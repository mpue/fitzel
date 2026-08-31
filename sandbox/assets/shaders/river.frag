#version 330 core

// Running water: brooks, rivers, canals.
//
// Deliberately NOT the global water plane's shader. That one mirrors the world
// into a planar reflection target rendered for ONE height, which is exactly the
// assumption a river breaks -- it is at a different height every ten metres, and
// on a fall it is not even horizontal. So the reflection here comes off the
// dynamic environment probe (the same cubemap lit.frag's reflective materials
// use) and the water's own body colour comes from how deep the channel was cut
// rather than from reading the depth buffer behind it. That costs the sharp
// mirror the lake has and buys a surface that can go down a hillside.

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;    // (metres across the channel, the FLOW coordinate along it)
in vec4 vData;  // (water depth m, whitewater, half-width m, local speed m/s)

out vec4 FragColor;

uniform vec3  uCameraPos;
uniform vec3  uLightDir;     // towards the light
uniform vec3  uLightColor;
uniform vec3  uAmbient;
uniform float uTime;

uniform samplerCube uEnvProbe;
uniform float uEnvMaxLod;

uniform vec3  uShallow;      // the water a finger deep
uniform vec3  uDeep;         // ...and over your head
uniform float uClarity;      // higher = the bed shows through deeper water
uniform float uReflect;      // Fresnel cap on the environment reflection
uniform float uRippleScale;  // ripple frequency (cycles per metre)
uniform float uRipple;       // ripple normal strength
uniform float uFlowSpeed;    // metres per second the surface pattern travels
uniform float uFoamWidth;    // metres of foam clinging to each bank
uniform float uSparkle;      // specular glitter off the ripples

// Atmospheric fog (matches lit.frag / water.frag).
uniform vec3  uFogColor;
uniform vec3  uFogSunColor;
uniform float uFogDensity;
uniform float uFogHeightFalloff;
uniform float uFogHeight;

uniform float uExposure;
uniform int   uTonemap;

vec3 acesTonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 toOutput(vec3 c) {
    if (uTonemap == 1) {
        c = acesTonemap(c * uExposure);
        c = pow(c, vec3(1.0 / 2.2));
    }
    return c;
}

// --- Value-noise fBm ---------------------------------------------------------
float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 345.45));
    p += dot(p, p + 34.345);
    return fract(p.x * p.y);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i),             hash21(i + vec2(1, 0)), u.x),
               mix(hash21(i + vec2(0, 1)), hash21(i + vec2(1, 1)), u.x), u.y);
}
float fbm(vec2 p) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 4; ++i) { s += a * vnoise(p); p *= 2.0; a *= 0.5; }
    return s;
}

// The ripple height field, in CHANNEL space: x across, y along. Three layers
// travelling downstream at different rates, so the surface reads as water being
// carried rather than as a texture sliding over it. Stretched along the flow
// (the 0.45) because that is what a current does to a ripple.
float ripples(vec2 p, float t) {
    vec2 q1 = p * uRippleScale        * vec2(1.0, 0.45) - vec2(0.0, t * 1.00);
    vec2 q2 = p * uRippleScale * 2.30 * vec2(1.0, 0.55) - vec2(0.0, t * 1.70);
    vec2 q3 = p * uRippleScale * 5.10 * vec2(1.0, 0.70) - vec2(0.0, t * 2.60);
    return fbm(q1) + 0.5 * fbm(q2) + 0.25 * fbm(q3);
}

void main() {
    float across = vUV.x;
    // NOT a distance: the generator hands over surface distance divided by the
    // local speed, so this is how long the water took to get here. Everything
    // below scrolls it by ONE clock (uTime * uFlowSpeed) and nothing scales that
    // clock by anything spatial.
    //
    // That restriction is the whole point and it is easy to undo by accident.
    // Multiplying the time by a per-fragment speed makes the phase depend on
    // position TIMES time, which puts a term in its gradient that grows with the
    // clock -- and after a couple of minutes that term is bigger than the real
    // one and the texture runs backwards wherever the speed changes. Shifting
    // this coordinate instead moves the pattern at the local speed everywhere,
    // keeps it continuous across the joins, and stretches it where the water
    // accelerates, which is what a current does to foam anyway.
    float along  = vUV.y;
    float hw     = max(vData.z, 0.05);
    float depthM = max(vData.x, 0.0);   // metres of water under this fragment
    float white  = clamp(vData.y, 0.0, 1.0);

    // The channel's own frame, rebuilt from screen-space derivatives the way
    // lit.frag builds a normal-map frame: T points downstream, B across. Doing it
    // here rather than shipping a tangent per vertex is what keeps the surface on
    // the same vertex format as everything else.
    vec3 Ng = normalize(vNormal);
    vec3 dp1 = dFdx(vWorldPos), dp2 = dFdy(vWorldPos);
    vec2 du1 = dFdx(vUV),       du2 = dFdy(vUV);
    float det = du1.x * du2.y - du2.x * du1.y;
    vec3 T = (abs(det) > 1e-9) ? (dp1 * du2.y - dp2 * du1.y) / det
                               : cross(Ng, vec3(1.0, 0.0, 0.0));
    // Guard the degenerate case rather than normalising a near-zero vector: at a
    // grazing angle the two screen derivatives can be almost parallel, and the
    // frame that comes out then is noise with a direction.
    T = T - Ng * dot(Ng, T);
    T = (dot(T, T) > 1e-12) ? normalize(T) : normalize(cross(Ng, vec3(1.0, 0.0, 0.0)));
    vec3 B = normalize(cross(Ng, T));

    // Ripple normal, from the gradient of the height field. Whitewater churns
    // faster and finer -- the flow multiplier rides in from the generator, which
    // already knows which stretches are racing.
    vec2  p = vec2(across, along);
    float t = uTime * uFlowSpeed;
    float e = 0.06;
    float h  = ripples(p, t);
    float hx = ripples(p + vec2(e, 0.0), t);
    float hy = ripples(p + vec2(0.0, e), t);
    float amp = uRipple * (1.0 + 2.5 * white);
    vec3  N = normalize(Ng - (B * (hx - h) + T * (hy - h)) * amp / e);

    vec3 V = normalize(uCameraPos - vWorldPos);
    vec3 L = normalize(uLightDir);

    // --- Body ---------------------------------------------------------------
    // How much water there is to look through, straight down. Beer-Lambert on the
    // channel's own depth rather than on the depth buffer: this surface has a bed
    // it was cut to, so it knows the answer without having to read the scene.
    //
    // The depth arrives per VERTEX in metres rather than as a fraction of one
    // channel depth, because there is no longer one: pools are deep, riffles are
    // shallow, and on a bend the deep line is against the outer bank. The colour
    // and the transparency follow all of that for free.
    float trans = exp(-depthM / max(uClarity, 0.05));
    vec3  body  = mix(pow(uDeep, vec3(2.2)), pow(uShallow, vec3(2.2)), trans);
    // Lit like any other surface, so water at dusk is not water at noon.
    body *= uAmbient + uLightColor * max(dot(Ng, L), 0.0) * 0.45;

    // --- Reflection ---------------------------------------------------------
    // Schlick from water's own index of refraction (~2% head-on), capped, and cut
    // back over shallow water where you see the bed rather than the sky.
    float cosT = clamp(dot(V, N), 0.0, 1.0);
    float F = 0.02 + 0.98 * pow(1.0 - cosT, 5.0);
    F = min(F, uReflect) * mix(0.35, 1.0, 1.0 - trans);
    vec3 R = reflect(-V, N);
    // Rougher where the water is broken: a rapid does not mirror anything.
    vec3 env = textureLod(uEnvProbe, R, (0.10 + 0.75 * white) * uEnvMaxLod).rgb;
    env = min(env, vec3(64.0));   // a reflection is never brighter than this

    vec3 color = mix(body, env, F);

    // Sharp sun glint off the ripples (HDR, so bloom picks it up).
    vec3  H = normalize(L + V);
    float spec = pow(max(dot(N, H), 0.0), 220.0);
    color += uLightColor * spec * 2.5 * uSparkle;

    // --- Foam ---------------------------------------------------------------
    // Two kinds, and they are not the same thing. The bank band is where the
    // water is dragging against the ground and is a fixed number of METRES in
    // from an edge that moves as the channel widens. The whitewater is the
    // profile's own gradient, arriving from the generator -- so foam is always on
    // the stretch the water is actually racing down.
    float bank = smoothstep(hw - uFoamWidth, hw, abs(across));
    float bankNoise = fbm(vec2(across * 1.6, along * 0.7 - t * 0.6));
    bank *= 0.30 + 0.70 * bankNoise;

    float churn = fbm(vec2(across * 2.2, along * 1.1 - t * 1.6)) *
                  fbm(vec2(across * 5.0 + 11.0, along * 2.6 - t * 2.9) );
    float rough = white * smoothstep(0.06, 0.42, churn) * 1.8;

    float foam = clamp(max(bank * 0.55, rough), 0.0, 1.0);
    // Foam is a lit surface, not a bright decal: it dims at night with everything
    // else, which is the difference between spray and a white sticker.
    vec3 foamColor = uAmbient * 1.25 + uLightColor * max(dot(Ng, L), 0.0) * 0.95;
    color = mix(color, foamColor, foam * 0.85);

    // --- Fog ----------------------------------------------------------------
    vec3  toFrag = vWorldPos - uCameraPos;
    float dist   = length(toFrag);
    vec3  rd     = toFrag / max(dist, 1e-4);
    float b = uFogHeightFalloff;
    float c = uFogDensity * exp(-(uCameraPos.y - uFogHeight) * b);
    float od = (abs(rd.y) > 1e-4)
             ? c * (1.0 - exp(-b * rd.y * dist)) / (b * rd.y)
             : c * dist;
    float fog = 1.0 - exp(-max(od, 0.0));
    float sunAmt = pow(max(dot(rd, L), 0.0), 4.0);
    color = mix(color, mix(uFogColor, uFogSunColor, sunAmt), clamp(fog, 0.0, 1.0));

    // Shallow water is translucent and deep water is not, plus foam is opaque
    // wherever it is. The fade at the very edge is what stops the waterline from
    // being a drawn line: a brook should run OUT over its gravel, not stop at it.
    float alpha = mix(0.10, 0.95, 1.0 - trans);
    alpha *= smoothstep(hw, hw - max(uFoamWidth, 0.15) * 0.6, abs(across));
    alpha = clamp(max(alpha, foam * 0.9), 0.0, 1.0);

    FragColor = vec4(toOutput(color), alpha);
}
