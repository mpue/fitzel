#pragma once

#include <fitzel/rhi/Types.hpp>

// The fog's density field, baked once into a 3D texture.
//
// It lives apart from VolumetricFog for one reason: it is the only part of the
// fog that can be looked at without a scene around it, and `fogcheck` needs
// exactly this and nothing else. Leaving it inside VolumetricFog.cpp meant the
// tool would have had to carry a SECOND copy of the noise -- and a check that
// renders a different field from the one the engine renders is worse than no
// check at all, because it is wrong in a way that looks right.
//
// Needs a current device (a window). Returns no texture on failure; the caller
// owns the texture and must destroy it (rhi::device().destroy).
fitzel::rhi::TextureHandle bakeFogNoise();
