#version 330 core

in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTexture;
uniform vec3 uLightDir;   // direction *towards* the light, world space

void main() {
    vec3 normal  = normalize(vNormal);
    float diff   = max(dot(normal, normalize(uLightDir)), 0.0);
    float ambient = 0.25;

    vec3 albedo = texture(uTexture, vUV).rgb;
    vec3 color  = albedo * (ambient + diff);

    FragColor = vec4(color, 1.0);
}
