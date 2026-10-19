#version 330 core
// LensDrops: the frame seen through what is on the lens.
//
// Wet (film.r + the running drops' heads): a liquid lens. Its slope (the
// thickness's gradient) bends the image behind it, which is out of focus that
// close to the eye -- the thicker, the blurrier. The liquid absorbs by
// thickness (Beer-Lambert, per channel: blood takes the green and blue, so a
// thin film is light red and see-through, a thick drop nearly black-red). The
// steep edge of a drop throws the light away -- a dark rim -- and the wet
// surface catches a highlight from above.
// Dried (film.g): no lens any more, only a brown tint and a little haze.

in vec2 vNdc;

uniform sampler2D uImage;
uniform sampler2D uFilm;
uniform sampler2D uHeads;
uniform vec2 uTexel;       // the image's
uniform vec2 uFilmTexel;   // the film's (half resolution)
uniform vec3 uAbsorb;      // per unit of thickness
uniform vec3 uDryTint;

out vec4 FragColor;

float thick(vec2 uv) { return texture(uFilm, uv).r + texture(uHeads, uv).r; }

vec3 blurred(vec2 uv, float r) {
    // eight taps round the point and the point: a small disc of defocus
    vec3 c = texture(uImage, uv).rgb;
    const float k = 0.70710678;
    vec2 o[8] = vec2[8](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1),
                        vec2(k, k), vec2(-k, k), vec2(k, -k), vec2(-k, -k));
    for (int i = 0; i < 8; ++i) c += texture(uImage, uv + o[i] * r * uTexel).rgb;
    return c / 9.0;
}

void main() {
    vec2 uv  = vNdc * 0.5 + 0.5;
    vec4 f   = texture(uFilm, uv);
    float h  = f.r + texture(uHeads, uv).r;
    float dry = f.g;
    vec3 img = texture(uImage, uv).rgb;
    if (h < 0.004 && dry < 0.004) { FragColor = vec4(img, 1.0); return; }

    // the surface's slope
    vec2 e = uFilmTexel * 1.5;
    vec2 g = vec2(thick(uv + vec2(e.x, 0.0)) - thick(uv - vec2(e.x, 0.0)),
                  thick(uv + vec2(0.0, e.y)) - thick(uv - vec2(0.0, e.y)));
    vec3 n = normalize(vec3(-g * 1.6, 1.0));
    float cover = smoothstep(0.01, 0.08, h);

    // through it: bent, out of focus, absorbed
    vec2  bent = uv + n.xy * 0.035 * cover;
    vec3  back = blurred(bent, (1.5 + 9.0 * clamp(h, 0.0, 1.2)) * cover);
    vec3  T    = exp(-uAbsorb * h);
    // a little light scattered inside the liquid, so it is not black in the dark
    vec3  wet  = back * T + vec3(0.13, 0.006, 0.005) * (1.0 - T.g) * clamp(h * 1.5, 0.0, 1.0);
    float rim  = clamp(length(g) * 1.4, 0.0, 0.55) * cover;
    wet *= 1.0 - rim;
    // the highlight: a light above and a little to the left
    vec3  L    = normalize(vec3(-0.35, 0.75, 0.55));
    float spec = pow(max(dot(reflect(-L, n), vec3(0.0, 0.0, 1.0)), 0.0), 48.0);
    wet += vec3(1.0, 0.92, 0.9) * spec * 0.8 * cover * smoothstep(0.08, 0.3, h);
    vec3 col = mix(img, wet, cover);

    // dried: a stain over everything behind it, and a little haze
    float d = clamp(dry, 0.0, 1.0);
    if (d > 0.004) {
        col = mix(col, blurred(uv, 3.0), d * 0.35);
        col *= mix(vec3(1.0), uDryTint, d);
    }
    FragColor = vec4(col, 1.0);
}
