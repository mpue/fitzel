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
//
// --- Three kinds of water, not one -------------------------------------------
// The same strip carries a pool, a rapid and a waterfall, and shading all three
// with one pattern is what makes water look like a texture of water. So there
// are three, and they are cross-faded by numbers the generator already knows:
//
//   lying    -- ripples: broad, stretched along the flow, wind-patched
//   breaking -- churn:  patchy foam whose COVERAGE follows the whitewater
//   falling  -- filaments: long thin strands running many times faster
//
// The blend to the third is `air` (see Course::air): how far this water stands
// clear of the ground it came off. It is a separate PATTERN cross-faded in, not
// a faster clock on the same one -- scaling the time by anything that varies
// across the surface puts a term in the phase gradient that grows without bound,
// and after a few minutes the texture tears wherever that thing changes. One
// clock per pattern, always.
//
// --- Why every noise here takes a footprint ----------------------------------
// A river is seen from a metre away and from three hundred, and the same fbm
// that reads as ripples up close is a screenful of salt-and-pepper at distance:
// each pixel lands on an unrelated part of the field and the foam crawls. So
// every octave is faded out once it is finer than the pixel that samples it, and
// what is left is the octave's MEAN rather than nothing -- a river that got
// darker with distance would be its own kind of wrong.

in vec3 vWorldPos;
in vec3 vNormal;
in vec2 vUV;    // (metres across the channel, the FLOW coordinate along it)
in vec4 vData;  // (water depth m, whitewater, half-width m, how airborne)

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
// `px` is how much of this noise's own coordinate one pixel covers. An octave
// finer than that carries no information -- it is a different random number per
// frame per pixel -- so it is replaced by its mean instead of being sampled.
// That is the whole reason the far half of a river stops crawling.
float fbm(vec2 p, float px) {
    float s = 0.0, a = 0.5, f = 1.0;
    for (int i = 0; i < 4; ++i) {
        // Full detail while an octave's cell is four pixels or more, gone by the
        // time it is barely one. Cutting earlier than that is how a river ends up
        // looking like a sheet of plastic at fifty metres, which is as wrong as
        // the crawling speckle the filter is here to stop.
        float fade = clamp((0.80 - px * f) / 0.55, 0.0, 1.0);
        s += a * (fade > 0.01 ? mix(0.5, vnoise(p * f), fade) : 0.5);
        a *= 0.5;
        f *= 2.0;
    }
    return s;
}

// Water lying on a bed. Two scales of noise travelling downstream and stretched
// along the flow, because that is what a current does to a ripple -- plus two
// wave TRAINS crossing them at a slight angle.
//
// The trains are what stop it reading as cloud. A noise field has no direction
// in it; a water surface has almost nothing else, and the eye knows the
// difference long before it can say why. They are kept quiet (a tenth of the
// amplitude) because a river covered in visible sine waves is the other failure.
float ripples(vec2 p, float t, float px) {
    float k = uRippleScale;
    vec2 q1 = p * k        * vec2(1.0, 0.42) - vec2(0.0, t * 1.00);
    vec2 q2 = p * k * 2.70 * vec2(1.0, 0.58) - vec2(0.0, t * 1.75);
    float n = fbm(q1, px * k) * 0.68 + fbm(q2, px * k * 2.7) * 0.32;
    // Faded on the same rule as an octave: a wave train finer than the pixel
    // sampling it is a moire pattern, and a moire pattern on water is unmissable.
    float wf = clamp((0.80 - px * k * 3.4) / 0.55, 0.0, 1.0);
    if (wf > 0.01) {
        float w1 = sin((p.x *  0.34 + p.y) * k * 2.3 - t * 2.4);
        float w2 = sin((p.x * -0.52 + p.y) * k * 3.4 - t * 3.2);
        n += wf * 0.075 * (w1 + 0.65 * w2);
    }
    return n;
}

