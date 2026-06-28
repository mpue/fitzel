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
            .title  = "Fitzel Sandbox",
            .vsync  = true,
        });

        Input  input(window);   // before Gui, so ImGui chains our callbacks
        Gui    gui(window);
        Camera camera({0.0f, 0.0f, 4.0f});

        // Shaders are copied next to the executable by CMake.
        Shader shader = Shader::fromFiles("assets/shaders/basic.vert",
                                          "assets/shaders/basic.frag");
        if (!shader.isValid()) {
            std::fprintf(stderr, "Failed to load shaders\n");
            return 1;
        }

        Mesh    cube    = Mesh::cube();
        Texture texture = Texture::checkerboard(256, 8);

        glEnable(GL_DEPTH_TEST);

        std::puts("[Fitzel] WASD = move, hold RMB = look, Q/E = down/up, ESC = quit");

        // Tweakable state, driven by the ImGui panel below.
        glm::vec3 lightDir{0.4f, 1.0f, 0.6f};
        glm::vec3 clearColor{0.07f, 0.08f, 0.10f};
        float     rotationSpeed = 0.5f;
        bool      wireframe     = false;

        double lastTime = window.time();

        while (window.isOpen()) {
            window.pollEvents();
            input.update();

            const double now = window.time();
            const float  dt  = static_cast<float>(now - lastTime);
            lastTime = now;

            // --- Input ---------------------------------------------------
            if (input.isKeyDown(GLFW_KEY_ESCAPE)) {
                window.requestClose();
            }

            // Hold right mouse to look around, unless ImGui wants the mouse.
            const bool look = input.isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)
                              && !gui.wantsMouse();
            if (look != input.isCursorLocked()) {
                input.setCursorLocked(look);
            }
            if (look) {
                const glm::vec2 d = input.mouseDelta();
                camera.processMouse(d.x, d.y);
            }
            if (!gui.wantsMouse()) {
                camera.processScroll(input.scrollDelta());
            }
            if (!gui.wantsKeyboard()) {
                if (input.isKeyDown(GLFW_KEY_W)) camera.processKeyboard(Camera::Direction::Forward, dt);
                if (input.isKeyDown(GLFW_KEY_S)) camera.processKeyboard(Camera::Direction::Backward, dt);
                if (input.isKeyDown(GLFW_KEY_A)) camera.processKeyboard(Camera::Direction::Left, dt);
                if (input.isKeyDown(GLFW_KEY_D)) camera.processKeyboard(Camera::Direction::Right, dt);
                if (input.isKeyDown(GLFW_KEY_E)) camera.processKeyboard(Camera::Direction::Up, dt);
                if (input.isKeyDown(GLFW_KEY_Q)) camera.processKeyboard(Camera::Direction::Down, dt);
            }

            // --- UI ------------------------------------------------------
            gui.beginFrame();
            if (ImGui::Begin("Fitzel")) {
                ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate,
                            1000.0f / ImGui::GetIO().Framerate);
                ImGui::Separator();
                ImGui::Text("Camera: %.2f, %.2f, %.2f",
                            camera.position().x, camera.position().y, camera.position().z);
                ImGui::SliderFloat("Move speed", &camera.moveSpeed, 0.5f, 20.0f);
                ImGui::Separator();
                ImGui::SliderFloat3("Light dir", &lightDir.x, -1.0f, 1.0f);
                ImGui::SliderFloat("Rotation", &rotationSpeed, 0.0f, 3.0f);
                ImGui::ColorEdit3("Background", &clearColor.x);
                ImGui::Checkbox("Wireframe", &wireframe);
            }
            ImGui::End();

            // --- Render --------------------------------------------------
            glPolygonMode(GL_FRONT_AND_BACK, wireframe ? GL_LINE : GL_FILL);
            glClearColor(clearColor.r, clearColor.g, clearColor.b, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            const glm::mat4 model =
                glm::rotate(glm::mat4(1.0f), static_cast<float>(now) * rotationSpeed,
                            glm::normalize(glm::vec3(0.5f, 1.0f, 0.0f)));
            const glm::mat4 viewProj =
                camera.projectionMatrix(window.aspectRatio()) * camera.viewMatrix();

            shader.bind();
            shader.setMat4("uModel", model);
            shader.setMat4("uViewProj", viewProj);
            shader.setVec3("uLightDir", lightDir);
            shader.setInt("uTexture", 0);
            texture.bind(0);
            cube.draw();

            // UI draws on top of the scene; reset polygon mode first.
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
