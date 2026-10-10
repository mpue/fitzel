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
//
//     name  x  y  z  yaw  pitch  fov  hour  settle  frames  every  vx  vz  turn  [shrink]
//
// moves the eye while the sequence runs: vx/vz metres per second, turn degrees
// of yaw per second, on the wall clock from the first picture on. With `every`
// 0 that is every frame -- what flickers only in motion shows up only like
// this. A moving sequence is written at 1/shrink size (default 4), or the PNGs
// alone would slow the frames down to one a second. A negative shrink -k keeps
// full resolution and writes the middle 1/k of the width and height instead --
// shrinking averages away exactly the pixel crawl one is looking for.
//
//     name  game  [hour]  [settle]  [frames]  [every]
//
// takes the picture the game itself shows -- its camera, a script's over the
// shoulder, whatever is the view -- instead of placing one.
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
    bool        gameView = false;    // "game": the game's own camera, untouched
    glm::vec2   vel{0.0f};           // m/s in x, z while the sequence runs
    float       turn = 0.0f;         // yaw degrees per second
    int         shrink = 1;          // pictures at 1/shrink size (-k: middle 1/k)
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
    // A view named "@<n>..." looks at whatever the host says target n is this
    // frame (a flying bird, say) instead of along its yaw/pitch. False = no such
    // target: the yaw/pitch stand.
    std::function<bool(int, glm::vec3&)> target;
    // ...and may be PLACED by the host instead: when this says where target
    // n's eye is this frame (riding in a tram, say), the eye goes there and
    // looks at `at`, whatever the line's position and yaw/pitch say.
    std::function<bool(int, glm::vec3& eye, glm::vec3& at)> eye;

    // Optional (`--shots-trace <samples>`): after each view's picture, a
    // PATH-TRACED one of the same view, written beside it as <name>_traced.png.
    // The runner holds the view while it renders. startTrace asks the host to
    // harvest and render; traceDone says when it has finished; saveTrace writes
    // it. All three empty: raster pictures only.
    std::function<void()>                   startTrace;
    std::function<bool()>                   traceDone;
    std::function<bool(const std::string&)> saveTrace;

private:
    std::vector<Shot> m_shots;
    std::string       m_outDir;
    int               m_index = -1;     // -1: not started
    double            m_now = 0.0;      // the clock at the last afterFrame
    double            m_seqStart = -1.0; // when this sequence's first picture was taken
    int               m_frame = 0;      // within a sequence
    double            m_since = 0.0;    // when the current shot / frame began
    bool              m_done  = false;
    bool              m_tracing = false; // holding the view for a traced still
};

} // namespace shotlist
