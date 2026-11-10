#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in vec4 aPaint; // terrain texture-paint weights (layers 0..3)

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uViewProj;
#include "camrel.glsl"
uniform vec4 uClipPlane; // world-space plane; fragments with dot < 0 are clipped

out vec3  vWorldPos;
// The same point relative to the eye (camrel.glsl), for the fragment shader's
// screen-space derivatives: far from the origin vWorldPos has a millimetre left of
// its last digit, the step between two pixels is less, and a tangent frame built
// from it comes out garbage -- normal-mapped surfaces glitter with white points.
out vec3  vRelPos;
out vec3  vNormal;
out vec2  vUV;
out vec4  vPaint;
out float vViewDepth; // positive distance along the view direction
flat out vec3 vModelT; // the object's origin, world (uWindowLocal)

void main() {
    vec4 world  = uModel * vec4(aPos, 1.0);
    vWorldPos   = world.xyz;
    vRelPos     = world.xyz - uCamPos;
    vNormal     = mat3(transpose(inverse(uModel))) * aNormal;
    vUV         = aUV;
    vPaint      = aPaint;
    vViewDepth  = -(uView * world).z;
    vModelT     = uModel[3].xyz;

    gl_ClipDistance[0] = dot(world, uClipPlane);
    gl_Position = clipOf(world.xyz);
}
