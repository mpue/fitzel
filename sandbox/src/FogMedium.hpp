#pragma once

#include <glm/glm.hpp>

// What a body of fog is MADE OF -- separate from where it stands.
//
// It lives in its own header for one reason: both sides of the fog need it and
// the two sides must not meet. VolumetricFog is a renderer (shaders, targets, a
// baked 3D texture); VolumetricFogComponent is scene data, and Component.hpp is
// deliberately free of any rendering dependency so that a component stays
// something you can copy, serialise and undo without a GL context in the room.
// A struct of plain numbers is what they can share without either including the
// other.
//
// The transform is NOT in here, and that is the split the whole design turns on:
// "how thick, what colour, how it moves" is the same question wherever a volume
// came from, while WHERE it is comes from the entity for a component and from
// the scene settings for the world-wide one.
struct FogMedium {
    // --- The stuff ----------------------------------------------------------
    float     density  = 0.08f;   // extinction per metre where the noise is solid
    glm::vec3 color{0.80f, 0.86f, 0.95f};
    float     coverage = 0.30f;   // how much of the volume has any fog in it

    // --- The noise ----------------------------------------------------------
    // Sampled in WORLD space, not in the box's: two volumes standing next to
    // each other then share one field and read as the same air, where a per-box
    // field would put a visible seam on the line between them.
    float     noiseScale = 0.010f; // world metres -> noise space (smaller = bigger banks)
    // How much finer the field is VERTICALLY than horizontally.
    //
    // Without this the fog is not volumetric-looking however well it is marched,
    // and the reason is arithmetic rather than art. The field is addressed in
    // world metres and is isotropic, so at the default scale one feature is a
    // hundred metres across -- on every axis. A ground layer is forty metres
    // tall: it spans a couple of dozen features sideways and less than half a
    // feature top to bottom. Every ray at the same x/z then walks through the
    // same value, the only thing that changes with height is the analytic
    // thinning, and the result reads exactly like a 2D pattern extruded upward,
    // because that is what it is.
    //
    // Squashing the lookup vertically is the standard answer for a layered
    // medium and it is also the physical one: real fog banks are wide and flat,
    // their billows far shorter than they are broad. 3 puts roughly one feature
    // per ten metres of height, which is what turns the blanket into banks with
    // holes, ragged tops and wisps that a march can actually find.
    float     verticalDetail = 3.0f;
    float     detail     = 0.45f;  // how hard the worley band breaks the shape up
    float     warp       = 0.35f;  // domain warp: the swirl inside a bank
    glm::vec3 wind{2.0f, 0.0f, 0.6f}; // metres per second the field drifts

    // --- How it meets the box's walls ---------------------------------------
    float edge          = 0.25f; // outer fraction of each half-extent that fades
    float heightFalloff = 0.8f;  // how fast it thins toward the top (0 = even)

    // --- Lighting -----------------------------------------------------------
    float anisotropy       = 0.55f; // forward scattering: the glow around the sun
    float sunIntensity     = 1.0f;  // scales the sun's HDR radiance
    float ambientIntensity = 1.0f;  // ..and the sky/haze radiance
    bool  shafts     = true;  // sample the sun cascades (god rays through the mist)
    bool  selfShadow = true;  // short march toward the sun (depth inside a bank)

    // Samples along the ray through THIS volume. A small placed volume needs far
    // fewer than a world-wide one: what matters is the step LENGTH, and a march
    // is always cut to the piece of ray actually inside the box.
    int   steps = 32;
};

// The defaults above suit a volume the size of a world. A PLACED one is metres
// across, not hundreds, and inherits three of them badly:
//
//  - the noise. At the world scale a bank is about a hundred metres wide, which
//    is larger than the whole volume -- so a small box lands inside a single
//    feature and comes out either uniformly full or, worse, uniformly empty
//    depending on where it happens to stand. That reads as a broken component,
//    and it is only a frequency.
//  - the thickness. Extinction is per metre, so the same number over twenty
//    metres instead of six hundred is almost nothing.
//  - the vertical thinning, which exists so world mist hugs the ground. A bank
//    placed in an archway is meant to fill it.
//
// The vertical detail is NOT one of them: it is a ratio, not a length, so a
// twenty-metre bank wants its billows squashed by the same factor a
// six-hundred-metre layer does.
//
// Fewer steps too: the march is cut to the piece of ray actually inside the box,
// so a short crossing gets a fine step count for free.
inline FogMedium placedFogDefaults() {
    FogMedium m;
    m.density       = 0.18f;
    m.noiseScale    = 0.05f;   // banks about twenty metres across
    m.heightFalloff = 0.3f;
    m.steps         = 24;
    return m;
}
