// recordercheck -- does a take come back as the game was, and does a video come
// out of it?
//
//   1. Recording: frame 0 holds every entity, later frames only what moved; a
//      spawned entity is kept with its JSON, a removed one is marked gone.
//   2. The file: saved and loaded back field for field; a truncated or foreign
//      file is refused, not half-read.
//   3. Sampling: at a frame's time the frame exactly, between two frames the
//      blend, backwards as well as forwards (the cached states), across a
//      rotation flip the short way round, across a teleport no smear.
//   4. Replay over a scene: poses applied, the spawned entity back and hidden
//      until its moment, the removed one hidden after its; a spawn whose id the
//      scene has since used gets another; the scene put back exactly at the end.
//   5. Video: an AVI written frame by frame (odd size, cropped even) whose
//      RIFF structure and index add up and whose first frame decodes to the
//      picture handed in; an export through the session on its fixed clock;
//      an MP4 too when ffmpeg is there.
//
// No GL, no window.
//
//   build/linux/bin/recordercheck [--out <dir>]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include <stb_image.h>

#include <glm/gtc/quaternion.hpp>

#include <fitzel/scene/Camera.hpp>

#include "../src/Component.hpp"
#include "../src/GameRecorder.hpp"
#include "../src/RecorderSession.hpp"
#include "../src/VideoWriter.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;
int passes   = 0;

void check(bool ok, const std::string& what) {
    if (ok) {
        ++passes;
    } else {
        ++failures;
        std::printf("FAIL  %s\n", what.c_str());
    }
}

bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }
bool near(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f) {
    return glm::length(a - b) <= eps;
}

Entity makeEntity(int id, const std::string& name, glm::vec3 at) {
    Entity e;
    e.id = id;
    e.name = name;
    e.localCenter = at;
    e.center = at;
    return e;
}

// The scene's rotation order (Rz * Ry * Rx), for comparing orientations
// rather than Euler triples.
glm::mat3 rotOf(const glm::vec3& deg) {
    const glm::vec3 r = glm::radians(deg);
    return glm::mat3_cast(glm::angleAxis(r.z, glm::vec3(0, 0, 1)) *
                          glm::angleAxis(r.y, glm::vec3(0, 1, 0)) *
                          glm::angleAxis(r.x, glm::vec3(1, 0, 0)));
}
bool sameRotation(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f) {
    const glm::mat3 ma = rotOf(a), mb = rotOf(b);
    for (int c = 0; c < 3; ++c)
        if (glm::length(ma[c] - mb[c]) > eps) return false;
    return true;
}

// --- The game: a scene and what moves in it ------------------------------------
//   1 ground   never moves
//   2 crate    slides along +X at 3 m/s
//   3 spinner  turns about Y at 90 deg/s
//   4 runner   a skinned figure whose clip clock runs
//   5 bomb     removed at frame 40
//  20 shot     spawned at frame 25, flies up
constexpr int   kFrames = 120;
constexpr float kDt     = 1.0f / 60.0f;

struct Game {
    std::vector<Entity> ents;
    fitzel::Camera cam{glm::vec3(0, 2, 10), -90.0f, 0.0f};
    float tod = 12.0f, storm = 0.0f;

    Game() {
        ents.push_back(makeEntity(1, "ground", {0, 0, 0}));
        ents.push_back(makeEntity(2, "crate", {0, 1, 0}));
        ents.push_back(makeEntity(3, "spinner", {5, 1, 0}));
        Entity runner = makeEntity(4, "runner", {-5, 0, 0});
        auto ac = std::make_unique<AnimationComponent>();
        ac->clip = 1;
        runner.components.items.push_back(std::move(ac));
        ents.push_back(std::move(runner));
        ents.push_back(makeEntity(5, "bomb", {0, 5, 5}));
    }

