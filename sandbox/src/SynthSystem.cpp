#include "SynthSystem.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

#include <fitzel/asset/Vfs.hpp>
#include <fitzel/audio/Audio.hpp>

#include "Component.hpp"
#include "Document.hpp"

SynthComponent* SynthSystem::component(int id) const {
    if (!m_doc) return nullptr;
    Entity* e = m_doc->find(id);
    return e ? e->components.get<SynthComponent>() : nullptr;
}

bool SynthSystem::fail(const std::string& why) {
    m_error = why;
    std::fprintf(stderr, "[Synth] %s\n", why.c_str());
    return false;
}

std::string SynthSystem::resolve(const std::string& name, const char* folder,
                                 const char* ext) const {
    if (name.empty()) return {};
    std::filesystem::path file(name);
    if (!file.has_extension()) file += ext;   // "lead" means lead.json
    const std::filesystem::path content =
        (m_project && !m_project->empty())
            ? std::filesystem::path(*m_project).parent_path() / "content"
            : std::filesystem::path("content");
    // Where the panel saves it; then anywhere under content/ the name points
    // ("music/theme.mid"); then as written, for a path the author typed out.
    for (const std::filesystem::path& p :
         {content / folder / file, content / file, file}) {
        const std::string s = p.generic_string();
        if (fitzel::vfs::exists(s)) return s;
    }
    return (content / folder / file).generic_string();   // for the error message
}

std::string SynthSystem::resolvePatch(const std::string& name) const {
    return resolve(name, "patches", ".json");
}
std::string SynthSystem::resolveMidi(const std::string& name) const {
    return resolve(name, "midi", ".mid");
}

SynthSystem::Running* SynthSystem::ensure(int id) {
    SynthComponent* sc = component(id);
    if (!sc) {
        fail("object " + std::to_string(id) + " has no Synth");
        return nullptr;
    }
    if (!m_audio || !m_audio->ok()) {
        fail("no audio device");
        return nullptr;
    }
    const int voices = std::clamp(sc->voices, 1, 32);
    auto it = m_players.find(id);
    if (it != m_players.end() && it->second.player.isValid() &&
        it->second.patch == sc->patch && it->second.voices == voices)
        return &it->second;

    if (sc->patch.empty()) {
        fail("the Synth on object " + std::to_string(id) + " has no patch");
        return nullptr;
    }
    if (auto b = m_broken.find(id); b != m_broken.end() && b->second == sc->patch)
        return nullptr;   // failed before, said so then (m_error still holds why)
    fitzel::synth::Patch patch;
    std::string          err;
    const std::string    path = resolvePatch(sc->patch);
    bool ok = fitzel::synth::Patch::load(path, patch, &err);
    fitzel::SynthPlayer player;
    if (ok) {
        player = fitzel::SynthPlayer::create(*m_audio, patch, voices, &err);
        ok     = player.isValid();
    }
    if (!ok) {
        m_broken[id] = sc->patch;
        fail("patch '" + sc->patch + "': " + err);
        return nullptr;
    }
    m_broken.erase(id);
    // Assigning over a running one is safe: the old voice leaves the mixer as
    // it goes (see SynthPlayer::Impl).
    Running& r = m_players[id];
    r.player   = std::move(player);
    r.patch    = sc->patch;
    r.voices   = voices;
    r.midi.clear();
    r.player.setVolume(0.0f);   // update() sets the real level before it is heard
    r.player.start();
    return &r;
}

bool SynthSystem::play(int id, bool reload) {
    if (reload) {
        m_players.erase(id);
        m_broken.erase(id);
    }
    Running* r = ensure(id);
    if (!r) return false;
    const SynthComponent* sc = component(id);
    r->player.start();
    if (sc && !sc->midi.empty()) return playMidi(id, {}, -1);
    return true;
}

