#include "SongSystem.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>

#include <fitzel/asset/Vfs.hpp>
#include <fitzel/audio/Audio.hpp>

#include "Component.hpp"
#include "Document.hpp"

namespace sg = fitzel::song;

SongPlayerComponent* SongSystem::component(int id) const {
    if (!m_doc) return nullptr;
    Entity* e = m_doc->find(id);
    return e ? e->components.get<SongPlayerComponent>() : nullptr;
}

bool SongSystem::fail(const std::string& why) {
    m_error = why;
    std::fprintf(stderr, "[Song] %s\n", why.c_str());
    return false;
}

SongSystem::Running* SongSystem::find(int id) {
    auto it = m_players.find(id);
    return it == m_players.end() ? nullptr : &it->second;
}
const SongSystem::Running* SongSystem::find(int id) const {
    auto it = m_players.find(id);
    return it == m_players.end() ? nullptr : &it->second;
}

std::string SongSystem::resolveSong(const std::string& name) const {
    if (name.empty()) return {};
    std::filesystem::path file(name);
    if (!file.has_extension()) file += ".json";
    const std::filesystem::path content =
        (m_project && !m_project->empty()) ? std::filesystem::path(*m_project).parent_path() / "content"
                                           : std::filesystem::path("content");
    // Where the sequencer saves it; then anywhere under content/ the name
    // points; then as written.
    for (const std::filesystem::path& p : {content / "songs" / file, content / file, file}) {
        const std::string s = p.generic_string();
        if (fitzel::vfs::exists(s)) return s;
    }
    return (content / "songs" / file).generic_string();
}

void SongSystem::route(fitzel::Mixer& mixer,
                       std::function<int(const std::string&, const std::string&)> strip) {
    m_mixer = &mixer;
    m_strip = std::move(strip);   // update() points every player again with it
}

bool SongSystem::play(int id, bool reload) {
    SongPlayerComponent* sc = component(id);
    if (!sc) return fail("object " + std::to_string(id) + " has no Song Player");
    if (sc->song.empty()) return fail("the Song Player on object " + std::to_string(id) + " has no song");
    if (!m_audio || !m_audio->ok()) return fail("no audio device");

    Running* r = find(id);
    if (!r || reload || r->name != sc->song || r->loop != sc->loop || !r->player.isValid()) {
        sg::Song song;
        std::string err;
        if (!sg::Song::load(resolveSong(sc->song), song, &err)) return fail("song '" + sc->song + "': " + err);
        // Looping: the song's own loop range if it has one (an intro, then the
        // part that repeats), else all of it. Not looping: once, to its end.
        if (sc->loop) {
            if (!song.loop || song.loopEnd <= song.loopStart) {
                song.loop      = true;
                song.loopStart = 0;
                song.loopEnd   = std::max(song.length(), song.ticksPerBar());
            }
        } else {
            song.loop = false;
        }
        fitzel::SongPlayer player = r && r->player.isValid() ? std::move(r->player)
                                                            : fitzel::SongPlayer::create(*m_audio, &err);
        if (!player.isValid()) return fail("song '" + sc->song + "': " + err);
        sg::Files files;
        files.sound = m_resolveSound;
        files.patch = m_resolvePatch;
        player.engine().setFiles(files);
        player.engine().setSong(song);
        player.engine().setStopAtEnd(!sc->loop);
        Running& slot = m_players[id];
        slot.player = std::move(player);
        slot.name   = sc->song;
        slot.song   = std::move(song);
        slot.loop   = sc->loop;
        r = &slot;
        for (const sg::Track& t : r->song.tracks) {
            const std::string why = r->player.engine().instrumentError(t.uid);
            if (!why.empty()) fail("song '" + sc->song + "', track " + t.name + ": " + why);
        }
    }
    r->endedFor = 0.0;
    r->player.setVolume(0.0f);   // update() sets the real level before it is heard
    r->player.engine().stop();
    r->player.engine().seek(0);
    r->player.engine().play();
    r->player.start();
    return true;
}

void SongSystem::stop(int id) {
    if (Running* r = find(id)) r->player.stop();
}

bool SongSystem::playing(int id) const {
    const Running* r = find(id);
    return r && r->player.isValid() && r->player.isRunning() && r->player.engine().playing();
}

bool SongSystem::anyPlaying() const {
    for (const auto& [id, r] : m_players)
        if (r.player.isValid() && r.player.isRunning() && r.player.engine().playing()) return true;
    return false;
}

