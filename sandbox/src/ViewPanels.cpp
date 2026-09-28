#include "ViewPanels.hpp"

#include <imgui.h>

#include <fitzel/render/Renderer.hpp>
#include <fitzel/scene/Camera.hpp>
#include <fitzel/world/Terrain.hpp>

namespace viewui {

void drawStats(const StatsState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Stats", &s.show)) {
        const char* sceneNames[] = {"Nature", "Empty (build)"};
        if (ImGui::Combo("Scene", &s.scene, sceneNames, 2) && s.applyScene) s.applyScene(s.scene);
        ImGui::Separator();
        ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate,
                    1000.0f / ImGui::GetIO().Framerate);
        ImGui::Text("Camera: %.0f, %.0f, %.0f",
                    s.camera.position().x, s.camera.position().y, s.camera.position().z);
        ImGui::Text("Chunks: %d loaded, %d pending",
                    s.streamer.loadedChunkCount(), s.streamer.pendingChunkCount());
        ImGui::Text("Entities: %d (%d selected)", s.entityCount, s.selectedCount);
        ImGui::Text("Draws: %d visible, %d culled", s.renderer.lastDrawn(), s.renderer.lastCulled());
        ImGui::Separator();
        ImGui::SliderFloat("Move speed", &s.camera.moveSpeed, 2.0f, 80.0f);
        ImGui::SliderInt("View distance", &s.viewRadius, 2, 9, "%d chunks");
        ImGui::SameLine();
        ImGui::Text("(%.0f m)", s.viewRadius * s.streamer.settings().chunkSize);
        // The culling limit. Deliberately next to View distance and the draw
        // counters above: those three are one dial each on the same trade, and
        // the counters are the readout you tune against.
        ImGui::Checkbox("Auto far plane", &s.farPlaneAuto);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Tie the culling limit to the streamed terrain\n"
                              "(1.7 chunks past the ring, so its corners\n"
                              "stay inside the frustum) -- and to the\n"
                              "roadside city's range, whichever reaches\n"
                              "further, so a skyline is never sliced off\n"
                              "before its own range runs out. Turn off to\n"
                              "set it by hand.");
        ImGui::BeginDisabled(s.farPlaneAuto);
        ImGui::SliderFloat("Far plane", &s.farPlaneManual, 100.0f, 5000.0f, "%.0f m");
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Where the camera stops drawing. Past the\n"
                              "streamed terrain you see the ring end;\n"
                              "past ~2000 m the depth buffer starts\n"
                              "fighting itself in the distance (the near\n"
                              "plane is 0.1 m). Set by hand this does NOT\n"
                              "follow the city's range, so a city set to\n"
                              "reach further than this gets cut off here.\n"
                              "Shadows stop at the streamed terrain either\n"
                              "way -- pushing this out costs draws, not\n"
                              "shadow resolution.");
        ImGui::SameLine();
        ImGui::TextDisabled("now %.0f m", s.camera.farPlane());
        ImGui::Separator();
        if (ImGui::Button("Reset layout")) s.resetLayout = true;
    }
    ImGui::End();
}

void drawCamera(const CameraState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Camera", &s.show)) {
        if (ImGui::Checkbox("First-person (Shift+F)", &s.fpsMode) && s.setFirstPerson)
            s.setFirstPerson(s.fpsMode);
        ImGui::SameLine();
        ImGui::TextDisabled(s.fpsMode ? "(walk + jump, Esc to exit)"
                                      : "(hold right mouse: look + WASD/QE fly)");
        // Read from the camera every frame (mouse-look may have changed it), and
        // written back only when a slider is actually edited.
        float fov = s.camera.fov(), yaw = s.camera.yaw(), pitch = s.camera.pitch();
        if (ImGui::SliderFloat("FOV",   &fov,   25.0f, 100.0f, "%.0f deg")) s.camera.setFov(fov);
        if (ImGui::SliderFloat("Yaw",   &yaw,  -180.0f, 180.0f, "%.0f"))    s.camera.setYaw(yaw);
        if (ImGui::SliderFloat("Pitch", &pitch, -89.0f, 89.0f, "%.0f"))     s.camera.setPitch(pitch);
    }
    ImGui::End();
}

} // namespace viewui
