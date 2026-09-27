#version 330 core

// HalfResSky: the sky drawn at reduced resolution, stretched over the frame.
in vec2 vNdc;
out vec4 FragColor;

uniform sampler2D uSky;

void main() {
    FragColor = vec4(texture(uSky, vNdc * 0.5 + 0.5).rgb, 1.0);
}
