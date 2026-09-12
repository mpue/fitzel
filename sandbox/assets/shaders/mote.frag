#version 330 core

in vec2 vUV;
in vec3 vCol;
out vec4 FragColor;

// 1 = the motion pass: flag the pixels a visible mote covers (a = 2) so the
// temporal resolve takes them from this frame (taa.frag) instead of averaging
// a speck that is somewhere else every frame into nothing.
uniform int uReactive;

void main() {
    float d2 = dot(vUV, vUV);
    if (d2 > 1.0) discard;
    // A soft point: the quad is at least a pixel or two, and a hard disc of
    // that size stair-steps as it drifts.
    float k = exp(-d2 * 3.0) - 0.05;
    vec3  c = vCol * max(k, 0.0) * 1.6;
    if (uReactive == 1) {
        if (dot(c, vec3(0.2126, 0.7152, 0.0722)) < 0.01) discard;
        FragColor = vec4(0.0, 0.0, 0.0, 2.0);
        return;
    }
    FragColor = vec4(c, 1.0);   // additive (ONE, ONE)
}
