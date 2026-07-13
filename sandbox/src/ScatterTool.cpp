#include "ScatterTool.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <imgui.h>
#include <ImGuizmo.h>

#include <fitzel/world/Terrain.hpp>

#include "Component.hpp"
#include "Document.hpp"
#include "ModelLibrary.hpp"
#include "SceneTypes.hpp"

namespace scatterui {

namespace {

// Terrain normal via central differences of the height field.
glm::vec3 groundNormal(const fitzel::TerrainStreamer& streamer, float x, float z) {
    const float e  = 0.5f;
    const float hx = streamer.heightAt(x + e, z) - streamer.heightAt(x - e, z);
    const float hz = streamer.heightAt(x, z + e) - streamer.heightAt(x, z - e);
    return glm::normalize(glm::vec3(-hx, 2.0f * e, -hz));
}

// Euler angles (degrees) of `m` via the same ImGuizmo decompose the gizmo and
// renderer use, so scatter rotations round-trip exactly like gizmo edits.
glm::vec3 eulerOf(const glm::mat4& m) {
    float t[3], r[3], s[3];
    ImGuizmo::DecomposeMatrixToComponents(glm::value_ptr(m), t, r, s);
    return glm::vec3(r[0], r[1], r[2]);
}

// Split a ModelLibrary node key ("path#3f") back into the source file path +
// node index (-1 for a plain whole-file model), so the placed entity's
// ModelComponent serializes like a hand-placed one. See ModelLibrary::importNode.
void splitNodeKey(const std::string& key, std::string& path, int& nodeIndex) {
    path = key; nodeIndex = -1;
    const std::size_t h = key.rfind('#');
    if (h == std::string::npos || h + 2 >= key.size()) return;
    const char tail = key.back();
    if (tail != 'f' && tail != 'n') return;
    const std::string digits = key.substr(h + 1, key.size() - h - 2);
    for (char c : digits)
        if (!std::isdigit(static_cast<unsigned char>(c))) return;
    path      = key.substr(0, h);
    nodeIndex = std::stoi(digits);
}

// The enabled palette as (modelId, cumulative weight); empty when nothing to place.
std::vector<std::pair<int, float>> palette(const Settings& s, ModelLibrary& models,
                                           float& total) {
    std::vector<std::pair<int, float>> pal;
    total = 0.0f;
    for (const Source& src : s.sources)
        if (src.enabled && src.weight > 0.0f && models.byId(src.modelId)) {
            total += src.weight;
            pal.emplace_back(src.modelId, total);
        }
    return pal;
}

int pickWeighted(const std::vector<std::pair<int, float>>& pal, float total, float u) {
    const float t = u * total;
    for (const auto& [id, cum] : pal)
        if (t <= cum) return id;
    return pal.back().first;
}

bool tooClose(glm::vec2 p, const std::vector<glm::vec2>& pts, float spacing) {
    if (spacing <= 0.0f) return false;
    for (const glm::vec2& q : pts)
        if (glm::length(p - q) < spacing) return true;
    return false;
}

// Try to place one instance of `lm` at world XZ `p`: apply the ground filters
// and build the full entity (transform + components). `baseYaw` (radians) is
// the facing used when random yaw is off (roadside objects face the road).
// Returns false when the spot is rejected.
bool placeAt(const Settings& s, const fitzel::TerrainStreamer& streamer,
             LoadedModel& lm, glm::vec2 p, float waterLevel, float baseYaw,
             std::mt19937& rng, Entity& out) {
    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    const float h = streamer.heightAt(p.x, p.y);
    if (h < s.heightMin || h > s.heightMax) return false;
    if (s.avoidWater && h < waterLevel + 0.1f) return false;
    const glm::vec3 n = groundNormal(streamer, p.x, p.y);
    if (glm::degrees(std::acos(glm::clamp(n.y, -1.0f, 1.0f))) > s.maxSlope)
        return false;

    const float lo = std::min(s.scaleMin, s.scaleMax);
    const float hi = std::max(s.scaleMin, s.scaleMax);
    const float sc = lo + (hi - lo) * u(rng);

    out = Entity{};
    out.type = EntityType::Model;
    auto mc = std::make_unique<ModelComponent>();
    splitNodeKey(lm.path, mc->modelPath, mc->nodeIndex);
    mc->modelId = lm.id;
    mc->scale   = sc;
    out.components.items.push_back(std::move(mc));
    if (s.addCollider) {
        auto pc = std::make_unique<PhysicsComponent>();
        pc->dynamic = false; // scenery: a static collider
        out.components.items.push_back(std::move(pc));
    }
    out.half = glm::max(0.5f * lm.size() * sc, glm::vec3(1e-3f));

    // Rotation: yaw about the up axis first, then the lean -- a blend toward
    // the terrain normal plus a random jitter -- applied in world space so the
    // lean's azimuth doesn't spin along with the yaw.
    const float     yaw = s.randomYaw ? u(rng) * glm::two_pi<float>() : baseYaw;
    const glm::vec3 up(0.0f, 1.0f, 0.0f);
    glm::vec3 target = glm::normalize(
        glm::mix(up, n, glm::clamp(s.alignSlope, 0.0f, 1.0f)));
    if (s.tiltJitter > 0.01f) {
        const float az  = u(rng) * glm::two_pi<float>();
        const float mag = glm::radians(s.tiltJitter) * u(rng);
        target = glm::normalize(
            target + std::tan(mag) * glm::vec3(std::cos(az), 0.0f, std::sin(az)));
    }
    glm::quat lean(1.0f, 0.0f, 0.0f, 0.0f);
    const float d = glm::clamp(glm::dot(up, target), -1.0f, 1.0f);
    if (d < 0.9999f)
        lean = glm::angleAxis(std::acos(d), glm::normalize(glm::cross(up, target)));
    const glm::quat q = lean * glm::angleAxis(yaw, up);
    out.rotation = out.localRotation = eulerOf(glm::mat4_cast(q));

    // The render transform centres the model's AABB at `center`, so lift by
    // half.y to rest its base on the ground, minus the sink embed.
    out.center = out.localCenter = glm::vec3(p.x, h + out.half.y - s.sink, p.y);
    out.name   = lm.name;
    return true;
}

} // namespace

std::vector<Entity> buildStamp(const Settings& s, ModelLibrary& models,
                               const fitzel::TerrainStreamer& streamer,
                               glm::vec2 center, float waterLevel,
                               const std::vector<glm::vec2>& occupied,
                               std::mt19937& rng) {
    std::vector<Entity> out;
    float total = 0.0f;
    const auto pal = palette(s, models, total);
    if (pal.empty()) return out;

    std::uniform_real_distribution<float> u(0.0f, 1.0f);

    // Cluster seeds: with clumping > 0, samples huddle around a few of these
    // instead of spreading uniformly over the disc.
    const int nSeeds = std::max(1, s.perStamp / 3);
    std::vector<glm::vec2> seeds;
    seeds.reserve(nSeeds);
    for (int i = 0; i < nSeeds; ++i) {
        const float a = u(rng) * glm::two_pi<float>();
        const float r = s.radius * std::sqrt(u(rng));
        seeds.push_back(center + r * glm::vec2(std::cos(a), std::sin(a)));
    }

    std::vector<glm::vec2> placed; // accepted this stamp (for spacing)
    const int attempts = s.perStamp * 6; // dart throwing: over-sample, reject
    for (int i = 0; i < attempts && static_cast<int>(out.size()) < s.perStamp; ++i) {
        glm::vec2 p;
        if (u(rng) < s.clumping) {
            const auto& seed = seeds[std::min<std::size_t>(
                static_cast<std::size_t>(u(rng) * nSeeds), seeds.size() - 1)];
            const float a = u(rng) * glm::two_pi<float>();
            const float r = s.radius * 0.25f * u(rng);
            p = seed + r * glm::vec2(std::cos(a), std::sin(a));
            if (glm::length(p - center) > s.radius) continue;
        } else {
            const float a = u(rng) * glm::two_pi<float>();
            const float r = s.radius * std::sqrt(u(rng));
            p = center + r * glm::vec2(std::cos(a), std::sin(a));
        }
        if (tooClose(p, placed, s.minSpacing) || tooClose(p, occupied, s.minSpacing))
            continue;

        LoadedModel* lm = models.byId(pickWeighted(pal, total, u(rng)));
        if (!lm) continue;
        Entity e;
        if (!placeAt(s, streamer, *lm, p, waterLevel, 0.0f, rng, e)) continue;
        placed.push_back(p);
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<Entity> buildRoadside(const Settings& s, ModelLibrary& models,
                                  const fitzel::TerrainStreamer& streamer,
                                  const std::vector<glm::vec2>& centerline,
                                  float roadHalfWidth, float waterLevel,
                                  const std::vector<glm::vec2>& occupied,
                                  std::mt19937& rng) {
    std::vector<Entity> out;
    if (centerline.size() < 2) return out;
    float total = 0.0f;
    const auto pal = palette(s, models, total);
    if (pal.empty()) return out;

    std::uniform_real_distribution<float> u(0.0f, 1.0f);
    std::vector<glm::vec2> placed;
    const float step   = std::max(0.5f, s.roadStep);
    float       next   = step * 0.5f; // first station half a step in
    float       walked = 0.0f;
    bool        left   = true;        // toggled by the Alternate side mode

    for (std::size_t i = 1; i < centerline.size(); ++i) {
        const glm::vec2 a   = centerline[i - 1], b = centerline[i];
        const float     seg = glm::length(b - a);
        if (seg < 1e-4f) continue;
        const glm::vec2 dir  = (b - a) / seg;
        const glm::vec2 perp(-dir.y, dir.x); // left of the travel direction
        while (next <= walked + seg) {
            const glm::vec2 c = a + dir * (next - walked);
            next += step;
            // Which side(s) this station populates.
            const bool doLeft  = s.roadSide == 0 || s.roadSide == 2 ||
                                 (s.roadSide == 3 && left);
            const bool doRight = s.roadSide == 1 || s.roadSide == 2 ||
                                 (s.roadSide == 3 && !left);
            left = !left;
            for (int side = 0; side < 2; ++side) {
                if (side == 0 ? !doLeft : !doRight) continue;
                const float sign = (side == 0) ? 1.0f : -1.0f;
                // Jitter the lateral offset a little so rows don't look laser-set.
                const float off = roadHalfWidth +
                                  s.roadOffset * (0.75f + 0.5f * u(rng));
                const glm::vec2 p = c + perp * (sign * off);
                if (tooClose(p, placed, s.minSpacing) ||
                    tooClose(p, occupied, s.minSpacing))
                    continue;
                LoadedModel* lm = models.byId(pickWeighted(pal, total, u(rng)));
                if (!lm) continue;
                // Without random yaw, face along the road (fences, lamp posts).
                const float baseYaw = std::atan2(dir.x, dir.y);
                Entity e;
                if (!placeAt(s, streamer, *lm, p, waterLevel, baseYaw, rng, e))
                    continue;
                placed.push_back(p);
                out.push_back(std::move(e));
            }
        }
        walked += seg;
    }
    return out;
}

std::vector<int> collectInBrush(const Document& doc, int groupId,
                                glm::vec2 c, float radius) {
    std::vector<int> ids;
    if (groupId < 0) return ids;
    for (const Entity& e : doc.entities())
        if (e.parent == groupId &&
            glm::length(glm::vec2(e.center.x, e.center.z) - c) <= radius)
            ids.push_back(e.id);
    return ids;
}

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Scatter", &s.show)) {
        if (ImGui::Checkbox("Scatter mode", &s.scatterMode) && s.scatterMode)
            s.grassPaintMode = s.roadEditMode = s.treePaintMode =
                s.flowerPaintMode = s.sculptMode = s.paintMode = false; // brush owns the LMB
        if (s.scatterMode)
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f),
                "Drag = scatter | hold Alt (or Erase) = remove");
        else
            ImGui::TextDisabled("Enable to scatter objects onto the terrain");
        ImGui::Checkbox("Erase", &s.brushErase);

