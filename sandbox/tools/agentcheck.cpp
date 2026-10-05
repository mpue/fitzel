// The town-agents check: town_agents.lua (or any script given) running headless
// against a pretend town and the real language model.
//
// A ScriptSystem is built as Play builds it; the host answers the town calls
// with a small grid town of its own (homes, a school, a police station, offices,
// a park, a stop) and walks figures straight along the "paths" it hands out.
// Wall time runs at about five times game time, so a minute here is five in
// the game. Everything the script logs is printed, and the check fails when:
//   * the script errors;
//   * no figure was spawned, or none ever walked anywhere;
//   * a frame waited for the model;
//   * with Ollama running, no decision came from the model (only fallbacks);
//   * the chat (T, typing, Enter, Tab to a person) does not send what was typed,
//     or -- with Ollama -- the town or the person does not answer.
// Without Ollama the model part is SKIPPED; the figures must still walk.
//
//   build/release/bin/agentcheck.exe [script.lua] [seconds of wall time, default 60]
//                                    [--no-prefabs]   (a project without npc_* prefabs)
// Run from the repo root (default script: sandbox/scripts/town_agents.lua).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <glm/glm.hpp>

#include "Component.hpp"
#include "ScriptHost.hpp"
#include "ScriptSystem.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    const fs::path script = argc > 1 ? fs::path(argv[1]) : fs::path("sandbox/scripts/town_agents.lua");
    const double wallSecs = argc > 2 ? std::atof(argv[2]) : 60.0;
    if (!fs::exists(script)) {
        std::printf("FAIL no script at %s\n", script.generic_string().c_str());
        return 1;
    }

    // --- The pretend town: streets every 80 m, a 5 x 5 grid round the origin.
    std::vector<ScriptPlace> places;
    const char* streets[] = {"Lindenstraße", "Goethestraße", "Am Markt", "Bahnhofstraße",
                             "Schulweg", "Gartenstraße"};
    for (int i = 0; i <= 5; ++i) {
        const float x = -200.0f + i * 80.0f;
        for (int k = 0; k < 5; ++k) {
            const float z = -160.0f + k * 80.0f;
            for (int side = 0; side < 2; ++side) {
                ScriptPlace p;
                p.kind = (k + side) % 4 == 0 ? "flat" : "home";
                p.street = streets[i];
                p.number = k * 2 + side + 1;
                p.name = p.street + " " + std::to_string(p.number);
                p.pos = {x + (side ? 6.0f : -6.0f), 0.15f, z};
                p.at = {x + (side ? 14.0f : -14.0f), z};
                places.push_back(p);
            }
        }
    }
    auto civic = [&](const char* name, const char* kind, float x, float z) {
        ScriptPlace p;
        p.name = name; p.kind = kind; p.street = "Am Markt";
        p.pos = {x, 0.15f, z}; p.at = {x + 10.0f, z};
        places.push_back(p);
    };
    civic("Schule", "school", -40.0f, -40.0f);
    civic("Polizeiwache", "police", 40.0f, 40.0f);
    civic("Rathaus", "townhall", 0.0f, 0.0f);
    civic("Stadtpark", "park", 120.0f, -40.0f);
    civic("Bibliothek", "library", -120.0f, 40.0f);
    civic("Krankenhaus", "hospital", 120.0f, 120.0f);
    civic("Bushaltestelle Am Markt", "stop", 6.0f, 20.0f);
    for (int i = 0; i < 3; ++i) {
        ScriptPlace p;
        p.name = "Bahnhofstraße " + std::to_string(40 + i);
        p.kind = i == 2 ? "works" : "office";
        p.street = "Bahnhofstraße";
        p.pos = {40.0f + i * 20.0f, 0.15f, -120.0f};
        p.at = {40.0f + i * 20.0f, -130.0f};
        places.push_back(p);
    }

    struct Fig { glm::vec3 pos{0.0f}; glm::vec3 rot{0.0f}; bool visible = false; float walked = 0.0f; };
    std::map<int, Fig> figs;
    int nextId = 100;
    std::vector<std::string> lines;

    ScriptHost host;
    host.log = [&](const std::string& l) {
        lines.push_back(l);
        std::printf("  | %s\n", l.c_str());
    };
    host.camPos = {0.0f, 1.7f, 0.0f};
    host.townPlaces = [&] { return places; };
    host.townPath = [&](glm::vec2 a, glm::vec2 b) {
        std::vector<glm::vec3> out;
        const float d = glm::length(b - a);
        const int n = std::max(1, static_cast<int>(d / 8.0f));
        for (int i = 1; i <= n; ++i) {
            const glm::vec2 p = a + (b - a) * (static_cast<float>(i) / n);
            out.push_back({p.x, 0.15f, p.y});
        }
        return out;
    };
    host.streetAt = [&](glm::vec2, float, float& dist) { dist = 3.0f; return std::string("Am Markt"); };
    // --no-prefabs: a project without the npc_* prefabs -- every spawnPrefab
    // fails, and the script must fall back to stand-ins made with game.spawn.
    bool noPrefabs = false;
    for (int i = 1; i < argc; ++i) noPrefabs |= std::string(argv[i]) == "--no-prefabs";
    int standIns = 0;
    host.spawnPrefab = [&](const std::string&, glm::vec3 pos, float yaw) {
        if (noPrefabs) return 0;
        const int id = nextId++;
        figs[id] = {pos, {0.0f, yaw, 0.0f}, false, 0.0f};
        return id;
    };
    host.spawn = [&](const ScriptSpawn& s) {
        const int id = nextId++;
        figs[id] = {s.pos, {0.0f, 0.0f, 0.0f}, false, 0.0f};
        ++standIns;
        return id;
    };
    host.getPos = [&](int id, glm::vec3& out) {
        auto it = figs.find(id);
        if (it == figs.end() || !it->second.visible) return false;
        out = it->second.pos;
        return true;
    };
    host.setPos = [&](int id, glm::vec3 p) { if (figs.count(id)) figs[id].pos = p; };
    host.setRot = [&](int id, glm::vec3 r) { if (figs.count(id)) figs[id].rot = r; };
    host.moveCharacter = [&](int id, glm::vec2 v, float dt, glm::vec3& foot, bool& ground,
                             bool& terrain) {
        auto it = figs.find(id);
        if (it == figs.end() || !it->second.visible) return false;
        it->second.pos.x += v.x * dt;
        it->second.pos.z += v.y * dt;
        it->second.walked += glm::length(v) * dt;
        foot = {it->second.pos.x, 0.15f, it->second.pos.z};
        ground = true;
        terrain = false;
        return true;
    };
    // The player at the keyboard: a few frames of typing into the chat (T opens
    // it, a question to the town, Enter; later Tab to a person, a question,
    // Enter, Esc). Keys pressed and text typed per frame.
    std::map<int, std::vector<int>> keysAt;
    std::map<int, std::string>      typedAt;
    const int kT = 'T', kEnter = 257, kTab = 258, kEsc = 256;
    keysAt[300] = {kT};
    typedAt[301] = "t";   // the T that opened it, as the window sees it -- must be dropped
    typedAt[302] = "Wer ist gerade wo?";
    keysAt[303] = {kEnter};
    keysAt[900] = {kTab};
    typedAt[901] = "Hallo! Kommst du mit in den Stadtpark?";
    keysAt[902] = {kEnter};
    keysAt[1400] = {kEsc};
    // The free camera: K takes the eye, 1 flies to the first person and follows,
    // K gives the eye back.
    keysAt[1500] = {'K'};
    keysAt[1510] = {'1'};
    keysAt[1800] = {'K'};
    // Something for everybody: "/alle" in the chat sets it, and everybody
    // thinks again about what to do now.
    keysAt[1850] = {kT};
    typedAt[1852] = "/alle Heute ist Stadtfest im Stadtpark, alle wollen unbedingt hin.";
    keysAt[1853] = {kEnter};
    keysAt[1860] = {kEsc};
    int frame = 0;
    glm::vec3 camAt(0.0f);
    int camSets = 0, tookEye = 0, gaveBack = 0;
    float followGap = 1e9f;
    host.setCamPos = [&](glm::vec3 p) { camAt = p; ++camSets; };
    host.setCamDir = [](glm::vec3) {};
    host.setActiveCamera = [&](int id) { (id < 0 && frame < 1600 ? tookEye : gaveBack) += 1; };
    host.keyPressed = [&](int kc) {
        const auto it = keysAt.find(frame);
        return it != keysAt.end() &&
               std::find(it->second.begin(), it->second.end(), kc) != it->second.end();
    };
    host.keyDown = [&](int) { return false; };
    host.textInput = [&] {
        const auto it = typedAt.find(frame);
        return it != typedAt.end() ? it->second : std::string();
    };
    host.worldToHud = [](glm::vec3, glm::vec2& out) { out = {960.0f, 540.0f}; return true; };
    host.measureText = [](const std::string& s, float size, bool) {
        return glm::vec2(static_cast<float>(s.size()) * size * 0.5f, size);
    };

    ScriptSystem scripts;
    scripts.setHost(&host);
    ScriptComponent sc;
    sc.file = script.filename().generic_string();
    Entity e;
    e.id = 1;

    const auto t0 = std::chrono::steady_clock::now();
    float t = 0.0f;
    const float dt = 0.1f;
    int frames = 0;
    double worstFrameMs = 0.0;
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < wallSecs) {
        const auto f0 = std::chrono::steady_clock::now();
        host.hudCmds.clear();
        frame = frames;
        scripts.update(e, sc, script.generic_string(), dt, t);
        worstFrameMs = std::max(worstFrameMs, std::chrono::duration<double, std::milli>(
                                                  std::chrono::steady_clock::now() - f0).count());
        if (!scripts.lastError().empty()) {
            std::printf("FAIL script error: %s\n", scripts.lastError().c_str());
            return 1;
        }
        for (auto& [id, f] : figs) f.visible = true;   // spawned: there next frame
        if (frame == 1790 && !figs.empty()) {
            // Following the first person (the first spawned): close behind it.
            const glm::vec3 p = figs.begin()->second.pos;
            followGap = glm::length(glm::vec2(camAt.x - p.x, camAt.z - p.z));
        }
        t += dt;
        ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    int decisions = 0, fallbacks = 0, meetings = 0, said = 0, noServer = 0;
    bool askedTown = false, townAnswered = false, askedPerson = false, personAnswered = false;
    bool contextSet = false;
    int afterContext = 0, toTheFete = 0;
    for (const std::string& l : lines) {
        if (l.find("Für alle: Heute ist Stadtfest") != std::string::npos) contextSet = true;
        if (contextSet && l.find(": -> ") != std::string::npos) {
            ++afterContext;
            toTheFete += l.find(": -> Stadtpark") != std::string::npos ? 1 : 0;
        }
        if (l.find("Chat Alex -> Stadt: Wer ist gerade wo?") != std::string::npos) askedTown = true;
        else if (l.find("Chat Alex -> ") != std::string::npos &&
                 l.find("Kommst du mit") != std::string::npos) askedPerson = true;
        else if (l.find("Chat Stadt: ") != std::string::npos &&
                 l.find("weiß ich gerade nicht") == std::string::npos && l.size() > 40)
            townAnswered = true;
        else if (l.find("\tChat ") != std::string::npos) personAnswered = true;
        if (l.find(": -> ") != std::string::npos) ++decisions;
        if (l.find("unbrauchbare Antwort") != std::string::npos) ++fallbacks;
        if (l.find(" trifft ") != std::string::npos) ++meetings;
        if (l.find("\t   ") != std::string::npos) ++said;   // a dialogue line, indented
        if (l.find("no model server") != std::string::npos) ++noServer;
    }
    int walkers = 0;
    float walked = 0.0f;
    for (const auto& [id, f] : figs) {
        walkers += f.walked > 5.0f ? 1 : 0;
        walked += f.walked;
    }
    std::printf("\n%d frames (%.0f s game time), worst frame %.1f ms\n", frames, t, worstFrameMs);
    std::printf("%zu figures, %d walked somewhere (%.0f m in all)\n", figs.size(), walkers, walked);
    std::printf("%d model decisions, %d unusable answers, %d meetings, %d lines spoken\n",
                decisions, fallbacks, meetings, said);
    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++failures;
    };
    expect(!figs.empty(), "figures spawned");
    if (noPrefabs) expect(standIns > 0 && standIns == static_cast<int>(figs.size()),
                          "without the prefabs, every person is a stand-in");
    expect(walkers > 0, "figures walk to places");
    expect(worstFrameMs < 50.0, "no frame waits for the model");
    if (frames > 1800) {
        expect(tookEye > 0 && camSets > 200, "K takes the camera and flies it");
        expect(followGap < 20.0f, "1 follows the first person");
        std::printf("       %.1f m behind the person it follows\n", followGap);
        expect(gaveBack > 0, "K again gives the camera back");
    }
    if (frames > 1860) {
        expect(contextSet, "/alle sets what holds for everybody");
        // How far the model follows it is the model's business: reported only.
        std::printf("       after it: %d of %d decisions go to the fete in the park\n",
                    toTheFete, afterContext);
    }
    if (wallSecs >= 40.0) {
        expect(askedTown, "the chat opens on T, takes typed text (not the T) and sends on Enter");
        expect(askedPerson, "Tab picks a person to write to");
    }
    if (noServer > 0) {
        std::printf("  SKIP no Ollama: the model part was not tested\n");
    } else {
        expect(decisions > 0, "the model makes decisions");
        if (wallSecs >= 40.0) {
            expect(townAnswered, "the town answers in the chat");
            expect(personAnswered, "the person answers in the chat");
        }
    }
    std::printf(failures ? "%d failure(s)\n" : "agent check passed\n", failures);
    return failures ? 1 : 0;
}
