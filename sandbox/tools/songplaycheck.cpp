// The song check, the game's half: does a Song Player component play the song
// the sequencer saved, and does a script steer it?
//
// songcheck measures the engine offline and songpanelcheck the window. What
// neither reaches is the chain a game uses: a song saved as a file in a
// project, a Song Player naming it, SongSystem finding it and playing it on a
// real device, and a Lua script calling song.* on the object's id. Every link
// of that chain is a name matched against a name -- so this builds a throwaway
// project on disk and runs the real ScriptSystem against it.
//
//   build/linux/bin/songplaycheck
// Exits non-zero if a check fails; skips (exit 0) without an audio device. You
// hear a few seconds of drums and organ, quietly.

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <fitzel/asset/Vfs.hpp>
#include <fitzel/audio/Audio.hpp>
#include <fitzel/audio/Mixer.hpp>
#include <fitzel/audio/Song.hpp>

#include "Component.hpp"
#include "Document.hpp"
#include "ScriptHost.hpp"
#include "ScriptSystem.hpp"
#include "SongSystem.hpp"

namespace sg = fitzel::song;

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const std::string& what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what.c_str());
}

void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

sg::Note note(std::int64_t tick, std::int64_t len, int pitch, int vel) {
    sg::Note n;
    n.tick = tick;
    n.length = len;
    n.pitch = pitch;
    n.velocity = vel;
    return n;
}

// Drums and an organ chord, `bars` long at `bpm`.
sg::Song groove(double bpm, int bars) {
    sg::Song s;
    s.name = "groove";
    s.bpm  = bpm;
    const std::int64_t bar = s.ticksPerBar();
    s.addTrack(sg::TrackKind::Instrument, "Drums").instrument.kind = sg::InstrumentKind::Drums;
    sg::Clip beat;
    beat.uid    = s.newUid();
    beat.length = bar * bars;
    beat.loop   = bar;
    for (int i = 0; i < 4; ++i) beat.notes.push_back(note(i * sg::kPpq, 100, i % 2 ? 38 : 36, 110));
    s.tracks[0].clips.push_back(beat);
    s.addTrack(sg::TrackKind::Instrument, "Organ").instrument.patch = "builtin:Organ";
    sg::Clip ch;
    ch.uid    = s.newUid();
    ch.length = bar * bars;
    for (int p : {57, 60, 64}) ch.notes.push_back(note(0, bar * bars - 120, p, 80));
    s.tracks[1].clips.push_back(ch);
    return s;
}

} // namespace