double SongSystem::seconds(int id) const {
    const Running* r = find(id);
    if (!r || !r->player.isValid()) return -1.0;
    return r->song.seconds(static_cast<std::int64_t>(r->player.ticks()));
}

double SongSystem::beats(int id) const {
    const Running* r = find(id);
    if (!r || !r->player.isValid()) return -1.0;
    return r->player.ticks() / static_cast<double>(r->song.ticksPerBeat());
}

void SongSystem::barBeat(int id, int& bar, int& beat) const {
    bar = beat = 0;
    const Running* r = find(id);
    if (!r || !r->player.isValid()) return;
    const auto t = static_cast<std::int64_t>(r->player.ticks());
    bar  = static_cast<int>(t / r->song.ticksPerBar()) + 1;
    beat = static_cast<int>((t % r->song.ticksPerBar()) / r->song.ticksPerBeat()) + 1;
}

bool SongSystem::seek(int id, double beat) {
    Running* r = find(id);
    if (!r) return fail("the song on object " + std::to_string(id) + " is not started");
    r->player.engine().seek(static_cast<std::int64_t>(std::max(0.0, beat) * static_cast<double>(r->song.ticksPerBeat())));
    return true;
}

bool SongSystem::setTrackMute(int id, const std::string& track, bool mute) {
    Running* r = find(id);
    if (!r) return fail("the song on object " + std::to_string(id) + " is not started");
    for (sg::Track& t : r->song.tracks)
        if (t.name == track) {
            if (t.mute != mute) {
                t.mute = mute;
                r->player.engine().setSong(r->song);
            }
            return true;
        }
    return fail("song '" + r->name + "' has no track '" + track + "'");
}

bool SongSystem::setTrackVolume(int id, const std::string& track, float volume) {
    Running* r = find(id);
    if (!r) return fail("the song on object " + std::to_string(id) + " is not started");
    for (sg::Track& t : r->song.tracks)
        if (t.name == track) {
            t.volume = std::clamp(volume, 0.0f, 2.0f);
            r->player.engine().setSong(r->song);
            return true;
        }
    return fail("song '" + r->name + "' has no track '" + track + "'");
}

bool SongSystem::setTempo(int id, double bpm) {
    Running* r = find(id);
    if (!r) return fail("the song on object " + std::to_string(id) + " is not started");
    r->song.bpm = std::clamp(bpm, 20.0, 400.0);
    r->player.engine().setSong(r->song);
    return true;
}

float SongSystem::takePeak(int id) {
    Running* r = find(id);
    return r && r->player.isValid() ? r->player.engine().takeMasterPeak() : 0.0f;
}

void SongSystem::update(const glm::vec3& listener, float gain) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last = clock::now();
    const clock::time_point now = clock::now();
    const double dt = std::min(0.25, std::chrono::duration<double>(now - last).count());
    last = now;
    for (auto it = m_players.begin(); it != m_players.end();) {
        Entity* e = m_doc ? m_doc->find(it->first) : nullptr;
        SongPlayerComponent* sc = e ? e->components.get<SongPlayerComponent>() : nullptr;
        if (!sc) {
            // The object (or its Song Player) went away: so does its music.
            it = m_players.erase(it);
            continue;
        }
        Running& r = it->second;
        if (!r.player.isValid()) { ++it; continue; }
        // Its outputs: the song's own mix to the component's channel, a track
        // with a channel of its own there (or to the component's, failing
        // that). Every frame; the player wires again only what changed.
        if (m_mixer && m_strip) {
            const std::string ch = sc->channel.empty() ? std::string("Music") : sc->channel;
            r.player.route(*m_mixer, m_strip(ch, "Music"),
                           [&](const std::string& name) { return m_strip(name, ch); });
        }
        float vol = std::clamp(sc->volume, 0.0f, 2.0f) * gain;
        if (sc->spatial) {
            const float dist = glm::distance(listener, e->center);
            vol *= std::clamp(1.0f - dist / std::max(sc->radius, 0.01f), 0.0f, 1.0f);
        }
        if (!e->activeInHierarchy) vol = 0.0f;   // switched off keeps its place, not its voice
        r.player.setVolume(vol);
        // Ran out (not looping): its last notes ring for a while, then the
        // voice leaves the mixer -- a plugin costs its CPU even in silence.
        if (r.player.isRunning() && r.player.engine().ended()) {
            r.endedFor += dt;
            if (r.endedFor > 4.0) r.player.stop();
        }
        ++it;
    }
}
