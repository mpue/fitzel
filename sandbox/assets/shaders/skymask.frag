#version 330 core

// HalfResSky::renderBehind, step 2: the texels that may skip the sky -- no sky
// in their own block nor in any neighbour's, since the stretch back to full
// resolution filters across one texel. Those draw (and so mark the stencil);
// every other one is discarded and gets the sky.
in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uCover;   // step 1's answer, per reduced texel

void main() {
    ivec2 size = textureSize(uCover, 0);
    ivec2 c = ivec2(gl_FragCoord.xy);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            if (texelFetch(uCover, clamp(c + ivec2(x, y), ivec2(0), size - 1), 0).r > 0.5)
                discard;
    FragColor = vec4(0.0);
}