    void step(int frame) {
        const float t = frame * kDt;
        for (Entity& e : ents) {
            if (e.id == 2) e.localCenter.x = 3.0f * t;
            if (e.id == 3) e.localRotation.y = std::fmod(90.0f * t, 360.0f);
            if (e.id == 4) e.components.get<AnimationComponent>()->time = t * 1.5f;
            if (e.id == 20) e.localCenter.y = 1.0f + 10.0f * (t - 25 * kDt);
        }
        if (frame == 25) ents.push_back(makeEntity(20, "shot", {1, 1, 1}));
        if (frame == 40)
            ents.erase(std::remove_if(ents.begin(), ents.end(), [](const Entity& e) { return e.id == 5; }),
                       ents.end());
        cam.setPosition(glm::vec3(3.0f * t, 2.0f, 10.0f));
        tod = 12.0f + t;
        storm = std::min(1.0f, t);
    }
};

std::string spawnJson(const Entity& e) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "{\"name\":\"%s\",\"x\":%g,\"y\":%g,\"z\":%g}", e.name.c_str(),
                  e.localCenter.x, e.localCenter.y, e.localCenter.z);
    return buf;
}

gamerec::Take recordGame() {
    Game g;
    gamerec::Recorder rec;
    rec.begin("check.fitzel");
    for (int f = 0; f < kFrames; ++f) {
        g.step(f);
        rec.capture(f == 0 ? 0.0f : kDt, gamerec::camStateOf(g.cam), g.tod, g.storm, g.ents, spawnJson);
    }
    return rec.end();
}

// --- 1. Recording ----------------------------------------------------------------
void checkRecording(const gamerec::Take& t) {
    check(t.frames.size() == kFrames, "one frame per capture");
    check(near(static_cast<float>(t.duration()), (kFrames - 1) * kDt, 1e-4f), "duration is game time");
    check(t.frames[0].changes.size() == 5, "frame 0 holds every entity");
    bool groundLater = false, bombGone = false, shotSpawned = false;
    std::size_t maxLater = 0;
    for (std::size_t i = 1; i < t.frames.size(); ++i) {
        for (const gamerec::Change& c : t.frames[i].changes) {
            groundLater |= c.id == 1;
            if (c.id == 5 && c.gone) bombGone = i == 40;
            if (c.id == 20 && i == 25) shotSpawned = true;
        }
        maxLater = std::max(maxLater, t.frames[i].changes.size());
    }
    check(!groundLater, "what never moves is never recorded again");
    check(maxLater <= 5, "later frames are sparse (4 movers + 1 removal at most)");
    check(bombGone, "the removed entity is marked gone in its frame");
    check(shotSpawned, "the spawned entity appears in its frame");
    check(t.spawns.size() == 1 && t.spawns[0].id == 20 &&
              t.spawns[0].json.find("\"shot\"") != std::string::npos,
          "the spawn is kept with its JSON");
    const auto& f0 = t.frames[0];
    bool skin = false;
    for (const auto& c : f0.changes) skin |= c.id == 4 && c.pose.skin == 1 && c.pose.clip == 1;
    check(skin, "a figure's clip is recorded");
}

// --- 2. The file -----------------------------------------------------------------
bool sameTake(const gamerec::Take& a, const gamerec::Take& b) {
    if (a.scene != b.scene || a.frames.size() != b.frames.size() || a.spawns.size() != b.spawns.size())
        return false;
    for (std::size_t i = 0; i < a.spawns.size(); ++i)
        if (a.spawns[i].id != b.spawns[i].id || a.spawns[i].json != b.spawns[i].json) return false;
    for (std::size_t i = 0; i < a.frames.size(); ++i) {
        const auto &fa = a.frames[i], &fb = b.frames[i];
        if (fa.t != fb.t || fa.cam.pos != fb.cam.pos || fa.cam.front != fb.cam.front ||
            fa.cam.up != fb.cam.up || fa.cam.fov != fb.cam.fov || fa.timeOfDay != fb.timeOfDay ||
            fa.storm != fb.storm || fa.changes.size() != fb.changes.size())
            return false;
        for (std::size_t k = 0; k < fa.changes.size(); ++k) {
            const auto &ca = fa.changes[k], &cb = fb.changes[k];
            if (ca.id != cb.id || ca.gone != cb.gone || (!ca.gone && !(ca.pose == cb.pose)))
                return false;
        }
    }
    return true;
}

