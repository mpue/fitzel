#version 330 core
// Spray sprite. Droplets (vFlat 0) are bright, tight, and fade over life;
// surface foam (vFlat 1) is a soft, low-alpha whitish patch with a broken,
// cloudy edge; mist (vFlat 2) is a faint haze that thins as it swells. All of
// it lit by the sun and the sky, so spray at dusk is dusk-coloured.
in  float vLife;
in  float vFlat;
out vec4  frag;

uniform vec3 uLight;     // the sun's light reaching a horizontal surface
uniform vec3 uAmbient;   // the sky's

// Cheap value noise so foam edges look broken/cloudy instead of a clean disc.
float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float noise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i), b = hash(i + vec2(1, 0));
    float c = hash(i + vec2(0, 1)), d = hash(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(d, d);
    if (r2 > 1.0) discard;
    float soft = 1.0 - r2;
    // White water scatters what falls on it: the sky from all round, the sun
    // from above. Linear, like the rest of the HDR pass.
    vec3 lit = uAmbient * 1.15 + uLight * 0.85;

    if (vFlat > 1.5) {
        float n = noise(gl_PointCoord * 3.0 + vLife * 2.0);
        float a = soft * soft * (0.5 + 0.5 * n) * vLife * 0.18;
        frag = vec4(lit * 0.95, a);
    } else if (vFlat > 0.5) {
        // Surface foam: cloudy, soft, translucent white that thins over life.
        float n = noise(gl_PointCoord * 5.0);
        float mask = smoothstep(0.15, 0.9, soft * (0.55 + 0.7 * n));
        float a = mask * vLife * vLife * 0.55;
        frag = vec4(lit * vec3(0.94, 0.97, 1.0), a);
    } else {
        // Droplet: bright core fading to a bluish rim, fading out over life.
        vec3  col = mix(vec3(0.72, 0.86, 0.96), vec3(1.0), vLife);
        float a   = soft * vLife * 0.8;
        frag = vec4(lit * col, a);
    }
}
