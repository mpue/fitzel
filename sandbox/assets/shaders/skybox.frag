#version 330 core

// Draws an HDRI environment cubemap as the sky background. Reconstructs the
// world-space view ray from the fullscreen quad and samples the cubemap.

in vec2 vNdc;
out vec4 FragColor;

uniform mat4        uInvViewProj;
uniform vec3        uCameraPos;
uniform samplerCube uEnv;
uniform float       uIntensity;
uniform float       uExposure;
uniform int         uTonemap; // 1 = ACES + gamma (final), 0 = linear HDR

vec3 acesTonemap(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}
vec3 toOutput(vec3 c) {
    if (uTonemap == 1) {
        c = acesTonemap(c * uExposure);
        c = pow(c, vec3(1.0 / 2.2));
    }
    return c;
}

void main() {
    // From the near plane to the far one rather than from the eye: the same
    // ray under a perspective lens, and under an orthographic one the only
    // right one -- every pixel looking the same way, not fanned out round the
    // eye (a top view showed the sun beside the object it looked down on).
    vec4 far   = uInvViewProj * vec4(vNdc, 1.0, 1.0);
    vec4 near  = uInvViewProj * vec4(vNdc, -1.0, 1.0);
    vec3 dir   = normalize(far.xyz / far.w - near.xyz / near.w);
    vec3 col   = texture(uEnv, dir).rgb * uIntensity;
    FragColor  = vec4(toOutput(col), 1.0);
}
