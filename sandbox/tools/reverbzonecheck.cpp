// reverbzonecheck -- Reverb Zones (ReverbZones.hpp) without a window or a sound
// device: a zone gets a bus of its own with sends from Ambient and SFX, picks an
// existing bus by name, follows a rename in the Mixer, opens its sends inside its
// box (turned and faded at the edge, the largest of two zones on one bus), and
// lets go of a bus once its zone is gone; the component saves and loads.
//   build/linux/bin/reverbzonecheck

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/Component.hpp"
#include "../src/MixerPanel.hpp"
#include "../src/ReverbZones.hpp"

namespace {

int g_fail = 0;
void check(bool ok, const char* what) {
    std::printf("  [%s] %s\n", ok ? " ok " : "FAIL", what);
    if (!ok) ++g_fail;
}
bool near(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }

Entity zone(int id, const glm::vec3& c, const glm::vec3& half, const glm::vec3& rot = glm::vec3(0.0f)) {
    Entity e;
    e.id = id;
    e.name = "Zone " + std::to_string(id);
    e.center = c;
    e.half = half;
    e.rotation = rot;
    e.components.items.push_back(std::make_unique<ReverbZoneComponent>());
    return e;
}

ReverbZoneComponent& zc(Entity& e) { return *e.components.get<ReverbZoneComponent>(); }

const mixerui::Strip* aux(const mixerui::Desk& d, const std::string& name) {
    for (const auto& s : d.strips) if (s.kind == mixerui::Kind::Aux && s.name == name) return &s;
    return nullptr;
}
const mixerui::Strip* channel(const mixerui::Desk& d, const std::string& name) {
    for (const auto& s : d.strips) if (s.kind == mixerui::Kind::Channel && s.name == name) return &s;
    return nullptr;
}

} // namespace

