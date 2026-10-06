#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/render/Renderer.hpp>   // PointLight, SpotLight

#include "SceneTypes.hpp"

class CitySystem;
class EditMeshCache;
class ModelLibrary;
namespace prefab { struct Prefab; }

// The towns' street lamps (cityplan::Rule::lampPrefabs), drawn from their
// prefabs and lit with the prefabs' own lights once it gets dark -- or, where a
// town names no prefab, the standard lamp: a mast, an arm out over the street
// and a lantern with its glass underneath, warm light, built here.
//
// The town only decides WHERE a lamp stands (cityplan::derive -> Town::lamps).
// A lamp is a prefab -- a model, a Light component, perhaps a second head --
// and the merged town geometry can hold none of that, so this draws them: each
// prefab flattened once into its parts and lights in its own frame, then every
// lamp near the eye submitted at its spot, the way the roadside posts are (the
// renderer culls them per pass).
//
// The light is the scarce part. The lit shader takes sixteen point lights and
// eight spots for the whole frame, and a town has hundreds of lamps, so only the
// nearest burn for real -- fading out towards the edge of their ring, so none
// pops on or off as the eye moves -- and every other lamp is its glowing glass
// alone, which is all a lamp two streets away is to the eye anyway. The glass
// and the lights go out by day together (nightFactor).
class TownLamps {
public:
    // Where the lamps' prefabs come from (main's prefab cache by name), the
    // models their entities show, and the cache that uploads the ones modelled
    // in the editor -- the same three the traffic dresses its vehicles with.
    std::function<const prefab::Prefab*(const std::string&)> findPrefab;
    ModelLibrary*  models    = nullptr;
    EditMeshCache* meshCache = nullptr;

    float reach       = 320.0f;   // metres: lamps farther from the eye are not drawn
    float lightReach  = 75.0f;    // metres: lamps farther off light nothing
    float shadowReach = 60.0f;    // metres: nearer lamps cast the sun's shadow

    // Re-flatten when the towns were derived anew, or when a prefab a lamp
    // wears was edited and saved. Once a frame, after the towns' update.
    // (The standard lamp's two materials are find-or-created in `materials`.)
    void update(const CitySystem& towns, std::vector<MaterialDef>& materials);

    // Every part of every lamp within `reach` of `eye`: mesh, material, model
    // matrix, and whether it is near enough to cast the sun's shadow.
    void forEachDraw(const glm::vec3& eye,
                     const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&,
                                              const glm::mat4&, bool nearEye)>& fn) const;

    // The lamps' lights for this frame, appended to the frame's lists until
    // they hold `pointCap` / `spotCap`: the nearest lamp first, each scaled by
    // `on` (see nightFactor) and faded towards lightReach. Nothing by day.
    //
    // A point light whose prefab says it casts shadows casts them here too --
    // but the renderer shadows only a few point lights in all (`shadowCap`,
    // counting the scene's own shadowed lights already in the list), so only
    // the nearest lamps get theirs, and the last of them fades its shadow out
    // on its way out (PointLight::shadowStrength) rather than dropping it.
    void collectLights(const glm::vec3& eye, float on,
                       std::vector<fitzel::PointLight>& points, int pointCap,
                       std::vector<fitzel::SpotLight>& spots, int spotCap,
                       int shadowCap) const;

    // The lamp prefabs' glowing materials and the strength each glows with
    // when lit; the caller sets strength * on on the frame's GPU copy (as the
    // traffic lights do), so the library's own material never changes.
    void forEachGlow(const std::function<void(const fitzel::AssetId&, float)>& fn) const;

    int count() const { return static_cast<int>(m_lamps.size()); }

    // 0 by day, 1 once the sun is low enough for the lamps to be on: they come
    // on as it sets and are fully lit with it just under the horizon.
    static float nightFactor(const glm::vec3& sunDir);

private:
    struct Part {
        const fitzel::Mesh* mesh = nullptr;
        fitzel::AssetId     material;
        glm::mat4           local{1.0f};   // in the lamp's frame (see flatten)
    };
    struct Light {
        int       type = 0;                // 0 point, 1 spot
        glm::vec3 pos{0.0f}, dir{0.0f, -1.0f, 0.0f}, color{1.0f};
        float     range = 12.0f, cosInner = 0.9f, cosOuter = 0.8f;
        bool      castShadows = false;     // point only, as in the scene
        float     shadowBias  = 0.003f;
    };
    struct Look {
        std::string            name;
        int                    forward = 0;
        bool                   standard = false;    // the built-in lamp, no prefab
        const prefab::Prefab*  source  = nullptr;   // to notice an edited prefab
        std::vector<Part>      parts;
        std::vector<Light>     lights;
    };
    struct Placed {
        glm::mat4 frame{1.0f};
        glm::vec3 pos{0.0f};
        int       look = -1;
    };

    bool flatten(const prefab::Prefab& p, int lookIndex, Look& out) const;
    void rebuild(const CitySystem& towns, std::vector<MaterialDef>& materials);
    // The standard lamp as a look (its meshes built once, kept in m_standard).
    Look standardLook(std::vector<MaterialDef>& materials);
    std::vector<fitzel::Mesh> m_standard;   // pole, glass

    std::vector<Look>   m_looks;
    std::vector<Placed> m_lamps;
    std::vector<std::pair<fitzel::AssetId, float>> m_glow;
    int m_revision   = -1;
    int m_sinceCheck = 0;
};