void checkFile(const gamerec::Take& t, const fs::path& dir) {
    const std::string path = (dir / "check.frec").string();
    std::string err;
    check(gamerec::save(t, path, err), "save: " + err);
    gamerec::Take back;
    check(gamerec::load(back, path, err), "load: " + err);
    check(sameTake(t, back), "a take survives save/load field for field");

    // Truncated: refused, and the take handed in is left alone.
    {
        std::ifstream in(path, std::ios::binary);
        std::vector<char> bytes((std::istreambuf_iterator<char>(in)), {});
        const std::string cut = (dir / "cut.frec").string();
        std::ofstream(cut, std::ios::binary).write(bytes.data(), bytes.size() / 2);
        gamerec::Take keep = back;
        check(!gamerec::load(keep, cut, err) && sameTake(keep, back), "a truncated take is refused");
    }
    {
        const std::string junk = (dir / "junk.frec").string();
        std::ofstream(junk, std::ios::binary) << "not a recording at all";
        gamerec::Take keep;
        check(!gamerec::load(keep, junk, err) && keep.empty(), "a foreign file is refused");
    }
}

// --- 3. Sampling -----------------------------------------------------------------
void checkSampling(const gamerec::Take& t) {
    gamerec::Cursor cur;
    cur.reset(&t);
    gamerec::State st;
    gamerec::CamState cam;
    float tod = 0, storm = 0;

    // Exactly at frame 60, then between 60 and 61, then back to 10.
    cur.sample(t.frames[60].t, st, cam, tod, storm);
    check(st.count(2) && near(st[2].pos.x, 3.0f * 60 * kDt), "at a frame: that frame's pose");
    check(near(cam.pos.x, 3.0f * 60 * kDt) && near(tod, 12.0f + 60 * kDt), "at a frame: its eye and clock");
    check(!st.count(5), "the removed entity is gone after its frame");
    check(st.count(20), "the spawned entity is there after its frame");

    const double mid = 0.5 * (t.frames[60].t + t.frames[61].t);
    cur.sample(mid, st, cam, tod, storm);
    check(near(st[2].pos.x, 3.0f * 60.5f * kDt), "between frames: the blend");
    check(near(st[4].time, 1.5f * 60.5f * kDt), "between frames: a clip clock blends too");
    check(near(cam.pos.x, 3.0f * 60.5f * kDt), "between frames: the eye blends");

    cur.sample(t.frames[10].t, st, cam, tod, storm);
    check(near(st[2].pos.x, 3.0f * 10 * kDt) && st.count(5) && !st.count(20),
          "backwards: the state of the earlier frame");
    cur.sample(t.frames[119].t, st, cam, tod, storm);
    check(near(st[2].pos.x, 3.0f * 119 * kDt), "forwards past a cached state");
    cur.sample(1e9, st, cam, tod, storm);
    check(near(st[2].pos.x, 3.0f * 119 * kDt), "past the end: the last frame");
    cur.sample(-5.0, st, cam, tod, storm);
    check(near(st[2].pos.x, 0.0f), "before the start: the first frame");

    // Rotation the short way: 170 -> -170 about Y passes 180, not 0.
    gamerec::Pose a, b;
    a.rot = {0, 170, 0};
    b.rot = {0, -170, 0};
    const gamerec::Pose m = gamerec::lerp(a, b, 0.5f);
    check(sameRotation(m.rot, {0, 180, 0}, 1e-2f), "a rotation blends the short way round");
    // Two triples for one orientation: no spin in between.
    a.rot = {180, 30, 180};
    b.rot = {0, 150, 0};
    check(sameRotation(a.rot, b.rot), "(the two triples are one orientation)");
    check(sameRotation(gamerec::lerp(a, b, 0.5f).rot, b.rot, 1e-2f),
          "an Euler flip between frames does not spin the object");
    // A jump is a jump.
    a = {};
    b = {};
    b.pos = {100, 0, 0};
    check(gamerec::lerp(a, b, 0.3f).pos == a.pos && gamerec::lerp(a, b, 0.7f).pos == b.pos,
          "a teleport is not smeared across the frame between");
}

