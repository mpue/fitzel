#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace fitzel { class Camera; }

// --- Shot list (`--shots <file>`) --------------------------------------------
//
// A look change has to be LOOKED at, from the same places, before and after.
// --profile-shot answers that for one view, from wherever the scene happens to
// start -- one launch, one picture, and a 1 GB project takes half a minute to
// load. A landscape needs a dozen views at once: the valley from the ridge, the
// meadow at knee height, the lake at dusk.
//
// This plays the project and walks a list of fixed views, holding each long
// enough for streaming, TAA and auto exposure to settle, and writes one PNG per
// view (or a short numbered sequence, for things that move). Then it quits.
//
// The list is plain text, one view per line:
//
//     name  x  y  z  yaw  pitch  [fov]  [hour]  [settle]  [frames]  [every]
//
// `y` written as `~1.7` means 1.7 m above the ground at (x, z). yaw/pitch are
// the fly camera's degrees (yaw 0 looks down +X, 90 down +Z). `hour` < 0 keeps
// the scene's clock; otherwise the time of day is held there. `frames` > 1
// writes name_00.png, name_01.png ... `every` seconds apart. '#' starts a
// comment.
namespace shotlist {

struct Shot {
    std::string name;
    glm::vec3   pos{0.0f};
    bool        groundRel = false;   // pos.y is metres above the terrain
    float       yaw = 0.0f, pitch = 0.0f, fov = 60.0f;
    float       hour = -1.0f;
    float       settle = 3.0f;
    int         frames = 1;
    float       every = 0.25f;
};

class Runner {
public:
    // Parses `file`; PNGs go to `outDir` (empty: the list's own folder).
    bool load(const std::string& file, const std::string& outDir);
    bool active() const { return !m_shots.empty() && !m_done; }

    // Before the frame is drawn: put the eye on the current shot and, if the
    // shot names an hour, hold the clock there.
    void applyCamera(fitzel::Camera& cam,
                     const std::function<float(float, float)>& groundAt,
                     float& timeOfDay);

    // After the swap: photograph the front buffer when the current shot is due.
    // Returns true once the last shot has been written.
    bool afterFrame(double now, int fbWidth, int fbHeight);

    // Written to the log beside each picture: whatever state the host thinks
    // explains it (what had streamed in, what the frame cost).
    std::function<std::string()> status;

private:
    std::vector<Shot> m_shots;
    std::string       m_outDir;
    int               m_index = -1;     // -1: not started
    int               m_frame = 0;      // within a sequence
    double            m_since = 0.0;    // when the current shot / frame began
    bool              m_done  = false;
};

} // namespace shotlist