// Water that is falling. Strands ten times longer than they are wide, running
// several times faster than anything on the ground does, at two scales so the
// sheet shears rather than sliding down in one piece.
float filaments(vec2 p, float t, float px) {
    vec2 q1 = vec2(p.x * 2.60,        p.y * 0.26 - t * 3.1);
    vec2 q2 = vec2(p.x * 6.30 + 11.0, p.y * 0.52 - t * 4.6);
    return fbm(q1, px * 2.6) * 0.62 + fbm(q2, px * 6.3) * 0.38;
}

void main() {
    float across = vUV.x;
    // NOT a distance: the generator hands over surface distance divided by the
    // local speed, so this is how long the water took to get here. Everything
    // below scrolls it by ONE clock (uTime * uFlowSpeed) and nothing scales that
    // clock by anything spatial -- see the header for what that costs.
    float along  = vUV.y;
    float hw     = max(vData.z, 0.05);
    float depthM = max(vData.x, 0.0);   // metres of water under this fragment
    float white  = clamp(vData.y, 0.0, 1.0);
    float air    = clamp(vData.w, 0.0, 1.0);   // 0 on the bed, 1 in mid air

    // How much of the channel one pixel covers, in metres. Everything that
    // samples a noise field is filtered against this.
    float px = max(length(vec2(dFdx(across), dFdy(across))),
                   length(vec2(dFdx(along),  dFdy(along))));
    px = max(px, 1e-4);

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

    // How far from flat this bit of surface is hanging. A curtain's own normal
    // says it better than anything the generator could send: at the lip it is
    // still pointing at the sky, at the bottom of the drop it is pointing at you.
    float steep = clamp(1.0 - Ng.y, 0.0, 1.0);
    // How much this water is FALLING rather than running, which is the question
    // the pattern turns on. A jet in mid air is the obvious case, and `air` says
    // so outright -- but a cascade tearing down a rock face at sixty degrees is
    // the same water doing the same thing with a floor under it, and it wants the
    // same filaments. Steep AND broken, because a steep pane of glassy water
    // (the drawdown right at a lip) is not torn yet.
    float falling = clamp(max(air, steep * white), 0.0, 1.0);

    vec2  p = vec2(across, along);
    float t = uTime * uFlowSpeed;

    // --- The surface pattern -------------------------------------------------
    // Two independent fields, cross-faded by `air`. Never one field on two
    // clocks: see the header.
    // Sampled at the PIXEL's scale, not at a fixed epsilon -- a gradient taken
    // over less than a pixel is a gradient of the sampling, not of the water.
    float e = max(0.06, px * 1.2);
    // Only the field this water actually needs is evaluated. Almost every river
    // pixel in a scene is lying flat and almost every waterfall pixel is not, and
    // either field is eight noise lookups -- so the branch pays for itself many
    // times over and is coherent across a wavefront wherever it matters.
    float rh = 0.0, rhx = 0.0, rhy = 0.0;
    if (falling < 0.995) {
        rh  = ripples(p,                t, px);
        rhx = ripples(p + vec2(e, 0.0), t, px);
        rhy = ripples(p + vec2(0.0, e), t, px);
    }
    float fil = 0.0, filx = 0.0, fily = 0.0;
    if (falling > 0.005) {
        // Sharpened: real strands are bright ropes with dark water between them,
        // not a grey field that averages to the same thing.
        fil  = clamp((filaments(p,                t, px) - 0.5) * 1.7 + 0.5, 0.0, 1.0);
        filx = clamp((filaments(p + vec2(e, 0.0), t, px) - 0.5) * 1.7 + 0.5, 0.0, 1.0);
        fily = clamp((filaments(p + vec2(0.0, e), t, px) - 0.5) * 1.7 + 0.5, 0.0, 1.0);
    }
    float h  = mix(rh,  fil,  falling);
    float hx = mix(rhx, filx, falling);
    float hy = mix(rhy, fily, falling);

    // Not one smoothness everywhere, which is most of what tells still water from
    // painted water. Three things move it: the whitewater (a rapid is chopped),
    // the drop (a curtain is grooved along its fall, not rippled across it), and
    // one very slow, very large patch noise standing in for wind -- water in the
    // lee of a bank is glass and water in the open is not, and that difference is
    // free here.
    float gust = fbm(vec2(across * 0.05, along * 0.045 - t * 0.03), px * 0.05);
    float amp  = uRipple * (1.0 + 3.0 * white) * mix(0.45 + 1.1 * gust, 2.6, falling);
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
    // Broken water is full of air, and air-in-water is not the same substance as
    // water: it is pale, it is bright, and you cannot see into it. So the body of
    // a rapid is pulled towards its own shallow colour, brightened. Without this
    // the gaps BETWEEN the foam read as the ground showing through a stain, which
    // is what makes a torrent look like spilt paint on a hillside.
    body = mix(body, pow(uShallow, vec3(2.2)) * 1.7 + 0.02, white * 0.55);
    // Lit like any other surface, so water at dusk is not water at noon.
    body *= uAmbient + uLightColor * max(dot(Ng, L), 0.0) * 0.45;

    // --- Reflection ---------------------------------------------------------
    // Schlick from water's own index of refraction (~2% head-on), capped, and cut
    // back over shallow water where you see the bed rather than the sky.
    float cosT = clamp(dot(V, N), 0.0, 1.0);
    float F = 0.02 + 0.98 * pow(1.0 - cosT, 5.0);
    F = min(F, uReflect) * mix(0.35, 1.0, 1.0 - trans);
    vec3 R = reflect(-V, N);
    // Rougher where the water is broken: a rapid does not mirror anything, and a
    // curtain mirrors nothing at all.
    float blur = clamp(0.10 + 0.75 * white + 0.60 * air, 0.0, 1.0);
    vec3 env = textureLod(uEnvProbe, R, blur * uEnvMaxLod).rgb;
    env = min(env, vec3(64.0));   // a reflection is never brighter than this

    vec3 color = mix(body, env, F * (1.0 - 0.7 * air));

    // Sharp sun glint off the ripples (HDR, so bloom picks it up). Broken water
    // has no glint to give -- it has already been replaced by foam below.
    vec3  Hv = normalize(L + V);
    float spec = pow(max(dot(N, Hv), 0.0), 220.0);
    color += uLightColor * spec * 2.5 * uSparkle * (1.0 - white);

    // --- Foam ---------------------------------------------------------------
    // Two kinds, and they are not the same thing. The bank band is where the
    // water is dragging against the ground and is a fixed number of METRES in
    // from an edge that moves as the channel widens. The whitewater is the
    // profile's own gradient, arriving from the generator -- so foam is always on
    // the stretch the water is actually racing down.
    // The noise moves the band's EDGE rather than its brightness: a foam line
    // whose opacity is multiplied by a noise comes out as a row of spikes, and a
    // foam line whose width is comes out as the ragged, wandering edge water
    // actually leaves along a bank.
    float bandNoise = fbm(vec2(across * 1.6, along * 0.7 - t * 0.6), px * 1.6);
    float bank = smoothstep(hw - uFoamWidth * (0.35 + 1.15 * bandNoise), hw,
                            abs(across));

    // Patchy, not an even fizz: two scales MULTIPLIED, so the field has holes in
    // it however hard the water is working.
    float c1 = fbm(vec2(across * 1.7,        along * 0.75 - t * 1.5), px * 1.7);
    float c2 = fbm(vec2(across * 4.3 + 7.0,  along * 1.90 - t * 2.6), px * 4.3);
    float churn = c1 * (0.45 + 1.15 * c2);
    // COVERAGE, not brightness. A threshold moved by the whitewater means a
    // half-broken stretch is half covered in foam with green water between --
    // multiplying instead would make it uniformly grey, which is the single
    // thing that makes rapids look like spilt milk.
    //
    // The edge softens with distance for the same reason the octaves fade: a
    // hard threshold on a filtered field is a dithered mess a hundred metres out.
    //
    // And the threshold never reaches the bottom of the field, however hard the
    // water is working: a rapid at full white is nine tenths covered, not ten,
    // and the tenth that is left is the green water showing between the foam --
    // which is the difference between whitewater and a white polygon.
    float soft  = 0.12 + 0.9 * px;
    float thr   = mix(1.10, 0.22, clamp(white * 0.92, 0.0, 1.0));
    // ...and the threshold itself drifts, on the same slow wide noise the wind
    // patches use. Foam does not arrive evenly: a boil at the foot of a fall is
    // packed white in one place and open water two metres away, and without this
    // the whole of it comes out as one flat sheet at whatever coverage the
    // whitewater asked for.
    thr += (gust - 0.5) * 0.40 * white;
    float rough = smoothstep(thr - soft, thr + soft, churn);
    // A curtain is foam all the way down, but it is STRANDED foam: bright dense
    // ropes with thinner water between them, which is the whole look of falling
    // water and is what the filament field is for.
    rough = mix(rough, 0.22 + 0.78 * smoothstep(0.26, 0.80, fil + 0.08), falling);

    float foam = clamp(max(bank * 0.55, rough), 0.0, 1.0);
    // Foam is a lit surface, not a bright decal: it dims at night with everything
    // else, which is the difference between spray and a white sticker.
    // Lit by the RIPPLED normal, not the flat one. Foam is not paint on a plane:
    // it is a metre of lumps, and shading it off the strip normal is what makes a
    // rapid read as a white sticker laid over the water rather than as water.
    vec3 foamColor = uAmbient * 1.05 + uLightColor * max(dot(N, L), 0.0) * 0.62;
    // ...and it is not one flat white either. Aerated water is bright where it is
    // thick and grey-blue in its own shadow, so the churn that decided WHERE the
    // foam is also decides how bright it is.
    //
    // The span matters more than it looks. Foam mixed at full brightness lands
    // above the tonemap's shoulder everywhere, every variation in it clips to the
    // same white, and a waterfall comes out as a cut-out of paper. Kept under it,
    // the same numbers read as structure.
    foamColor *= 0.50 + 0.80 * clamp(mix(churn, fil, falling), 0.0, 1.0);
    color = mix(color, foamColor, foam * 0.85);

    // Light coming THROUGH the sheet. A backlit fall glows and a backlit pool
    // does not -- there is a metre of water in one and forty of it in the other
    // -- so this is the one term that is allowed to be about the drop alone.
    float back = pow(max(-dot(V, L), 0.0), 2.0);
    color += uLightColor * back * air * (0.25 + 0.55 * fil) * 0.8;

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
    // Whatever the surface REFLECTS did not come through it, so it cannot be
    // blended away as if it had. Without this a clear ankle-deep brook is 80%
    // transparent and loses its sky with the rest -- which is why shallow water
    // kept coming out as a pale green stain on the ground rather than as water
    // you can see the bottom of.
    alpha = clamp(max(max(alpha, F * 0.9), foam * 0.9), 0.0, 1.0);
    // A curtain has no bed behind it to be transparent ABOUT: it is thin and
    // glassy where it leaves the lip and dense where it has torn itself apart
    // further down, and its own strands are what you see through.
    alpha = mix(alpha,
                mix(0.30, 0.92, steep) * (0.45 + 0.55 * smoothstep(0.20, 0.85, fil)),
                air);
    alpha *= smoothstep(hw, hw - max(uFoamWidth, 0.15) * 0.6, abs(across));

    FragColor = vec4(toOutput(color), clamp(alpha, 0.0, 1.0));
}