// --- 4. Replay over a scene --------------------------------------------------------
struct Scene {
    std::vector<Entity> ents;
    fitzel::Camera cam{glm::vec3(50, 50, 50), 30.0f, -20.0f};
    float tod = 7.0f, storm = 0.4f;
    int nextId = 100;
    double wall = 0.0;

    gamerec::Host host() {
        return gamerec::Host{
            ents, cam, tod, storm, [this] { return wall; }, [] { return std::string("check.fitzel"); },
            [] { return std::string(); }, spawnJson,
            [](const std::string& js, Entity& out) {
                const auto at = js.find("\"name\":\"");
                if (at == std::string::npos) return false;
                out.name = js.substr(at + 8, js.find('"', at + 8) - at - 8);
                return true;
            },
            [this] { return nextId++; }};
    }
};

int indexOf(const std::vector<Entity>& ents, const std::string& name) {
    for (int i = 0; i < static_cast<int>(ents.size()); ++i)
        if (ents[i].name == name) return i;
    return -1;
}

void checkReplay(const gamerec::Take& take, bool collide) {
    const std::string tag = collide ? " (spawn id taken)" : "";
    Scene sc;
    Game start;   // the scene as it stands in the editor: the game's frame 0
    sc.ents = start.ents;
    if (collide) sc.ents.push_back(makeEntity(20, "added later", {9, 9, 9}));
    const std::vector<Entity> before = sc.ents;

    gamerec::Session ss(sc.host());
    gamerec::Take copy = take;
    std::string err;
    const fs::path tmp = fs::temp_directory_path() / "recordercheck_replay.frec";
    gamerec::save(copy, tmp.string(), err);
    check(ss.loadTake(tmp.string()), "the session loads a take" + tag);
    check(ss.beginReplay(), "a replay starts" + tag);
    check(sc.ents.size() == before.size() + 1, "the spawned entity is put back" + tag);
    const int shot = indexOf(sc.ents, "shot");
    check(shot >= 0, "...under its name" + tag);
    if (shot < 0) return;
    check(collide ? sc.ents[shot].id != 20 : sc.ents[shot].id == 20,
          collide ? "...under a fresh id when its own is taken" : "...under its own id");

    ss.head = take.frames[10].t;
    ss.apply();
    ss.applySkins();
    const int crate = indexOf(sc.ents, "crate"), bomb = indexOf(sc.ents, "bomb"),
              runner = indexOf(sc.ents, "runner");
    check(near(sc.ents[crate].localCenter.x, 3.0f * 10 * kDt), "replay: the pose of the moment" + tag);
    check(!sc.ents[shot].active, "replay: the spawn is hidden before its moment" + tag);
    check(sc.ents[bomb].active, "replay: the bomb is there before its removal" + tag);
    check(near(sc.cam.position().x, 3.0f * 10 * kDt) && near(sc.tod, 12.0f + 10 * kDt),
          "replay: the eye and the clock" + tag);
    const auto* ac = sc.ents[runner].components.get<AnimationComponent>();
    check(ac && near(ac->time, 1.5f * 10 * kDt) && !ac->playing && ac->started,
          "replay: the figure's clock, held" + tag);

    ss.head = take.frames[80].t;
    ss.apply();
    check(sc.ents[shot].active && near(sc.ents[shot].localCenter.y, 1.0f + 10.0f * 55 * kDt),
          "replay: the spawn flies after its moment" + tag);
    check(!sc.ents[bomb].active, "replay: the bomb is hidden after its removal" + tag);
    if (collide) {
        const int added = indexOf(sc.ents, "added later");
        check(added >= 0 && sc.ents[added].localCenter == glm::vec3(9, 9, 9),
              "replay: an entity that only shares the spawn's id is left alone");
    }

    // Free camera: the editor's eye stays where it is put.
    ss.freeCam = true;
    sc.cam.setPosition({1, 2, 3});
    ss.apply();
    check(sc.cam.position() == glm::vec3(1, 2, 3), "replay: a free camera is not taken over" + tag);
    ss.freeCam = false;

    // Ticking runs the playhead, pauses at the end, or loops.
    ss.head = 0.0;
    ss.speed = 2.0f;
    ss.tick(0.5f);
    check(near(static_cast<float>(ss.head), 1.0f), "replay: the playhead runs at its speed" + tag);
    ss.tick(10.0f);
    check(ss.paused && ss.head == take.duration(), "replay: it stops at the end" + tag);
    ss.paused = false;
    ss.loop = true;
    ss.head = 0.0;
    ss.speed = 1.0f;
    ss.tick(static_cast<float>(take.duration()) + 0.25f);
    check(!ss.paused && near(static_cast<float>(ss.head), 0.25f), "replay: it loops" + tag);

    ss.endReplay();
    bool same = sc.ents.size() == before.size();
    for (std::size_t i = 0; same && i < before.size(); ++i)
        same = sc.ents[i].id == before[i].id && sc.ents[i].localCenter == before[i].localCenter &&
               sc.ents[i].localRotation == before[i].localRotation &&
               sc.ents[i].active == before[i].active;
    check(same, "the scene is put back when the replay ends" + tag);
    check(sc.cam.position() == glm::vec3(50, 50, 50) && near(sc.cam.yaw(), 30.0f) && sc.tod == 7.0f &&
              sc.storm == 0.4f,
          "...and the eye, the clock and the weather" + tag);
    fs::remove(tmp);
}

