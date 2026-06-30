#include <cmath>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <imgui.h>

#include <fitzel/Fitzel.hpp>

using namespace fitzel;

// On laptops with hybrid graphics (NVIDIA Optimus / AMD PowerXpress), ask the
// driver to run us on the discrete high-performance GPU instead of the iGPU.
#if defined(_WIN32)
extern "C" {
    __declspec(dllexport) unsigned long NvOptimusEnablement = 1;
    __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

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
        Camera camera({0.0f, 34.0f, 80.0f}, -90.0f, -13.0f);
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

        // Terrain PBR-ish albedo textures (triplanar), loaded from the repo's
        // textures/ folder (path injected by CMake).
        const std::string texDir = FITZEL_TEXTURE_DIR;
        Texture texSand   = Texture::fromFile(texDir + "/coast_sand_01_diff_4k.jpg");
        Texture texGround = Texture::fromFile(texDir + "/aerial_rocks_01_diff_4k.jpg");
        Texture texCliff  = Texture::fromFile(texDir + "/rocky_terrain_02_diff_4k.jpg");
        Texture texSnow   = Texture::fromFile(texDir + "/snow_02_diff_4k.jpg");
        // Matching triplanar normal maps. PolyHaven ships these as DWAA-compressed
        // EXR (which most loaders, incl. tinyexr, can't decode), so they're
        // converted to PNG once (see README); no vertical flip.
        Texture texSandN   = Texture::fromFile(texDir + "/coast_sand_01_nor_gl_4k.png", false);
        Texture texGroundN = Texture::fromFile(texDir + "/aerial_rocks_01_nor_gl_4k.png", false);
        Texture texCliffN  = Texture::fromFile(texDir + "/rocky_terrain_02_nor_gl_4k.png", false);
        Texture texSnowN   = Texture::fromFile(texDir + "/snow_02_nor_gl_4k.png", false);
        if (!texSand.isValid() || !texGround.isValid() ||
            !texCliff.isValid() || !texSnow.isValid()) {
            std::fprintf(stderr, "Warning: some terrain textures failed to load from %s\n",
                         texDir.c_str());
        }
        float texScale       = 0.08f; // world units -> texture tiling
        float normalStrength = 1.0f;

        // Materials describe surface appearance; the renderer feeds in lighting.
        Material terrainMat(lit);
        terrainMat.set("uColorMode", 1)
                  .setTexture("uTexSand", texSand, 6)
                  .setTexture("uTexGround", texGround, 3)
                  .setTexture("uTexCliff", texCliff, 4)
                  .setTexture("uTexSnow", texSnow, 5)
                  .setTexture("uTexSandN", texSandN, 8)
                  .setTexture("uTexGroundN", texGroundN, 9)
                  .setTexture("uTexCliffN", texCliffN, 10)
                  .setTexture("uTexSnowN", texSnowN, 11);

        Material cubeMat(lit);
        cubeMat.set("uColorMode", 2).setTexture("uTexture", texture, 0);

        // World streaming + renderer with cascaded shadows.
        TerrainSettings settings;
        TerrainStreamer streamer(settings, /*radius=*/5);
        int             viewRadius = 5; // view distance in chunks
        Renderer        renderer(2048, 4);
        DirectionalLight light;

        // Water: planar reflection/refraction targets + a surface quad.
        Shader water = Shader::fromFiles("assets/shaders/water.vert",
                                         "assets/shaders/water.frag");
        if (!water.isValid()) {
            std::fprintf(stderr, "Failed to load water shader\n");
            return 1;
        }
        // A tessellated water grid so Gerstner waves can displace its vertices.
        const int gridN = 400;
        std::vector<Vertex>        waterVerts;
        std::vector<std::uint32_t> waterIdx;
        waterVerts.reserve(static_cast<std::size_t>(gridN) * gridN);
        for (int z = 0; z < gridN; ++z) {
            for (int x = 0; x < gridN; ++x) {
                const float fx = static_cast<float>(x) / (gridN - 1) - 0.5f;
                const float fz = static_cast<float>(z) / (gridN - 1) - 0.5f;
                waterVerts.push_back({{fx, 0.0f, fz}, {0, 1, 0},
                                      {static_cast<float>(x) / (gridN - 1),
                                       static_cast<float>(z) / (gridN - 1)}});
            }
        }
        for (int z = 0; z < gridN - 1; ++z) {
            for (int x = 0; x < gridN - 1; ++x) {
                const std::uint32_t i0 = static_cast<std::uint32_t>(z * gridN + x);
                const std::uint32_t i1 = i0 + 1;
                const std::uint32_t i2 = i0 + gridN;
                const std::uint32_t i3 = i2 + 1;
                waterIdx.insert(waterIdx.end(), {i0, i2, i1, i1, i2, i3});
            }
        }
        Mesh waterMesh = Mesh::create(waterVerts, waterIdx);
        // Half-resolution reflection/refraction: the water distortion hides it
        // and it roughly quarters the cost of those two textured passes.
        RenderTarget reflectRT(640, 360);
        RenderTarget refractRT(640, 360, RenderTarget::Format::RGBA8, /*depthTex=*/true);

        float     waterLevel   = -2.0f;
        glm::vec3 waterColor{0.10f, 0.30f, 0.38f};
        float     waveStrength = 0.018f;
        float     waveScale    = 0.06f;
        float     foamWidth    = 2.5f;
        float     waveHeight   = 0.6f; // Gerstner swell amplitude
        float     waveChoppy   = 0.6f;

        // Sky + volumetric clouds (fullscreen raymarch pass).
        Shader sky = Shader::fromFiles("assets/shaders/sky.vert",
                                       "assets/shaders/sky.frag");
        if (!sky.isValid()) {
            std::fprintf(stderr, "Failed to load sky shader\n");
            return 1;
        }
        const std::vector<Vertex> fsVerts = {
            {{-1.0f, -1.0f, 0.0f}, {0, 0, 1}, {0, 0}},
            {{ 1.0f, -1.0f, 0.0f}, {0, 0, 1}, {1, 0}},
            {{ 1.0f,  1.0f, 0.0f}, {0, 0, 1}, {1, 1}},
            {{-1.0f,  1.0f, 0.0f}, {0, 0, 1}, {0, 1}},
        };
        Mesh fsQuad = Mesh::create(fsVerts, {0, 1, 2, 0, 2, 3});

        // HDR scene buffer + post-processing (bloom, god rays, lens flare, tonemap).
        Shader composite = Shader::fromFiles("assets/shaders/sky.vert",
                                             "assets/shaders/composite.frag");
        if (!composite.isValid()) {
            std::fprintf(stderr, "Failed to load composite shader\n");
            return 1;
        }
        int hdrW = 0, hdrH = 0;
        window.framebufferSize(hdrW, hdrH);
        RenderTarget hdrRT(hdrW, hdrH, RenderTarget::Format::RGBA16F, /*depthTex=*/true);
        float bloomIntensity = 0.35f;
        float rayIntensity   = 0.5f;

        // SSAO (half-res), reconstructing position/normal from the HDR depth.
        Shader ssao = Shader::fromFiles("assets/shaders/sky.vert",
                                        "assets/shaders/ssao.frag");
        if (!ssao.isValid()) { std::fprintf(stderr, "Failed to load ssao shader\n"); return 1; }
        RenderTarget ssaoRT(hdrW / 2, hdrH / 2);
        {
            std::mt19937 rng(1337u);
            std::uniform_real_distribution<float> d(0.0f, 1.0f);
            ssao.bind();
            const int kn = 24;
            for (int i = 0; i < kn; ++i) {
                glm::vec3 s(d(rng) * 2.0f - 1.0f, d(rng) * 2.0f - 1.0f, d(rng));
                s = glm::normalize(s) * d(rng);
                const float t = static_cast<float>(i) / kn;
                s *= glm::mix(0.1f, 1.0f, t * t); // cluster samples near the origin
                ssao.setVec3("uKernel[" + std::to_string(i) + "]", s);
            }
            ssao.setInt("uKernelSize", kn);
        }
        float ssaoStrength = 0.7f;
        float ssaoRadius   = 1.5f;
        float ssaoBias     = 0.03f;
        float ssaoPower    = 1.6f;

        // Day/night cycle.
        float timeOfDay = 7.3f;    // hours [0,24)
        float dayLength = 240.0f;  // real seconds per full 24h (0 = frozen)

        // Cloud controls.
        float cloudCoverage = 0.5f;
        float cloudDensity  = 1.0f;
        float cloudScale    = 0.0025f;
        float cloudSpeed    = 5.0f;
        float cloudBottom   = 140.0f;
        float cloudTop      = 320.0f;

        // Weather: 0 = clear .. 1 = storm. Drives clouds, light, fog, waves, rain.
        float weather     = 0.0f;
        bool  autoWeather = true;

        // Rain: falling line streaks in a box that follows the camera.
        Shader rain = Shader::fromFiles("assets/shaders/rain.vert",
                                        "assets/shaders/rain.frag");
        if (!rain.isValid()) { std::fprintf(stderr, "Failed to load rain shader\n"); return 1; }
        const int   rainDrops   = 14000;
        const float rainBoxHalf = 55.0f;
        const float rainBoxH    = 95.0f;
        GLuint rainVAO = 0, rainVBO = 0;
        {
            std::mt19937 rr(99u);
            std::uniform_real_distribution<float> u(0.0f, 1.0f);
            std::vector<float> data;
            data.reserve(static_cast<std::size_t>(rainDrops) * 2 * 5);
            for (int i = 0; i < rainDrops; ++i) {
                const float bx = (u(rr) - 0.5f) * 2.0f * rainBoxHalf;
                const float bz = (u(rr) - 0.5f) * 2.0f * rainBoxHalf;
                const float ys = u(rr) * rainBoxH;
                const float sp = glm::mix(30.0f, 55.0f, u(rr));
                data.insert(data.end(), {bx, ys, bz, sp, 0.0f});
                data.insert(data.end(), {bx, ys, bz, sp, 1.0f});
            }
            glGenVertexArrays(1, &rainVAO);
            glBindVertexArray(rainVAO);
            glGenBuffers(1, &rainVBO);
            glBindBuffer(GL_ARRAY_BUFFER, rainVBO);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(data.size() * sizeof(float)),
                         data.data(), GL_STATIC_DRAW);
            const GLsizei stride = 5 * sizeof(float);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, (void*)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, stride, (void*)(3 * sizeof(float)));
            glEnableVertexAttribArray(2);
            glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, (void*)(4 * sizeof(float)));
            glBindVertexArray(0);
        }

        // --- Audio: weather-driven sound layers --------------------------
        Audio audio;
        const std::string soundDir = FITZEL_SOUND_DIR;
        Sound rainSnd    = Sound::fromFile(audio, soundDir + "/rain.wav", true);
        Sound windSnd    = Sound::fromFile(audio, soundDir + "/wind.wav", true);
        Sound breezeSnd  = Sound::fromFile(audio, soundDir + "/breeze.wav", true);
        Sound thunderSnd = Sound::fromFile(audio, soundDir + "/thunder.wav", false);
        rainSnd.setVolume(0.0f);   rainSnd.play();   // loops; volume follows weather
        windSnd.setVolume(0.0f);   windSnd.play();
        breezeSnd.setVolume(0.0f); breezeSnd.play();
        float masterVolume = 0.8f;
        bool  muted        = false;
        bool  prevFlashOn  = false;

        // Atmospheric fog (subtle by default; aerial perspective, not haze soup).
        float fogDensity = 0.0028f;
        float fogFalloff = 0.035f;

        // Tonemapping exposure + HSV colour grade.
        float exposure   = 1.0f;
        float hueShift   = 0.0f;
        float saturation = 1.0f;
        float valueGain  = 1.0f;

        // Camera angle controls.
        float camFov   = camera.fov();
        float camYaw   = camera.yaw();
        float camPitch = camera.pitch();

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

            // View distance: drive the streaming radius and the camera far plane.
            streamer.setRadius(viewRadius);
            camera.setFarPlane(
                std::max(250.0f, viewRadius * streamer.settings().chunkSize * 1.7f));

            // Stream terrain chunks around the camera.
            streamer.update(camera.position());

            // --- Weather: drift (auto) and derive storm parameters ----------
            if (autoWeather) {
                const float target = glm::clamp(
                    0.5f + 0.42f * std::sin(static_cast<float>(now) * 0.018f)
                         + 0.18f * std::sin(static_cast<float>(now) * 0.011f + 2.1f),
                    0.0f, 1.0f);
                weather += (target - weather) * std::min(1.0f, dt * 0.3f);
            }
            weather = glm::clamp(weather, 0.0f, 1.0f);

            const float effCoverage  = glm::mix(cloudCoverage, 0.97f, weather);
            const float effDensity   = glm::mix(cloudDensity, 2.7f, weather);
            const float effWind      = glm::mix(cloudSpeed, 26.0f, weather);
            const float effCloudBot  = glm::mix(cloudBottom, 80.0f, weather);
            const float effWaveH     = glm::mix(waveHeight, 2.4f, weather);
            const float effWaveC     = glm::mix(waveChoppy, 0.95f, weather);
            const float effFog       = fogDensity + weather * 0.011f;
            const float rainIntensity = glm::smoothstep(0.45f, 0.85f, weather);
            const float lightDim     = glm::mix(1.0f, 0.30f, weather);

            // Lightning: brief flashes once the storm is strong.
            float flash = 0.0f;
            if (weather > 0.5f) {
                const float ft  = static_cast<float>(now) * 0.55f;
                const float rnd = glm::fract(std::sin(std::floor(ft) * 127.1f) * 43758.5f);
                if (rnd > 0.9f) {
                    flash = std::exp(-glm::fract(ft) * 7.0f) * (weather - 0.5f) * 2.0f;
                }
            }

            // Weather audio: cross-fade the looping layers, fire thunder on a
            // fresh lightning flash.
            audio.setMasterVolume(muted ? 0.0f : masterVolume);
            rainSnd.setVolume(rainIntensity);
            windSnd.setVolume(glm::smoothstep(0.15f, 1.0f, weather) * 0.9f);
            breezeSnd.setVolume((1.0f - glm::smoothstep(0.0f, 0.5f, weather)) * 0.5f);
            const bool flashOn = flash > 0.25f;
            if (flashOn && !prevFlashOn) {
                thunderSnd.setVolume(glm::clamp(weather, 0.3f, 1.0f));
                thunderSnd.play();
            }
            prevFlashOn = flashOn;

            // --- Day/night: advance time, derive sun direction and lighting ---
            if (dayLength > 0.1f) {
                timeOfDay += dt * (24.0f / dayLength);
                timeOfDay = std::fmod(timeOfDay, 24.0f);
            }
            const float phi = (timeOfDay / 24.0f) * 6.2831853f - 1.5707963f;
            const glm::vec3 sunDir =
                glm::normalize(glm::vec3(std::cos(phi), std::sin(phi), 0.18f));
            const float dayF   = glm::smoothstep(-0.12f, 0.18f, sunDir.y);
            const float lowSun = 1.0f - glm::clamp(sunDir.y / 0.3f, 0.0f, 1.0f);
            const glm::vec3 sunCol =
                glm::mix(glm::vec3(1.0f, 0.97f, 0.9f), glm::vec3(1.0f, 0.55f, 0.26f), lowSun);
            light.direction = sunDir;
            // HDR radiance: the sun is much brighter than 1 so tonemapping
            // produces highlights and contrast instead of a flat look.
            light.color   = sunCol * (0.12f + 0.95f * dayF) * 3.4f * lightDim;
            light.ambient = glm::mix(glm::vec3(0.015f, 0.02f, 0.04f),
                                     glm::vec3(0.12f, 0.14f, 0.18f), dayF);
            // Overcast: dimmer, greyer, cooler ambient.
            light.ambient = glm::mix(light.ambient,
                                     glm::vec3(0.05f, 0.06f, 0.08f), weather * 0.7f);
            // Lightning flash lights the scene briefly.
            light.color   += glm::vec3(0.8f, 0.85f, 1.0f) * (flash * 6.0f);
            light.ambient += glm::vec3(0.5f, 0.55f, 0.7f) * flash;
            renderer.setExposure(exposure);

            // Atmospheric fog, tinted by time of day to match the sky horizon.
            // Colours are authored in sRGB and linearised for the linear-space
            // blend (tonemapping converts back on output).
            Fog fog;
            fog.height        = waterLevel;
            fog.density       = effFog;
            fog.heightFalloff = fogFalloff;
            const glm::vec3 hazeDisp =
                glm::mix(glm::vec3(0.03f, 0.04f, 0.09f), glm::vec3(0.62f, 0.74f, 0.92f), dayF);
            const glm::vec3 sunHazeDisp =
                glm::mix(hazeDisp, glm::vec3(1.0f, 0.62f, 0.34f), 0.7f * dayF);
            fog.color    = glm::pow(hazeDisp, glm::vec3(2.2f));
            fog.sunColor = glm::pow(sunHazeDisp, glm::vec3(2.2f));
            renderer.setFog(fog);

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
                ImGui::SliderInt("View distance", &viewRadius, 2, 9, "%d chunks");
                ImGui::SameLine();
                ImGui::Text("(%.0f m)", viewRadius * streamer.settings().chunkSize);

                if (ImGui::CollapsingHeader("Camera")) {
                    // Sync from the camera (mouse-look may have changed it), then
                    // apply only when a slider is actually edited.
                    camFov = camera.fov(); camYaw = camera.yaw(); camPitch = camera.pitch();
                    if (ImGui::SliderFloat("FOV",   &camFov, 25.0f, 100.0f, "%.0f deg"))
                        camera.setFov(camFov);
                    if (ImGui::SliderFloat("Yaw",   &camYaw, -180.0f, 180.0f, "%.0f"))
                        camera.setYaw(camYaw);
                    if (ImGui::SliderFloat("Pitch", &camPitch, -89.0f, 89.0f, "%.0f"))
                        camera.setPitch(camPitch);
                }

                if (ImGui::CollapsingHeader("Weather", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::Checkbox("Auto weather", &autoWeather);
                    ImGui::SliderFloat("Storm", &weather, 0.0f, 1.0f);
                    ImGui::Text("Rain %.0f%%   Lightning %s", rainIntensity * 100.0f,
                                weather > 0.5f ? "armed" : "off");
                    ImGui::Separator();
                    ImGui::Checkbox("Mute", &muted);
                    ImGui::SameLine();
                    ImGui::SliderFloat("Volume", &masterVolume, 0.0f, 1.0f);
                    if (!audio.ok()) ImGui::TextDisabled("(audio device unavailable)");
                }

                if (ImGui::CollapsingHeader("Sky, clouds & atmosphere", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat("Time of day", &timeOfDay, 0.0f, 24.0f, "%.1f h");
                    ImGui::SliderFloat("Day length",  &dayLength, 0.0f, 600.0f, "%.0f s");
                    ImGui::SliderFloat("Coverage",    &cloudCoverage, 0.0f, 1.0f);
                    ImGui::SliderFloat("Density",     &cloudDensity, 0.0f, 3.0f);
                    ImGui::SliderFloat("Cloud scale", &cloudScale, 0.001f, 0.006f, "%.4f");
                    ImGui::SliderFloat("Wind",        &cloudSpeed, 0.0f, 20.0f);
                    ImGui::SliderFloat("Fog density", &fogDensity, 0.0f, 0.02f, "%.4f");
                    ImGui::SliderFloat("Fog falloff", &fogFalloff, 0.005f, 0.1f, "%.3f");
                    ImGui::SliderFloat("Exposure",   &exposure, 0.2f, 3.0f);
                    ImGui::SliderFloat("Bloom",      &bloomIntensity, 0.0f, 1.5f);
                    ImGui::SliderFloat("Sun rays",   &rayIntensity, 0.0f, 1.5f);
                    ImGui::SliderFloat("SSAO",       &ssaoStrength, 0.0f, 1.0f);
                    ImGui::SliderFloat("SSAO radius",&ssaoRadius, 0.2f, 4.0f);
                    ImGui::SliderFloat("Cascade split", &renderer.shadows().splitLambda, 0.0f, 1.0f);
                    ImGui::SeparatorText("Colour grade (HSV)");
                    ImGui::SliderFloat("Hue",        &hueShift, -180.0f, 180.0f, "%.0f");
                    ImGui::SliderFloat("Saturation", &saturation, 0.0f, 2.0f);
                    ImGui::SliderFloat("Brightness", &valueGain, 0.3f, 2.0f);
                }
                if (ImGui::CollapsingHeader("Water", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat("Level",       &waterLevel, -15.0f, 15.0f);
                    ImGui::SliderFloat("Swell height",&waveHeight, 0.0f, 2.5f);
                    ImGui::SliderFloat("Choppiness",  &waveChoppy, 0.0f, 1.0f);
                    ImGui::SliderFloat("Ripples",     &waveStrength, 0.0f, 0.05f, "%.3f");
                    ImGui::SliderFloat("Ripple size", &waveScale, 0.01f, 0.2f, "%.3f");
                    ImGui::SliderFloat("Shore foam",  &foamWidth, 0.0f, 8.0f);
                    ImGui::ColorEdit3("Tint",         &waterColor.x);
                }
                if (ImGui::CollapsingHeader("Terrain", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat("Height",     &uiSettings.heightScale, 0.0f, 30.0f);
                    ImGui::SliderFloat("Ridges",     &uiSettings.ridgeScale, 0.0f, 50.0f);
                    ImGui::SliderFloat("Continents", &uiSettings.continentAmp, 0.0f, 3.0f);
                    ImGui::SliderFloat("Biome size", &uiSettings.biomeFreq, 0.0005f, 0.004f, "%.4f");
                    ImGui::SliderFloat("Terraces",   &uiSettings.terrace, 0.0f, 1.0f);
                    ImGui::SliderFloat("Warp",       &uiSettings.warpStrength, 0.0f, 40.0f);
                    ImGui::SliderFloat("Frequency",  &uiSettings.frequency, 0.003f, 0.05f, "%.3f");
                    ImGui::SliderInt  ("Octaves",    &uiSettings.octaves, 1, 8);
                    ImGui::SliderFloat("Seed",       &uiSettings.seed, 0.0f, 100.0f);
                    if (ImGui::Button("Regenerate")) {
                        streamer.settings() = uiSettings;
                        streamer.rebuild();
                        streamer.update(camera.position());
                    }
                }
                if (ImGui::CollapsingHeader("Terrain material (slope)", ImGuiTreeNodeFlags_DefaultOpen)) {
                    ImGui::SliderFloat("Texture scale", &texScale, 0.02f, 0.2f, "%.3f");
                    ImGui::SliderFloat("Normal strength", &normalStrength, 0.0f, 1.0f);
                    ImGui::SliderFloat("Rock slope",    &look.rockSlope, 0.0f, 1.0f);
                    ImGui::SliderFloat("Slope blend",   &look.slopeSharpness, 0.02f, 0.4f);
                    ImGui::SliderFloat("Snow level",    &look.snowLevel, 0.0f, 40.0f);
                    ImGui::SliderFloat("Detail bump",   &look.detailStrength, 0.0f, 4.0f);
                }
            }
            ImGui::End();

            // Push the (possibly edited) terrain blend params into the material.
            terrainMat.set("uSnowLevel", look.snowLevel)
                      .set("uRockSlope", look.rockSlope)
                      .set("uSlopeSharpness", look.slopeSharpness)
                      .set("uDetailScale", look.detailScale)
                      .set("uDetailStrength", look.detailStrength)
                      .set("uTexScale", texScale)
                      .set("uSandLevel", waterLevel + 1.0f)
                      .set("uNormalStrength", normalStrength)
                      .set("uWaterLevel", waterLevel);

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

            // --- Multi-pass render with sky and planar water ------------
            renderer.prepareShadows(); // shadows once, from the real camera

            const glm::vec3& camPos = camera.position();
            const glm::mat4  view   = camera.viewMatrix();
            const glm::mat4  mainVP = proj * view;

            // Fullscreen sky + volumetric clouds for a given view.
            auto drawSky = [&](const glm::mat4& invViewProj, const glm::vec3& eye,
                               bool tonemap) {
                glDisable(GL_DEPTH_TEST);
                glDepthMask(GL_FALSE);
                glDisable(GL_CULL_FACE);
                sky.bind();
                sky.setMat4("uInvViewProj", invViewProj);
                sky.setVec3("uCameraPos", eye);
                sky.setVec3("uSunDir", light.direction);
                sky.setVec3("uSunColor", light.color);
                sky.setFloat("uTime", static_cast<float>(now));
                sky.setFloat("uCoverage", glm::mix(0.62f, 0.10f, effCoverage));
                sky.setFloat("uCloudDensity", effDensity);
                sky.setFloat("uCloudScale", cloudScale);
                sky.setFloat("uCloudSpeed", effWind);
                sky.setFloat("uCloudBottom", effCloudBot);
                sky.setFloat("uCloudTop", cloudTop);
                sky.setFloat("uExposure", exposure);
                sky.setInt("uTonemap", tonemap ? 1 : 0);
                fsQuad.draw();
                glDepthMask(GL_TRUE);
                glEnable(GL_DEPTH_TEST);
                glEnable(GL_CULL_FACE);
            };

            // 1) Reflection: sky + scene mirrored across the water plane,
            //    clipping everything below the surface.
            const glm::mat4 mirror =
                glm::translate(glm::mat4(1.0f), {0.0f, 2.0f * waterLevel, 0.0f}) *
                glm::scale(glm::mat4(1.0f), {1.0f, -1.0f, 1.0f});
            const glm::mat4 reflView = view * mirror;
            const glm::vec3 reflEye{camPos.x, 2.0f * waterLevel - camPos.y, camPos.z};

            // Reflection/refraction render LINEAR (tonemap=false) so the water
            // shader can sample and tonemap them once at the end.
            reflectRT.bind();
            glClear(GL_DEPTH_BUFFER_BIT);
            drawSky(glm::inverse(proj * reflView), reflEye, false);
            glCullFace(GL_FRONT); // mirroring flips winding
            renderer.renderScene(reflView, proj, reflEye,
                                 glm::vec4(0, 1, 0, -waterLevel + 0.1f), false);
            glCullFace(GL_BACK);

            // 2) Refraction: scene only, clipping above water (deep-water clear).
            refractRT.bind();
            glClearColor(waterColor.r * 0.5f, waterColor.g * 0.5f,
                         waterColor.b * 0.5f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            renderer.renderScene(view, proj, camPos,
                                 glm::vec4(0, -1, 0, waterLevel + 0.1f), false);

            // 3) Main pass: sky + full scene rendered LINEAR into the HDR buffer
            //    (tonemapping happens in the composite pass).
            if (hdrRT.width() != fbW || hdrRT.height() != fbH) {
                hdrRT  = RenderTarget(fbW, fbH, RenderTarget::Format::RGBA16F, true);
                ssaoRT = RenderTarget(std::max(1, fbW / 2), std::max(1, fbH / 2));
            }
            hdrRT.bind();
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            drawSky(glm::inverse(mainVP), camPos, false);
            renderer.renderScene(view, proj, camPos, Renderer::kNoClip, false);

            // 4) The water surface: a large quad following the camera, sampling
            //    the reflection/refraction targets with Fresnel + ripples.
            glm::mat4 waterModel =
                glm::translate(glm::mat4(1.0f), {camPos.x, waterLevel, camPos.z});
            waterModel = glm::scale(waterModel, glm::vec3(1400.0f, 1.0f, 1400.0f));

            water.bind();
            water.setMat4("uModel", waterModel);
            water.setMat4("uViewProj", mainVP);
            water.setVec3("uCameraPos", camPos);
            water.setVec3("uLightDir", light.direction);
            water.setVec3("uLightColor", light.color);
            water.setFloat("uTime", static_cast<float>(now));
            water.setVec3("uWaterColor", waterColor);
            water.setFloat("uWaveStrength", waveStrength);
            water.setFloat("uWaveScale", waveScale);
            water.setFloat("uWaveHeight", effWaveH);
            water.setFloat("uChoppy", effWaveC);
            water.setVec3("uAmbient", light.ambient);
            water.setVec3("uFogColor", fog.color);
            water.setVec3("uFogSunColor", fog.sunColor);
            water.setFloat("uFogDensity", fog.density);
            water.setFloat("uFogHeightFalloff", fog.heightFalloff);
            water.setFloat("uFogHeight", fog.height);
            water.setFloat("uExposure", exposure);
            water.setInt("uTonemap", 0); // linear into HDR; composite tonemaps
            water.setInt("uReflection", 0);
            water.setInt("uRefraction", 1);
            water.setInt("uRefractionDepth", 2);
            water.setFloat("uNear", camera.nearPlane());
            water.setFloat("uFar", camera.farPlane());
            water.setFloat("uFoamWidth", foamWidth);
            reflectRT.bindColorTexture(0);
            refractRT.bindColorTexture(1);
            refractRT.bindDepthTexture(2);
            waterMesh.draw();

            // --- Rain streaks (storm), into the HDR buffer --------------
            if (rainIntensity > 0.001f) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                rain.bind();
                rain.setMat4("uViewProj", mainVP);
                rain.setVec3("uBoxCenter", camPos);
                rain.setFloat("uBoxHeight", rainBoxH);
                rain.setFloat("uBoxHalf", rainBoxHalf);
                rain.setFloat("uStreak", glm::mix(1.2f, 3.0f, weather));
                rain.setFloat("uTime", static_cast<float>(now));
                rain.setVec3("uWind",
                             glm::normalize(glm::vec3(0.6f, 0.0f, 0.3f)) *
                                 glm::mix(0.05f, 0.6f, weather));
                rain.setVec3("uRainColor",
                             glm::clamp(light.ambient * 2.5f + light.color * 0.12f,
                                        glm::vec3(0.0f), glm::vec3(2.0f)));
                rain.setFloat("uIntensity", rainIntensity);
                glBindVertexArray(rainVAO);
                glDrawArrays(GL_LINES, 0, rainDrops * 2);
                glBindVertexArray(0);
                glDepthMask(GL_TRUE);
                glDisable(GL_BLEND);
            }

            // --- SSAO: occlusion from the HDR depth buffer (half-res) ---
            ssaoRT.bind();
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            ssao.bind();
            hdrRT.bindDepthTexture(0);
            ssao.setInt("uDepth", 0);
            ssao.setMat4("uProjection", proj);
            ssao.setMat4("uInvProjection", glm::inverse(proj));
            ssao.setFloat("uRadius", ssaoRadius);
            ssao.setFloat("uBias", ssaoBias);
            ssao.setFloat("uPower", ssaoPower);
            fsQuad.draw();
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);

            // --- Composite: bloom + god rays + lens flare + tonemap ------
            // Project the sun to screen space for the rays/flare.
            const glm::vec4 sunClip = mainVP * glm::vec4(camPos + sunDir * 3000.0f, 1.0f);
            glm::vec2 sunUV(0.5f);
            float sunOnScreen = 0.0f;
            if (sunClip.w > 1e-4f) {
                sunUV = glm::vec2(sunClip) / sunClip.w * 0.5f + 0.5f;
                if (sunUV.x > -0.3f && sunUV.x < 1.3f &&
                    sunUV.y > -0.3f && sunUV.y < 1.3f && sunDir.y > -0.05f) {
                    sunOnScreen = 1.0f;
                }
            }

            RenderTarget::unbind(fbW, fbH);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glDisable(GL_CULL_FACE);
            composite.bind();
            hdrRT.bindColorTexture(0);
            composite.setInt("uHdr", 0);
            ssaoRT.bindColorTexture(2);
            composite.setInt("uAO", 2);
            composite.setFloat("uAoStrength", ssaoStrength);
            composite.setVec2("uTexel", {1.0f / fbW, 1.0f / fbH});
            composite.setFloat("uAspect", aspect);
            composite.setFloat("uExposure", exposure);
            composite.setVec2("uSunUV", sunUV);
            composite.setFloat("uSunOnScreen", sunOnScreen);
            composite.setVec3("uSunColor", sunCol);
            composite.setFloat("uBloom", bloomIntensity);
            composite.setFloat("uRays", rayIntensity);
            composite.setFloat("uHueShift", hueShift);
            composite.setFloat("uSaturation", saturation);
            composite.setFloat("uValue", valueGain);
            fsQuad.draw();
            glDepthMask(GL_TRUE);
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);

            gui.endFrame();
            window.swapBuffers();
        }

        glDeleteBuffers(1, &rainVBO);
        glDeleteVertexArrays(1, &rainVAO);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Fatal: %s\n", e.what());
        return 1;
    }

    return 0;
}
