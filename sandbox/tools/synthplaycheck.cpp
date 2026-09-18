// The synth check, the game's half: does a Synth component on an object play
// what a script tells it to?
//
// synthcheck measures the engine -- the patch graph, the MIDI reader, the voices
// -- offline. What it cannot reach is the chain a game actually uses: a patch
// and a song saved as files in a project, a Synth component naming them, the
// SynthSystem finding those files, and a Lua script calling synth.noteOn and
// synth.playMidi on the object's id. Every link of that chain is a name matched
// against a name, which is exactly where a feature works in the test and does
// nothing in the game. So this builds a throwaway project on disk and runs the
// real ScriptSystem against it, with a real audio device.
//
//   build/release/bin/synthplaycheck.exe
// Exits non-zero if any check fails; skips (exit 0) without an audio device.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/asset/Vfs.hpp>
#include <fitzel/audio/Synth.hpp>

#include "Component.hpp"
#include "Document.hpp"
#include "ScriptHost.hpp"
#include "ScriptSystem.hpp"
#include "SynthSystem.hpp"

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

fitzel::synth::ModuleDef module(int id, fitzel::synth::ModuleType t,
                                const std::string& mode = {}) {
    fitzel::synth::ModuleDef m;
    m.id   = id;
    m.type = t;
    m.mode = mode;
    return m;
}

// pitch -> Note -> Osc, gate -> ADSR -> the Osc's level: the Poly convention.
fitzel::synth::Patch keysPatch() {
    using namespace fitzel::synth;
    Patch p;
    p.name = "keys";
    p.inputs.push_back({"pitch", 0.0f, 127.0f, 60.0f});
    p.inputs.push_back({"gate", 0.0f, 1.0f, 0.0f});
    p.inputs.push_back({"bright", 0.0f, 1.0f, 0.5f});   // a dial for synth.set
    ModuleDef pitch = module(1, ModuleType::Param, "pitch");
    pitch.setParam("smooth", 0.0f);
    ModuleDef gate = module(2, ModuleType::Param, "gate");
    gate.setParam("smooth", 0.0f);
    ModuleDef env = module(4, ModuleType::Adsr);
    env.setParam("attack", 0.002f);
    env.setParam("sustain", 1.0f);
    env.setParam("release", 0.05f);
    ModuleDef osc = module(5, ModuleType::Osc, "sine");
    osc.setParam("freq", 0.0f);
    p.modules = {pitch, gate, module(3, ModuleType::Note), env, osc,
                 module(6, ModuleType::Out), module(7, ModuleType::Param, "bright")};
    p.wires   = {{1, 3, 0}, {3, 5, 0}, {2, 4, 0}, {4, 5, 1}, {5, 6, 0}};
    return p;
}

// Two quarter notes at 120 BPM: C4 then E4. One second long.
std::vector<char> songBytes() {
    return {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, (char)0xE0,
            'M', 'T', 'r', 'k', 0, 0, 0, 19,
            0x00, (char)0x90, 60, 100,
            (char)0x83, 0x60, 60, 0,
            0x00, 64, 100,
            (char)0x83, 0x60, 64, 0,
            0x00, (char)0xFF, 0x2F, 0x00};
}

void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

} // namespace

