#version 330 core
// LensDrops: the film dries. The wet thickness (r) thins away; part of what
// goes stays behind as a stain (g), which only ever grows -- the lens gets
// dirtier the longer it is in it.

in vec2 vNdc;

uniform sampler2D uFilm;
uniform float uWetKeep;   // exp(-dt / tau)
uniform float uDryGain;   // share of the dried blood that stays as a stain

out vec4 FragColor;

void main() {
    vec4  f   = texture(uFilm, vNdc * 0.5 + 0.5);
    float wet = f.r * uWetKeep;
    float dry = min(f.g + (f.r - wet) * uDryGain, 1.2);
    FragColor = vec4(wet, dry, 0.0, 0.0);
}
