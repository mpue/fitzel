#version 330 core
// LensDrops: one quad into the lens film or the drops' heads -- a splash
// stamped from its texture, a drop's head, the streak it leaves. Built from the
// fullscreen quad: aPos.xy in [-1, 1] is the quad's own frame.

layout(location = 0) in vec3 aPos;

uniform vec2  uCenter;     // NDC
uniform vec2  uHalf;       // half size, in NDC-y units (shares of the view's height x 2)
uniform vec2  uRot;        // cos, sin of its turn
uniform float uInvAspect;  // height / width: x is squeezed back after the turn

out vec2 vLocal;

void main() {
    vLocal = aPos.xy;
    vec2 p = aPos.xy * uHalf;
    p = vec2(p.x * uRot.x - p.y * uRot.y, p.x * uRot.y + p.y * uRot.x);
    p.x *= uInvAspect;
    gl_Position = vec4(uCenter + p, 0.0, 1.0);
}
