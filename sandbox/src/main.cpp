#include <cmath>
#include <cstdio>

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
            .title  = "Fitzel - Terrain & Shadows",
            .vsync  = true,
        });

        Input  input(window);                 // before Gui (callback chaining)
        Gui    gui(window);
        Camera camera({0.0f, 16.0f, 42.0f}, -90.0f, -20.0f);

        Shader lit   = Shader::fromFiles("assets/shaders/lit.vert",
                                         "assets/shaders/lit.frag");
        Shader depth = Shader::fromFiles("assets/shaders/depth.vert",
                                         "assets/shaders/depth.frag");
        if (!lit.isValid() || !depth.isValid()) {
            std::fprintf(stderr, "Failed to load shaders\n");
            return 1;
        }

        // Scene content.
        TerrainParams terrainParams;
        Terrain  terrain = Terrain::generate(terrainParams);
        Mesh     cube    = Mesh::cube();
        Texture  texture = Texture::checkerboard(256, 8);
        ShadowMap shadow(2048);

        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);

        std::puts("[Fitzel] WASD/QE = move, hold RMB = look, ESC = quit");

        // Tweakable state.
        glm::vec3 lightDir{0.5f, 1.0f, 0.35f};
        glm::vec3 lightColor{1.0f, 0.97f, 0.9f};
        bool      wireframe   = false;
        bool      animateCube = true;

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

            // --- UI ------------------------------------------------------
            bool regenerate = false;
            gui.beginFrame();
            if (ImGui::Begin("Fitzel")) {
                ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate,
                            1000.0f / ImGui::GetIO().Framerate);
                ImGui::Text("Camera: %.1f, %.1f, %.1f",
                            camera.position().x, camera.position().y, camera.position().z);
                ImGui::SliderFloat("Move speed", &camera.moveSpeed, 1.0f, 40.0f);

                if (ImGui::CollapsingHeader("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat3("Light dir", &lightDir.x, -1.0f, 1.0f);
                    ImGui::ColorEdit3("Light color", &lightColor.x);
                }
                if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat("Height",    &terrainParams.heightScale, 0.0f, 20.0f);
                    ImGui::SliderFloat("Frequency", &terrainParams.frequency, 0.005f, 0.15f, "%.3f");
                    ImGui::SliderInt  ("Octaves",   &terrainParams.octaves, 1, 8);
                    ImGui::SliderFloat("Seed",      &terrainParams.seed, 0.0f, 100.0f);
                    regenerate = ImGui::Button("Regenerate");
                }
                ImGui::Separator();
                ImGui::Checkbox("Wireframe", &wireframe);
                ImGui::SameLine();
                ImGui::Checkbox("Animate cube", &animateCube);
            }
            ImGui::End();

            if (regenerate) {
                terrain = Terrain::generate(terrainParams);
            }

            // --- Scene transforms ---------------------------------------
            static float cubeAngle = 0.0f;
            if (animateCube) cubeAngle += dt * 0.6f;
            const float bob = std::sin(static_cast<float>(now)) * 1.5f;
            const float groundY = terrain.heightAt(0.0f, 0.0f);

            glm::mat4 cubeModel(1.0f);
            cubeModel = glm::translate(cubeModel, {0.0f, groundY + 9.0f + bob, 0.0f});
            cubeModel = glm::rotate(cubeModel, cubeAngle,
                                    glm::normalize(glm::vec3(0.4f, 1.0f, 0.2f)));
            cubeModel = glm::scale(cubeModel, glm::vec3(3.0f));

            const glm::mat4 terrainModel(1.0f);

            // Light frustum bounds the whole terrain.
            const float     radius = terrainParams.worldSize * 0.72f;
            const glm::vec3  center{0.0f, 1.0f, 0.0f};
            const glm::mat4  lightSpace = shadow.lightSpaceMatrix(lightDir, center, radius);

            int fbW = 0, fbH = 0;
            window.framebufferSize(fbW, fbH);

            // --- Pass 1: depth from the light's POV ----------------------
            shadow.begin();
            depth.bind();
            depth.setMat4("uLightSpace", lightSpace);
            depth.setMat4("uModel", terrainModel);
            terrain.mesh().draw();
            depth.setMat4("uModel", cubeModel);
            cube.draw();
            shadow.end(fbW, fbH);

            // --- Pass 2: lit scene with shadows --------------------------
            glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
            glClearColor(0.55f, 0.70f, 0.92f, 1.0f); // sky
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            const glm::mat4 viewProj =
                camera.projectionMatrix(window.aspectRatio()) * camera.viewMatrix();

            lit.bind();
            lit.setMat4("uViewProj", viewProj);
            lit.setMat4("uLightSpace", lightSpace);
            lit.setVec3("uViewPos", camera.position());
            lit.setVec3("uLightDir", lightDir);
            lit.setVec3("uLightColor", lightColor);
            lit.setInt("uShadowMap", 1);
            lit.setInt("uTexture", 0);
            shadow.bindDepthTexture(1);

            // Terrain (height/slope palette).
            lit.setMat4("uModel", terrainModel);
            lit.setInt("uColorMode", 1);
            terrain.mesh().draw();

            // Cube (textured), our shadow caster.
            lit.setMat4("uModel", cubeModel);
            lit.setInt("uColorMode", 2);
            texture.bind(0);
            cube.draw();

            glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            gui.endFrame();

            window.swapBuffers();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }

    return 0;
}
