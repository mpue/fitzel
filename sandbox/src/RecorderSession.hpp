#pragma once

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

#include "GameRecorder.hpp"
#include "VideoWriter.hpp"

namespace fitzel { class Camera; }

// The game recorder as the editor drives it: the take in hand, its replay over
// the scene and the video export -- everything between GameRecorder's data and
// the Recorder panel (RecorderPanel.hpp), kept out of main so main only calls
// the hooks below at the right points of its frame:
//
//   frame start   now = session.clock.now(window.time())
//   after anims   session.tick(realDt); session.apply()     (before resolveHierarchy)
//   before skin   session.applySkins()                     (after the graph clear)
//   Play, late    session.capture(dt)                       (after skinning)
//   after render  if (session.wantsFrame()) session.exportFrame(pixels, w, h)
//
// A replay works like Play does: the scene is copied when it starts and put
// back when it ends, so whatever the take does to the entities -- and whatever
// is edited meanwhile -- is gone afterwards.
namespace gamerec {

CamState camStateOf(const fitzel::Camera& cam);
void     applyCam(fitzel::Camera& cam, const CamState& s);

// What the session reaches into. Every member is main's and outlives it.
struct Host {
    std::vector<Entity>& entities;
    fitzel::Camera&      camera;
    float&               timeOfDay;
    float&               storm;
    std::function<double()>                                 wallTime;
    std::function<std::string()>                            sceneName;  // shown with a take
    std::function<std::string()>                            takesDir;   // where takes are kept
    std::function<std::string(const Entity&)>              spawnJson;  // entity -> scene JSON
    std::function<bool(const std::string& json, Entity& out)> readSpawn;
    std::function<int()>                                    newId;      // a fresh entity id
};

class Session {
public:
    explicit Session(Host host) : m_host(std::move(host)) {}

    // --- Settings (edited by the panel) --------------------------------------
    bool  autoRecord = false;  // every Play run is recorded
    float speed      = 1.0f;   // replay rate
    bool  loop       = false;
    bool  freeCam    = false;  // replay: the editor's camera, not the recorded one
    int   exportFps     = 30;
    int   exportQuality = 90;  // 1..100
    videoout::Format exportFormat = videoout::Format::Mp4;
    std::string status;        // the last thing that happened, one line

    Clock clock;

    // --- Recording -----------------------------------------------------------
    void onPlayStart();        // ends a replay first; starts recording if autoRecord
    void onPlayStop();         // a running recording becomes the take in hand
    void startRecording();
    void stopRecording();
    bool recording() const { return m_rec.active(); }
    const Recorder& recorder() const { return m_rec; }
    // Once a frame in Play, after everything has moved.
    void capture(float dt);

    // --- The take in hand ----------------------------------------------------
    bool hasTake() const { return !m_take.empty(); }
    const Take& take() const { return m_take; }
    const std::string& takePath() const { return m_takePath; }   // "" until saved
    bool takeSaved() const { return m_saved; }
    bool saveTake(const std::string& name);   // into takesDir(), as <name>.frec
    bool loadTake(const std::string& path);
    std::vector<std::string> listTakes() const;   // .frec files in takesDir(), newest first

    // --- Replay --------------------------------------------------------------
    bool   beginReplay();
    void   endReplay();
    // A scene load took the scene over: the replay ends without putting the
    // old one back over the new.
    void   dropReplay();
    bool   replaying() const { return m_replaying; }
    bool   paused = false;
    double head   = 0.0;       // seconds into the take
    void   tick(float realDt);
    void   apply();
    void   applySkins();

    // --- Video ---------------------------------------------------------------
    // Plays the take from the start at exportFps on a fixed clock and writes
    // every frame to `path`. The size is the view's, taken from the first frame.
    bool  beginExport(const std::string& path);
    void  cancelExport();
    bool  exporting() const { return m_exporting; }
    bool  wantsFrame() const { return m_exporting; }
    float exportProgress() const;
    const std::string& exportPath() const { return m_exportPath; }
    // The finished frame of this tick, RGBA8 bottom-up.
    void  exportFrame(const unsigned char* rgba, int w, int h);
    std::string defaultVideoPath() const;

private:
    void finishExport(bool ok, const std::string& msg);
    int  sceneIndex(int recId) const;

    Host        m_host;
    Recorder    m_rec;
    Take        m_take;
    std::string m_takePath;
    bool        m_saved = false;

    // Replay
    bool                m_replaying = false;
    Cursor              m_cursor;
    State               m_state;
    std::vector<Entity> m_savedEntities;
    CamState            m_savedCam;
    float               m_savedYaw = 0.0f, m_savedPitch = 0.0f;
    float               m_savedTod = 0.0f, m_savedStorm = 0.0f;
    std::unordered_map<int, int> m_idMap;      // take id -> scene id (spawns may move)
    std::unordered_map<int, int> m_index;      // scene id -> entities index, per frame
    std::unordered_set<int>      m_tracked;    // every id the take ever moves

    // Export
    bool             m_exporting = false;
    bool             m_exportOwnsReplay = false;
    int              m_warmup = 0;
    int              m_exportFrame = 0;
    std::string      m_exportPath;
    videoout::Writer m_writer;
};

} // namespace gamerec
