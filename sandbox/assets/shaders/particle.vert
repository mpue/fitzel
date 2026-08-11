#version 330 core

// One camera-facing quad per particle. There is no per-vertex buffer at all --
// the four corners come from gl_VertexID as a triangle strip, so the only thing
// crossing the bus is the per-instance stream. Same trick billboard.vert uses for
// the tree impostors.
layout(location = 0) in vec3  iPos;
layout(location = 1) in float iSize;   // world metres across
layout(location = 2) in float iRot;    // radians about the view axis
layout(location = 3) in vec4  iColor;  // rgb premultiplied by brightness, a = fade
layout(location = 4) in vec3  iVel;    // for the stretched (spark) form

uniform mat4  uViewProj;
uniform vec3  uCamRight;
uniform vec3  uCamUp;
uniform vec3  uCamPos;
// Metres of extra length per m/s of particle speed. 0 = a plain round billboard;
// anything above turns the quad along its travel, which is what makes sparks and
// rain read as streaks rather than dots.
uniform float uStretch;

out vec2 vUv;
out vec4 vColor;
out vec3 vWorld;

void main() {
    vec2 c = vec2((gl_VertexID == 1 || gl_VertexID == 3) ?  0.5 : -0.5,
                  (gl_VertexID >= 2)                     ?  0.5 : -0.5);
    vUv    = c + 0.5;
    vColor = iColor;

    vec3  right = uCamRight, up = uCamUp;
    float sx = iSize, sy = iSize;
    vec2  r  = c;

    if (uStretch > 0.0 && dot(iVel, iVel) > 1.0e-4) {
        // Lay the long axis along the direction of travel and grow it with speed.
        // The other axis is whatever is perpendicular to that ON SCREEN, so the
        // streak keeps its width however the camera is turned. Rotation is
        // ignored here: the velocity has already decided which way it points.
        vec3 dir  = normalize(iVel);
        vec3 view = normalize(iPos - uCamPos);
        vec3 sd   = cross(view, dir);
        if (dot(sd, sd) > 1.0e-6) {
            up    = dir;
            right = normalize(sd);
            sy    = iSize + length(iVel) * uStretch;
        }
    } else {
        float s = sin(iRot), co = cos(iRot);
        r = vec2(c.x * co - c.y * s, c.x * s + c.y * co);
    }

    vWorld      = iPos + right * (r.x * sx) + up * (r.y * sy);
    gl_Position = uViewProj * vec4(vWorld, 1.0);
}
