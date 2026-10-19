#version 330 core
// LensDrops: what one quad adds (additively) to the lens -- its thickness,
// into the wet channel (r) or the dried one (g), as `uChannel` says.

in vec2 vLocal;

uniform sampler2D uTex;
uniform int   uMode;      // 0 the texture's alpha, 1 a hemisphere, 2 a streak, 3 a soft blob
uniform float uAmount;
uniform vec2  uChannel;

out vec4 FragColor;

void main() {
    float h;
    if (uMode == 0) {
        h = texture(uTex, vLocal * 0.5 + 0.5).a;
    } else if (uMode == 1) {
        // a drop on glass: round, as high in the middle as surface tension lets it
        h = sqrt(max(0.0, 1.0 - dot(vLocal, vLocal)));
    } else if (uMode == 2) {
        // the streak behind a running drop: across x, soft at both ends
        h = sqrt(max(0.0, 1.0 - vLocal.x * vLocal.x)) * (1.0 - smoothstep(0.8, 1.0, abs(vLocal.y)));
    } else {
        float d = length(vLocal);
        h = 1.0 - smoothstep(0.2, 1.0, d);
        h *= h;
    }
    FragColor = vec4(uChannel * h * uAmount, 0.0, 0.0);
}
