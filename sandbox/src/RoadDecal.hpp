#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Mesh.hpp>

// Decals for RoadSystem: an image laid flat ON the carriageway -- start-line
// grids, direction arrows, boost pads, oil stains, sponsor logos, patched tarmac.
//
// A decal is a RULE, like a side-object line, not a piece of authored geometry:
// "this image, this far along the road, this far off the middle, this big". The
// patch is DERIVED from the road's own centreline and re-derived whenever the
// road is rebuilt, so a decal follows the corner it was placed in, banks with the
// carriageway, rides up onto a bridge deck, and never has to be re-aligned after
// a control point moves.
//
// It is a projection in the sense that matters here -- the image is wrapped onto
// the road surface, not pasted onto a floating quad -- but it is done by lofting
// a patch out of the same cross-section the ribbon was lofted from, rather than
// by re-projecting depth in screen space. That buys the two things a deferred
// decal cannot give this renderer: it works in a forward pass with no depth
// prepass, and it is exact on a banked or humped road, where a box projection
// smears the image down the cross-fall.
//
// The cost is the boundary: a decal knows about the ROAD, so it stops at the
// road's edge and cannot spill onto the terrain beside it. That is the right
// trade for what these are for; scattering an image on open ground is the
// scatter tool's job, not this one's.
//
// Pure geometry, like RoadSide: this walks a centreline and returns vertices. It
// resolves no textures, holds no GL state and knows nothing about materials --
// RoadSystem owns those (see RoadSystem::rebuildDecals).
namespace roaddecal {

// How the image's alpha is spent, which is the whole opaque-vs-transparent
// question and the one choice that has to be made per decal rather than guessed:
//
//   Cutout -- alpha is a stencil: a texel is either painted or it isn't. Drawn in
//             the OPAQUE queue, so it never sorts wrongly against anything and
//             costs nothing extra. The right default, and what arrows, numbers,
//             logos and grid squares want.
//   Blend  -- alpha is coverage: the image is blended over the tarmac and the
//             asphalt shows through. Drawn in the transparent queue (sorted
//             back-to-front, no depth write). What a stain, a scorch mark, a
//             puddle of spilt oil or a soft-edged shadow wants -- and the only
//             mode where `opacity` below 1 means anything.
//   Opaque -- alpha is ignored: the whole patch is painted. For a full-width
//             surface swap (a concrete apron, a repaved stretch, a start grid
//             that covers the tarmac completely).
enum class Blend { Cutout, Blend, Opaque };

inline const char* blendName(Blend b) {
    switch (b) {
        case Blend::Cutout: return "Cut out";
        case Blend::Blend:  return "Blended";
        case Blend::Opaque: return "Opaque";
    }
    return "Decal";
}

// One decal rule. Everything metric, measured on the road, so the numbers read
// the same whatever the spline sampling happens to be -- and so a decal can be
// placed by TYPING where it goes rather than by dragging it into position, which
// is the only way to land a start line on the start line without a steady hand.
struct Decal {
    bool        enabled = true;
    std::string texture;            // image file ("" = the decal draws nothing)

    // Where it sits. `dist` is metres from the start of the road to the patch's
    // CENTRE, along the centreline; `offset` is metres right of the middle
    // (negative = left). Both are on the same footing as a side object's offset.
    float dist    = 0.0f;
    float offset  = 0.0f;
    // How big, in metres: `length` along the drive, `width` across it.
    float length  = 6.0f;
    float width   = 4.0f;

    // Repeats, for the things that come in rows: `repeat` copies, each `spacing`
    // metres further along (0 = butt-jointed, i.e. one length). A dashed centre
    // line, a run of chevrons, a grid of start boxes.
    int   repeat  = 1;
    float spacing = 0.0f;

    // Turn the IMAGE within the patch, in degrees about the patch centre. The
    // patch itself always follows the road -- that is what keeps it on the
    // surface -- so this rotates the UVs, not the geometry: 180 flips an arrow
    // to face oncoming traffic, 90 lays it across the road. On a patch that is
    // not square, an odd angle stretches the image; that is the trade for
    // turning a picture without lifting it off the tarmac.
    float spin    = 0.0f;

    Blend blend   = Blend::Cutout;
    float opacity = 1.0f;           // Blend mode only: 1 = the image as painted
    float cutoff  = 0.5f;           // Cutout mode only: alpha below this is dropped
    glm::vec3 tint{1.0f};           // multiplies the image (colour a grey stencil)

    // Glow, the reason a boost pad reads at night: the image's lit texels are
    // added after lighting, so the pad stays bright when the sun is gone. 0 (the
    // default) is an ordinary painted marking.
    glm::vec3 glow{0.2f, 0.8f, 1.0f};
    float     glowStrength = 0.0f;

    // Metres above the carriageway. Small on purpose: enough to clear the
    // ribbon's depth without the patch reading as a floating sticker. Raise it
    // if a decal flickers against the road on a distant, steeply-viewed stretch.
    float lift = 0.02f;

    // Value equality, so the road's undo snapshot can tell a real edit from a
    // no-op (same reason roadside::Line has one).
    bool operator==(const Decal& o) const {
        return enabled == o.enabled && texture == o.texture && dist == o.dist &&
               offset == o.offset && length == o.length && width == o.width &&
               repeat == o.repeat && spacing == o.spacing && spin == o.spin &&
               blend == o.blend && opacity == o.opacity && cutoff == o.cutoff &&
               tint == o.tint && glow == o.glow &&
               glowStrength == o.glowStrength && lift == o.lift;
    }
    bool operator!=(const Decal& o) const { return !(*this == o); }
};

// The defaults a mode seeds when a decal is created. Not a different kind of
// thing -- the fields below are what actually drive it -- just a sensible
// starting point per mode, so "+ Stain" arrives already translucent and soft
// instead of as a hard-edged opaque rectangle the user has to talk down.
Decal preset(Blend b);

// Total length of a sampled centreline, in metres. What the editor clamps `dist`
// against, so "put it at the end" is a number the user can actually reach.
float centerlineLength(const std::vector<glm::vec2>& center);

// Loft one decal's patches onto the road surface (all its repeats in ONE mesh --
// they share an image and a material, so they are one draw).
//
// `center`, `surfaceY` and `bankDeg` are the road's sampled centreline and the
// profile lofted on it -- exactly what RoadSystem publishes, and exactly what the
// ribbon itself was built from, so the patch cannot disagree with the asphalt
// about where the surface is. The geometry comes back in WORLD space: draw it
// with an identity model matrix.
//
// Empty for a disabled decal, an empty image, a road shorter than two samples, or
// a patch that falls entirely off the end of the road.
fitzel::MeshData generate(const Decal& d, const std::vector<glm::vec2>& center,
                          const std::vector<float>& surfaceY,
                          const std::vector<float>& bankDeg);

} // namespace roaddecal
