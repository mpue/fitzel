#version 330 core

// The construction grid: one quad on an axis plane, and no geometry behind
// it at all -- the four corners come from gl_VertexID as a triangle strip (the
// same trick particle.vert uses). The quad rides along with the camera, so the
// grid reads as endless while only a uniform ever moves.
//
// Its size is the fade distance, not some large round number: past that the
// fragment shader has taken the lines to nothing, so a bigger quad would only
// shade pixels that end up fully transparent.
uniform mat4  uViewProj;
uniform vec3  uCamPos;
uniform int   uAxis;     // the axis the plane is square to: 0 X, 1 Y (ground), 2 Z
uniform float uPlane;    // where on that axis it lies (through the 3D cursor)
uniform float uExtent;   // half the quad's side, in metres

out vec3 vWorld;

void main() {
    vec2 c = vec2((gl_VertexID == 1 || gl_VertexID == 3) ?  1.0 : -1.0,
                  (gl_VertexID >= 2)                     ?  1.0 : -1.0);
    // Centred on the eye within the plane; the in-plane pairs are (z, y),
    // (x, z) and (x, y) -- grid.frag reads them in the same order.
    if (uAxis == 0)
        vWorld = vec3(uPlane, uCamPos.y + c.y * uExtent, uCamPos.z + c.x * uExtent);
    else if (uAxis == 2)
        vWorld = vec3(uCamPos.x + c.x * uExtent, uCamPos.y + c.y * uExtent, uPlane);
    else
        vWorld = vec3(uCamPos.x + c.x * uExtent, uPlane, uCamPos.z + c.y * uExtent);
    gl_Position = uViewProj * vec4(vWorld, 1.0);
}
