#version 330 core

// The proxy box of one fog volume.
//
// Not a fullscreen quad, and that is the whole reason many small volumes are
// affordable: a box standing in one archway covers the handful of pixels it
// covers, and the march only runs on those. A fullscreen pass per volume would
// make the tenth volume cost exactly as much as the first.
//
// The unit cube (-0.5..0.5) is placed by uModel -- the same translate * rotate *
// scale the entity gizmo and the selection wireframe are built from, so the box
// marched is the box drawn.
//
// The fragment stage wants the view ray, not an interpolated position: it takes
// it from gl_FragCoord against the target's size, which is exact and needs
// nothing passed across from here. So there is no varying at all.

layout(location = 0) in vec3 aPos;

uniform mat4 uModel;
uniform mat4 uViewProj;

void main() {
    gl_Position = uViewProj * uModel * vec4(aPos, 1.0);
}
