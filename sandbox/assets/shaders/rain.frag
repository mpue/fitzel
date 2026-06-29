#version 330 core

in float vFade;
out vec4 FragColor;

uniform vec3  uRainColor; // lit, so rain dims at night / in shadow of storm
uniform float uIntensity;

void main() {
    FragColor = vec4(uRainColor, 0.35 * uIntensity * vFade);
}
