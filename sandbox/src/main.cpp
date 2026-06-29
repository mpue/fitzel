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
        Camera camera({0.0f, 10.0f, 78.0f}, -90.0f, -8.0f);
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

        // Slope/height-driven terrain palette, exposed as material parameters.
        struct TerrainLook {
            glm::vec3 sand{0.76f, 0.70f, 0.48f};
            glm::vec3 grass{0.23f, 0.42f, 0.16f};
            glm::vec3 rock{0.38f, 0.34f, 0.30f};
            glm::vec3 snow{0.92f, 0.94f, 0.98f};
            float snowLevel      = 16.0f;
            float rockSlope      = 0.62f; // flatter than this -> rock
            float slopeSharpness = 0.14f;
            float detailScale    = 0.35f; // micro-detail frequency
            float detailStrength = 1.5f;  // normal-perturbation strength
        } look;

        // Materials describe surface appearance; the renderer feeds in lighting.
        Material terrainMat(lit);
        terrainMat.set("uColorMode", 1)
                  .set("uColorSand", look.sand)
                  .set("uColorGrass", look.grass)
                  .set("uColorRock", look.rock)
                  .set("uColorSnow", look.snow);

        Material cubeMat(lit);
        cubeMat.set("uColorMode", 2).setTexture("uTexture", texture, 0);

        // World streaming + renderer with cascaded shadows.
        TerrainSettings settings;
        TerrainStreamer streamer(settings, /*radius=*/4);
        Renderer        renderer(2048, 4);
        DirectionalLight light;

        // Water: planar reflection/refraction targets + a surface quad.
        Shader water = Shader::fromFiles("assets/shaders/water.vert",
                                         "assets/shaders/water.frag");
        if (!water.isValid()) {
            std::fprintf(stderr, "Failed to load water shader\n");
            return 1;
        }
        const std::vector<Vertex> waterVerts = {
            {{-0.5f, 0.0f, -0.5f}, {0, 1, 0}, {0, 0}},
            {{ 0.5f, 0.0f, -0.5f}, {0, 1, 0}, {1, 0}},
            {{ 0.5f, 0.0f,  0.5f}, {0, 1, 0}, {1, 1}},
            {{-0.5f, 0.0f,  0.5f}, {0, 1, 0}, {0, 1}},
        };
        Mesh waterMesh = Mesh::create(waterVerts, {0, 3, 2, 0, 2, 1});
        RenderTarget reflectRT(1280, 720);
        RenderTarget refractRT(1280, 720);

        float     waterLevel   = -2.0f;
        glm::vec3 waterColor{0.10f, 0.30f, 0.38f};
        float     waveStrength = 0.018f;
        float     waveScale    = 0.06f;

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

            const bool mouseLook = input.isMouseButtonDown(GLFW_MOUSE_BUTTON_RIGHT)
                                   && !gui.wantsMouse();
            if (mouseLook != input.isCursorLocked()) input.setCursorLocked(mouseLook);
            if (mouseLook) {
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
                ImGui::Text("Chunks: %d loaded, %d pending",
                            streamer.loadedChunkCount(), streamer.pendingChunkCount());
                ImGui::Text("Draws: %d visible, %d culled",
                            renderer.lastDrawn(), renderer.lastCulled());
                ImGui::SliderFloat("Move speed", &camera.moveSpeed, 2.0f, 80.0f);

                if (ImGui::CollapsingHeader("Lighting & Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat3("Light dir", &light.direction.x, -1.0f, 1.0f);
                    ImGui::ColorEdit3("Light color", &light.color.x);
                    ImGui::SliderFloat("Cascade split", &renderer.shadows().splitLambda, 0.0f, 1.0f);
                }
                if (ImGui::CollapsingHeader("Water", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat("Level",      &waterLevel, -15.0f, 15.0f);
                    ImGui::SliderFloat("Waves",      &waveStrength, 0.0f, 0.05f, "%.3f");
                    ImGui::SliderFloat("Ripple size",&waveScale, 0.01f, 0.2f, "%.3f");
                    ImGui::ColorEdit3("Tint",        &waterColor.x);
                }
                if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat("Height",    &uiSettings.heightScale, 0.0f, 30.0f);
                    ImGui::SliderFloat("Ridges",    &uiSettings.ridgeScale, 0.0f, 50.0f);
                    ImGui::SliderFloat("Warp",      &uiSettings.warpStrength, 0.0f, 40.0f);
                    ImGui::SliderFloat("Frequency", &uiSettings.frequency, 0.003f, 0.05f, "%.3f");
                    ImGui::SliderInt  ("Octaves",   &uiSettings.octaves, 1, 8);
                    ImGui::SliderFloat("Seed",      &uiSettings.seed, 0.0f, 100.0f);
                    if (ImGui::Button("Regenerate")) {
                        streamer.settings() = uiSettings;
                        streamer.rebuild();
                        streamer.update(camera.position());
                    }
                }
                if (ImGui::CollapsingHeader("Terrain material (slope)", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat("Rock slope",   &look.rockSlope, 0.0f, 1.0f);
                    ImGui::SliderFloat("Slope blend",  &look.slopeSharpness, 0.02f, 0.4f);
                    ImGui::SliderFloat("Snow level",   &look.snowLevel, 0.0f, 40.0f);
                    ImGui::SliderFloat("Detail scale",    &look.detailScale, 0.05f, 1.0f);
                    ImGui::SliderFloat("Detail strength", &look.detailStrength, 0.0f, 4.0f);
                    ImGui::ColorEdit3("Grass", &look.grass.x);
                    ImGui::ColorEdit3("Rock",  &look.rock.x);
                    ImGui::ColorEdit3("Snow",  &look.snow.x);
                }
            }
            ImGui::End();

            // Push the (possibly edited) slope/height palette into the material.
            terrainMat.set("uColorSand", look.sand)
                      .set("uColorGrass", look.grass)
                      .set("uColorRock", look.rock)
                      .set("uColorSnow", look.snow)
                      .set("uSnowLevel", look.snowLevel)
                      .set("uRockSlope", look.rockSlope)
                      .set("uSlopeSharpness", look.slopeSharpness)
                      .set("uDetailScale", look.detailScale)
                      .set("uDetailStrength", look.detailStrength);

            // --- Submit the opaque scene once ---------------------------
            int fbW = 0, fbH = 0;
            window.framebufferSize(fbW, fbH);
            const float     aspect = window.aspectRatio();
            const glm::mat4 proj   = camera.projectionMatrix(aspect);

            renderer.setViewport(fbW, fbH);
            renderer.begin(camera, aspect, light);

            for (const TerrainChunk* chunk : streamer.visibleChunks()) {
                renderer.submit(chunk->mesh(), terrainMat, glm::mat4(1.0f));
            }
            for (const glm::vec2& spot : cubeSpots) {
                const float gy = streamer.heightAt(spot.x, spot.y);
                glm::mat4 m(1.0f);
                m = glm::translate(m, {spot.x, gy + 2.0f, spot.y});
                m = glm::scale(m, glm::vec3(4.0f));
                renderer.submit(cube, cubeMat, m);
            }
            {   // floating, spinning shadow caster
                const float gy  = streamer.heightAt(0.0f, 0.0f);
                const float bob = std::sin(static_cast<float>(now)) * 2.0f;
                glm::mat4 m(1.0f);
                m = glm::translate(m, {0.0f, gy + 14.0f + bob, 0.0f});
                m = glm::rotate(m, static_cast<float>(now) * 0.6f,
                                glm::normalize(glm::vec3(0.4f, 1.0f, 0.2f)));
                m = glm::scale(m, glm::vec3(6.0f));
                renderer.submit(cube, cubeMat, m);
            }

            // --- Multi-pass render with planar water --------------------
            renderer.prepareShadows(); // shadows once, from the real camera

            auto clearScene = [] {
                glClearColor(0.55f, 0.70f, 0.92f, 1.0f); // sky
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            };

            const glm::vec3& camPos = camera.position();

            // 1) Reflection: render the scene mirrored across the water plane,
            //    clipping everything below the surface.
            const glm::mat4 mirror =
                glm::translate(glm::mat4(1.0f), {0.0f, 2.0f * waterLevel, 0.0f}) *
                glm::scale(glm::mat4(1.0f), {1.0f, -1.0f, 1.0f});
            const glm::mat4 reflView = camera.viewMatrix() * mirror;
            const glm::vec3 reflEye{camPos.x, 2.0f * waterLevel - camPos.y, camPos.z};

            reflectRT.bind();
            clearScene();
            glCullFace(GL_FRONT); // mirroring flips winding
            renderer.renderScene(reflView, proj, reflEye,
                                 glm::vec4(0, 1, 0, -waterLevel + 0.1f));
            glCullFace(GL_BACK);

            // 2) Refraction: render the scene normally, clipping above water.
            refractRT.bind();
            clearScene();
            renderer.renderScene(camera.viewMatrix(), proj, camPos,
                                 glm::vec4(0, -1, 0, waterLevel + 0.1f));

            // 3) Main pass: the full scene, no clipping.
            RenderTarget::unbind(fbW, fbH);
            clearScene();
            renderer.renderScene(camera.viewMatrix(), proj, camPos, Renderer::kNoClip);

            // 4) The water surface: a large quad following the camera, sampling
            //    the reflection/refraction targets with Fresnel + ripples.
            glm::mat4 waterModel =
                glm::translate(glm::mat4(1.0f), {camPos.x, waterLevel, camPos.z});
            waterModel = glm::scale(waterModel, glm::vec3(1400.0f, 1.0f, 1400.0f));

            water.bind();
            water.setMat4("uModel", waterModel);
            water.setMat4("uViewProj", proj * camera.viewMatrix());
            water.setVec3("uCameraPos", camPos);
            water.setVec3("uLightDir", light.direction);
            water.setVec3("uLightColor", light.color);
            water.setFloat("uTime", static_cast<float>(now));
            water.setVec3("uWaterColor", waterColor);
            water.setFloat("uWaveStrength", waveStrength);
            water.setFloat("uWaveScale", waveScale);
            water.setInt("uReflection", 0);
            water.setInt("uRefraction", 1);
            reflectRT.bindColorTexture(0);
            refractRT.bindColorTexture(1);
            waterMesh.draw();

            gui.endFrame();
            window.swapBuffers();
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }

    return 0;
}
