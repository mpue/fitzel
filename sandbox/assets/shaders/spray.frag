#version 330 core
// Foam sprite. Droplets (vFlat 0) are bright, tight, and fade over life; surface
// foam (vFlat 1) is a soft, low-alpha whitish patch with a broken, cloudy edge.
in  float vLife;
in  float vFlat;
out vec4  frag;

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

    if (vFlat > 0.5) {
        // Surface foam: cloudy, soft, translucent white that thins over life.
        float n = noise(gl_PointCoord * 5.0);
        float mask = smoothstep(0.15, 0.9, soft * (0.55 + 0.7 * n));
        float a = mask * vLife * vLife * 0.55;
        frag = vec4(vec3(0.94, 0.97, 1.0), a);
    } else {
        // Droplet: bright core fading to a bluish rim, fading out over life.
        vec3  col = mix(vec3(0.72, 0.86, 0.96), vec3(1.0), vLife);
        float a   = soft * vLife * 0.8;
        frag = vec4(col, a);
    }
}
