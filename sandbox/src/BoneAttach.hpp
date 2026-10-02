#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "SceneTypes.hpp" // Entity

class ModelLibrary;
namespace fitzel { struct ModelData; }

// Things a figure carries: the pistol in a hand, a hat on a head, a torch.
//
// A carried object is an ordinary entity that follows one bone of an animated
// model. It cannot simply be the figure's child: the scene graph knows the
// figure's transform, not where its hand is in the clip it is playing -- that
// only exists once the skinning pass has posed the skeleton for this frame. So
// the skinning pass hands every pose it draws to this, and right after it every
// carried object is put on its bone (apply), before anything is drawn. The
// object stays a root entity with its own size; only its place and its turn
// are the bone's.
//
// Play-time state, like the scripts that make it (game.attach): cleared when
// Play starts and stops.
namespace boneattach {

// The joint called `name` in a model's skeleton -- exactly, else ignoring case.
// -1 if there is none.
int jointIndex(const fitzel::ModelData& model, const std::string& name);

class Attachments {
public:
    // What the skinning pass just drew for `figure`.
    void storePose(int figure, const std::vector<glm::mat4>& palette);

    // A bone of `figure` in the world as last drawn, without scale. False for an
    // unknown figure or bone, or a figure not posed yet.
    bool boneWorld(const std::vector<Entity>& entities, ModelLibrary& models,
                   int figure, const std::string& bone, glm::mat4& out) const;
    // The bones a figure has, in skeleton order (empty for anything else).
    std::vector<std::string> boneNames(const std::vector<Entity>& entities,
                                       ModelLibrary& models, int figure) const;

    // Hang `child` on `figure`'s `bone`. `offset` places it in the bone's space
    // (rigid); null keeps it where it is now, relative to the bone. A child with
    // a parent is made a root first, keeping its place. False if either object
    // or the bone is unknown, or (without an offset) the figure is not posed yet.
    bool attach(std::vector<Entity>& entities, ModelLibrary& models, int child,
                int figure, const std::string& bone, const glm::mat4* offset);
    void detach(int child);
    bool carried(int child) const { return m_links.count(child) != 0; }

    // Every carried object onto its bone, from the poses stored this frame.
    void apply(std::vector<Entity>& entities, ModelLibrary& models) const;

    void clear();

private:
    struct Link {
        int       figure = -1;
        int       joint  = -1;
        glm::mat4 offset{1.0f};
    };
    // The bone's world matrix from a stored pose, scale taken out.
    bool jointWorld(const Entity& figure, ModelLibrary& models, int joint,
                    glm::mat4& out) const;

    std::unordered_map<int, std::vector<glm::mat4>> m_pose;  // figure -> palette
    std::unordered_map<int, Link>                   m_links; // child -> its bone
};

} // namespace boneattach
