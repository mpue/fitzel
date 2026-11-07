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

#include <atomic>
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
#include <fitzel/audio/SongPlayer.hpp>

#include "Component.hpp"
#include "Document.hpp"
#include "ScriptHost.hpp"
#include "ScriptSystem.hpp"
#include "SongSystem.hpp"

namespace sg = fitzel::song;

namespace {

// An insert that keeps what goes through its strip (the left side), the way
// the real graph delivers it -- room made before, nothing allocated on the
// audio thread.
struct Tap final : fitzel::mixfx::External {
    std::vector<float>       left;
    std::atomic<std::size_t> used{0};
    explicit Tap(std::size_t frames) : left(frames) {}
    void process(float* buf, unsigned frames, unsigned channels) override {
        std::size_t at = used.load(std::memory_order_relaxed);
        for (unsigned f = 0; f < frames && at < left.size(); ++f) left[at++] = buf[f * channels];
        used.store(at, std::memory_order_relaxed);
    }
};

// Where the hits are: the windows (of 256) whose level jumps to three times
// the window before, a hit apart from the last by a tenth of a second at least.
std::vector<long> hits(const std::vector<float>& x, std::size_t n) {
    std::vector<long> out;
    const std::size_t w = 256;
    double prev = 1e-9;
    long last = -100000;
    for (std::size_t i = 0; i + w <= n; i += w) {
        double e = 0.0;
        for (std::size_t k = 0; k < w; ++k) e += static_cast<double>(x[i + k]) * x[i + k];
        e = std::sqrt(e / w);
        if (e > 0.01 && e > prev * 3.0 && static_cast<long>(i) - last > 4800) {
            out.push_back(static_cast<long>(i));
            last = static_cast<long>(i);
        }
        prev = std::max(e, 1e-9);
    }
    return out;
}

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
    songs.route(audio.mixer(), [&](const std::string&, const std::string&) { return audio.mixer().master(); });
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

