#include "ObjectIcons.hpp"

#include <algorithm>
#include <cmath>

#include "Component.hpp"
#include "Pictogram.hpp"
#include "SceneTypes.hpp"
#include "ToolbarIcons.hpp"

namespace objicon {

namespace {

constexpr float kPi = 3.14159265f;

float stroke(float r) { return std::max(1.5f, r * 0.15f); }

ImVec2 at(ImVec2 c, float r, float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); }

ImU32 fade(ImU32 col, float k) {
    const int a = static_cast<int>(((col >> IM_COL32_A_SHIFT) & 0xFF) * k);
    return (col & ~IM_COL32_A_MASK) | (static_cast<ImU32>(a) << IM_COL32_A_SHIFT);
}

void arrowHead(ImDrawList* dl, ImVec2 tip, ImVec2 dir, float s, ImU32 col) {
    const ImVec2 n(-dir.y, dir.x);
    dl->AddTriangleFilled(tip,
                          ImVec2(tip.x - dir.x * s + n.x * s * 0.8f, tip.y - dir.y * s + n.y * s * 0.8f),
                          ImVec2(tip.x - dir.x * s - n.x * s * 0.8f, tip.y - dir.y * s - n.y * s * 0.8f),
                          col);
}

// A flag on its pole: the pennant of a checkpoint, the chequered one of the
// finish.
void flag(ImDrawList* dl, ImVec2 c, float r, ImU32 col, bool chequered) {
    const float t = stroke(r);
    dl->AddLine(at(c, r, -0.62f, -0.9f), at(c, r, -0.62f, 0.92f), col, t);
    if (!chequered) {
        dl->AddTriangleFilled(at(c, r, -0.62f, -0.9f), at(c, r, 0.86f, -0.5f),
                              at(c, r, -0.62f, -0.1f), col);
        return;
    }
    const ImVec2 a = at(c, r, -0.62f, -0.9f), b = at(c, r, 0.88f, -0.06f);
    const float  w = (b.x - a.x) / 4.0f, h = (b.y - a.y) / 2.0f;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 4; ++x)
            if (((x + y) & 1) == 0)
                dl->AddRectFilled(ImVec2(a.x + w * x, a.y + h * y),
                                  ImVec2(a.x + w * (x + 1), a.y + h * (y + 1)), col);
    dl->AddRect(a, b, col, 0.0f, 0, t * 0.7f);
}

} // namespace

Kind kindOf(const Entity& e) {
    if (e.type == EntityType::Light) {
        const auto* l = e.components.get<LightComponent>();
        return (l && l->type == 1) ? Kind::SpotLight : Kind::PointLight;
    }
    if (e.type != EntityType::Empty) return Kind::None;
    const ComponentList& c = e.components;
    if (c.get<CameraComponent>())         return Kind::Camera;
    if (c.get<CameraSwitcherComponent>()) return Kind::CameraSwitcher;
    if (c.get<AudioSourceComponent>())    return Kind::AudioSource;
    if (c.get<TriggerSoundComponent>())   return Kind::SoundZone;
    if (c.get<ReverbZoneComponent>())     return Kind::ReverbZone;
    if (c.get<SynthComponent>() || c.get<SongPlayerComponent>()) return Kind::Synth;
    if (c.get<PlayerStartComponent>())    return Kind::PlayerStart;
    if (c.get<SceneTriggerComponent>())   return Kind::SceneTrigger;
    if (c.get<FinishLineComponent>())     return Kind::FinishLine;
    if (c.get<CheckpointComponent>())     return Kind::Checkpoint;
    if (c.get<TriggerComponent>())        return Kind::Trigger;
    if (c.get<DoorOpenerComponent>())     return Kind::DoorOpener;
    if (c.get<SpawnerComponent>())        return Kind::Spawner;
    if (c.get<ParticleComponent>())       return Kind::Particles;
    if (c.get<VolumetricFogComponent>())  return Kind::Fog;
    if (c.get<ScriptComponent>())         return Kind::Script;
    return Kind::Empty;
}

