#include "AddMenu.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "Component.hpp"
#include "SceneOps.hpp"   // kPlaneHalfY
#include "UiStyle.hpp"
#include "ViewportFrame.hpp"

namespace addmenu {

namespace {

// --- Set-ups past a component's defaults -------------------------------------

void spotDown(Entity& e) {
    if (auto* l = e.components.get<LightComponent>()) l->type = 1;
    e.localRotation = e.rotation = glm::vec3(90.0f, 0.0f, 0.0f);   // +Z (the beam) down
}
void shadowLight(Entity& e) {
    if (auto* l = e.components.get<LightComponent>()) l->castShadows = true;
}
void mainCamera(Entity& e) {
    if (auto* c = e.components.get<CameraComponent>()) c->activeOnStart = true;
}
void followCamera(Entity& e) {
    if (auto* c = e.components.get<CameraComponent>()) c->mode = CameraComponent::Follow;
}
void dynamicBody(Entity& e) {
    if (auto* p = e.components.get<PhysicsComponent>()) p->dynamic = true;
}
void staticBody(Entity& e) {
    if (auto* p = e.components.get<PhysicsComponent>()) p->dynamic = false;
}
void floatingBox(Entity& e) {
    dynamicBody(e);
    if (auto c = components::create("buoyancy")) e.components.items.push_back(std::move(c));
}

using T = EntityType;
using S = Special;

// Category colours: a dot in front of every row, so the eye finds a group in a
// filtered list where the headings are far apart.
ImU32 categoryColor(const char* cat) {
    const std::string c = cat;
    if (c == "Shapes")   return IM_COL32(160, 166, 176, 255);
    if (c == "Lights")   return IM_COL32(250, 210, 90, 255);
    if (c == "Cameras")  return IM_COL32(120, 190, 250, 255);
    if (c == "Gameplay") return IM_COL32(120, 220, 140, 255);
    if (c == "Physics")  return IM_COL32(240, 140, 90, 255);
    if (c == "Audio")    return IM_COL32(200, 140, 240, 255);
    if (c == "Effects")  return IM_COL32(240, 120, 180, 255);
    if (c == "Racing")   return IM_COL32(230, 90, 90, 255);
    return IM_COL32(170, 170, 170, 255);
}

} // namespace

const std::vector<Item>& catalog() {
    static const std::vector<Item> items = {
        // --- Shapes -----------------------------------------------------------
        {"Shapes", "Box", "A solid box. Becomes anything in the Modeling panel.", "cube block",
         T::Box, {0.5f, 0.5f, 0.5f}, true},
        {"Shapes", "Ramp", "A wedge rising along +Z.", "slope wedge", T::Ramp, {0.5f, 0.5f, 0.5f}, true},
        {"Shapes", "Cylinder", "A solid cylinder.", "pillar column tube", T::Cylinder,
         {0.5f, 0.5f, 0.5f}, true},
        {"Shapes", "Sphere", "A solid ball.", "ball", T::Sphere, {0.5f, 0.5f, 0.5f}, true},
        {"Shapes", "Plane", "A flat quad facing up.", "floor quad ground", T::Plane,
         {0.5f, 0.5f, 0.5f}, true},
        {"Shapes", "Empty", "An invisible point: a pivot to group and move things by.",
         "group pivot null marker", T::Empty, {0.5f, 0.5f, 0.5f}},

        // --- Lights -----------------------------------------------------------
        {"Lights", "Point light", "Shines in every direction, 2.5 m above the ground.",
         "lamp bulb", T::Light, {0.3f, 0.3f, 0.3f}, false, 2.2f, "light"},
        {"Lights", "Point light (shadows)", "A point light that casts shadows.",
         "lamp bulb shadow", T::Light, {0.3f, 0.3f, 0.3f}, false, 2.2f, "light", shadowLight},
        {"Lights", "Spot light", "A cone of light, 3.5 m up and pointing down. Turn it "
         "with the gizmo.", "lamp cone torch", T::Light, {0.3f, 0.3f, 0.3f}, false, 3.2f,
         "light", spotDown},

        // --- Cameras ----------------------------------------------------------
        {"Cameras", "Camera", "A fixed camera at eye height. Aim it with the gizmo, or use "
         "Align to view.", "view shot static", T::Empty, {0.5f, 0.5f, 0.5f}, false, 1.2f, "camera"},
        {"Cameras", "Camera from view", "A camera exactly where the viewport looks from.",
         "view shot", T::Empty, {0.5f, 0.5f, 0.5f}, false, 0.0f, nullptr, nullptr,
         S::CameraFromView},
        {"Cameras", "Main camera", "The camera Play starts with.", "view start play",
         T::Empty, {0.5f, 0.5f, 0.5f}, false, 1.2f, "camera", mainCamera},
        {"Cameras", "Follow camera", "Trails the object it hangs under: add it as a child.",
         "chase third person", T::Empty, {0.5f, 0.5f, 0.5f}, false, 1.2f, "camera", followCamera},
        {"Cameras", "Camera switcher", "Cuts between cameras while playing.", "cut switch",
         T::Empty, {0.5f, 0.5f, 0.5f}, false, 0.0f, "camera_switcher"},

        // --- Gameplay ---------------------------------------------------------
        {"Gameplay", "Player start", "Where the player appears when Play begins, facing "
         "its -Z.", "spawn fps begin start", T::Empty, {0.4f, 0.9f, 0.4f}, false, 0.0f,
         "player_start"},
        {"Gameplay", "Trigger", "Fires when the player walks in: messages, sounds, scripts.",
         "zone area volume event", T::Empty, {1.0f, 1.0f, 1.0f}, false, 0.0f, "trigger"},
        {"Gameplay", "Scene trigger", "Loads another scene when the player walks in.",
         "level exit portal door load", T::Empty, {1.0f, 1.0f, 1.0f}, false, 0.0f, "scene_trigger"},
        {"Gameplay", "Spawner", "Emits objects on a timer while playing.",
         "emit fountain enemies", T::Empty, {0.5f, 0.5f, 0.5f}, false, 0.0f, "spawner"},
        {"Gameplay", "Collectible", "A pickup: walk into it for points.",
         "coin pickup item score", T::Sphere, {0.3f, 0.3f, 0.3f}, false, 0.6f, "collectible"},
        {"Gameplay", "Door", "Swings open when told to (a Door opener, or open at start).",
         "gate", T::Box, {0.5f, 1.05f, 0.05f}, false, 0.0f, "door"},
        {"Gameplay", "Door opener", "Opens a door while the player is near.",
         "button switch", T::Empty, {1.0f, 1.0f, 1.0f}, false, 0.0f, "door_opener"},
        {"Gameplay", "Lift", "A platform that rises while the player stands on it.",
         "elevator platform", T::Box, {1.2f, 0.1f, 1.2f}, false, 0.0f, "lift"},
        {"Gameplay", "Mover", "Moves back and forth on its own.", "platform moving oscillate",
         T::Box, {1.0f, 0.15f, 1.0f}, false, 0.5f, "mover"},
        {"Gameplay", "Script", "An object that runs a Lua script.", "lua code logic",
         T::Empty, {0.5f, 0.5f, 0.5f}, false, 0.0f, "script"},

        // --- Physics ----------------------------------------------------------
        {"Physics", "Physics box", "A box that falls, tumbles and can be pushed.",
         "crate dynamic rigid body", T::Box, {0.4f, 0.4f, 0.4f}, false, 0.5f, "physics",
         dynamicBody},
        {"Physics", "Physics ball", "A ball that rolls.", "dynamic rigid body sphere",
         T::Sphere, {0.3f, 0.3f, 0.3f}, false, 0.5f, "physics", dynamicBody},
        {"Physics", "Static collider", "A solid that does not move: walls, floors, blockers.",
         "wall collision static", T::Box, {0.5f, 0.5f, 0.5f}, true, 0.0f, "physics", staticBody},
        {"Physics", "Floating box", "A crate that floats on water.", "buoyancy water boat",
         T::Box, {0.4f, 0.4f, 0.4f}, false, 0.5f, "physics", floatingBox},
        {"Physics", "Wave maker", "Makes waves where it moves through water.", "water ripple",
         T::Empty, {0.5f, 0.5f, 0.5f}, false, 0.0f, "wave_maker"},

        // --- Audio ------------------------------------------------------------
        {"Audio", "Audio source", "Plays a sound or music from here.", "sound music speaker",
         T::Empty, {0.5f, 0.5f, 0.5f}, false, 0.5f, "audio_source"},
        {"Audio", "Sound zone", "Plays while the player is near: a waterfall, a machine.",
         "ambient sound area", T::Empty, {2.0f, 2.0f, 2.0f}, false, 0.0f, "trigger_sound"},
        {"Audio", "Reverb zone", "A room the sound rings in. Size it with the gizmo.",
         "echo hall room", T::Empty, {5.0f, 3.0f, 5.0f}, false, 0.0f, "reverb_zone"},
        {"Audio", "Synth", "A playable synthesizer voice.", "music instrument note",
         T::Empty, {0.5f, 0.5f, 0.5f}, false, 0.5f, "synth"},

        // --- Effects ----------------------------------------------------------
        {"Effects", "Particles", "Smoke, sparks, fire, dust.", "emitter smoke fire sparks",
         T::Empty, {0.5f, 0.5f, 0.5f}, false, 0.0f, "particles"},
        {"Effects", "Volumetric fog", "A body of mist filling the box.", "mist haze volume",
         T::Empty, {8.0f, 3.0f, 8.0f}, false, 0.0f, "volumetric_fog"},
        {"Effects", "Decal", "Lays its material's image on whatever is in the box.",
         "sticker projection graffiti", T::Box, {1.0f, 0.5f, 1.0f}, false, -0.5f, "decal"},

        // --- Racing -----------------------------------------------------------
        {"Racing", "Start / finish", "Where a lap begins and ends.", "lap line race",
         T::Empty, {4.0f, 3.0f, 0.3f}, false, 0.0f, "finish_line"},
        {"Racing", "Checkpoint", "A gate every lap has to pass.", "gate lap race",
         T::Empty, {4.0f, 3.0f, 0.3f}, false, 0.0f, "checkpoint"},
        {"Racing", "Grid position", "A starting slot on the grid.", "start slot race",
         T::Empty, {1.0f, 0.5f, 2.0f}, false, 0.0f, "grid_position"},
        {"Racing", "Boost pad", "Speeds up whatever drives over it.", "speed race",
         T::Box, {1.5f, 0.05f, 3.0f}, false, 0.0f, "boost_pad"},

        // --- World ------------------------------------------------------------
        {"World", "Terrain", "The scene's ground (one per scene: selects it if there is "
         "one).", "ground landscape", T::Empty, {0.5f, 0.5f, 0.5f}, false, 0.0f, nullptr,
         nullptr, S::Terrain},
    };
    return items;
}

bool matches(const Item& it, const char* f) {
    if (!f || !f[0]) return true;
    // Every word of the filter has to be found somewhere: "spot light" finds the
    // Spot light, "light" all three.
    std::string word;
    const std::string all = std::string(it.label) + " " + it.category + " " +
                            (it.keywords ? it.keywords : "");
    auto found = [&](const std::string& w) { return w.empty() || ui::icontains(all.c_str(), w.c_str()); };
    for (const char* p = f;; ++p) {
        if (*p == ' ' || *p == '\0') {
            if (!found(word)) return false;
            word.clear();
            if (*p == '\0') break;
        } else {
            word += *p;
        }
    }
    return true;
}

Entity make(const Item& it, int id, const Target& t, const glm::vec3& newHalf) {
    Entity e;
    e.type = it.type;
    e.half = it.newSize ? newHalf : it.half;
    if (it.type == EntityType::Plane) e.half.y = sceneops::kPlaneHalfY;
    if (it.component)
        if (auto c = components::create(it.component)) e.components.items.push_back(std::move(c));
    // A Light entity draws from its Light component: never one without it.
    if (it.type == EntityType::Light && !e.components.get<LightComponent>())
        e.components.items.push_back(std::make_unique<LightComponent>());
    if (it.setup) it.setup(e);
    e.id   = id;
    e.name = std::string(it.label) + " " + std::to_string(id);
    e.localCenter = e.center =
        t.onGround ? t.at + glm::vec3(0.0f, e.half.y + it.lift, 0.0f) : t.at;
    return e;
}

bool body(const Actions& a, const Target& t) {
    static char filter[64] = "";
    const float em = ImGui::GetFontSize();
    if (ImGui::IsWindowAppearing()) {
        filter[0] = '\0';
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::PushItemWidth(em * 17.0f);
    ui::searchBox("##addFilter", filter, sizeof filter, "Search...");
    ImGui::PopItemWidth();
    const bool enter = ImGui::IsItemFocused() &&
                       (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));

    auto offered = [&](const Item& it) {
        if (it.special == Special::CameraFromView && !a.cameraFromView) return false;
        if (it.special == Special::Terrain && !a.terrain) return false;
        return matches(it, filter);
    };
    const auto& items = catalog();
    int rows = 0, heads = 0;
    const Item* first = nullptr;
    const char* lastCat = nullptr;
    for (const Item& it : items) {
        if (!offered(it)) continue;
        if (!first) first = &it;
        ++rows;
        if (!lastCat || std::string(lastCat) != it.category) { ++heads; lastCat = it.category; }
    }

    const Item* chosen = nullptr;
    if (enter && first) chosen = first;

    const float rowH  = ImGui::GetTextLineHeightWithSpacing() + 2.0f;
    const float headH = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y + 4.0f;
    const float want  = rows * rowH + heads * headH + ImGui::GetStyle().WindowPadding.y * 2.0f;
    const float h     = std::clamp(want, rowH * 2.0f, em * 24.0f);
    if (ImGui::BeginChild("##addList", ImVec2(em * 17.0f, h), ImGuiChildFlags_NavFlattened)) {
        if (rows == 0) ImGui::TextDisabled("Nothing called \"%s\".", filter);
        lastCat = nullptr;
        for (const Item& it : items) {
            if (!offered(it)) continue;
            if (!lastCat || std::string(lastCat) != it.category) {
                lastCat = it.category;
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::SeparatorText(it.category);
                ImGui::PopStyleColor();
            }
            ImGui::PushID(&it);
            // A dot in the category's colour, then the name. With a filter the
            // first row is lit: that is the one Enter adds.
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float dot = em * 0.22f;
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2(p.x + dot + 2.0f, p.y + ImGui::GetTextLineHeight() * 0.5f + 1.0f), dot,
                categoryColor(it.category));
            ImGui::SetCursorScreenPos(ImVec2(p.x + dot * 2.0f + 8.0f, p.y));
            const bool lit = filter[0] && &it == first;
            if (ImGui::Selectable(it.label, lit, ImGuiSelectableFlags_None, ImVec2(0.0f, 0.0f)))
                chosen = &it;
            if (it.hint && it.hint[0]) ImGui::SetItemTooltip("%s", it.hint);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    if (!chosen) return false;
    if (a.add) a.add(*chosen, t);
    ImGui::CloseCurrentPopup();
    return true;
}

void viewport(const ViewportFrame& view, bool editing, const ImVec2* rightClick,
              const Actions& a) {
    static Target target;
    const ImGuiIO& io = ImGui::GetIO();
    ImVec2 at(-1.0f, -1.0f);
    if (editing && rightClick) at = *rightClick;
    if (editing && view.hovered && !io.WantTextInput && io.KeyShift && !io.KeyCtrl &&
        !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_A, false))
        at = io.MousePos;
    if (at.x >= 0.0f) {
        // The ground under that point: the terrain, or failing that (the sky,
        // no terrain) a spot ten metres out along the ray, on the ground there.
        const glm::vec2 ndc((at.x - view.origin.x) / view.w * 2.0f - 1.0f,
                            1.0f - (at.y - view.origin.y) / view.h * 2.0f);
        glm::vec3 hit;
        target = Target{};
        if (view.pickTerrain && view.pickTerrain(ndc, view.viewProj, hit)) {
            target.at = hit;
        } else {
            const glm::mat4 inv = glm::inverse(view.viewProj);
            glm::vec4 n = inv * glm::vec4(ndc, -1.0f, 1.0f), f = inv * glm::vec4(ndc, 1.0f, 1.0f);
            const glm::vec3 o = glm::vec3(n) / n.w;
            const glm::vec3 d = glm::normalize(glm::vec3(f) / f.w - o);
            glm::vec3 p = o + d * 10.0f;
            if (d.y < -1e-3f && o.y > 0.0f) p = o + d * std::min(10.0f, -o.y / d.y);
            if (view.groundAt) p.y = std::min(p.y, view.groundAt(p.x, p.z));
            target.at = p;
        }
        ImGui::SetNextWindowPos(at);
        ImGui::OpenPopup("##addMenu");
    }
    if (ImGui::BeginPopup("##addMenu")) {
        ImGui::TextDisabled("Add");
        body(a, target);
        ImGui::EndPopup();
    }
}

} // namespace addmenu