    // A track in a mixer channel of its own: the drums through strip B, the
    // rest of the song through the component's strip A -- both sounding, each
    // only what it should, B's fader ruling the drums.
    {
        fitzel::Mixer& mx = audio.mixer();
        const int stripA = mx.addStrip(fitzel::Mixer::Kind::Channel);
        const int stripB = mx.addStrip(fitzel::Mixer::Kind::Channel);
        sg::Song routed = groove(120.0, 4);
        routed.tracks[0].output = "Song: Drums";
        routed.save((root / "content/songs/routed.json").generic_string());
        songs.route(mx, [&](const std::string& name, const std::string& fallback) {
            if (name == "Song: Drums") return stripB;
            if (name == "Lead") return stripA;
            return fallback == "Lead" ? stripA : mx.master();
        });
        comp->song    = "routed";
        comp->loop    = true;
        comp->channel = "Lead";
        check(songs.play(7, true), "a song with a track in a channel of its own starts");
        float a = 0.0f, b = 0.0f, l = 0.0f, r = 0.0f;
        for (int i = 0; i < 60; ++i) {
            tick(1.0f / 60.0f, false);
            mx.takePeak(stripA, l, r); a = std::max(a, std::max(l, r));
            mx.takePeak(stripB, l, r); b = std::max(b, std::max(l, r));
        }
        check(a > 0.02f && b > 0.02f, "its drums sound through their own strip, the organ through the song's (" +
                                          std::to_string(a) + ", " + std::to_string(b) + ")");
        mx.setGain(stripB, 0.0f);
        tick(0.1f, false);
        mx.takePeak(stripB, l, r);
        float a2 = 0.0f, b2 = 0.0f;
        for (int i = 0; i < 40; ++i) {
            tick(1.0f / 60.0f, false);
            mx.takePeak(stripA, l, r); a2 = std::max(a2, std::max(l, r));
            mx.takePeak(stripB, l, r); b2 = std::max(b2, std::max(l, r));
        }
        check(b2 < 1e-4f && a2 > 0.02f, "...and that strip's fader down silences the drums alone");
        mx.setGain(stripB, 1.0f);

        // ...and in time: the song runs at its own pace whether its drums go
        // to the master or into a subgroup (B into A, as the sequencer's "Song:
        // Drums" goes into Music) -- not faster, as if read twice a period.
        // With an aux bus on the desk, as every scene has its Reverb: each
        // channel's send outputs feed it, so a channel is read by two nodes.
        const int aux = mx.addStrip(fitzel::Mixer::Kind::Aux);
        for (int topology = 0; topology < 2; ++topology) {
            mx.setOutput(stripB, topology == 0 ? mx.master() : stripA);
            songs.seek(7, 0.0);
            tick(0.1f, false);
            const double s0 = songs.seconds(7);
            const auto w0 = std::chrono::steady_clock::now();
            float pb = 0.0f;
            for (int i = 0; i < 90; ++i) {
                tick(1.0f / 60.0f, false);
                mx.takePeak(stripB, l, r);
                pb = std::max(pb, std::max(l, r));
            }
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
            const double rate = (songs.seconds(7) - s0) / wall;
            check(std::fabs(rate - 1.0) < 0.06 && pb > 0.02f,
                  std::string(topology == 0 ? "drums in a channel to the master" : "drums in a subgroup of the song's channel") +
                      ": the song plays at its own pace (x" + std::to_string(rate) + "), the drums heard (" + std::to_string(pb) + ")");
        }
        // What the drums' strip actually gets, recorded in the graph, with the
        // desk as the editor has it: drums into a subgroup of the song's
        // channel, an aux bus on every channel's sends. Hits a beat apart, every
        // one -- a stream cut up or doubled would put them anywhere.
        {
            mx.setOutput(stripB, stripA);
            auto tap = std::make_shared<Tap>(static_cast<std::size_t>(48000 * 4));
            mx.setInserts(stripB, std::vector<fitzel::mixfx::Slot>{{fitzel::mixfx::Type::Plugin, tap}});
            songs.seek(7, 0.0);
            tick(2.5f, false);
            mx.setInserts(stripB, std::vector<fitzel::mixfx::Slot>{});
            const std::size_t got = tap->used.load();
            const auto at = hits(tap->left, got);
            int regular = 0;
            std::string gaps;
            for (std::size_t i = 1; i < at.size(); ++i) {
                const long d = at[i] - at[i - 1];
                if (std::labs(d - 24000) <= 512) ++regular;
                if (i < 8) gaps += " " + std::to_string(d);
            }
            check(got > 48000 && at.size() >= 4 && regular == static_cast<int>(at.size()) - 1,
                  "the drums' strip gets the drums whole: a hit every half second (" + std::to_string(at.size()) +
                      " hits, gaps" + gaps + ")");
        }
        // As in the editor: the player already running when the channel is
        // made -- the drums first on the master (the desk has not made the
        // strip yet), then moved to the new strip while the song plays. And the
        // song edited meanwhile (a new plan a frame), as the sequencer does.
        {
            fitzel::SongPlayer pre = fitzel::SongPlayer::create(audio);
            sg::Song song = groove(120.0, 4);
            pre.engine().setSong(song);
            pre.route(mx, stripA);
            pre.setVolume(0.3f);
            pre.start();
            pre.engine().play();
            tick(0.3f, false);
            song.tracks[0].output = "Song: Drums";
            pre.engine().setSong(song);
            pre.route(mx, stripA, [&](const std::string&) { return -1; });   // the strip not there yet
            tick(1.0f / 60.0f, false);
            const int late = mx.addStrip(fitzel::Mixer::Kind::Channel);
            mx.setOutput(late, stripA);
            auto tap = std::make_shared<Tap>(static_cast<std::size_t>(48000 * 4));
            mx.setInserts(late, std::vector<fitzel::mixfx::Slot>{{fitzel::mixfx::Type::Plugin, tap}});
            pre.route(mx, stripA, [&](const std::string&) { return late; });
            pre.engine().seek(0);
            const double s0 = pre.engine().positionSeconds();
            const auto w0 = std::chrono::steady_clock::now();
            for (int i = 0; i < 150; ++i) {
                if (i % 2 == 0) pre.engine().setSong(song);   // edits while it plays
                pre.route(mx, stripA, [&](const std::string&) { return late; });
                tick(1.0f / 60.0f, false);
            }
            const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
            const double rate = (pre.engine().positionSeconds() - s0) / wall;
            mx.setInserts(late, std::vector<fitzel::mixfx::Slot>{});
            const auto at = hits(tap->left, tap->used.load());
            int regular = 0;
            std::string gaps;
            for (std::size_t i = 1; i < at.size(); ++i) {
                const long d = at[i] - at[i - 1];
                if (std::labs(d - 24000) <= 512) ++regular;
                if (i < 8) gaps += " " + std::to_string(d);
            }
            check(std::fabs(rate - 1.0) < 0.06 && at.size() >= 4 && regular == static_cast<int>(at.size()) - 1,
                  "a running player moved onto a channel made meanwhile: in time (x" + std::to_string(rate) +
                      "), the drums whole (" + std::to_string(at.size()) + " hits, gaps" + gaps + ")");
            pre.stop();
            mx.removeStrip(late);
        }
        mx.setOutput(stripB, mx.master());
        mx.removeStrip(aux);
        songs.stop(7);
        comp->channel = "Music";
    }

    comp->song = "missing";
    check(!songs.play(7, true) && songs.lastError().find("missing") != std::string::npos, "a song that is not there is named in the error");

    doc.entities().clear();
    songs.update(glm::vec3(0.0f), 1.0f);
    check(!songs.playing(7) && songs.beats(7) < 0.0, "an object that is gone takes its music with it");

    std::filesystem::remove_all(root);
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
