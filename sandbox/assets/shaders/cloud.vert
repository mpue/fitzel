#version 330 core

// One proxy box per cloud, all of them in a single instanced draw.
//
// A box per cloud rather than one fullscreen pass, for the reason VolumetricFog
// draws its volumes the same way: a cloud covering a twentieth of the screen then
// costs a twentieth of the fill. The old sky march charged every sky pixel the
// full price whether there was cloud along that ray or not, which on a
// fair-weather day is mostly blue.
//
// Instanced rather than one draw each, because this renderer is bound by how many
// draws it issues rather than by their triangles -- sixty clouds would otherwise
// be sixty state changes a frame. The instance buffer is rewritten per frame
// anyway (it has to be sorted back to front for blending), and at sixty entries
// that is five kilobytes.

layout(location = 0) in vec3 aPos;      // unit cube, -0.5..0.5

// Per instance: where the box is, how big, which way round, and which cloud out
// of the atlas it draws. Packed rather than a matrix -- two vec4 attributes
// instead of eight, and the shader can build both the matrix and its inverse
// from them without a second upload.
layout(location = 1) in vec4 aPosSize;  // xyz = box centre (world), w = width
layout(location = 2) in vec4 aMisc;     // x = height, y = yaw, zw = atlas slot uv

uniform mat4 uViewProj;
uniform vec3 uSlotScale;                // size of one slot in atlas space

out vec3 vWorld;
// flat: every fragment of one box shares these, and interpolating them would be
// both wasteful and wrong.
flat out vec3 vCentre;
flat out vec3 vHalf;      // half extents, world units
flat out vec2 vYawSC;     // sin, cos of the instance yaw
flat out vec3 vSlotOff;

void main() {
    float cy = cos(aMisc.y), sy = sin(aMisc.y);
    vec3 half3 = vec3(aPosSize.w, aMisc.x, aPosSize.w) * 0.5;

    // Scale, then yaw about Y, then translate. Done by hand because the inverse
    // is wanted in the fragment shader and a rotation plus a scale inverts in
    // closed form -- no matrix inversion anywhere in the frame.
    vec3 s = aPos * half3 * 2.0;
    vec3 r = vec3(s.x * cy + s.z * sy, s.y, -s.x * sy + s.z * cy);
    vWorld = aPosSize.xyz + r;

    vCentre  = aPosSize.xyz;
    vHalf    = half3;
    vYawSC   = vec2(sy, cy);
    vSlotOff = vec3(aMisc.z, 0.0, aMisc.w) * uSlotScale;

    // Pinned inside the far plane instead of being clipped by it.
    //
    // The scene's far plane is a few kilometres (it is fitted to the terrain and
    // the skyline, and capped at 5 km), while the cloud field reaches to the
    // horizon -- tens of kilometres. As real geometry out there, nearly every box
    // fell outside the frustum and was thrown away, so the sky came up empty and
    // no cloud setting appeared to do anything. The old sky never hit this: a
    // fullscreen quad sits ON the far plane by construction.
    //
    // Clamping z rather than writing z = w keeps the NEAR plane doing its job,
    // and w is untouched so x/y and every varying stay perspective-correct. The
    // clouds write no depth and are tested against none, so where they land in
    // the depth range means nothing to anyone.
    vec4 clip = uViewProj * vec4(vWorld, 1.0);
    clip.z = min(clip.z, clip.w * 0.9999);
    gl_Position = clip;
}