// --- 5. Video ---------------------------------------------------------------------
std::uint32_t rd32(const std::vector<unsigned char>& b, std::size_t at) {
    return b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (std::uint32_t(b[at + 3]) << 24);
}

// A test card: red across, green up, blue by frame. Bottom-up like glReadPixels.
std::vector<unsigned char> card(int w, int h, int frame) {
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<unsigned char>(x * 255 / (w - 1));
            p[1] = static_cast<unsigned char>(y * 255 / (h - 1));
            p[2] = static_cast<unsigned char>((frame * 37) & 255);
            p[3] = 255;
        }
    return px;
}

void checkAvi(const fs::path& dir) {
    const int W = 65, H = 49, N = 20;   // odd on purpose: written 64x48
    const std::string path = (dir / "check.avi").string();
    videoout::Writer w;
    std::string err;
    check(w.open(path, W, H, 30.0, videoout::Format::Avi, 90, err), "AVI opens: " + err);
    for (int f = 0; f < N; ++f) w.frame(card(W, H, f).data());
    std::string msg;
    check(w.close(msg), "AVI closes: " + msg);
    check(w.width() == 64 && w.height() == 48, "an odd size is cropped even");

    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> b((std::istreambuf_iterator<char>(in)), {});
    check(b.size() > 224 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(&b[8], "AVI ", 4) == 0,
          "AVI: a RIFF AVI file");
    if (b.size() <= 224) return;
    check(rd32(b, 4) == b.size() - 8, "AVI: the RIFF size is the file's");
    check(rd32(b, 48) == N && rd32(b, 140) == N, "AVI: the frame count, in both headers");
    check(rd32(b, 64) == 64 && rd32(b, 68) == 48, "AVI: the size in the header");
    check(std::memcmp(&b[220], "movi", 4) == 0, "AVI: the frames list where the header says");
    const std::size_t idx = 220 + rd32(b, 216);
    check(idx + 8 <= b.size() && std::memcmp(&b[idx], "idx1", 4) == 0 && rd32(b, idx + 4) == N * 16u,
          "AVI: an index with one entry per frame");
    if (idx + 8 + N * 16 > b.size()) return;
    bool chunksOk = true;
    for (int f = 0; f < N; ++f) {
        const std::size_t e = idx + 8 + f * 16;
        const std::size_t at = 220 + rd32(b, e + 8);
        chunksOk = chunksOk && std::memcmp(&b[at], "00dc", 4) == 0 && rd32(b, at + 4) == rd32(b, e + 12);
    }
    check(chunksOk, "AVI: every index entry points at its frame");

    // The first frame decodes to the card, top-down: top-left is (0, green max).
    const std::size_t at = 220 + rd32(b, idx + 8 + 8);
    int jw = 0, jh = 0, jc = 0;
    unsigned char* img = stbi_load_from_memory(&b[at + 8], static_cast<int>(rd32(b, at + 4)), &jw, &jh, &jc, 3);
    check(img && jw == 64 && jh == 48, "AVI: the frame is a 64x48 JPEG");
    if (img) {
        auto px = [&](int x, int y) { return img + (static_cast<std::size_t>(y) * jw + x) * 3; };
        check(px(0, 0)[0] < 20 && px(0, 0)[1] > 230, "AVI: top-left is the card's top-left (not flipped)");
        check(px(63, 47)[0] > 230 && px(63, 47)[1] < 30, "AVI: bottom-right likewise");
        stbi_image_free(img);
    }
}

