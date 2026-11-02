#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <glm/glm.hpp>

#include "SailCloth.hpp"
#include "SceneTypes.hpp"   // Entity, LoadedModel
#include "Wind.hpp"

// The sails at Play time: every model part with a Sail component, posed by the
// wind each frame (SailCloth.hpp does the posing; this is the glue).
//
// A model's part is drawn from its LoadedModel's meshes, which are triangle
// lists -- three corners per triangle, shared by nobody. Re-uploading those
// every frame would send three times the corners there are, so Play swaps in an
// indexed copy of each sail mesh (its corners shared) and Stop swaps the
// modelled one back. The meshes belong to the model, not the entity: two ships
// of the same file share one set of sails, posed for the first of them.
class SailSystem {
public:
    using ModelOf = std::function<LoadedModel*(int modelId)>;

    // At Play start, after the scene's transforms are resolved: find the
    // sails, work out where each piece is tied (what the rest of its model
    // touches it), swap in the posable meshes.
    void begin(const std::vector<Entity>& entities, const ModelOf& modelOf);
    // Once a frame in Play, after the hull has moved: pose every sail in the
    // air -- its direction and gusts (`air`), its speed in m/s.
    void update(const std::vector<Entity>& entities, const ModelOf& modelOf,
                const wind::State& air, float windSpeed, float dt);
    // At Stop: the modelled meshes back.
    void end(const ModelOf& modelOf);
    bool empty() const { return m_sails.empty(); }

private:
    struct Sail {
        int                          entity = -1;
        int                          model  = -1;
        int                          mesh   = -1;     // which of the model's meshes
        std::unique_ptr<fitzel::Mesh> modelled;       // what Play swapped out
        sail::Cloth                  cloth;
        std::vector<fitzel::Vertex>  posed;
        std::vector<float>           gust;            // per piece, this frame
    };
    std::vector<Sail> m_sails;
};