int main() {
    std::printf("songplaycheck -- a Song Player, a project and a script\n");
    fitzel::Audio audio;
    if (!audio.ok()) {
        std::printf("  ..   no audio device; skipped\n");
        return 0;
    }

    const std::filesystem::path root = std::filesystem::temp_directory_path() / "fitzel-songplaycheck";
    std::filesystem::remove_all(root);
    const std::string project = (root / "game.fitzel").generic_string();
    writeFile(root / "game.fitzel", "{}");
    std::filesystem::create_directories(root / "content/songs");   // as the sequencer does
    check(groove(120.0, 4).save((root / "content/songs/groove.json").generic_string()),
          "a song saved where the Song Sequencer saves them");
    check(groove(240.0, 1).save((root / "content/songs/sting.json").generic_string()), "...and a short one");
    writeFile(root / "scripts/conductor.lua", R"(
        asked = false
        function start(e)
            game.log("start " .. tostring(song.play(e.id)) .. " " .. tostring(song.isPlaying(e.id)))
        end
        function update(e, dt, t)
            if t > 0.8 and not asked then
                asked = true
                local bar, beat = song.bar(e.id)
                game.log(string.format("at %s %s %s %s %s %s", tostring(song.mute(e.id, "Drums", true)),
                    tostring(song.mute(e.id, "Nope", true)), tostring(song.volume(e.id, "Organ", 0.5)),
                    tostring(song.setTempo(e.id, 90)), tostring(bar), tostring(beat)))
                game.log("beat " .. tostring(song.beat(e.id)) .. " time " .. tostring(song.time(e.id)))
            end
        end
    )");

    Document doc;
    Entity   e;
    e.id     = 7;
    e.center = glm::vec3(0.0f);
    auto sp  = std::make_unique<SongPlayerComponent>();
    sp->song        = "groove";
    sp->playOnStart = false;
    sp->volume      = 0.25f;
    e.components.items.push_back(std::move(sp));
    doc.entities().push_back(std::move(e));

    SongSystem songs;
    songs.bind(audio, doc, project, [&](const std::string& n) { return (root / "content/sounds" / n).generic_string(); },
               [&](const std::string& n) { return (root / "content/patches" / (n + ".json")).generic_string(); });
    songs.route(audio.mixer(), [&](const std::string&) { return audio.mixer().master(); });
    check(fitzel::vfs::exists(songs.resolveSong("groove")), "a song named without its extension is found");
    check(!songs.play(99) && !songs.lastError().empty(), "an object without a Song Player is refused, with a reason");

    ScriptHost host;
    host.songs = &songs;
    std::vector<std::string> said;
    host.log = [&](const std::string& s) { said.push_back(s); };
    ScriptSystem scripts;
    scripts.setHost(&host);
    ScriptComponent script;
    script.file = "conductor.lua";
    const std::string scriptPath = (root / "scripts/conductor.lua").generic_string();

    Entity& obj = doc.entities().front();
    float   t   = 0.0f;
    float   peak = 0.0f;
    auto tick = [&](float seconds, bool runScript = true) {
        for (float end = t + seconds; t < end; t += 1.0f / 60.0f) {
            if (runScript) scripts.update(obj, script, scriptPath, 1.0f / 60.0f, t);
            songs.update(glm::vec3(0.0f), 1.0f);
            peak = std::max(peak, songs.takePeak(7));
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    };

    tick(0.5f);
    check(scripts.lastError().empty(), "the script runs without an error");
    if (!scripts.lastError().empty()) std::printf("       (%s)\n", scripts.lastError().c_str());
    check(!said.empty() && said.front() == "start true true", "song.play starts the object's song, song.isPlaying says so");
    check(peak > 0.05f, "...and it sounds (peak " + std::to_string(peak) + ")");
    tick(0.6f);
    std::string at, beat;
    for (const std::string& s : said) {
        if (s.rfind("at ", 0) == 0) at = s;
        if (s.rfind("beat ", 0) == 0) beat = s;
    }
    std::printf("       (%s / %s)\n", at.c_str(), beat.c_str());
    check(at.rfind("at true false true true 1 ", 0) == 0,
          "song.mute and song.volume find a track by name (and refuse one there is not), song.setTempo takes");
    // At 0.8 s and 120 BPM the song is past beat 1.6 of bar 1 -- with the
    // device's buffering, somewhere in beats 2 or 3.
    check(at.size() > 2 && (at.back() == '2' || at.back() == '2' || at.back() == '3'), "song.bar says bar 1, beat 2 or 3");
    const double b0 = songs.beats(7);
    tick(1.0f, false);
    const double b1 = songs.beats(7);
    check(std::fabs((b1 - b0) - 1.5) < 0.3, "at 90 BPM the song moves a beat and a half a second (" + std::to_string(b1 - b0) + ")");
    check(songs.seek(7, 8.0) && songs.beats(7) >= 7.9 && songs.beats(7) < 8.6, "song.seek moves it to a beat");

    // Looping: the whole song (16 beats) comes round, and plays on past its end.
    songs.seek(7, 14.0);
    tick(2.2f, false);
    check(songs.playing(7) && songs.beats(7) < 6.0, "a looping song comes round to its start (beat " + std::to_string(songs.beats(7)) + ")");

    // Not looping: once, then it stops by itself.
    SongPlayerComponent* comp = doc.entities().front().components.get<SongPlayerComponent>();
    comp->song = "sting";
    comp->loop = false;
    check(songs.play(7), "play() starts the component's song, as Play does");
    tick(1.6f, false);
    check(!songs.playing(7), "...and a song that does not loop stops at its end");

    comp->song = "missing";
    check(!songs.play(7, true) && songs.lastError().find("missing") != std::string::npos, "a song that is not there is named in the error");

    doc.entities().clear();
    songs.update(glm::vec3(0.0f), 1.0f);
    check(!songs.playing(7) && songs.beats(7) < 0.0, "an object that is gone takes its music with it");

    std::filesystem::remove_all(root);
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
