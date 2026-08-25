#version 330 core

// One cumulus, marched through its baked density volume.
//
// Everything about the shape was decided offline (see CloudShape.hpp) -- this
// only has to light it and integrate it. That trade is the whole point: a volume
// fetch replaces seven octaves of noise at eight hashes each, so the samples
// this shader can afford go into LIGHT instead of into re-deriving the same
// cloud sixty times a second. The old march spent five coarse light steps on a
// smoothed version of the shape and therefore lit every bulge as though the
// surface were flat; this one walks the real density, finely, and that is what
// draws the cauliflower.

in vec3 vWorld;
flat in vec3 vCentre;
flat in vec3 vHalf;
flat in vec2 vYawSC;    // sin, cos of the instance yaw
flat in vec3 vSlotOff;
out vec4 FragColor;

uniform vec3  uCameraPos;

// The atlas: every baked cloud in one 3D texture, because GL 3.3 has no array of
// them. Each instance reads its own slot through vSlotOff/uSlotScale.
uniform sampler3D uVolume;
uniform vec3  uSlotScale;

uniform vec3  uSunDir;     // towards the sun, world space
uniform vec3  uSunColor;   // HDR radiance
uniform vec3  uAmbient;    // sky fill, linear -- see the note at ambient use

uniform float uDensity;    // optical density multiplier (1 = as baked)
uniform float uStepScale;  // 1 = default march resolution; >1 coarser, cheaper
uniform float uFogDensity; // aerial perspective, per metre
uniform float uExposure;
uniform int   uTonemap;

const float PI = 3.14159265;

// Extinction per metre through fully dense cloud. A cumulus goes optically thick
// in a hundred metres or so, which is why its edges are translucent and its body
// is not.
// These followed the shape and have to be re-tuned when it changes. Raising the
// iso level in the bake (to keep the furrows between buds from filling in) takes
// mass out of the cloud, and mass is what casts the shadow -- so the same
// coefficients that were right for the smoother version left the new one lit
// almost flat. The look is a product of both halves, and neither can be tuned
// on its own.
const float kExtinct = 0.042;
// The shadow coefficient, and it is NOT the same number as the view one even
// though both are "extinction per metre".
//
// What the light march hands it is a depth in the hundreds: eight even steps of
// ~60 m through dense cloud sum to several hundred metres of path. At 0.040 that
// saturates instantly -- everything past a hundred metres of cloud is fully
// black, the graded band is a couple of pixels at the very rim, and what fills
// the whole interior is the multiple-scattering floor, which by design does NOT
// vary. That is the cotton wool: a shadow term with real structure in it,
// squashed flat by an exponent too steep to show any of it.
//
// It was raised to 0.040 earlier to fight exactly that flatness, which made it
// worse. At 0.006 the same depths land across the useful part of the curve:
// ~0.8 lit through a little cloud, ~0.35 through a few hundred metres, ~0.08
// buried deep -- a gradient rather than a switch.
const float kSunSig  = 0.016;

vec3 acesTonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float phaseHG(float c, float g) {
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * c, 1.5));
}

// Density at a point in cube space (-0.5..0.5). Outside the cube is empty --
// checked rather than clamped, because a clamped fetch would smear the boundary
// voxels out along the ray for as far as the march runs, and in an atlas it
// would smear the NEIGHBOURING cloud instead.
float density(vec3 p) {
    vec3 uvw = p + 0.5;
    if (any(lessThan(uvw, vec3(0.0))) || any(greaterThan(uvw, vec3(1.0))))
        return 0.0;
    return texture(uVolume, uvw * uSlotScale + vSlotOff).r;
}

// World point -> this instance's cube space. The box is a scale and a yaw, both
// of which invert in closed form, so nothing here needs a matrix.
vec3 toCube(vec3 w) {
    vec3 d = w - vCentre;
    float sy = vYawSC.x, cy = vYawSC.y;
    vec3 r = vec3(d.x * cy - d.z * sy, d.y, d.x * sy + d.z * cy);
    return r / (vHalf * 2.0);
}

// ...and the same for a direction, which under a non-uniform scale is not the
// same operation as for a point.
vec3 dirToCube(vec3 w) {
    float sy = vYawSC.x, cy = vYawSC.y;
    vec3 r = vec3(w.x * cy - w.z * sy, w.y, w.x * sy + w.z * cy);
    return r / (vHalf * 2.0);
}

// NO CONE SAMPLING, and that is a decision rather than an omission.
//
// The standard light march (Horizon Zero Dawn, and every implementation since)
// spreads its samples into a narrow cone about the sun instead of walking a
// straight line. It was tried here and it costs more than it buys, because the
// reason it exists does not apply: a cone AVERAGES, and what it averages away is
// the noise of sampling a procedural field a few times per step. This volume is
// baked and trilinearly filtered -- it has no such noise. So the cone had
// nothing to smooth but the shading itself, and even at a spread of five per
// cent of the cloud it is wider than the buds are, which turned the cauliflower
// back into a white lump. A straight line it is.

