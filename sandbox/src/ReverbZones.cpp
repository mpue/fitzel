#include "ReverbZones.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include <glm/gtc/matrix_transform.hpp>

#include "Component.hpp"

namespace reverbzones {

namespace {

// A new zone's bus: a room, all wet, with Ambient and SFX sent in.
mixerui::Strip& makeBus(mixerui::Desk& desk, const std::string& name) {
    mixerui::Strip& bus = desk.add(mixerui::Kind::Aux, name.empty() ? std::string("Room") : name);
    mixerui::Insert in;
    in.type = fitzel::mixfx::Type::Reverb;
    in.values = {0.75f, 0.4f, 1.0f};          // size, damping, mix: a bus reverb is all wet
    bus.inserts.push_back(in);
    const int uid = bus.uid;                  // (add may move strips: look it up again below)
    for (mixerui::Strip& s : desk.strips) {
        if (s.kind != mixerui::Kind::Channel) continue;
        if (s.name == "SFX") s.sends[uid].level = mixerui::fromDb(-3.0f);
        if (s.name == "Ambient") s.sends[uid].level = mixerui::fromDb(-6.0f);
    }
    return *desk.find(uid);
}

mixerui::Strip* auxNamed(mixerui::Desk& desk, const std::string& name) {
    if (name.empty()) return nullptr;
    for (mixerui::Strip& s : desk.strips)
        if (s.kind == mixerui::Kind::Aux && s.name == name) return &s;
    return nullptr;
}

} // namespace

float inside(const glm::vec3& p, const glm::vec3& c, const glm::vec3& rotDeg, const glm::vec3& half, float fade) {
    const float k = 3.14159265f / 180.0f;
    const glm::mat4 R = glm::rotate(glm::mat4(1.0f), rotDeg.z * k, glm::vec3(0, 0, 1)) *
                        glm::rotate(glm::mat4(1.0f), rotDeg.y * k, glm::vec3(0, 1, 0)) *
                        glm::rotate(glm::mat4(1.0f), rotDeg.x * k, glm::vec3(1, 0, 0));
    const glm::vec3 local = glm::transpose(glm::mat3(R)) * (p - c);
    const glm::vec3 room = glm::abs(half) - glm::abs(local);   // how far inside each pair of faces
    const float depth = std::min(room.x, std::min(room.y, room.z));
    if (depth < 0.0f) return 0.0f;
    if (fade <= 1e-4f) return 1.0f;
    const float u = std::min(1.0f, depth / fade);
    return u * u * (3.0f - 2.0f * u);
}

void update(std::vector<Entity>& entities, mixerui::Desk& desk, const glm::vec3& listener, float dt) {
    // The bus a zone is tied to: the one it names; else the one it was tied to
    // (renamed in the Mixer); else none yet.
    auto tied = [&](const ReverbZoneComponent& z) -> mixerui::Strip* {
        if (mixerui::Strip* b = auxNamed(desk, z.bus)) return b;
        if (z.busUid > 0)
            if (mixerui::Strip* was = desk.find(z.busUid); was && was->kind == mixerui::Kind::Aux) return was;
        return nullptr;
    };
    std::set<int> claimed;       // buses some zone is tied to
    for (const Entity& e : entities)
        if (const auto* z = e.components.get<ReverbZoneComponent>())
            if (const mixerui::Strip* b = tied(*z)) claimed.insert(b->uid);
    std::map<int, float> want;   // aux uid -> the listener's weight in its zones
    for (Entity& e : entities) {
        auto* z = e.components.get<ReverbZoneComponent>();
        if (!z) continue;
        mixerui::Strip* bus = tied(*z);
        if (!bus && z->bus.empty()) {
            // A zone without a bus takes a "Room" no zone is tied to before it
            // makes another: undoing and redoing the zone's making would
            // otherwise leave a new Room behind every time.
            for (mixerui::Strip& s : desk.strips)
                if (s.kind == mixerui::Kind::Aux && s.name.rfind("Room", 0) == 0 && !claimed.count(s.uid)) {
                    bus = &s;
                    break;
                }
        }
        if (!bus) bus = &makeBus(desk, z->bus);
        claimed.insert(bus->uid);
        z->bus = bus->name;
        z->busUid = bus->uid;
        const float w = e.activeInHierarchy ? inside(listener, e.center, e.rotation, e.half, z->fade) : 0.0f;
        float& slot = want[bus->uid];
        slot = std::max(slot, w);
    }
    // Buses no zone sends into any more are plain buses again.
    for (auto it = desk.zoneWeight.begin(); it != desk.zoneWeight.end();)
        it = want.count(it->first) ? std::next(it) : desk.zoneWeight.erase(it);
    const float k = dt > 0.0f ? 1.0f - std::exp(-dt / 0.25f) : 1.0f;
    for (const auto& [uid, w] : want) {
        auto it = desk.zoneWeight.find(uid);
        if (it == desk.zoneWeight.end()) desk.zoneWeight[uid] = w;
        else it->second += (w - it->second) * k;
    }
}

} // namespace reverbzones
