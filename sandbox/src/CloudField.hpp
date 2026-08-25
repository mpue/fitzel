#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Shader.hpp>

#include "CloudShape.hpp"

// A sky full of cumulus: a small library of baked clouds, hung at many places.
//
// The shapes come from CloudShape (spheres budding off spheres, rasterised into
// a cube of density). This owns what happens after that: the GPU resources, the
// placement, and the draw. It owns them outright, as VolumetricFog and PostChain
// do -- two shaders, one atlas texture, one instance buffer, and nothing else in
// the program touches any of it.
//
// WHY BAKED. The clouds do not deform. At the speed this game is played nobody
// could see it if they did, and buying that gives up nothing while buying a
// great deal: a volume fetch replaces seven octaves of noise, so the samples go
// into LIGHTING instead of into re-deriving the same cloud sixty times a second.
// The old procedural sky spent its whole budget on shape and had five coarse
// steps left for the sun; this one can walk the real density finely, which is
// what makes a bulge read as a bulge.
//
// WHY AN ATLAS. GL 3.3 has no array of 3D textures, so the library lives in one
// texture with the clouds side by side, and each instance reads its own slot.
// Eight variants at 96 cubed is seven megabytes; with a yaw and a size on every
// instance, eight is enough that the repetition does not read as repetition.
//
// WHY INSTANCED. This renderer is bound by how many draws it issues, not by
// their triangles -- sixty clouds as sixty draws would be a real cost. They go
// out as one, with the instance buffer rewritten each frame. It has to be
// rewritten anyway: alpha blending needs them sorted back to front, and which
// one is at the back depends on where the camera is.
namespace clouds {

// Voxels per side of one baked cloud, and how the library is laid out inside the
// atlas. Four across and two deep keeps the texture's longest side at 384, well
// inside anything GL 3.3 guarantees for a 3D texture.
constexpr int kSlotRes  = 96;
constexpr int kSlotsX   = 4;
constexpr int kSlotsZ   = 2;
constexpr int kVariants = kSlotsX * kSlotsZ;

// The rules a scene stores. Not the volume and not the instances -- those are
// derived from this, the same bargain the road's side objects and the roadside
// city make, and for the same reason: a seed and a dozen numbers instead of
// seven megabytes in the scene file.
struct Settings {
    bool enabled = true;
    // int rather than uint32: this is a scene field and an editor slider before
    // it is a generator input, and both of those want a plain signed number.
    // bake() casts it.
    int  seed = 1;

    // How many clouds are hung. Cost is close to linear in this on paper, but
    // not in practice: what a cloud costs is the SCREEN it covers, and most of a
    // full field is small and far away. Being generous here is what stops the
    // sky ending in mid-air -- a field that runs out before the horizon does
    // reads as a handful of props hung in front of a backdrop.
    int   count = 190;

    // The condensation level: the altitude every base sits at. ONE altitude for
    // all of them, which is not a simplification but the physics -- air rising
    // from the same ground condenses at the same height, which is why a real
    // cumulus field looks like it is standing on a sheet of glass.
    float baseHeight = 1350.0f;
    // ...give or take. A little scatter, because the ground is not all the same
    // temperature. Keep it small: this is a jitter, not a range.
    float baseJitter = 90.0f;

    // How far out the field is scattered, in metres from the origin. Has to
    // reach the horizon: at a base of 1350 m the last clouds a driver can see
    // are twenty-odd kilometres out, and anything short of that leaves a visible
    // edge to the weather.
    float spread = 32000.0f;

    // Cloud width in metres: the full span the field draws from, not the size of
    // an average cloud. Each species takes its own band out of it (humilis the
    // bottom, congestus the top -- see bake), and within a band small is
    // commoner than large, so most clouds land well below the maximum.
    //
    // The span wants to be wide. Real fair-weather cumulus run from a couple of
    // hundred metres to a few kilometres in the same sky, and that spread is
    // most of what gives a sky depth: the little ones are what a big one is read
    // as big against.
    float sizeMin = 320.0f;
    float sizeMax = 2400.0f;

    // Optical density multiplier: 1 is as baked, lower is thinner and more
    // translucent, higher is a heavier, darker-bottomed cloud.
    float density = 1.0f;

