// The sky's own colour without anything in it -- no disc, no stars, no cloud.
// Shared by sky.frag (which draws it) and farterrain.frag (whose ranges dissolve
// into it): the colour a silhouette fades to has to be the colour of the sky
// behind it, or every ridge carries a seam. Linear radiance out; the gradient is
// authored in sRGB.
//
// The golden hour is most of this function. A sun a few degrees up does not
// just tint a strip of the horizon: its light has crossed two hundred
// kilometres of air, the blue is gone out of it, and the haze it lights up
// glows far up the dome around it. So three things follow the sun down --
// the dome loses its noon blue, a broad forward-scatter lobe of gold spreads
// around the sun, and the horizon band on the sun's side burns orange. A blue
// dome with an orange strip along the bottom, which is what the gradient alone
// gives, is exactly the lilac sunset a cheap sky is recognised by.
// The golden hour: whole with the sun a few degrees up, gone twenty degrees up,
// and gone again once it has set. main.cpp weighs the haze and the fill light
// with the same curve.
float goldenHour(vec3 sunDir) {
    return (1.0 - smoothstep(0.0, 0.35, sunDir.y)) * smoothstep(-0.10, 0.0, sunDir.y);
}

// Which part of the dome: 1 looking at the sun's azimuth, falling to nothing
// about sixty degrees round from it. The warmth sits over the sunset; the rest
// of the sky stays lavender, and a sky warm all the way round is as wrong as
// one blue all the way round.
float sunSideOf(vec3 dir, vec3 sunDir) {
    float azim = dot(normalize(vec3(dir.x, 0.0, dir.z) + 1e-5),
                     normalize(vec3(sunDir.x, 0.0, sunDir.z) + 1e-5));
    return pow(0.5 + 0.5 * azim, 5.0);
}

vec3 skyGradient(vec3 dir, vec3 sunDir) {
    float day = smoothstep(-0.12, 0.18, sunDir.y); // 0 night -> 1 day

    vec3 dayZenith   = vec3(0.20, 0.42, 0.80);
    vec3 dayHorizon  = vec3(0.70, 0.82, 0.95);
    vec3 nightZenith = vec3(0.01, 0.02, 0.06);
    vec3 nightHoriz  = vec3(0.04, 0.06, 0.12);

    float h = clamp(dir.y, 0.0, 1.0);
    vec3 zenith  = mix(nightZenith, dayZenith, day);
    vec3 horizon = mix(nightHoriz,  dayHorizon, day);

    // How low the sun is. `lowSun` is the old weight, already halved by `day`
    // at the horizon; `gold` is the golden hour itself.
    //
    // The golden-hour colours go in AFTER the night mix, as colours in their own
    // right. `day` is only one half with the sun still two degrees up, so a
    // dusk tint mixed in before it came out half night-blue: that is where the
    // lilac came from, not from any tint.
    float lowSun  = (1.0 - smoothstep(0.0, 0.35, sunDir.y)) * day;
    float gold    = goldenHour(sunDir);
    float sunSide = sunSideOf(dir, sunDir);
    // The dome's blue goes peach over the sunset and a greyed lavender elsewhere
    // -- at the brightness the dusk sky already had, only in other colours: a
    // brighter dome pushes the clouds into the tonemap's shoulder, where they
    // stop being clouds, and the auto exposure darkens the ground for it.
    // The zenith stays a lavender even over the sun: the gaps between the
    // clouds are what the lit clouds are seen against, and a warm gap leaves an
    // orange cloud nothing to stand out from.
    zenith  = mix(zenith,  mix(vec3(0.26, 0.27, 0.38), vec3(0.32, 0.29, 0.36), sunSide),
                  gold * 0.60);
    horizon = mix(horizon, mix(vec3(0.42, 0.42, 0.50), vec3(0.52, 0.44, 0.42), sunSide),
                  gold * 0.60);
    vec3 col = mix(horizon, zenith, pow(h, 0.5));

    // Warm band along the horizon on the sun's side. Weaker than it was when it
    // was all the warmth the sky had: on the peach horizon it now lies on, at
    // full strength it burnt every range near the sun to an orange sheet.
    float toSun = max(dot(normalize(vec3(dir.x, 0.0, dir.z) + 1e-5),
                          normalize(vec3(sunDir.x, 0.0, sunDir.z) + 1e-5)), 0.0);
    col += vec3(0.85, 0.35, 0.10) * mix(1.0, 0.6, gold) * lowSun * pow(toSun, 3.0) * (1.0 - h);

    // The aerosol glow: Mie scattering is strongly forward, so the haze right
    // around a low sun is the brightest thing in the sky after the sun itself,
    // and it fades over tens of degrees rather than at the disc's edge. Two
    // lobes -- a tight gold core and a wide peach skirt -- because one power
    // is either a spot or a wash.
    float sd   = max(dot(dir, normalize(sunDir)), 0.0);
    float core = pow(sd, 24.0);
    float wide = pow(sd, 4.0) * (1.0 - 0.5 * h);
    col += (vec3(1.00, 0.76, 0.38) * core * 0.55 + vec3(0.95, 0.55, 0.30) * wide * 0.06)
         * gold;

    return pow(max(col, vec3(0.0)), vec3(2.2));
}