        ImGui::SeparatorText("Objects to scatter");
        // Sync the palette with the model library: keep known entries'
        // settings, pick up new imports, drop deleted models.
        {
            std::vector<Source> synced;
            synced.reserve(s.models.count());
            for (std::size_t i = 0; i < s.models.count(); ++i) {
                LoadedModel* lm = s.models.at(i);
                if (!lm) continue;
                Source src;
                src.modelId = lm->id;
                for (const Source& old : s.cfg.sources)
                    if (old.modelId == lm->id) { src = old; break; }
                synced.push_back(src);
            }
            s.cfg.sources = std::move(synced);
        }
        int enabledCount = 0;
        for (Source& src : s.cfg.sources) {
            LoadedModel* lm = s.models.byId(src.modelId);
            if (!lm) continue;
            ImGui::PushID(src.modelId);
            ImGui::Checkbox(lm->name.c_str(), &src.enabled);
            if (src.enabled) {
                ++enabledCount;
                ImGui::SameLine();
                ImGui::SetNextItemWidth(90.0f);
                ImGui::SliderFloat("##w", &src.weight, 0.1f, 5.0f, "w %.1f");
            }
            ImGui::PopID();
        }
        if (s.cfg.sources.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                "Import models first (Assets panel / drag & drop).");
        else if (enabledCount == 0)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f),
                "Tick at least one model to scatter.");

        ImGui::SeparatorText("Brush");
        ImGui::SliderFloat("Radius", &s.cfg.radius, 1.0f, 40.0f, "%.1f m");
        ImGui::SliderInt("Objects per stamp", &s.cfg.perStamp, 1, 40);
        ImGui::SliderFloat("Min spacing", &s.cfg.minSpacing, 0.0f, 15.0f, "%.1f m");
        ImGui::SliderFloat("Clumping", &s.cfg.clumping, 0.0f, 1.0f);

        ImGui::SeparatorText("Randomize");
        ImGui::DragFloatRange2("Scale", &s.cfg.scaleMin, &s.cfg.scaleMax,
                               0.01f, 0.05f, 10.0f, "min %.2f", "max %.2f");
        ImGui::Checkbox("Random yaw", &s.cfg.randomYaw);
        ImGui::SliderFloat("Tilt jitter", &s.cfg.tiltJitter, 0.0f, 30.0f, "%.0f deg");
        ImGui::SliderFloat("Align to slope", &s.cfg.alignSlope, 0.0f, 1.0f);
        ImGui::SliderFloat("Sink into ground", &s.cfg.sink, 0.0f, 2.0f, "%.2f m");

        ImGui::SeparatorText("Placement filters");
        ImGui::SliderFloat("Max slope", &s.cfg.maxSlope, 0.0f, 90.0f, "%.0f deg");
        ImGui::DragFloatRange2("Height band", &s.cfg.heightMin, &s.cfg.heightMax,
                               0.5f, -1000.0f, 1000.0f, "%.0f m", "%.0f m");
        ImGui::Checkbox("Avoid water", &s.cfg.avoidWater);
        ImGui::SameLine();
        ImGui::Checkbox("Static collider", &s.cfg.addCollider);

        ImGui::SeparatorText("Along the road");
        ImGui::SliderFloat("Step", &s.cfg.roadStep, 1.0f, 40.0f, "%.1f m");
        ImGui::SliderFloat("Edge offset", &s.cfg.roadOffset, 0.0f, 20.0f, "%.1f m");
        const char* sides[] = {"Left", "Right", "Both", "Alternate"};
        ImGui::Combo("Side", &s.cfg.roadSide, sides, IM_ARRAYSIZE(sides));
        ImGui::BeginDisabled(!s.roadAvailable || enabledCount == 0);
        if (ImGui::Button("Place along road")) s.placeRoadside();
        ImGui::EndDisabled();
        if (!s.roadAvailable)
            ImGui::TextDisabled("Draw a road first (Roads panel).");

        ImGui::Separator();
        ImGui::Text("Scattered objects: %d", s.scatteredCount);
        ImGui::SameLine();
        ImGui::BeginDisabled(s.scatteredCount == 0);
        if (ImGui::Button("Clear all")) s.clearAll();
        ImGui::EndDisabled();
    }
    ImGui::End();
}

} // namespace scatterui