void checkExport(const gamerec::Take& take, const fs::path& dir) {
    Scene sc;
    Game start;
    sc.ents = start.ents;
    gamerec::Session ss(sc.host());
    const fs::path tmp = dir / "export.frec";
    std::string err;
    gamerec::save(take, tmp.string(), err);
    ss.loadTake(tmp.string());
    ss.exportFormat = videoout::Format::Avi;
    ss.exportFps = 30;
    const std::string out = (dir / "export.avi").string();
    check(ss.beginExport(out), "an export starts");
    check(ss.replaying(), "...as a replay");

    // The host's frame loop, as main runs it.
    double last = ss.clock.now(sc.wall);
    bool fixedDt = true;
    int rendered = 0;
    while (ss.exporting() && rendered < 10000) {
        sc.wall += 0.37;   // a slow machine: the real frame takes far longer
        const double now = ss.clock.now(sc.wall);
        fixedDt = fixedDt && near(static_cast<float>(now - last), 1.0f / 30.0f, 1e-6f);
        last = now;
        ss.apply();
        const std::vector<unsigned char> px = card(80, 60, rendered);
        ss.exportFrame(px.data(), 80, 60);
        ++rendered;
    }
    check(fixedDt, "an export runs on a fixed 1/fps clock");
    const int expect = static_cast<int>(std::floor(take.duration() * 30.0)) + 1;
    check(!ss.exporting() && fs::exists(out), "the export finishes: " + ss.status);
    check(ss.status.find(std::to_string(expect) + " frames") != std::string::npos,
          "the video holds the take at 30 fps (" + std::to_string(expect) + " frames): " + ss.status);
    check(!ss.replaying(), "the export's replay ends with it");
    const double after = ss.clock.now(sc.wall + 0.01);
    check(after > last, "the clock runs on forwards after an export");

    // A view that changes size stops it, and the half file goes.
    ss.beginExport((dir / "resized.avi").string());
    for (int i = 0; i < 20 && ss.exporting(); ++i) {
        const int w = i < 15 ? 80 : 120;
        const std::vector<unsigned char> px = card(w, 60, i);
        ss.exportFrame(px.data(), w, 60);
    }
    check(!ss.exporting() && ss.status.find("size") != std::string::npos &&
              !fs::exists(dir / "resized.avi"),
          "a resize stops the export and removes the partial file");

    if (videoout::ffmpegAvailable()) {
        videoout::Writer w;
        const std::string mp4 = (dir / "check.mp4").string();
        check(w.open(mp4, 64, 48, 30.0, videoout::Format::Mp4, 80, err), "MP4 opens: " + err);
        for (int f = 0; f < 30; ++f) w.frame(card(64, 48, f).data());
        std::string msg;
        check(w.close(msg) && fs::file_size(mp4) > 1000, "MP4 written: " + msg);
    } else {
        std::printf("note  no ffmpeg on this machine: MP4 not checked\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    fs::path out = fs::temp_directory_path() / "recordercheck";
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--out") out = argv[i + 1];
    fs::create_directories(out);

    const gamerec::Take take = recordGame();
    checkRecording(take);
    checkFile(take, out);
    checkSampling(take);
    checkReplay(take, false);
    checkReplay(take, true);
    checkAvi(out);
    checkExport(take, out);

    std::printf("%s  %d passed, %d failed  (files in %s)\n", failures ? "FAIL" : "OK", passes, failures,
                out.string().c_str());
    return failures ? 1 : 0;
}
