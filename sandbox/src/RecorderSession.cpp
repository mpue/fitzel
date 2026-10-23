#include "RecorderSession.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

#include <fitzel/scene/Camera.hpp>

namespace gamerec {
namespace {

namespace fs = std::filesystem;

// Frames rendered at the take's first moment before the first one is written:
// TAA, motion blur and auto exposure all remember the frame before, and that
// frame was wherever the editor's camera happened to be.
constexpr int kWarmupFrames = 12;

std::string seconds(double s) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.1f s", s);
    return buf;
}

} // namespace

CamState camStateOf(const fitzel::Camera& cam) {
    return {cam.position(), cam.front(), cam.up(), cam.fov()};
}

void applyCam(fitzel::Camera& cam, const CamState& s) {
    cam.setPosition(s.pos);
    cam.setFov(s.fov);
    // Yaw/pitch first, for whatever reads them (yaw 0 looks down +X, 90 down
    // +Z); then the basis, which carries the roll they cannot.
    const glm::vec3 f = glm::normalize(s.front);
    cam.setYaw(glm::degrees(std::atan2(f.z, f.x)));
    cam.setPitch(glm::degrees(std::asin(glm::clamp(f.y, -1.0f, 1.0f))));
    cam.setBasis(s.front, s.up);
}

// --- Recording ----------------------------------------------------------------

void Session::onPlayStart() {
    if (m_exporting) cancelExport();
    if (m_replaying) endReplay();
    if (autoRecord) startRecording();
}

void Session::onPlayStop() {
    if (m_rec.active()) stopRecording();
}

void Session::startRecording() {
    if (m_replaying) endReplay();
    m_rec.begin(m_host.sceneName ? m_host.sceneName() : std::string());
    status = "Recording...";
}

void Session::stopRecording() {
    if (!m_rec.active()) return;
    Take t = m_rec.end();
    if (t.empty()) { status = "Nothing recorded."; return; }
    m_take = std::move(t);
    m_takePath.clear();
    m_saved = false;
    m_cursor.reset(nullptr);
    status = "Recorded " + seconds(m_take.duration()) + " (" +
             std::to_string(m_take.frames.size()) + " frames) -- not saved yet.";
}

void Session::capture(float dt) {
    if (!m_rec.active()) return;
    m_rec.capture(dt, camStateOf(m_host.camera), m_host.timeOfDay, m_host.storm,
                  m_host.entities, m_host.spawnJson);
}

// --- Files ----------------------------------------------------------------------

bool Session::saveTake(const std::string& name) {
    if (m_take.empty()) { status = "No take to save."; return false; }
    const std::string dir = m_host.takesDir ? m_host.takesDir() : std::string();
    if (dir.empty()) { status = "No folder to save takes in."; return false; }
    std::string stem = name.empty() ? std::string("take") : name;
    for (char& c : stem)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|')
            c = '_';
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::string path = (fs::path(dir) / (stem + ".frec")).string();
    std::string err;
    if (!save(m_take, path, err)) { status = "Save failed: " + err; return false; }
    m_takePath = path;
    m_saved = true;
    status = "Saved " + fs::path(path).filename().string();
    return true;
}

bool Session::loadTake(const std::string& path) {
    Take t;
    std::string err;
    if (!load(t, path, err)) { status = "Load failed: " + err; return false; }
    if (m_exporting) cancelExport();
    if (m_replaying) endReplay();
    m_take = std::move(t);
    m_takePath = path;
    m_saved = true;
    m_cursor.reset(nullptr);
    head = 0.0;
    status = "Loaded " + fs::path(path).filename().string() + ", " +
             seconds(m_take.duration());
    const std::string scene = m_host.sceneName ? m_host.sceneName() : std::string();
    if (!m_take.scene.empty() && !scene.empty() && m_take.scene != scene)
        status += " -- recorded in " + m_take.scene + ", not this scene";
    return true;
}

