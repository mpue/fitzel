#version 330 core

// Birds and butterflies (Wildlife.hpp): one small body mesh, posed per instance
// -- wings beating or held, the body banked into its turn -- in the vertex
// shader, so a flock of sixty costs one draw.

layout(location = 0) in vec3  aPos;    // body frame: x right, y up, z forward (span 1)
layout(location = 1) in float aWing;   // 0 body, else side * fraction of the half-span
layout(location = 2) in vec2  aUv;     // butterflies: wing pattern (hind wings u >= 2)
layout(location = 3) in vec3  iPos;
layout(location = 4) in vec3  iRot;    // yaw, pitch, bank
layout(location = 5) in vec2  iAnim;   // wingbeat phase (rad), how much it beats (0 glide)
layout(location = 6) in float iScale;
layout(location = 7) in vec3  iColor;
layout(location = 8) in float iBelly;  // how much lighter the underside is

uniform mat4 uViewProj;
uniform int  uKind;                    // 0 bird, 1 butterfly

out vec3  vWorldPos;
out vec2  vUv;
out vec3  vColor;
out float vBelly;
out vec3  vUp;
out float vWing;

vec2 rot(vec2 v, float a) { float c = cos(a), s = sin(a); return vec2(c * v.x - s * v.y, s * v.x + c * v.y); }

void main() {
    vec3 p = aPos;
    float side = sign(aWing);
    float w    = abs(aWing);
    if (w > 0.0) {
        float x = abs(p.x);
        if (uKind == 0) {
            // Arm about the shoulder, hand about the elbow and a little behind
            // it in the beat: the wing flexes instead of flapping like a board.
            float beat  = sin(iAnim.x);
            float arm   = 0.12 + iAnim.y * (0.85 * beat + 0.15);
            float hand  = iAnim.y * 0.55 * sin(iAnim.x - 0.9);
            const float sh = 0.03, el = 0.25;
            if (x <= el) {
                vec2 q = rot(vec2(x - sh, p.y), arm);
                x = sh + q.x; p.y = q.y;
            } else {
                vec2 e = rot(vec2(el - sh, 0.0), arm);
                vec2 q = rot(vec2(x - el, p.y), arm + hand);
                x = sh + e.x + q.x; p.y = e.y + q.y;
            }
        } else {
            // Butterfly: both wings swing up together about the body.
            float open = 0.15 + iAnim.y * (0.5 + 0.5 * sin(iAnim.x)) * 1.35;
            vec2 q = rot(vec2(x, p.y), open);
            x = q.x; p.y = q.y;
        }
        p.x = side * x;
    }

    // Bank (about forward), pitch (about right), yaw (about up).
    float cb = cos(iRot.z), sb = sin(iRot.z);
    p = vec3(cb * p.x - sb * p.y, sb * p.x + cb * p.y, p.z);
    vec3 up = vec3(-sb, cb, 0.0);
    float cp = cos(iRot.y), sp = sin(iRot.y);
    p  = vec3(p.x, cp * p.y + sp * p.z, -sp * p.y + cp * p.z);
    up = vec3(up.x, cp * up.y + sp * up.z, -sp * up.y + cp * up.z);
    float cy = cos(iRot.x), sy = sin(iRot.x);
    p  = vec3(cy * p.x + sy * p.z, p.y, -sy * p.x + cy * p.z);
    up = vec3(cy * up.x + sy * up.z, up.y, -sy * up.x + cy * up.z);

    vec3 wp = iPos + p * iScale;
    vWorldPos = wp;
    vUv    = aUv;
    vColor = iColor;
    vBelly = iBelly;
    vUp    = up;
    vWing  = w;
    gl_Position = uViewProj * vec4(wp, 1.0);
}
