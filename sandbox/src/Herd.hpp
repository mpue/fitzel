#pragma once

#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/render/Renderer.hpp>
#include <fitzel/world/Model.hpp>

// --- A herd grazing in the meadow --------------------------------------------------
//
// A skinned, animated model (horses, deer, cattle -- whatever the project has)
// turned into a small herd that lives in a meadow: each animal grazes for a
// while, walks a few metres to fresh grass, grazes again, and keeps near the
// others. Every animal is its own skinned copy of the model on its own clock,
// so no two move in step -- the engine's AnimationComponent shares one mesh per
// model, which would put a whole herd into the same pose at the same instant.
//
// CPU skinning (sampleSkeleton/skinPrimitive), a mesh per animal per material,
// submitted to the renderer like any object: it is lit, fogged, and it casts
// its shadow into the grass.
class Herd {
public:
    // The model and which of its clips are which: grazing (standing) and
    // walking, with the walk's ground speed in metres per second.
    struct Config {
        std::string model;             // absolute path (.fbx / .glb)
        int   count      = 6;
        glm::vec2 centre{0.0f};        // the meadow
        float radius     = 45.0f;
        float height     = 1.65f;      // metres at the withers-ish (model is normalised)
        int   grazeClip  = 0;
        int   walkClip   = 2;
        float walkSpeed  = 1.4f;
        float yawOffset  = 0.0f;       // degrees: model forward vs +Z
    };

    bool load(const Config& cfg, fitzel::Shader& lit);
    bool loaded() const { return m_ok; }

    struct World {
        std::function<float(float, float)> ground;
        std::function<bool(float, float)>  walkable;   // false: water, road, ...
    };
    void update(float dt, const World& w);
    void submit(fitzel::Renderer& r);

    std::string status() const;
    std::string statusShort() const;
    int count() const { return static_cast<int>(m_animals.size()); }
    glm::vec3 animalPos(int i) const {
        return (i >= 0 && i < count()) ? m_animals[i].pos : glm::vec3(0.0f);
    }

private:
    struct Animal {
        glm::vec3 pos{0.0f};
        float yaw = 0.0f;          // radians, heading in XZ (0 = +Z)
        glm::vec2 target{0.0f};
        bool  walking = false;
        float clipTime = 0.0f;
        float stateLeft = 0.0f;    // seconds until the next decision
        float blend = 0.0f;        // 0 grazing .. 1 walking (pose blend weight)
        std::vector<fitzel::Mesh> meshes;   // one per primitive
        glm::mat4 model{1.0f};
    };

    void decide(Animal& a, const World& w);

    Config m_cfg;
    bool   m_ok = false;
    fitzel::ModelData m_model;
    float  m_scale = 1.0f;
    glm::vec3 m_offset{0.0f};      // model space: what puts the feet on the ground
    std::vector<std::unique_ptr<fitzel::Texture>> m_tex;
    std::vector<std::unique_ptr<fitzel::Material>> m_mats;
    std::vector<Animal> m_animals;
    std::vector<fitzel::Vertex> m_scratch;
    std::mt19937 m_rng{1776u};
    float uni() { return std::uniform_real_distribution<float>(0.0f, 1.0f)(m_rng); }
};