int main() {
    std::printf("reverbzonecheck\n");

    // --- a new zone makes its bus -------------------------------------------------
    mixerui::Desk desk;
    std::vector<Entity> ents;
    ents.push_back(zone(1, glm::vec3(0.0f), glm::vec3(5.0f, 3.0f, 5.0f)));
    zc(ents[0]).fade = 2.0f;
    reverbzones::update(ents, desk, glm::vec3(100.0f), 0.0f);
    const mixerui::Strip* room = aux(desk, "Room");
    check(room != nullptr, "a zone without a bus gets one, \"Room\"");
    check(room && room->inserts.size() == 1 && room->inserts[0].type == fitzel::mixfx::Type::Reverb,
          "the new bus has a reverb insert");
    check(zc(ents[0]).bus == "Room" && room && zc(ents[0]).busUid == room->uid, "the zone names its bus and keeps its uid");
    const mixerui::Strip* sfx = channel(desk, "SFX");
    const mixerui::Strip* amb = channel(desk, "Ambient");
    const mixerui::Strip* mus = channel(desk, "Music");
    check(sfx && room && sfx->sends.count(room->uid) && near(mixerui::toDb(sfx->sends.at(room->uid).level), -3.0f, 0.05f),
          "SFX sends into it at -3 dB");
    check(amb && room && amb->sends.count(room->uid) && near(mixerui::toDb(amb->sends.at(room->uid).level), -6.0f, 0.05f),
          "Ambient sends into it at -6 dB");
    check(mus && room && (!mus->sends.count(room->uid) || mus->sends.at(room->uid).level == 0.0f), "Music does not");
    const int roomUid = room ? room->uid : -1;
    check(desk.zoneWeight.count(roomUid) && near(desk.zoneWeight[roomUid], 0.0f), "outside the zone its sends are shut");
    const std::size_t strips = desk.strips.size();
    reverbzones::update(ents, desk, glm::vec3(100.0f), 0.0f);
    check(desk.strips.size() == strips, "a second frame makes no second bus");

    // --- where the listener is ------------------------------------------------------
    reverbzones::update(ents, desk, glm::vec3(0.0f), 0.0f);
    check(near(desk.zoneWeight[roomUid], 1.0f), "in the middle: fully open");
    reverbzones::update(ents, desk, glm::vec3(4.0f, 0.0f, 0.0f), 0.0f);   // 1 m inside the x faces
    check(near(desk.zoneWeight[roomUid], 0.5f), "1 m inside with a 2 m fade: half open (smoothstep)");
    reverbzones::update(ents, desk, glm::vec3(0.0f, 2.9f, 0.0f), 0.0f);   // 0.1 m under the ceiling
    check(desk.zoneWeight[roomUid] > 0.0f && desk.zoneWeight[roomUid] < 0.05f, "near the ceiling: barely");
    reverbzones::update(ents, desk, glm::vec3(5.5f, 0.0f, 0.0f), 0.0f);
    check(near(desk.zoneWeight[roomUid], 0.0f), "just outside: shut");
    // the follow: a quarter second
    reverbzones::update(ents, desk, glm::vec3(0.0f), 0.25f);
    check(desk.zoneWeight[roomUid] > 0.6f && desk.zoneWeight[roomUid] < 0.66f, "walking in, it opens over a quarter of a second");

    // a turned zone: a long thin box turned 90 degrees round Y
    check(near(reverbzones::inside(glm::vec3(0, 0, 8), glm::vec3(0.0f), glm::vec3(0, 90, 0), glm::vec3(10, 2, 1), 0.0f), 1.0f),
          "a turned box: inside along its turned length");
    check(near(reverbzones::inside(glm::vec3(8, 0, 0), glm::vec3(0.0f), glm::vec3(0, 90, 0), glm::vec3(10, 2, 1), 0.0f), 0.0f),
          "a turned box: outside across it");

    // --- two zones, one bus; another naming an existing bus ------------------------
    ents.push_back(zone(2, glm::vec3(20.0f, 0, 0), glm::vec3(3.0f)));
    zc(ents[1]).bus = "Room";
    zc(ents[1]).fade = 0.0f;
    reverbzones::update(ents, desk, glm::vec3(20.0f, 0, 0), 0.0f);
    check(desk.strips.size() == strips, "a zone naming an existing bus makes none");
    check(near(desk.zoneWeight[roomUid], 1.0f), "two zones on one bus: the one the listener is in counts");
    ents.push_back(zone(3, glm::vec3(-20.0f, 0, 0), glm::vec3(3.0f)));
    zc(ents[2]).bus = "Reverb";   // the desk's own bus
    reverbzones::update(ents, desk, glm::vec3(0.0f), 0.0f);
    const mixerui::Strip* rv = aux(desk, "Reverb");
    check(rv && desk.zoneWeight.count(rv->uid) && near(desk.zoneWeight[rv->uid], 0.0f),
          "a zone on the desk's Reverb bus drives that bus");

    // --- a rename in the Mixer is followed --------------------------------------------
    desk.rename(roomUid, "Cave");
    reverbzones::update(ents, desk, glm::vec3(0.0f), 0.0f);
    check(zc(ents[0]).bus == "Cave" && zc(ents[1]).bus == "Cave", "renamed in the Mixer: both zones follow");
    check(desk.strips.size() == strips, "...and no bus is made for the old name");

    // --- a zone gone: its bus is a plain bus again --------------------------------------
    ents.erase(ents.begin() + 2);
    reverbzones::update(ents, desk, glm::vec3(0.0f), 0.0f);
    check(rv && !desk.zoneWeight.count(rv->uid), "the Reverb bus's zone removed: its sends are its own again");

    // --- a zone made again (undo, redo) takes the Room no zone uses -----------------------
    {
        mixerui::Desk d2;
        std::vector<Entity> e2;
        e2.push_back(zone(10, glm::vec3(0.0f), glm::vec3(4.0f)));
        reverbzones::update(e2, d2, glm::vec3(0.0f), 0.0f);
        const std::size_t n = d2.strips.size();
        e2.clear();                                            // undone
        reverbzones::update(e2, d2, glm::vec3(0.0f), 0.0f);
        e2.push_back(zone(10, glm::vec3(0.0f), glm::vec3(4.0f))); // redone: bus "" again
        reverbzones::update(e2, d2, glm::vec3(0.0f), 0.0f);
        check(d2.strips.size() == n && zc(e2[0]).bus == "Room", "made again, a zone takes the unused Room: no Room 2");
        e2.push_back(zone(11, glm::vec3(9.0f, 0, 0), glm::vec3(2.0f)));   // a second zone of its own
        reverbzones::update(e2, d2, glm::vec3(0.0f), 0.0f);
        check(d2.strips.size() == n + 1 && zc(e2[1]).bus == "Room 2", "a second zone still gets a bus of its own");
    }

    // --- the component saves and loads ----------------------------------------------------
    nlohmann::json j;
    zc(ents[0]).fade = 3.5f;
    zc(ents[0]).save(j);
    ReverbZoneComponent back;
    back.load(j);
    check(back.bus == "Cave" && back.busUid == roomUid && near(back.fade, 3.5f), "the component saves and loads (bus, uid, fade)");
    check(components::create("reverb_zone") != nullptr, "registered as \"reverb_zone\"");

    std::printf(g_fail ? "reverbzonecheck: %d FAILED\n" : "reverbzonecheck: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
