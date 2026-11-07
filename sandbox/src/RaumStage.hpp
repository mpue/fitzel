#pragma once

#include <memory>

#include <glm/glm.hpp>

#include <fitzel/rhi/Types.hpp>

struct GLFWwindow;
namespace fitzel {
class Camera;
class Mesh;
}

// The game as a diorama in a room of raum, the 3D desktop
// (~/Documents/devel/raum, protocols/raum-stage-v1.xml).
//
// raum shows a stage as an ordinary window while you work in its room. When
// the camera leans back, the window goes, and the game draws its world from
// raum's camera instead -- every frame, onto the table raum lays on the room's
// floor -- and raum sets that picture into the room. The game keeps running
// all the while: its scripts, its own camera and its HUD carry on as if it
// were still in front; only what reaches the screen is drawn from elsewhere.
//
// Asked for by whoever starts the player: RAUM_STAGE=1 in the environment.
// Without it, or under any other compositor, every call here is a no-op and
// the player is the player.
class RaumStage {
public:
    RaumStage();
    ~RaumStage();
    RaumStage(const RaumStage&)            = delete;
    RaumStage& operator=(const RaumStage&) = delete;

    // RAUM_STAGE=1: the window must be made with an alpha channel for it.
    static bool wanted();

    // After the window exists: ask raum to make it a stage. False when this is
    // not raum (or not Wayland) -- nothing else changes then.
    bool connect(GLFWwindow* window);
    bool connected() const;

    // Once a frame, after the window's events: take raum's latest camera.
    void poll();
    // Drawing from raum's camera this frame (the room is leant back).
    bool diorama() const;

    // The square of the world that lies on raum's table: its middle, on the
    // ground, and its edge in metres. A game says it (game.setStageWorld);
    // until then the square round the origin, 1024 m.
    void setWorld(const glm::vec3& centre, float size);
    glm::vec3 worldCentre() const;
    float     worldSize() const;

    // raum's camera, in the world's metres, put into `cam` (eye, basis, field
    // of view; the far plane reaches past the whole square).
    void applyCamera(fitzel::Camera& cam) const;
    // After the swap: the game's own eye back into `cam`, as it was before
    // applyCamera (a Camera cannot be assigned).
    static void restoreCamera(fitzel::Camera& cam, const fitzel::Camera& own);

    // The finished picture, in the bound framebuffer, before the HUD goes on:
    // drawn from raum's camera, everything that is not the square's world is
    // cleared to nothing (alpha 0); from our own, alpha is made 1 everywhere,
    // so the window is not see-through in front. `depth` is the scene's depth
    // (the whole pane), `viewProj` the camera it was drawn with.
    void finishPicture(fitzel::Mesh& fsQuad, fitzel::rhi::TextureHandle depth,
                       const glm::mat4& viewProj, int width, int height);

    // Right before the swap: the camera this frame was drawn from, and the
    // game's own (where its eye is while it draws from raum's).
    void frameDrawn(const fitzel::Camera& drawn, const fitzel::Camera& own);

    struct Impl;   // RaumStage.cpp; its Wayland listeners reach in

private:
    std::unique_ptr<Impl> m;
};
