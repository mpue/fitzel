#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "CameraSystem.hpp"   // camerasys::Pose
#include "SceneTypes.hpp"     // Entity, MaterialDef

namespace fitzel {
class RenderTarget;
class Texture;
}

// --- What a camera sees, on a surface ------------------------------------------
// A monitor in a control room, a security screen, a mirror, a picture-in-
// picture on a billboard: a material names a camera of the scene
// (MaterialDef::cameraName), and whatever wears that material shows what the
// camera sees, live -- in the editor, in Play and in the exported game.
//
// Once a frame, after the scene has been drawn, every camera some material
// names is drawn once more from its own pose, into a picture of the size the
// material asks for; the material's base colour -- and, when it glows like a
// screen, its emission map -- point at that picture. As with a video, the
// binding happens at run time only: a file keeps the camera's name and the
// size, never a texture.
//
// The picture is drawn into a target of its own and then copied into the
// texture the materials show. A screen that sees itself therefore shows the
// frame before -- the corridor of mirrors -- instead of reading the very
// picture it is being drawn into, which the GPU does not allow.
//
// Each picture costs a pass over the scene, so it is drawn at most 30 times a
// second, and once however many materials show it. It is the same pass the
// editor's camera preview draws: sky, terrain, objects and trees, finished
// (tonemapped) -- not grass, water or the post chain, which are made around the
// main camera.
namespace camtex {

class CameraTextures {
public:
    CameraTextures();
    ~CameraTextures();
    CameraTextures(const CameraTextures&)            = delete;
    CameraTextures& operator=(const CameraTextures&) = delete;

    // Draw the scene as seen through `view`/`proj` from `pose` into the target
    // that is bound, sky first (main's: the same as the camera preview's).
    using DrawView = std::function<void(const glm::mat4& view, const glm::mat4& proj,
                                        const camerasys::Pose& pose)>;
    // Where camera `id` stands and looks this frame (CameraSystem::pose).
    using PoseOf = std::function<bool(int id, camerasys::Pose& out)>;

    // Once a frame, after the scene is drawn: the pictures that are due, and
    // the materials pointed at them. Leaves the framebuffers and the viewport
    // as it found them.
    void update(std::vector<MaterialDef>& materials, const std::vector<Entity>& entities,
                const PoseOf& pose, float nearPlane, float farPlane, double now,
                const DrawView& draw);
    // How many pictures the last update drew (0 when none was due).
    int drawnLastUpdate() const { return m_drawn; }
    void clear();   // out of line: a Feed holds a RenderTarget this header only names

    static constexpr double kInterval = 1.0 / 30.0;   // seconds between two pictures

private:
    // One camera at one size: where it is drawn, and what the materials show.
    struct Feed {
        std::unique_ptr<fitzel::RenderTarget> target;
        std::shared_ptr<fitzel::Texture>      picture;
        double next = 0.0;    // when it may be drawn again
        bool   used = false;  // some material showed it this update
    };
    std::unordered_map<std::string, Feed> m_feeds;   // "name#WxH"
    int m_drawn = 0;
};

// The camera of that name in the scene (one with a Camera component), or null.
const Entity* findCamera(const std::vector<Entity>& entities, const std::string& name);

// A material's camera in a file: written only when it has one; read when the
// file has one (the rest of the material is left as it is otherwise). Inline,
// so whatever reads and writes materials needs nothing linked for it.
inline void save(const MaterialDef& md, nlohmann::json& j) {
    if (md.cameraName.empty()) return;
    j["camera"]     = md.cameraName;
    j["cameraSize"] = {md.cameraSize.x, md.cameraSize.y};
    j["cameraGlow"] = md.cameraGlow;
}

inline void load(const nlohmann::json& j, MaterialDef& md) {
    if (!j.contains("camera") || !j["camera"].is_string()) return;
    md.cameraName = j["camera"].get<std::string>();
    if (j.contains("cameraSize") && j["cameraSize"].is_array() && j["cameraSize"].size() == 2 &&
        j["cameraSize"][0].is_number() && j["cameraSize"][1].is_number())
        md.cameraSize = glm::ivec2(j["cameraSize"][0].get<int>(), j["cameraSize"][1].get<int>());
    if (j.contains("cameraGlow") && j["cameraGlow"].is_boolean()) md.cameraGlow = j["cameraGlow"].get<bool>();
}

} // namespace camtex
