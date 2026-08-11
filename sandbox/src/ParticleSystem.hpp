#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/graphics/InstancedMesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/graphics/Texture.hpp>

#include "FrameRender.hpp"

namespace fitzel { class AssetDatabase; }
class ParticleComponent;
struct Entity;

// General-purpose particle emitters, attached to scene objects.
//
// The sandbox already had a handful of hard-wired effect layers -- rain, boat
// spray, skid marks, contrails -- each with its own pool, shader and rules. Those
// stay: they know things about their effect that no general system should have to
// (spray knows about the waterline, skid marks about tyre slip). This is the
// other half: an emitter the AUTHOR configures, on any object, for the effects a
// track needs and the engine cannot guess -- engine exhaust, sparks off a barrier,
// steam from a vent, dust, embers, snow.
//
// The definition lives on the entity as a ParticleComponent, so it is edited in
// the Inspector, saved with the scene and copied with a prefab like anything
// else. What lives here is the simulation and the draw.
//
// Drawing is one instanced quad per particle, corners from gl_VertexID, oriented
// to the camera in the vertex shader (see particle.vert). Unlit on purpose:
// particles are their own light in every engine that does this well, and running
// them through the lit pass would cost shadow lookups per fragment for something
// that is mostly transparent.
class ParticleSystem : public RendererBase {
public:
    ParticleSystem() = default;

    // Load the shader and build the instance buffer. Needs a live GL context;
    // returns false if the shader failed to compile (the system then draws
    // nothing rather than taking the frame down).
    bool init();
    bool ready() const { return m_ready; }

    // Advance every emitter in `entities` by `dt` and retire what has expired.
    // Emitters whose entity is deactivated stop emitting but keep integrating,
    // so switching one off lets its last puff finish rather than deleting it
    // mid-air. `assetDb` resolves sprite names (cached, so this is cheap).
    void update(std::vector<Entity>& entities, float dt,
                fitzel::AssetDatabase& assetDb);

    // Draw every live particle. Depth-tested against the scene but never writing
    // depth -- transparent sprites that wrote depth would erase each other.
    void draw(const FrameContext& ctx) override;

    // Drop every particle without touching the emitters' settings. For Play
    // starting or stopping, where the world is rebuilt and last session's smoke
    // should not still be hanging in the air.
    void clear() { m_pools.clear(); }

    int  liveCount() const;
    int  emitterCount() const { return static_cast<int>(m_pools.size()); }

    // Restart one emitter's cycle: replays its burst and reopens a finished
    // one-shot. The Inspector's Play button, and what a script would call to fire
    // an effect on cue.
    void restart(int entityId);

    // The craft an emitter takes its speed from: the Glider or Opponent on this
    // entity or on the nearest ancestor that has one. Fills `topSpeed` with that
    // craft's own maximum and `name` with what it found; returns false when the
    // emitter is not mounted on a craft at all (speed glow then does nothing).
    //
    // Public because the Inspector shows what it found: an emitter whose glow
    // silently never moves because it is parented to the wrong thing is a bad
    // half-hour, and one line of text prevents it.
    static bool speedSource(const std::vector<Entity>& entities, const Entity& e,
                            float& topSpeed, std::string& name);

private:
    struct Particle {
        glm::vec3 pos{0.0f}, vel{0.0f};
        float life = 0.0f, life0 = 1.0f;
        float size0 = 1.0f;      // per-particle size roll, applied to the curve
        float rot = 0.0f, spin = 0.0f;
    };
    // One emitter's live state. Keyed by entity id rather than held on the
    // component, so a component that is copied (prefab, undo, Play's snapshot)
    // does not carry a cloud of particles with it.
    struct Pool {
        std::vector<Particle> parts;
        float emitAccum = 0.0f;  // fractional particles owed at the current rate
        float cycleT    = 0.0f;  // seconds into the current emission cycle
        bool  burstDone = false;
        bool  seen      = false; // still has an entity this frame
        glm::vec3 lastPos{0.0f};
        bool  havePos = false;
        // Which frame `parts` are stored in. Tracked rather than assumed, so
        // flipping World space in the Inspector converts the live cloud instead
        // of reinterpreting local coordinates as world ones and flinging it to
        // the origin.
        bool  storedLocal = false;
    };

    // What one draw needs, gathered per emitter during update so draw() does no
    // scene walking of its own.
    struct Batch {
        std::shared_ptr<fitzel::Texture> tex;
        bool  additive = false;
        float stretch  = 0.0f;
        std::vector<float> data;   // packed instances
        float depth = 0.0f;        // distance to the camera, for back-to-front order
    };

    std::unordered_map<int, Pool> m_pools;
    std::vector<Batch>            m_batches;
    // Sprite cache: a name resolved once, not once per frame per emitter.
    std::unordered_map<std::string, std::shared_ptr<fitzel::Texture>> m_sprites;

    fitzel::Shader        m_shader;
    fitzel::InstancedMesh m_mesh;
    bool                  m_ready = false;
};