// Slab test against the unit cube, in cube space.
bool boxRange(vec3 ro, vec3 rd, out float t0, out float t1) {
    vec3 inv = 1.0 / rd;
    vec3 a = (vec3(-0.5) - ro) * inv;
    vec3 b = (vec3( 0.5) - ro) * inv;
    vec3 lo = min(a, b), hi = max(a, b);
    t0 = max(max(lo.x, lo.y), lo.z);
    t1 = min(min(hi.x, hi.y), hi.z);
    t0 = max(t0, 0.0);
    return t1 > t0;
}

void main() {
    // Into cube space. The box is scaled anisotropically (a cumulus is not a
    // cube), so directions are transformed and re-normalised rather than assumed
    // -- and the world length of one cube-space unit along THIS ray is measured
    // once, which is what keeps the extinction in metres where it belongs.
    vec3 rdW = normalize(vWorld - uCameraPos);
    vec3 ro  = toCube(uCameraPos);
    vec3 rdU = dirToCube(rdW);
    vec3 rd  = normalize(rdU);
    // One cube unit along rd is this many metres. (The yaw is a rotation, so it
    // contributes nothing to a length -- only the scale does.)
    float worldPerUnit = length(rd * vHalf * 2.0);

    float t0, t1;
    if (!boxRange(ro, rd, t0, t1)) discard;

    // Step size follows the volume's own resolution: about one voxel per step is
    // the most that can be resolved and the least that is worth paying for.
    //
    // ...scaled by distance. A cloud twenty kilometres out is a few dozen pixels
    // across, and marching it at the same fineness as the one overhead buys
    // nothing at all -- it is the same picture for eight times the samples. With
    // a couple of hundred clouds in a sky, nearly all of them are the far case.
    ivec3 vres = textureSize(uVolume, 0);
    float voxel = 1.0 / float(max(max(vres.x, vres.y), vres.z));
    float camDist = length(vCentre - uCameraPos);
    float lod = 1.0 + clamp((camDist - 3000.0) / 9000.0, 0.0, 3.0);
    float dt = max(voxel * uStepScale * lod, 1.0e-4);
    const int MAX_STEPS = 192;
    float span = t1 - t0;
    dt = max(dt, span / float(MAX_STEPS));

    // Sun direction in cube space, and the world length of one unit along it.
    vec3 sunC = normalize(dirToCube(uSunDir));
    float sunWorldPerUnit = length(sunC * vHalf * 2.0);

    float cosA = dot(rdW, uSunDir);

    // Dither the start so the step grid does not print itself across the cloud
    // as a set of shells.
    float dither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);
    float t = t0 + dt * dither;

    float T = 1.0;
    vec3  col = vec3(0.0);
    for (int i = 0; i < MAX_STEPS; ++i) {
        if (t > t1 || T < 0.02) break;
        vec3 p = ro + rd * t;
        float d = density(p) * uDensity;
        if (d <= 0.002) {
            // EMPTY-SPACE SKIPPING. The box is a box and the cloud inside it is
            // not: a good third of every ray is air. Striding through that at
            // three times the rate costs nothing where there is nothing, and the
            // stride is undone the moment density turns up -- the next sample
            // after a hit is back at the fine step, so an edge is never crossed
            // at the coarse one.
            t += dt * 3.0;
            continue;
        }
        {
            // --- Light march -------------------------------------------------
            // EVEN steps across a real fraction of the cloud, not a geometric fan.
            //
            // The fan is what made these look like cotton wool, and it took an
            // isolation test to see it: with the ambient turned off entirely the
            // cloud STILL came out evenly bright, so the fill was never what was
            // flattening it -- the march itself was returning almost no shadow
            // anywhere. Starting at ~1.2 voxels and growing by 1.55, the early
            // samples covered a couple of per cent of the cloud's width and the
            // late ones had already left the box, so the depth toward the sun
            // stayed near zero on the shaded side as well as the lit one.
            //
            // What darkens the inside of a cumulus is HOW MUCH CLOUD lies between
            // the point and the sun, so the march has to cross a real part of it.
            // Eight even steps of a eighteenth of the box reach just under half
            // way across -- enough that a point on the far side is properly
            // buried -- and cost the same eight fetches the fan did.
            // Fine at the start, growing gently. Both ends matter and the two
            // earlier attempts each had only one of them: steps of 60 m cannot
            // tell one bud from the next (a bud is 100-300 m, so the first sample
            // is still inside the one it started in, and the buds get no shadows
            // of their own), while a steep geometric fan left the cloud before it
            // had crossed anything. 22 m opening, 1.35 growth, eight steps --
            // resolves a neighbour and still reaches nearly half the box.
            float ls = 0.0;
            float lt = 0.0;
            float lstep = 0.020;
            for (int j = 0; j < 8; ++j) {
                lt += lstep;
                ls += density(p + sunC * lt) * uDensity * lstep * sunWorldPerUnit;
                lstep *= 1.35;
            }
            // --- Multiple scattering ----------------------------------------
            // Single scattering alone makes a cloud a silhouette: everything the
            // sun cannot reach directly goes black, and a backlit cumulus comes
            // out as a hole in the sky. Real ones are bright grey there, because
            // light that has bounced a few times inside still arrives -- and in
            // a medium this dense most of what leaves the cloud has bounced.
            //
            // The cheap standard approximation: sum a few octaves, each with
            // less extinction (light of higher order has effectively travelled
            // through thinner cloud), less energy, and a flatter phase function
            // (each bounce forgets more about where the sun was). It costs three
            // exponentials on a value already computed -- the light march itself
            // is not repeated.
            // Three orders, but the higher ones fall away FAST. The rate is
            // the whole difference between a cloud and a ball of cotton wool:
            // each order sees less extinction, so deep inside a cloud -- where
            // the first order has long since gone to zero -- the third is still
            // arriving, and it is the only thing lighting the shaded side. Set
            // too generously it floods that side up to nearly the lit one and
            // every cumulus turns into an evenly bright lump.
            //
            // 0.42/0.58 was too generous. At 0.30/0.44 the shadow is still open
            // (a real cumulus is grey there, never black) but the sun keeps the
            // upper hand, which is what gives a bulge a lit crown and a dark
            // underside instead of a uniform glow.
            // TWO orders, not three. The third was doing most of the damage:
            // its extinction is so weak that it barely varies through the cloud,
            // so it acts as a constant floor -- and in deep shadow it was
            // supplying about seventy per cent of the light. A floor that size
            // does not soften the shadow, it erases it, and no amount of work on
            // the first order can win against a constant added underneath it.
            //
            // Two orders keep what multiple scattering is actually for (a cumulus
            // is grey in shadow, never black) while leaving the direct term in
            // charge of the modelling.
            float sun = 0.0;
            for (int o = 0; o < 2; ++o) {
                float att = pow(0.25, float(o));  // extinction falls per order
                float con = pow(0.35, float(o));  // ...so does the energy
                float ecc = pow(0.50, float(o));  // ...and the directionality
                float ph = mix(phaseHG(cosA, 0.36 * ecc),
                               phaseHG(cosA, -0.22 * ecc), 0.4);
                sun += con * exp(-ls * kSunSig * att) * ph;
            }

            // Powder: a thin wisp scatters less back at you than a solid face,
            // so an edge darkens before it turns transparent. Without it every
            // bud dissolves evenly and they all read at the same depth. Only
            // applied on the lit side -- against the sun a thin edge is the
            // BRIGHTEST thing on a cloud, which is the silver lining.
            float powder = mix(1.0, 1.0 - exp(-d * 6.0),
                               clamp(0.5 - 0.5 * cosA, 0.0, 1.0));

            // Sky fill. Half the light on a cumulus comes from the hemisphere
            // rather than the sun, which is why its shaded side is pale blue-grey
            // and not black. Arriving top-down: the base sits under the cloud's
            // own bulk and the crown sits in the open, and that gradient is what
            // makes the flat bottom read as a flat bottom.
            float h = clamp(p.y + 0.5, 0.0, 1.0);
            vec3  amb = uAmbient * mix(0.42, 1.25, h);

            // The 5.0 was fitted against a sun that was not the engine's. With
            // the real one (uSunColor already carries a 3.4x HDR scale) it put
            // both the lit side AND most of the shaded side above the knee of
            // the tonemap, where everything resolves to the same white. A cloud
            // whose light and shadow both clip has no shape left to show -- which
            // is exactly the cotton-wool look, and no amount of work on the
            // scattering could have fixed it, because the difference was being
            // computed correctly and then thrown away by the curve.
            //
            // At 2.4 the crown sits just under the clip and the shaded side lands
            // near the middle of the range, which is where the modelling lives.
            vec3 lum = uSunColor * (sun * 2.4 * powder) + amb;

            float dens = d * dt * worldPerUnit * kExtinct;
            float a = 1.0 - exp(-dens);
            col += T * lum * a;
            T   *= exp(-dens);
        }
        t += dt;
    }

    float alpha = 1.0 - T;
    if (alpha <= 0.002) discard;

    // Aerial perspective, done as a loss of ALPHA rather than a blend toward a
    // sky colour this pass does not have. The background here IS the sky, so
    // letting more of it through is the same picture -- and it stays right when
    // one cloud is drawn over another, where a hard blend to a nominal sky
    // colour would paint haze over the nearer cloud.
    float dist = length(vWorld - uCameraPos);
    alpha *= exp(-dist * uFogDensity);

    if (uTonemap == 1) {
        col = acesTonemap(col * uExposure);
        col = pow(col, vec3(1.0 / 2.2));
    }
    // Premultiplied: the accumulation above already weighted colour by coverage.
    FragColor = vec4(col * (alpha / max(1.0 - T, 1.0e-4)), alpha);
}