void SynthSystem::stop(int id) {
    auto it = m_players.find(id);
    if (it == m_players.end()) return;
    it->second.player.stopMidi();
    it->second.player.allNotesOff();
    it->second.player.stop();
}

bool SynthSystem::running(int id) const {
    auto it = m_players.find(id);
    return it != m_players.end() && it->second.player.isRunning();
}

bool SynthSystem::noteOn(int id, int note, float velocity) {
    Running* r = ensure(id);
    if (!r) return false;
    r->player.noteOn(note, std::clamp(velocity, 0.0f, 1.0f));
    return true;
}

bool SynthSystem::noteOff(int id, int note) {
    auto it = m_players.find(id);
    if (it == m_players.end()) return false;
    it->second.player.noteOff(note);
    return true;
}

bool SynthSystem::allNotesOff(int id) {
    auto it = m_players.find(id);
    if (it == m_players.end()) return false;
    it->second.player.allNotesOff();
    return true;
}

bool SynthSystem::setDial(int id, const std::string& dial, float value) {
    Running* r = ensure(id);
    if (!r) return false;
    const int index = r->player.inputIndex(dial);
    if (index < 0) return fail("patch '" + r->patch + "' has no dial '" + dial + "'");
    r->player.setInput(index, value);
    return true;
}

bool SynthSystem::playMidi(int id, const std::string& file, int loop) {
    Running* r = ensure(id);
    if (!r) return false;
    const SynthComponent* sc = component(id);
    const std::string song = file.empty() ? (sc ? sc->midi : std::string()) : file;
    if (song.empty()) return fail("no MIDI song to play on object " + std::to_string(id));
    if (song != r->midi) {
        std::string err;
        if (!r->player.loadMidi(resolveMidi(song), &err)) return fail("song '" + song + "': " + err);
        r->midi = song;
    }
    if (loop >= 0) {
        r->player.setMidiLoop(loop != 0);
        // A script's choice holds against the component's until the song changes.
        if (SynthComponent* c = component(id)) c->loopMidi = loop != 0;
    }
    r->player.playMidi(true);
    return true;
}

bool SynthSystem::stopMidi(int id) {
    auto it = m_players.find(id);
    if (it == m_players.end()) return false;
    it->second.player.stopMidi();
    return true;
}

bool SynthSystem::midiPlaying(int id) const {
    auto it = m_players.find(id);
    return it != m_players.end() && it->second.player.midiPlaying();
}

bool SynthSystem::setTempo(int id, float scale) {
    SynthComponent* sc = component(id);
    if (!sc) return fail("object " + std::to_string(id) + " has no Synth");
    sc->tempo = std::clamp(scale, 0.05f, 8.0f);   // update() hands it on
    return true;
}

void SynthSystem::update(const glm::vec3& listener, float ambientGain) {
    for (auto it = m_players.begin(); it != m_players.end();) {
        Entity*         e  = m_doc ? m_doc->find(it->first) : nullptr;
        SynthComponent* sc = e ? e->components.get<SynthComponent>() : nullptr;
        if (!sc) {
            // The object (or its Synth) went away: so does its sound.
            it = m_players.erase(it);
            continue;
        }
        fitzel::SynthPlayer& p = it->second.player;
        float vol = std::clamp(sc->volume, 0.0f, 1.0f) * ambientGain;
        if (sc->spatial) {
            const float dist = glm::distance(listener, e->center);
            vol *= std::clamp(1.0f - dist / std::max(sc->radius, 0.01f), 0.0f, 1.0f);
        }
        if (!e->activeInHierarchy) vol = 0.0f;   // switched off keeps its place, not its voice
        p.setVolume(vol);
        p.setMidiLoop(sc->loopMidi);
        p.setTempo(std::clamp(sc->tempo, 0.05f, 8.0f));
        p.setChannel(sc->channel >= 1 && sc->channel <= 16 ? sc->channel - 1 : -1);
        p.setSkipDrums(!sc->drums);
        ++it;
    }
}
