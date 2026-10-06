#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/asset/AssetId.hpp>
#include <fitzel/graphics/Mesh.hpp>

#include "SceneTypes.hpp"   // MaterialDef
#include "TramSim.hpp"

class SplineSystem;
namespace fitzel {
class PhysicsWorld;
struct PointLight;
}

// The trams on the scene's tram lines, drawn: TramSim drives them on every
// track spline with trams on it (Style::trams), this keeps that in step with the
// splines and draws each tram as three cars built from boxes -- a modern
// low-floor tram, cab at both ends, so the one that changes ends at a terminus
// looks the same going back. Its materials ("Tram Body", ...) live in the
// project library like any other, so they can be recoloured there.
//
// The cars are hollow: a floor at the doors' sill, seats in bays between the
// doorways, poles and rails, light strips under the ceiling, windows to look
// through, and sliding doors that open at a stop on the side the stop is on.
// In Play each car is a platform in the physics world -- floor, walls, seats,
// cab wall and every door leaf -- so a figure can walk in at a stop and ride
// along (PhysicsWorld::addPlatform).
//
// Derived, never saved: the trams start afresh whenever a tram line changes.
class TramSystem {
public:
    // Once a frame, before the frame's GPU materials are built (the materials are
    // find-or-created here). `blockers` is what stands in the street this frame:
    // cars, people, the player. `people` are the feet of whoever walks through
    // a doorway on their own (the figures scripts walk, the player): one
    // standing in an open doorway keeps the doors open.
    void update(const SplineSystem& splines, float dt, std::vector<MaterialDef>& materials,
                const std::vector<tramsim::Blocker>& blockers,
                const std::vector<glm::vec3>& people = {});

    // Every part of every tram: mesh, material, model matrix. Always in the same
    // order, which is what gives the renderer each part's last placement for the
    // motion vectors. What is drawn is where the trams were when this frame
    // began -- where their bodies in the physics world are (syncBodies), so a
    // figure riding one stands on the floor that is drawn.
    void forEachDraw(const std::function<void(const fitzel::Mesh&, const fitzel::AssetId&,
                                              const glm::mat4&)>& fn) const;

    // In Play, after update: every car's body led to where the tram has just
    // driven (made the first time). `physics` null drops them.
    void syncBodies(fitzel::PhysicsWorld* physics, float dt);
    // The world the bodies were in is gone (Play started or stopped).
    void dropBodies();

    // The light strips: their material, how strongly they glow by night and by day.
    void forEachGlow(const std::function<void(const fitzel::AssetId&, float night,
                                              float day)>& fn) const;
    // After dark, a light in each car of the trams nearest `eye`, up to `cap`.
    void collectLights(const glm::vec3& eye, float night,
                       std::vector<fitzel::PointLight>& out, int cap) const;

    // The cars as boxes, for the street traffic to brake for.
    std::vector<tramsim::Box> boxes() const { return m_sim.boxes(); }
    int tramCount() const { return m_sim.tramCount(); }
    const tramsim::Sim& sim() const { return m_sim; }
    tramsim::Sim& sim() { return m_sim; }

private:
    enum Mat { Body, Accent, Glass, Roof, Dark, Floor, Seat, Pole, Light, Lining, MatCount };
    void buildMeshes();
    void snapshot();
    void buildDoors();

    // A tram as it was when the frame began (what is drawn).
    struct Shown {
        glm::mat4 car[tramsim::kSections];
        int       side[tramsim::kSections];
        float     door = 0.0f;
    };
    // A tram's doors, as built for drawing: per car, a mesh of its leaves per
    // material (glass, frame), and the opening they were built for.
    struct DoorMeshes {
        fitzel::Mesh glass[tramsim::kSections], frame[tramsim::kSections];
        float door = -1.0f;
        int   side[tramsim::kSections]{};
    };
    struct CarBodies {
        std::uint32_t shell = 0;
        std::uint32_t leaf[8]{};
    };

    tramsim::Sim    m_sim;
    int             m_revision = -1;
    bool            m_built = false;
    fitzel::AssetId m_mat[MatCount];
    // [0] a cab car (cab at +Z), [1] the middle car; per material, empty = none.
    std::vector<fitzel::Mesh> m_mesh[2];
    // ...and what a figure walks on in each, as boxes (centre, half extents).
    std::vector<glm::vec3> m_hitC[2], m_hitH[2];
    std::vector<Shown>      m_shown;
    std::vector<DoorMeshes> m_doors;
    fitzel::PhysicsWorld*   m_phys = nullptr;
    int                     m_bodyGen = -1;
    std::vector<std::array<CarBodies, tramsim::kSections>> m_bodies;
};