    // The mix of species, as weights. A calm afternoon is mostly humilis; a
    // building one grows congestus.
    float wHumilis   = 0.45f;
    float wMediocris = 0.40f;
    float wCongestus = 0.15f;

    // Whether going from `o` to this needs the volumes and the placements built
    // again. `enabled` is a flag and `density` is a uniform -- both take effect
    // the same frame -- while everything else is geometry, and geometry costs a
    // couple of hundred milliseconds. Without this split, nudging the density
    // slider would rebake the whole library.
    bool needsRebake(const Settings& o) const {
        return seed != o.seed || count != o.count ||
               baseHeight != o.baseHeight || baseJitter != o.baseJitter ||
               spread != o.spread || sizeMin != o.sizeMin || sizeMax != o.sizeMax ||
               wHumilis != o.wHumilis || wMediocris != o.wMediocris ||
               wCongestus != o.wCongestus;
    }

    bool operator==(const Settings& o) const {
        return enabled == o.enabled && seed == o.seed && count == o.count &&
               baseHeight == o.baseHeight && baseJitter == o.baseJitter &&
               spread == o.spread && sizeMin == o.sizeMin && sizeMax == o.sizeMax &&
               density == o.density && wHumilis == o.wHumilis &&
               wMediocris == o.wMediocris && wCongestus == o.wCongestus;
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }
};

// One placed cloud. Derived from Settings, never authored and never saved.
struct Instance {
    glm::vec3 centre;   // world centre of its box
    float     width;    // metres across
    float     height;   // metres tall
    float     yaw;      // radians about +Y
    int       slotX, slotZ;
};

class CloudField {
public:
    CloudField() = default;
    ~CloudField();

    CloudField(const CloudField&)            = delete;
    CloudField& operator=(const CloudField&) = delete;

    // Compile the shaders and build the proxy geometry. `shaderDir` is a
    // parameter rather than the hard-coded path the other systems use because
    // cloudcheck runs from a different working directory -- and a look tool that
    // cannot be pointed at the shaders it is checking is not a look tool.
    bool init(const std::string& shaderDir = "assets/shaders");

    // Grow the library, upload the atlas, and scatter the instances. Seconds at
    // most; called on scene load and whenever the settings change.
    void bake(const Settings& s);

    // Draw every cloud, back to front, in one instanced call. Blending and depth
    // state are set here and restored -- the caller does not have to know that a
    // cloud is transparent.
    //
    // Clouds are drawn AFTER the sky and BEFORE the scene, with no depth test:
    // they are further away than anything in the world, so there is nothing for
    // them to sort against. (A craft that climbs into the layer would need that
    // revisited; nothing in this game gets near the condensation level.)
    void render(const glm::mat4& viewProj, const glm::vec3& eye,
                const glm::vec3& sunDir, const glm::vec3& sunColor,
                const glm::vec3& ambient, float fogDensity,
                float exposure, bool tonemap);

    // Two different questions, and conflating them deadlocks the system.
    //
    // `valid` is "can draw": it needs the atlas, which only bake() creates.
    // `ready` is "can bake": it needs the shaders, and nothing else.
    //
    // A caller that gates its bake on valid() never bakes -- no atlas, so not
    // valid, so no bake, so no atlas. That is exactly what happened, and it fails
    // silently: an empty sky and every setting apparently inert, with nothing
    // anywhere reporting a problem. The check tool could not catch it either,
    // because it calls bake() outright and never asks.
    bool valid() const { return m_atlas != 0 && m_shader.isValid(); }
    bool ready() const { return m_shader.isValid(); }
    const Settings& settings() const { return m_settings; }
    const std::vector<Instance>& instances() const { return m_instances; }
    // How much VRAM the atlas takes, for the graphics panel to be honest about.
    std::size_t atlasBytes() const;

private:
    fitzel::Shader m_shader;
    std::uint32_t  m_atlas = 0;   // GL_TEXTURE_3D, the whole library
    std::uint32_t  m_vao   = 0;
    std::uint32_t  m_cube  = 0;   // unit cube positions
    std::uint32_t  m_inst  = 0;   // per-instance buffer, rewritten each frame

    Settings              m_settings;
    std::vector<Instance> m_instances;
    // Sort scratch, kept between frames so a frame does not allocate.
    std::vector<std::uint32_t> m_order;
    std::vector<glm::vec4>     m_upload;

    void release();
};

} // namespace clouds
