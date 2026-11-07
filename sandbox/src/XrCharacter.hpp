#pragma once

// The figures of an X-Ray game (S.T.A.L.K.E.R.): a skinned OGF model -- its
// skeleton, its skin, the textured parts -- and the motions it plays, which
// sit either in the model itself or in the .omf libraries it names.
//
// Everything stays in X-Ray's own axes and conventions here; the converter
// mirrors it into fitzel's (StalkerLife). Two of those conventions decide
// whether a figure stands or folds into a knot:
//   - a bone's rest transform is Fmatrix::setXYZi of the IK data's angles,
//     relative to the parent bone;
//   - a motion key's quaternion goes through Fmatrix::mk_xform, which builds
//     the TRANSPOSE of the textbook rotation (X-Ray multiplies row vectors).

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

namespace xray {

class FileSystem;

struct Bone {
    std::string name;
    int parent = -1;                    // index into the skeleton, -1 for a root
    glm::vec3 restRotation{0.0f};       // setXYZi angles (radians)
    glm::vec3 restOffset{0.0f};         // from the parent bone
};

struct SkinVertex {
    glm::vec3 pos{0.0f};
    glm::vec3 normal{0.0f, 1.0f, 0.0f};
    glm::vec2 uv{0.0f};
    std::uint16_t joints[4] = {0, 0, 0, 0};   // bones of the skeleton
    float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
};

// One textured piece of the skin, in the model's rest pose; its triangles as
// D3D winds them.
struct SkinPart {
    std::string texture;                // "act\act_stalker_novice"
    std::string shader;                 // "models\model"
    std::vector<SkinVertex> verts;
    std::vector<std::uint32_t> indices;
};

// One bone's keys in a motion, 30 per second: as many as the motion has
// frames, or one when the bone holds still. Local to the parent bone, X-Ray's
// axes; `rotation` holds the stored quaternion as it is (x, y, z, w into
// glm::quat's fields), for mk_xform.
struct BoneTrack {
    std::vector<glm::quat> rotation;
    std::vector<glm::vec3> offset;
};

struct Motion {
    std::string name;
    int   frames = 0;
    float speed = 1.0f;                 // the motion's own rate (its CMotionDef)
    bool  loops = true;                 // false: stops at its end (a death)
    // Per bone of the skeleton it was decoded for; empty where the library
    // has no such bone (an eye, a tail) -- that one keeps its rest pose.
    std::vector<BoneTrack> tracks;
};

constexpr float kMotionFps = 30.0f;

// The motions of a model or of an .omf file: the names, the partition each
// bone belongs to ("legs", "torso", "head" for people, "default" for a
// mutant), and the packed keys, decoded only for the motions asked for.
class MotionLibrary {
public:
    bool load(std::vector<std::uint8_t> bytes, std::string& error);
    bool empty() const { return motions_.empty(); }
    bool has(const std::string& name) const { return motions_.count(name) != 0; }
    std::vector<std::string> names() const;
    // The partition of a bone, by name; "" when the library does not know it.
    std::string partOf(const std::string& bone) const;
    bool decode(const std::string& name, const std::vector<Bone>& skeleton, Motion& out,
                std::string& error) const;

private:
    struct Entry {
        std::size_t at = 0, size = 0;   // the motion's chunk in bytes_
        float speed = 1.0f;
        bool  loops = true;
    };
    std::vector<std::uint8_t> bytes_;
    std::vector<std::string> trackBone_;              // track index -> bone name
    std::map<std::string, std::string> part_;         // bone name -> partition
    std::map<std::string, Entry> motions_;
};

struct Character {
    std::vector<Bone> bones;
    std::vector<SkinPart> parts;
    // The .omf libraries the model plays from, as the game names them
    // ("actors\stalker_animation" = meshes/actors/stalker_animation.omf).
    std::vector<std::string> motionRefs;
    MotionLibrary embedded;             // motions inside the model itself
};

bool readCharacter(const FileSystem& fs, const std::string& path, Character& out, std::string& error);

// meshes/<ref>.omf
bool readMotionLibrary(const FileSystem& fs, const std::string& ref, MotionLibrary& out,
                       std::string& error);

} // namespace xray
