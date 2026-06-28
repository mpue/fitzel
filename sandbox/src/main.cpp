#include <cmath>
#include <cstdio>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <fitzel/Fitzel.hpp>

using namespace fitzel;

int main() {
    try {
        Window window(WindowConfig{
            .width  = 1280,
            .height = 720,
            .title  = "Fitzel - Infinite Terrain, CSM, Materials",
            .vsync  = true,
        });

        Input  input(window);                  // before Gui (callback chaining)
        Gui    gui(window);
        Camera camera({0.0f, 45.0f, 90.0f}, -90.0f, -25.0f);
        camera.moveSpeed = 20.0f;

        Shader lit = Shader::fromFiles("assets/shaders/lit.vert",
                                       "assets/shaders/lit.frag");
        if (!lit.isValid()) {
            std::fprintf(stderr, "Failed to load lit shader\n");
            return 1;
        }

        // Shared assets.
        Mesh    cube    = Mesh::cube();
        Texture texture = Texture::checkerboard(256, 8);

        // Materials describe surface appearance; the renderer feeds in lighting.
        Material terrainMat(lit);
        terrainMat.set("uColorMode", 1);

        Material cubeMat(lit);
        cubeMat.set("uColorMode", 2).setTexture("uTexture", texture, 0);

        // World streaming + renderer with cascaded shadows.
        TerrainSettings settings;
        TerrainStreamer streamer(settings, /*radius=*/4);
        Renderer        renderer(2048, 4);
        DirectionalLight light;

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);

        std::puts("[Fitzel] WASD/QE = move, hold RMB = look, ESC = quit");

        // A scatter of shadow-casting cubes resting on the terrain.
        std::vector<glm::vec2> cubeSpots;
        for (int z = -2; z <= 2; ++z)
            for (int x = -2; x <= 2; ++x)
                if ((x + z) % 2 == 0)
                    cubeSpots.emplace_back(x * 34.0f + 7.0f, z * 34.0f - 5.0f);

        TerrainSettings uiSettings = settings; // editable copy for the panel
        double lastTime = window.time();

        while (window.isOpen()) {
            window.pollEvents();
            input.update();

            const double now = window.time();
            const float  dt  = static_cast<float>(now - lastTime);
            lastTime = now;

            // --- Input ---------------------------------------------------
            if (input.isKeyDown(GLFW_KEY_ESCAPE)) window.requestClose();

            const bool look = input.isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)
                              && !gui.wantsMouse();
            if (look != input.isCursorLocked()) input.setCursorLocked(look);
            if (look) {
                const glm::vec2 d = input.mouseDelta();
                camera.processMouse(d.x, d.y);
            }
            if (!gui.wantsMouse())    camera.processScroll(input.scrollDelta());
            if (!gui.wantsKeyboard()) {
                if (input.isKeyDown(GLFW_KEY_W)) camera.processKeyboard(Camera::Direction::Forward, dt);
                if (input.isKeyDown(GLFW_KEY_S)) camera.processKeyboard(Camera::Direction::Backward, dt);
                if (input.isKeyDown(GLFW_KEY_A)) camera.processKeyboard(Camera::Direction::Left, dt);
                if (input.isKeyDown(GLFW_KEY_D)) camera.processKeyboard(Camera::Direction::Right, dt);
                if (input.isKeyDown(GLFW_KEY_E)) camera.processKeyboard(Camera::Direction::Up, dt);
                if (input.isKeyDown(GLFW_KEY_Q)) camera.processKeyboard(Camera::Direction::Down, dt);
            }

            // Stream terrain chunks around the camera.
            streamer.update(camera.position());

            // --- UI ------------------------------------------------------
            gui.beginFrame();
            if (ImGui::Begin("Fitzel")) {
                ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate,
                            1000.0f / ImGui::GetIO().Framerate);
                ImGui::Text("Camera: %.0f, %.0f, %.0f",
                            camera.position().x, camera.position().y, camera.position().z);
                ImGui::Text("Loaded chunks: %d", streamer.loadedChunkCount());
                ImGui::SliderFloat("Move speed", &camera.moveSpeed, 2.0f, 80.0f);

                if (ImGui::CollapsingHeader("Lighting & Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat3("Light dir", &light.direction.x, -1.0f, 1.0f);
                    ImGui::ColorEdit3("Light color", &light.color.x);
                    ImGui::SliderFloat("Cascade split", &renderer.shadows().splitLambda, 0.0f, 1.0f);
                }
                if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat("Height",    &uiSettings.heightScale, 0.0f, 30.0f);
                    ImGui::SliderFloat("Frequency", &uiSettings.frequency, 0.003f, 0.05f, "%.3f");
                    ImGui::SliderInt  ("Octaves",   &uiSettings.octaves, 1, 8);
                    ImGui::SliderFloat("Seed",      &uiSettings.seed, 0.0f, 100.0f);
                    if (ImGui::Button("Regenerate")) {
                        streamer.settings() = uiSettings;
                        streamer.rebuild();
                        streamer.update(camera.position());
                    }
                }
            }
            ImGui::End();

            // --- Submit + render ----------------------------------------
            int fbW = 0, fbH = 0;
            window.framebufferSize(fbW, fbH);
            renderer.setViewport(fbW, fbH);

            glClearColor(0.55f, 0.70f, 0.92f, 1.0f); // sky
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            renderer.begin(camera, window.aspectRatio(), light);

            for (const TerrainChunk* chunk : streamer.visibleChunks()) {
                renderer.submit(chunk->mesh(), terrainMat, glm::mat4(1.0f));
            }

            // Static cubes resting on the ground.
            for (const glm::vec2& spot : cubeSpots) {
                const float gy = streamer.heightAt(spot.x, spot.y);
                glm::mat4 m(1.0f);
                m = glm::translate(m, {spot.x, gy + 2.0f, spot.y});
                m = glm::scale(m, glm::vec3(4.0f));
                renderer.submit(cube, cubeMat, m);
            }

            // One floating, spinning cube to show shadows cast onto terrain.
            {
                const float gy  = streamer.heightAt(0.0f, 0.0f);
                const float bob = std::sin(static_cast<float>(now)) * 2.0f;
                glm::mat4 m(1.0f);
                m = glm::translate(m, {0.0f, gy + 14.0f + bob, 0.0f});
                m = glm::rotate(m, static_cast<float>(now) * 0.6f,
                                glm::normalize(glm::vec3(0.4f, 1.0f, 0.2f)));
                m = glm::scale(m, glm::vec3(6.0f));
                renderer.submit(cube, cubeMat, m);
            }

            renderer.end();

            gui.endFrame();
            window.swapBuffers();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }

    return 0;
}