int main() {
    std::printf("synthplaycheck -- a Synth component, a project and a script\n");
    fitzel::Audio audio;
    if (!audio.ok()) {
        std::printf("  ..   no audio device; skipped\n");
        return 0;
    }

    // A project on disk, laid out the way the editor saves one.
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "fitzel-synthplaycheck";
    std::filesystem::remove_all(root);
    const std::string project = (root / "game.fitzel").generic_string();
    writeFile(root / "game.fitzel", "{}");
    std::filesystem::create_directories(root / "content/patches");   // as the panel does
    check(keysPatch().save((root / "content/patches/keys.json").generic_string()),
          "a patch saved where the Synth panel saves them");
    const std::vector<char> song = songBytes();
    writeFile(root / "content/midi/tune.mid", std::string(song.begin(), song.end()));
    writeFile(root / "scripts/band.lua", R"(
        started = false
        function start(e)
            local played  = synth.noteOn(e.id, "C4")
            local dialled = synth.set(e.id, "bright", 0.9)
            local nodial  = synth.set(e.id, "nonsense", 1)
            game.log(string.format("start %s %s %s %s %s %s", tostring(synth.note("A4")),
                tostring(synth.note("F#3")), tostring(synth.note("H2")),
                tostring(played), tostring(dialled), tostring(nodial)))
        end
        function update(e, dt, t)
            if t > 0.3 and not started then
                synth.noteOff(e.id)
                started = synth.playMidi(e.id, "tune", false)
                if not synth.isPlaying(e.id) then game.log("not playing") end
            end
        end
    )");

    Document doc;
    Entity   e;
    e.id     = 7;
    e.center = glm::vec3(0.0f);
    auto sc  = std::make_unique<SynthComponent>();
    sc->patch       = "keys";
    sc->playOnStart = false;
    sc->volume      = 0.3f;
    e.components.items.push_back(std::move(sc));
    doc.entities().push_back(std::move(e));

    SynthSystem synths;
    synths.bind(audio, doc, project);
    check(fitzel::vfs::exists(synths.resolvePatch("keys")),
          "a patch named without its extension is found");
    check(!synths.play(99) && !synths.lastError().empty(),
          "an object without a Synth is refused, with a reason");

    ScriptHost host;
    host.synths = &synths;
    std::vector<std::string> said;
    host.log = [&](const std::string& s) { said.push_back(s); };
    ScriptSystem scripts;
    scripts.setHost(&host);
    ScriptComponent script;
    script.file = "band.lua";
    const std::string scriptPath = (root / "scripts/band.lua").generic_string();

    Entity& obj = doc.entities().front();
    float   t   = 0.0f;
    auto tick = [&](float seconds) {
        for (float end = t + seconds; t < end; t += 1.0f / 60.0f) {
            scripts.update(obj, script, scriptPath, 1.0f / 60.0f, t);
            synths.update(glm::vec3(0.0f), 1.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    };

    tick(0.2f);
    check(scripts.lastError().empty(), "the script runs without an error");
    if (!scripts.lastError().empty()) std::printf("       (%s)\n", scripts.lastError().c_str());
    check(synths.running(7), "synth.noteOn from a script starts the object's synth");
    const std::string first = said.empty() ? std::string() : said.front();
    check(first.rfind("start 69 54 nil", 0) == 0, "synth.note reads note names (A4 = 69, F#3 = 54)");
    check(first.find("true true false") != std::string::npos,
          "noteOn and set answer true, an unknown dial answers false");
    if (!first.empty()) std::printf("       (%s)\n", first.c_str());

    tick(0.4f);
    check(synths.midiPlaying(7), "synth.playMidi finds the song under content/midi and plays it");
    tick(1.2f);
    check(!synths.midiPlaying(7), "...and it ends, because the script said not to loop");

    // The component's own "play on start" path, the one Play uses.
    SynthComponent* comp = doc.entities().front().components.get<SynthComponent>();
    comp->midi     = "tune.mid";
    comp->loopMidi = true;
    check(synths.play(7, true) && synths.midiPlaying(7), "play() starts the component's song");
    tick(1.3f);
    check(synths.midiPlaying(7), "...and a looping song is still going after its end");

    comp->patch = "missing";
    check(!synths.play(7, true) && synths.lastError().find("missing") != std::string::npos,
          "a patch that is not there is named in the error");
    check(!synths.noteOn(7, 60, 1.0f) && !synths.noteOn(7, 62, 1.0f),
          "...and asked again every frame, it stays refused without asking the disk");

    doc.entities().clear();
    synths.update(glm::vec3(0.0f), 1.0f);
    check(!synths.running(7), "an object that is gone takes its sound with it");

    std::filesystem::remove_all(root);
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