std::vector<std::string> Session::listTakes() const {
    std::vector<std::pair<fs::file_time_type, std::string>> found;
    const std::string dir = m_host.takesDir ? m_host.takesDir() : std::string();
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return {};
    for (const auto& de : fs::directory_iterator(dir, ec)) {
        if (!de.is_regular_file(ec) || de.path().extension() != ".frec") continue;
        found.emplace_back(de.last_write_time(ec), de.path().string());
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (auto& f : found) out.push_back(std::move(f.second));
    return out;
}

// --- Replay ---------------------------------------------------------------------

bool Session::beginReplay() {
    if (m_take.empty()) { status = "No take to play."; return false; }
    if (m_rec.active()) { status = "Stop recording first."; return false; }
    if (m_replaying) return true;
    auto& ents = m_host.entities;
    m_savedEntities = ents;
    m_savedCam   = camStateOf(m_host.camera);
    m_savedYaw   = m_host.camera.yaw();
    m_savedPitch = m_host.camera.pitch();
    m_savedTod   = m_host.timeOfDay;
    m_savedStorm = m_host.storm;

    m_tracked.clear();
    for (const Frame& f : m_take.frames)
        for (const Change& c : f.changes) m_tracked.insert(c.id);

    // What the run spawned, back into the scene -- hidden until the take
    // reaches the moment it appeared. Under its own id if that is free; an id
    // the scene has since given to something else gets a fresh one.
    std::unordered_set<int> used;
    for (const Entity& e : ents) used.insert(e.id);
    m_idMap.clear();
    std::vector<Entity> spawned;
    for (const Spawn& s : m_take.spawns) {
        Entity e;
        if (!m_host.readSpawn || !m_host.readSpawn(s.json, e)) continue;
        int id = s.id;
        if (used.count(id)) id = m_host.newId ? m_host.newId() : id;
        used.insert(id);
        m_idMap[s.id] = id;
        e.id = id;
        e.active = false;
        spawned.push_back(std::move(e));
    }
    for (Entity& e : spawned) {
        auto p = m_idMap.find(e.parent);
        if (p != m_idMap.end()) e.parent = p->second;
        ents.push_back(std::move(e));
    }

    m_cursor.reset(&m_take);
    if (head >= m_take.duration()) head = 0.0;
    m_replaying = true;
    paused = false;
    status = "Replaying " + seconds(m_take.duration());
    return true;
}

void Session::endReplay() {
    if (!m_replaying) return;
    m_host.entities = std::move(m_savedEntities);
    m_savedEntities.clear();
    fitzel::Camera& cam = m_host.camera;
    cam.setPosition(m_savedCam.pos);
    cam.setFov(m_savedCam.fov);
    cam.setYaw(m_savedYaw);
    cam.setPitch(m_savedPitch);
    m_host.timeOfDay = m_savedTod;
    m_host.storm     = m_savedStorm;
    m_replaying = false;
    m_state.clear();
    m_index.clear();
}

void Session::dropReplay() {
    if (m_exporting) {
        m_exportOwnsReplay = false;
        finishExport(false, "a scene load interrupted it");
    }
    m_replaying = false;
    m_savedEntities.clear();
    m_state.clear();
    m_index.clear();
}

void Session::tick(float realDt) {
    if (!m_replaying || paused || m_exporting) return;
    head += static_cast<double>(realDt) * speed;
    const double len = m_take.duration();
    if (head >= len) {
        if (loop && len > 0.0) {
            head = std::fmod(head, len);
        } else {
            head = len;
            paused = true;
        }
    }
    if (head < 0.0) head = 0.0;
}

int Session::sceneIndex(int recId) const {
    auto m = m_idMap.find(recId);
    const int id = m == m_idMap.end() ? recId : m->second;
    auto it = m_index.find(id);
    return it == m_index.end() ? -1 : it->second;
}

void Session::apply() {
    if (!m_replaying) return;
    auto& ents = m_host.entities;
    m_index.clear();
    for (int i = 0; i < static_cast<int>(ents.size()); ++i) m_index[ents[i].id] = i;

    CamState cam;
    float tod = m_host.timeOfDay, storm = m_host.storm;
    m_cursor.sample(head, m_state, cam, tod, storm);
    for (int id : m_tracked) {
        const int i = sceneIndex(id);
        if (i < 0) continue;
        auto p = m_state.find(id);
        if (p == m_state.end()) ents[i].active = false;   // not (or no longer) there
        else                    applyTransform(ents[i], p->second);
    }
    if (!freeCam || m_exporting) applyCam(m_host.camera, cam);
    m_host.timeOfDay = tod;
    m_host.storm     = storm;
}

void Session::applySkins() {
    if (!m_replaying) return;
    auto& ents = m_host.entities;
    for (const auto& [id, pose] : m_state) {
        if (pose.skin == 0) continue;
        const int i = sceneIndex(id);
        if (i >= 0 && i < static_cast<int>(ents.size())) applySkin(ents[i], pose);
    }
}

// --- Video ----------------------------------------------------------------------

std::string Session::defaultVideoPath() const {
    const std::string dir = m_host.takesDir ? m_host.takesDir() : std::string();
    std::string stem = m_takePath.empty() ? std::string("take")
                                          : fs::path(m_takePath).stem().string();
    const char* ext = exportFormat == videoout::Format::Mp4 ? ".mp4" : ".avi";
    return (fs::path(dir.empty() ? std::string(".") : dir) / (stem + ext)).string();
}

bool Session::beginExport(const std::string& path) {
    if (m_exporting) return false;
    if (m_take.empty()) { status = "No take to export."; return false; }
    if (exportFormat == videoout::Format::Mp4 && !videoout::ffmpegAvailable()) {
        status = "MP4 needs ffmpeg (apt install ffmpeg) -- or pick AVI.";
        return false;
    }
    m_exportOwnsReplay = !m_replaying;
    if (!beginReplay()) return false;
    head = 0.0;
    paused = false;
    m_exportPath = path;
    m_exportFrame = 0;
    m_warmup = kWarmupFrames;
    m_exporting = true;
    clock.beginFixed(m_host.wallTime ? m_host.wallTime() : 0.0, exportFps);
    status = "Exporting video...";
    return true;
}

float Session::exportProgress() const {
    const double len = m_take.duration();
    if (!m_exporting || len <= 0.0) return 0.0f;
    return static_cast<float>(std::clamp(head / len, 0.0, 1.0));
}

void Session::exportFrame(const unsigned char* rgba, int w, int h) {
    if (!m_exporting) return;
    if (m_warmup > 0) { --m_warmup; return; }
    if (!m_writer.isOpen()) {
        std::string err;
        if (!m_writer.open(m_exportPath, w, h, exportFps, exportFormat, exportQuality, err)) {
            finishExport(false, err);
            return;
        }
    }
    // The view's size is the video's: a view resized halfway would hand in
    // frames of another size.
    if ((w & ~1) != m_writer.width() || (h & ~1) != m_writer.height() || w < m_writer.width() ||
        h < m_writer.height()) {
        finishExport(false, "the view changed size during the export");
        return;
    }
    if (!m_writer.frame(rgba)) {
        finishExport(false, "couldn't write frame " + std::to_string(m_exportFrame));
        return;
    }
    ++m_exportFrame;
    head = static_cast<double>(m_exportFrame) / std::max(exportFps, 1);
    if (head > m_take.duration() + 1e-9) finishExport(true, std::string());
}

void Session::cancelExport() {
    if (!m_exporting) return;
    finishExport(false, "cancelled");
}

void Session::finishExport(bool ok, const std::string& why) {
    std::string msg;
    bool closed = true;
    const bool wrote = m_writer.isOpen();
    if (wrote) closed = m_writer.close(msg);
    m_exporting = false;
    clock.endFixed(m_host.wallTime ? m_host.wallTime() : 0.0);
    if (ok && closed) {
        status = "Video written: " + fs::path(m_exportPath).filename().string() + " -- " + msg;
    } else {
        status = "Export stopped: " + (why.empty() ? msg : why);
        if (!ok && wrote) {
            std::error_code ec;
            fs::remove(m_exportPath, ec);   // a half-written file is no video
        }
    }
    if (m_exportOwnsReplay) endReplay();
    else                    paused = true;
}

} // namespace gamerec
