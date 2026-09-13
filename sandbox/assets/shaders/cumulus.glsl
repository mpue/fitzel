// --- The cumulus field, as a density (shared by sky.frag and cloudshadow.frag) --
//
// Out of sky.frag so the ground can be shaded by the very clouds the sky draws:
// the cloud-shadow pass marches the same density towards the sun. Needs the
// cloud uniforms declared by the includer: uTime, uCoverage, uCloudScale,
// uCloudSpeed, uCloudBottom, uCloudTop.

float hash13(vec3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}
float vnoise3(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i + vec3(0, 0, 0));
    float n100 = hash13(i + vec3(1, 0, 0));
    float n010 = hash13(i + vec3(0, 1, 0));
    float n110 = hash13(i + vec3(1, 1, 0));
    float n001 = hash13(i + vec3(0, 0, 1));
    float n101 = hash13(i + vec3(1, 0, 1));
    float n011 = hash13(i + vec3(0, 1, 1));
    float n111 = hash13(i + vec3(1, 1, 1));
    return mix(mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
               mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y), f.z);
}

// Rounded bulges rather than smooth blobs: the ridge of the noise, inverted, so
// the MAXIMA are domed and the minima are creases. This is the whole difference
// between a cauliflower and a cloud of smoke, and no amount of thresholding
// smooth noise will produce it.
float billow(vec3 p, int octaves) {
    float s = 0.0, a = 0.5;
    for (int i = 0; i < 5; ++i) {
        if (i >= octaves) break;
        s += a * (1.0 - abs(2.0 * vnoise3(p) - 1.0));
        p *= 2.03;
        a *= 0.5;
    }
    return s;
}

// How much cloud there can be at height `t` through the slab. `amount` is the
// coverage as a 0..1 quantity (uCoverage is a threshold, so it runs the other
// way): a thin fair-weather field is all base and no build.
float heightShape(float t, float amount) {
    float top = mix(0.34, 1.0, amount);
    return smoothstep(0.0, 0.07, t) * smoothstep(top, top * 0.45, t);
}

// Where in the noise field a world point samples from. The SHEAR is the point:
// the wind is stronger higher up, so a cloud's top is dragged ahead of its base
// and the whole thing leans. One add, and it is most of what stops a field of
// cumulus looking like it was stamped out of a sheet -- a vertical column of
// noise reads as a pillar, a leaning one reads as weather.
vec3 cloudSample(vec3 p, float t) {
    vec3 wind  = vec3(uTime * uCloudSpeed, 0.0, uTime * uCloudSpeed * 0.3);
    vec3 shear = vec3(t * (uCloudTop - uCloudBottom) * 0.5, 0.0, 0.0);
    return (p + wind + shear) * uCloudScale;
}

float cloudBase(vec3 p) {
    float amount = clamp(1.0 - uCoverage, 0.0, 1.0);
    float t = (p.y - uCloudBottom) / max(uCloudTop - uCloudBottom, 1.0);
    float n = billow(cloudSample(p, t), 4);
    return smoothstep(uCoverage, uCoverage + 0.20, n) * heightShape(t, amount);
}

float cloudDensity(vec3 p) {
    float base = cloudBase(p);
    if (base <= 0.0) return 0.0;
    // Finer billows, drifting slightly against the wind so the edges boil rather
    // than slide. Remapped against the base so a thick core survives untouched
    // and only the thin skirts are eaten away -- erode everywhere and the whole
    // field goes translucent instead of getting an edge.
    float t = (p.y - uCloudBottom) / max(uCloudTop - uCloudBottom, 1.0);
    vec3 q = cloudSample(p, t);
    float detail = billow(q * 6.1 - vec3(0.0, uTime * 0.04, 0.0), 3);
    float erode  = (1.0 - detail) * 0.42;
    return clamp((base - erode) / max(1.0 - erode, 1e-3), 0.0, 1.0);
}
