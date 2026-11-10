// --- Camera-relative clip space -------------------------------------------------
//
// A world far from the origin is made of big numbers: five kilometres out a float
// has half a millimetre left of its last digit. A view-projection that subtracts
// the eye INSIDE its matrix does that subtraction on numbers that big, rounds, and
// the rounding moves with every frame -- the camera's own motion and TAA's sub-pixel
// jitter -- so a vertex two metres in front of the eye swims by a pixel and the
// picture shimmers, the more the farther out. (Hence the reports of a camera that
// shakes "the farther from the origin you are".)
//
// So the eye comes off first: world - uCamPos of two numbers that close is EXACT in
// float, and what is left to the matrix is small. uViewProjRel is uViewProj with the
// eye's translation taken out (projection times the view's rotation alone), built
// on the CPU from the projection and the view, never from their product.
//
// The world position itself stays absolute for everything else a shader does with
// it -- lighting, shadows, fog, textures, noise.
uniform vec3 uCamPos;       // the eye the view was built from
uniform mat4 uViewProjRel;  // uViewProj without the eye's translation

vec4 clipOf(vec3 world) { return uViewProjRel * vec4(world - uCamPos, 1.0); }
