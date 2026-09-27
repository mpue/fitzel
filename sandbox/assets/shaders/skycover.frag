#version 330 core

// HalfResSky::renderBehind, step 1: which texels of the reduced sky target have
// any sky behind them -- a full-resolution pixel the finished scene left at the
// cleared depth (1.0) within the block the texel stands for.
in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uDepth;   // the scene's depth, full resolution
uniform int       uDiv;     // full-resolution pixels per reduced texel, each way

void main() {
    ivec2 size = textureSize(uDepth, 0);
    ivec2 base = ivec2(gl_FragCoord.xy) * uDiv;
    float sky = 0.0;
    for (int y = 0; y < uDiv; ++y)
        for (int x = 0; x < uDiv; ++x) {
            ivec2 p = min(base + ivec2(x, y), size - 1);
            if (texelFetch(uDepth, p, 0).r >= 1.0) sky = 1.0;
        }
    FragColor = vec4(sky);
}
