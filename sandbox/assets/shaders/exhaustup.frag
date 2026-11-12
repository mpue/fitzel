#version 330 core

// The exhaust march back onto the HDR scene (ExhaustFx::render). At full
// resolution a plain read; below it a four-tap tent, as the volumetric fog does.
// Blending is the caller's: src + dst * srcAlpha.

in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uFx;
uniform vec2      uTexel;
uniform int       uScaled;

void main() {
    vec2 uv = vNdc * 0.5 + 0.5;
    if (uScaled == 0) {
        FragColor = texture(uFx, uv);
        return;
    }
    vec2 o = uTexel * 0.75;
    FragColor = 0.25 * (texture(uFx, uv + vec2(-o.x, -o.y)) + texture(uFx, uv + vec2(o.x, -o.y)) +
                        texture(uFx, uv + vec2(-o.x, o.y)) + texture(uFx, uv + vec2(o.x, o.y)));
}