ImU32 tint(Kind k) {
    // The Add menu's category colours (AddMenu.cpp, categoryColor).
    switch (k) {
        case Kind::PointLight: case Kind::SpotLight:
            return IM_COL32(250, 210, 90, 255);
        case Kind::Camera: case Kind::CameraSwitcher:
            return IM_COL32(120, 190, 250, 255);
        case Kind::AudioSource: case Kind::SoundZone: case Kind::ReverbZone: case Kind::Synth:
            return IM_COL32(200, 140, 240, 255);
        case Kind::PlayerStart: case Kind::Trigger: case Kind::SceneTrigger:
        case Kind::Spawner: case Kind::DoorOpener: case Kind::Script:
            return IM_COL32(120, 220, 140, 255);
        case Kind::Particles: case Kind::Fog:
            return IM_COL32(240, 120, 180, 255);
        case Kind::Checkpoint: case Kind::FinishLine:
            return IM_COL32(230, 90, 90, 255);
        case Kind::Empty: case Kind::None:
            break;
    }
    return IM_COL32(170, 175, 185, 255);
}

void glyph(ImDrawList* dl, Kind k, ImVec2 c, float r, ImU32 col, ImU32 fill) {
    const float t = stroke(r);
    switch (k) {
    case Kind::None:
        break;
    case Kind::Empty:
        icon::shape(dl, EntityType::Empty, c, r, col);
        break;

    // --- lights: filled with the colour they shine ---------------------------
    case Kind::PointLight: {
        // A bulb: the glass, then the screw base under it.
        const ImVec2 g = at(c, r, 0.0f, -0.24f);
        dl->AddCircleFilled(g, r * 0.6f, fill);
        dl->AddCircle(g, r * 0.6f, col, 0, t);
        dl->AddRectFilled(at(c, r, -0.3f, 0.42f), at(c, r, 0.3f, 0.64f), col, r * 0.06f);
        dl->AddRectFilled(at(c, r, -0.17f, 0.72f), at(c, r, 0.17f, 0.9f), col, r * 0.08f);
        break;
    }
    case Kind::SpotLight: {
        // A lamp head pointing down, and the cone of light out of it.
        const ImVec2 beam[4] = {at(c, r, -0.62f, -0.1f), at(c, r, 0.62f, -0.1f),
                                at(c, r, 0.95f, 0.9f), at(c, r, -0.95f, 0.9f)};
        dl->AddConvexPolyFilled(beam, 4, fade(fill, 0.6f));
        const ImVec2 head[4] = {at(c, r, -0.3f, -0.9f), at(c, r, 0.3f, -0.9f),
                                at(c, r, 0.66f, -0.1f), at(c, r, -0.66f, -0.1f)};
        dl->AddConvexPolyFilled(head, 4, col);
        break;
    }

    // --- cameras -------------------------------------------------------------
    case Kind::Camera:
        icon::camera(dl, at(c, r, 0.0f, 0.06f), r, col);
        break;
    case Kind::CameraSwitcher: {
        // A camera over a two-way arrow: it cuts from one to another.
        icon::camera(dl, at(c, r, 0.0f, -0.22f), r * 0.66f, col);
        const ImVec2 a = at(c, r, -0.9f, 0.72f), b = at(c, r, 0.9f, 0.72f);
        dl->AddLine(ImVec2(a.x + r * 0.3f, a.y), ImVec2(b.x - r * 0.3f, b.y), col, t);
        arrowHead(dl, a, ImVec2(-1.0f, 0.0f), r * 0.34f, col);
        arrowHead(dl, b, ImVec2(1.0f, 0.0f), r * 0.34f, col);
        break;
    }

    // --- sound ---------------------------------------------------------------
    case Kind::AudioSource:
        picto::draw(dl, picto::Icon::Speaker, at(c, r, 0.08f, 0.0f), r, col);
        break;
    case Kind::SoundZone:
        // The speaker inside the ring it is heard in.
        picto::draw(dl, picto::Icon::Speaker, at(c, r, 0.06f, 0.0f), r * 0.6f, col);
        for (int s = 0; s < 12; s += 2) {
            const float a0 = s * kPi / 6.0f, a1 = (s + 1) * kPi / 6.0f;
            dl->PathArcTo(c, r * 0.95f, a0, a1, 5);
            dl->PathStroke(col, ImDrawFlags_None, t * 0.8f);
        }
        break;
    case Kind::ReverbZone: {
        // A sound between two walls, ringing out to both sides.
        dl->AddLine(at(c, r, -0.95f, -0.8f), at(c, r, -0.95f, 0.8f), col, t);
        dl->AddLine(at(c, r, 0.95f, -0.8f), at(c, r, 0.95f, 0.8f), col, t);
        dl->AddCircleFilled(c, r * 0.15f, col);
        for (int i = 0; i < 2; ++i) {
            const float rad = r * (0.38f + 0.3f * i);
            dl->PathArcTo(c, rad, -0.75f, 0.75f, 10);
            dl->PathStroke(col, ImDrawFlags_None, t * 0.85f);
            dl->PathArcTo(c, rad, kPi - 0.75f, kPi + 0.75f, 10);
            dl->PathStroke(col, ImDrawFlags_None, t * 0.85f);
        }
        break;
    }
    case Kind::Synth:
        picto::draw(dl, picto::Icon::Keys, c, r, col);
        break;

    // --- gameplay ------------------------------------------------------------
    case Kind::PlayerStart:
        // A figure: head and shoulders.
        dl->AddCircleFilled(at(c, r, 0.0f, -0.52f), r * 0.3f, col);
        dl->AddRectFilled(at(c, r, -0.62f, -0.1f), at(c, r, 0.62f, 0.88f), col, r * 0.5f,
                          ImDrawFlags_RoundCornersTop);
        break;
    case Kind::Trigger: {
        // A lightning bolt: something happens here.
        const ImVec2 bolt[6] = {at(c, r, 0.28f, -0.94f), at(c, r, -0.5f, 0.14f),
                                at(c, r, -0.04f, 0.14f), at(c, r, -0.26f, 0.94f),
                                at(c, r, 0.52f, -0.16f), at(c, r, 0.06f, -0.16f)};
        dl->AddConcavePolyFilled(bolt, 6, col);
        break;
    }
    case Kind::SceneTrigger:
        // A doorway with an arrow going out through it: on to another scene.
        dl->AddRect(at(c, r, -0.82f, -0.86f), at(c, r, 0.12f, 0.86f), col, 0.0f, 0, t);
        dl->AddLine(at(c, r, -0.3f, 0.0f), at(c, r, 0.62f, 0.0f), col, t);
        arrowHead(dl, at(c, r, 0.96f, 0.0f), ImVec2(1.0f, 0.0f), r * 0.4f, col);
        break;
    case Kind::Spawner:
        // A ring with a plus: more of something, made here.
        dl->AddCircle(c, r * 0.84f, col, 0, t);
        dl->AddLine(at(c, r, -0.46f, 0.0f), at(c, r, 0.46f, 0.0f), col, t * 1.2f);
        dl->AddLine(at(c, r, 0.0f, -0.46f), at(c, r, 0.0f, 0.46f), col, t * 1.2f);
        break;
    case Kind::DoorOpener:
        // A door with its handle.
        dl->AddRect(at(c, r, -0.5f, -0.88f), at(c, r, 0.5f, 0.88f), col, r * 0.06f, 0, t);
        dl->AddCircleFilled(at(c, r, 0.24f, 0.06f), r * 0.13f, col);
        break;
    case Kind::Script:
        // </> -- code.
        dl->AddLine(at(c, r, -0.38f, -0.56f), at(c, r, -0.92f, 0.0f), col, t);
        dl->AddLine(at(c, r, -0.92f, 0.0f), at(c, r, -0.38f, 0.56f), col, t);
        dl->AddLine(at(c, r, 0.38f, -0.56f), at(c, r, 0.92f, 0.0f), col, t);
        dl->AddLine(at(c, r, 0.92f, 0.0f), at(c, r, 0.38f, 0.56f), col, t);
        dl->AddLine(at(c, r, 0.2f, -0.78f), at(c, r, -0.2f, 0.78f), col, t);
        break;

    // --- effects -------------------------------------------------------------
    case Kind::Particles:
        icon::postFx(dl, c, r, col);
        break;
    case Kind::Fog:
        // Three drifting wisps.
        for (int row = 0; row < 3; ++row) {
            ImVec2 pts[13];
            const float y0 = -0.52f + 0.52f * row;
            const float x0 = (row == 1) ? -0.92f : -0.72f, x1 = (row == 1) ? 0.92f : 0.72f;
            for (int i = 0; i <= 12; ++i) {
                const float x = x0 + (x1 - x0) * static_cast<float>(i) / 12.0f;
                pts[i] = at(c, r, x, y0 + 0.13f * std::sin(x * kPi * 1.6f + row * 1.7f));
            }
            dl->AddPolyline(pts, 13, col, ImDrawFlags_None, t);
        }
        break;

    // --- racing --------------------------------------------------------------
    case Kind::Checkpoint:
        flag(dl, c, r, col, false);
        break;
    case Kind::FinishLine:
        flag(dl, c, r, col, true);
        break;
    }
}

} // namespace objicon
