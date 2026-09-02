#include "PathTraceCapture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <unordered_map>

#include <glm/gtc/matrix_inverse.hpp>

#include <fitzel/graphics/EnvironmentIBL.hpp>
#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/render/Renderer.hpp>
#include <fitzel/scene/Camera.hpp>

namespace pathcapture {
namespace {

// The uniform names lit.frag declares. Read rather than guessed: a material is
// whatever it set on itself, and asking it is the only way to stay right when
// somebody adds a surface parameter.
constexpr const char* kColorMode    = "uColorMode";
constexpr const char* kAlbedo       = "uAlbedo";
constexpr const char* kTint         = "uTint";
constexpr const char* kRoughness    = "uRoughness";
constexpr const char* kReflectivity = "uReflectivity";
constexpr const char* kAlpha        = "uAlpha";
constexpr const char* kGlass        = "uGlass";
constexpr const char* kIor          = "uIor";
constexpr const char* kAlphaCutout  = "uAlphaCutout";
constexpr const char* kAlphaCutoff  = "uAlphaCutoff";
constexpr const char* kEmission     = "uEmission";
constexpr const char* kEmissionStr  = "uEmissionStrength";
constexpr const char* kTexture      = "uTexture";
constexpr const char* kLayerCount   = "uLayerCount";
constexpr const char* kDetailScale  = "uDetailScale";
constexpr const char* kDetailStr    = "uDetailStrength";
// The most layers lit.frag declares (MAX_TERRAIN_LAYERS).
constexpr int kMaxTerrainLayers = 6;

// Quantised so a voxel mesh's per-vertex colours collapse back onto the palette
// they were painted from instead of producing one material per triangle.
std::uint32_t colorKey(const glm::vec3& c) {
    auto q = [](float v) {
        return static_cast<std::uint32_t>(glm::clamp(v, 0.0f, 1.0f) * 63.0f + 0.5f);
    };
    return (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}

// Box-downsample an RGBA8 image so its longest side fits `maxDim`. Integer
// factor, whole blocks averaged -- good enough for a base-colour map and far
// better than the alternative this replaces, which was to DROP the texture.
// A dropped map leaves the surface on its flat base colour, and a car whose
// livery is a 4K atlas then renders as a white blank with nothing on screen
// saying why.
pathtrace::Image downsample(fitzel::ImagePixels src, int maxDim) {
    pathtrace::Image out;
    int factor = 1;
    while (std::max(src.width, src.height) / factor > maxDim) factor *= 2;
    if (factor == 1) {
        out.width  = src.width;
        out.height = src.height;
        out.pixels = std::move(src.pixels);
        return out;
    }
    out.width  = std::max(1, src.width  / factor);
    out.height = std::max(1, src.height / factor);
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            int acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int by = 0; by < factor; ++by) {
                const int sy = y * factor + by;
                if (sy >= src.height) break;
                for (int bx = 0; bx < factor; ++bx) {
                    const int sx = x * factor + bx;
                    if (sx >= src.width) break;
                    const std::size_t o =
                        (static_cast<std::size_t>(sy) * src.width + sx) * 4;
                    for (int c = 0; c < 4; ++c) acc[c] += src.pixels[o + c];
                    ++n;
                }
            }
            const std::size_t d = (static_cast<std::size_t>(y) * out.width + x) * 4;
            for (int c = 0; c < 4; ++c)
                out.pixels[d + c] = static_cast<unsigned char>(acc[c] / std::max(n, 1));
        }
    }
    return out;
}

} // namespace

std::string Report::summary() const {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "%lld triangles from %d meshes (%d draws), %d materials, "
                  "%d textures (%d shrunk, %.1f MB), %lld culled, %.2fs",
                  triangles, meshes, instances, materials, textures, texturesShrunk,
                  static_cast<double>(textureBytes) / (1024.0 * 1024.0),
                  culled, seconds);
    return buf;
}

std::shared_ptr<pathtrace::Scene> capture(const fitzel::Renderer& renderer,
                                          const fitzel::Camera& camera,
                                          const Options& options,
                                          Report* report) {
    const auto started = std::chrono::steady_clock::now();
    auto scene = std::make_shared<pathtrace::Scene>();
    Report rep;

    // --- Camera -------------------------------------------------------------
    // Taken as a basis, not as yaw/pitch: a scene camera may be rolled (a
    // chase camera round a loop is), and there is no yaw/pitch pair for that.
    pathtrace::CameraDesc& cam = scene->camera;
    cam.position   = camera.position();
    cam.forward    = glm::normalize(camera.front());
    cam.up         = glm::normalize(camera.up());
    cam.right      = glm::normalize(camera.right());
    cam.fovDegrees = camera.fov();

    // --- Lights -------------------------------------------------------------
    const fitzel::DirectionalLight& light = renderer.light();
    scene->sun.direction        = glm::normalize(light.direction);
    scene->sun.color            = light.color;
    scene->sun.angularRadiusDeg = std::max(0.0f, options.sunAngleDeg);
    scene->sun.enabled          = true;

    for (const fitzel::PointLight& p : renderer.pointLights()) {
        pathtrace::Lamp lamp;
        lamp.position = p.position;
        lamp.color    = p.color;
        lamp.range    = p.range;
        lamp.radius   = std::max(0.0f, options.lampRadius);
        lamp.cosOuter = -2.0f; // omnidirectional
        scene->lamps.push_back(lamp);
    }
    for (const fitzel::SpotLight& sp : renderer.spotLights()) {
        pathtrace::Lamp lamp;
        lamp.position  = sp.position;
        lamp.direction = glm::normalize(sp.direction);
        lamp.color     = sp.color;
        lamp.range     = sp.range;
        lamp.radius    = std::max(0.0f, options.lampRadius);
        lamp.cosInner  = sp.cosInner;
        lamp.cosOuter  = sp.cosOuter;
        scene->lamps.push_back(lamp);
    }

    // --- Fog and exposure ---------------------------------------------------
    const fitzel::Fog& fog = renderer.fog();
    scene->fog.color         = fog.color;
    scene->fog.sunColor      = fog.sunColor;
    scene->fog.density       = fog.density;
    scene->fog.heightFalloff = fog.heightFalloff;
    scene->fog.height        = fog.height;
    scene->exposure          = renderer.exposure();
    scene->grade             = options.grade;

    // --- Environment --------------------------------------------------------
    // Intensity zero means the scene has a panorama loaded but is not lighting
    // from it (the Environment panel's IBL switch is off). Reading it anyway
    // would give a sky multiplied by nothing -- black -- where the viewport
    // shows the sky shader.
    if (!options.hdriPath.empty() && options.hdriIntensity > 0.0f) {
        // Through EnvironmentIBL, not through a second decode: the panorama is
        // rescaled to a common brightness when it is loaded, and a tracer that
        // read the raw file would light the scene at a different exposure than
        // the viewport with nothing to say which one was right.
        fitzel::EnvironmentIBL::Panorama pano =
            fitzel::EnvironmentIBL::loadPanorama(options.hdriPath);
        if (pano.valid()) {
            scene->env.pixels    = std::move(pano.pixels);
            scene->env.width     = pano.width;
            scene->env.height    = pano.height;
            scene->env.intensity = options.hdriIntensity;
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                          "sky: panorama %dx%d at intensity %.2f",
                          pano.width, pano.height, options.hdriIntensity);
            rep.notes.emplace_back(buf);
        } else {
            rep.notes.emplace_back("environment map could not be read: " +
                                   options.hdriPath);
        }
    }
    if (!scene->env.hasMap()) {
        // No panorama, so the flat ambient the raster path uses stands in --
        // spread over a gradient rather than left flat, because a surface facing
        // up and one facing down lit identically is the single most obvious
        // giveaway of an ambient term pretending to be a sky. The average is
        // kept at the ambient, so the overall brightness matches the viewport.
        const glm::vec3 amb = light.ambient;
        scene->env.zenith    = amb * 1.30f;
        scene->env.horizon   = amb;
        scene->env.ground    = amb * 0.45f;
        scene->env.intensity = 1.0f;
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "sky: no panorama, so the scene's flat ambient "
                      "(%.3f, %.3f, %.3f) as a gradient -- not the sky shader's "
                      "clouds", amb.r, amb.g, amb.b);
        rep.notes.emplace_back(buf);
    }

    // --- Geometry -----------------------------------------------------------
    const std::vector<fitzel::Renderer::Submission> queue = renderer.submissions();
    const glm::vec3 eye = cam.position;
    const float maxDist = options.maxDistance > 0.0f ? options.maxDistance : 0.0f;

    // Read each distinct mesh back exactly once, however many times it is drawn.
    // A wheel submitted four times, or a fence post two hundred, would otherwise
    // stall the GPU once per instance.
    std::unordered_map<const fitzel::Mesh*, fitzel::MeshData> meshCache;
    std::unordered_map<const fitzel::Texture*, int>           texCache;
    std::unordered_map<const fitzel::Material*, int>          matCache;
    // Voxel meshes colour their surfaces per vertex, so their materials are
    // built from the colour rather than from the material object.
    std::unordered_map<std::uint64_t, int>                    voxelMatCache;
    std::vector<glm::vec4>                                    paint;

    bool sawTerrainLayers = false;
    bool sawTerrainPaint  = false;
    bool sawWetness       = false;

    auto textureFor = [&](const fitzel::Texture* tex) -> int {
        if (!tex || !tex->isValid()) return -1;
        auto it = texCache.find(tex);
        if (it != texCache.end()) return it->second;
        fitzel::ImagePixels px = tex->readback();
        if (!px.valid()) {
            texCache[tex] = -1;
            rep.notes.emplace_back("a base-colour map could not be read back");
            return -1;
        }
        const bool shrunk = std::max(px.width, px.height) > options.maxTextureSize;
        pathtrace::Image img = downsample(std::move(px), options.maxTextureSize);
        if (shrunk) ++rep.texturesShrunk;
        rep.textureBytes += static_cast<long long>(img.pixels.size());
        const int index = static_cast<int>(scene->textures.size());
        scene->textures.push_back(std::move(img));
        texCache[tex] = index;
        return index;
    };

    // A material, translated into the tracer's terms. `opacity` comes from the
    // submission as well as the material, because the renderer's own per-submit
    // value is what routes a mesh into the transparent queue.
    auto materialFor = [&](const fitzel::Material* mat, float submitOpacity,
                           bool texAlphaIsTransparency) -> int {
        auto it = matCache.find(mat);
        if (it != matCache.end()) {
            // A material already seen keeps its first opacity. Two draws of one
            // material at different opacities is not a case the scene produces.
            return it->second;
        }
        pathtrace::Material m;
        const int colorMode = mat->get<int>(kColorMode, 0);
        m.albedo       = mat->get<glm::vec3>(kAlbedo, glm::vec3(0.72f, 0.72f, 0.74f));
        m.tint         = mat->get<glm::vec3>(kTint, glm::vec3(1.0f));
        m.roughness    = mat->get<float>(kRoughness, 0.4f);
        m.reflectivity = mat->get<float>(kReflectivity, 0.0f);
        m.opacity      = std::min(mat->get<float>(kAlpha, 1.0f), submitOpacity);
        m.glass        = mat->get<int>(kGlass, 0) == 1;
        m.ior          = std::max(1.0f, mat->get<float>(kIor, 1.5f));
        m.emission     = mat->get<glm::vec3>(kEmission, glm::vec3(0.0f));
        m.emissionStrength = mat->get<float>(kEmissionStr, 1.0f);
        m.alphaCutoff  = mat->get<float>(kAlphaCutoff, 0.5f);
        // Which of the three the engine means, taken from the engine rather
        // than guessed from the presence of an alpha channel. Guessing was the
        // bug: a car's livery atlas has alpha in it and is entirely opaque, and
        // treating that alpha as transparency let rays through the paintwork
        // to whatever was behind it -- which is both the wrong colour and a
        // great deal of noise, on exactly the object the picture is of.
        if (mat->get<int>(kAlphaCutout, 0) == 1)            m.alphaMode = 1;
        else if (texAlphaIsTransparency || m.glass ||
                 m.opacity < 0.999f)                        m.alphaMode = 2;
        else                                                m.alphaMode = 0;

        // Rain darkens and glosses sky-facing surfaces in the shader. Not
        // reproduced -- it is a look applied per fragment from world height and
        // slope, not a property of the material -- but a scene rendered in the
        // wet would otherwise come out inexplicably lighter than the viewport,
        // so it is reported.
        if (mat->get<float>("uWetness", 0.0f) > 0.01f) sawWetness = true;

        if (colorMode == 2) {
            m.texture = textureFor(mat->texture(kTexture));
        } else if (colorMode == 1) {
            // Terrain: its painted layers, each with the height and slope band
            // it covers and its own tiling. Read by NAME out of the material,
            // which is how they were set -- so a layer added to the terrain
            // editor arrives here without this code being told about it.
            const int layers = std::min(mat->get<int>(kLayerCount, 0),
                                        kMaxTerrainLayers);
            for (int i = 0; i < layers; ++i) {
                const std::string ix = "[" + std::to_string(i) + "]";
                pathtrace::TerrainLayer L;
                L.texture = textureFor(mat->texture("uLayerTex" + ix));
                L.band    = mat->get<glm::vec4>("uLayerBand" + ix, glm::vec4(0.0f));
                L.scale   = mat->get<float>("uLayerScale" + ix, 0.1f);
                if (L.texture >= 0) m.layers.push_back(L);
            }
            m.detailScale    = mat->get<float>(kDetailScale, 0.0f);
            m.detailStrength = mat->get<float>(kDetailStr, 0.0f);
            if (layers > 0 && m.layers.empty()) sawTerrainLayers = true;
            if (!m.layers.empty()) sawTerrainPaint = true;
        }

        const int index = static_cast<int>(scene->materials.size());
        scene->materials.push_back(m);
        matCache[mat] = index;
        return index;
    };

    for (const fitzel::Renderer::Submission& sub : queue) {
        if (!sub.mesh || !sub.material) continue;
        if (!options.includeTransparent && sub.opacity < 0.999f) continue;

        auto cached = meshCache.find(sub.mesh);
        if (cached == meshCache.end()) {
            fitzel::MeshData data = sub.mesh->readback();
            cached = meshCache.emplace(sub.mesh, std::move(data)).first;
            ++rep.meshes;
        }
        const fitzel::MeshData& data = cached->second;
        if (data.vertices.empty()) continue;
        ++rep.instances;

        const int colorMode = sub.material->get<int>(kColorMode, 0);
        const bool perVertexColor = colorMode == 3;
        const int  flatMaterial   = perVertexColor
                                  ? -1
                                  : materialFor(sub.material, sub.opacity,
                                                sub.textureAlphaIsTransparency);

        const glm::mat4 model = sub.model;
        // Normals need the inverse transpose or a non-uniform scale shears them
        // off the surface -- which shows up as lighting that slides across a
        // squashed object and is very hard to recognise as a normal problem.
        const glm::mat3 normalMat = glm::inverseTranspose(glm::mat3(model));

        const std::size_t triCount = data.indices.empty()
                                   ? data.vertices.size() / 3
                                   : data.indices.size() / 3;
        for (std::size_t t = 0; t < triCount; ++t) {
            const std::uint32_t i0 = data.indices.empty()
                                   ? static_cast<std::uint32_t>(t * 3 + 0) : data.indices[t * 3 + 0];
            const std::uint32_t i1 = data.indices.empty()
                                   ? static_cast<std::uint32_t>(t * 3 + 1) : data.indices[t * 3 + 1];
            const std::uint32_t i2 = data.indices.empty()
                                   ? static_cast<std::uint32_t>(t * 3 + 2) : data.indices[t * 3 + 2];
            if (i0 >= data.vertices.size() || i1 >= data.vertices.size() ||
                i2 >= data.vertices.size())
                continue;

            const fitzel::Vertex& v0 = data.vertices[i0];
            const fitzel::Vertex& v1 = data.vertices[i1];
            const fitzel::Vertex& v2 = data.vertices[i2];

            pathtrace::Triangle tri;
            tri.p0 = glm::vec3(model * glm::vec4(v0.position, 1.0f));
            tri.p1 = glm::vec3(model * glm::vec4(v1.position, 1.0f));
            tri.p2 = glm::vec3(model * glm::vec4(v2.position, 1.0f));

            if (maxDist > 0.0f) {
                const float d = std::min(glm::distance(tri.p0, eye),
                                std::min(glm::distance(tri.p1, eye),
                                         glm::distance(tri.p2, eye)));
                if (d > maxDist) { ++rep.culled; continue; }
            }

            // A degenerate triangle contributes nothing and costs a BVH leaf.
            const glm::vec3 cross = glm::cross(tri.p1 - tri.p0, tri.p2 - tri.p0);
            if (glm::dot(cross, cross) < 1e-16f) continue;

            tri.n0 = normalMat * v0.normal;
            tri.n1 = normalMat * v1.normal;
            tri.n2 = normalMat * v2.normal;
            tri.uv0 = v0.uv;
            tri.uv1 = v1.uv;
            tri.uv2 = v2.uv;

            // Terrain paint. Gathered for every triangle because the table has
            // to line up with the triangle list, and thrown away at the end if
            // the scene turns out to have no painted terrain in it -- which is
            // cheaper than deciding up front and being wrong.
            paint.push_back(v0.paint);
            paint.push_back(v1.paint);
            paint.push_back(v2.paint);

            if (perVertexColor) {
                // Voxels carry their colour in the paint attribute. One
                // material per distinct colour, quantised, so a painted model
                // collapses back onto its palette instead of producing tens of
                // thousands of identical materials.
                const glm::vec3 c = (glm::vec3(v0.paint) + glm::vec3(v1.paint) +
                                     glm::vec3(v2.paint)) / 3.0f;
                const std::uint64_t key = colorKey(c);
                auto vm = voxelMatCache.find(key);
                if (vm == voxelMatCache.end()) {
                    pathtrace::Material m;
                    m.albedo    = c;
                    m.roughness = sub.material->get<float>(kRoughness, 0.6f);
                    m.reflectivity = sub.material->get<float>(kReflectivity, 0.0f);
                    const int index = static_cast<int>(scene->materials.size());
                    scene->materials.push_back(m);
                    vm = voxelMatCache.emplace(key, index).first;
                }
                tri.material = vm->second;
            } else {
                tri.material = flatMaterial;
            }

            scene->triangles.push_back(tri);
        }
    }

    if (scene->materials.empty()) {
        // A scene with geometry but no material would index out of bounds.
        // Cannot happen from the loop above, but a tracer that trusts an empty
        // table crashes rather than renders, so the guard stays.
        scene->materials.emplace_back();
    }

    if (sawTerrainPaint && paint.size() == scene->triangles.size() * 3)
        scene->vertexPaint = std::move(paint);

    rep.triangles = static_cast<long long>(scene->triangles.size());
    rep.materials = static_cast<int>(scene->materials.size());
    rep.textures  = static_cast<int>(scene->textures.size());

    if (sawWetness)
        rep.notes.emplace_back("the scene is wet: the shader's rain darkening and "
                               "sheen are not traced, so surfaces render dry");
    if (sawTerrainLayers)
        rep.notes.emplace_back("a terrain layer's texture could not be read: "
                               "that layer falls back to the base colour");
    if (sawTerrainPaint)
        rep.notes.emplace_back("terrain layers are traced (height/slope bands, "
                               "triplanar, hand-painted weights); their normal "
                               "maps are not");
    rep.notes.emplace_back("grass, trees, particles, rain and water are not "
                               "traced: their geometry only exists in a shader");
    if (rep.culled > 0)
        rep.notes.emplace_back("geometry beyond the distance limit was left out "
                               "-- raise it if a reflection looks cut off");

    {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "look: exposure %.2f, grade sat %.2f warmth %.2f "
                      "contrast %.2f hue %.0f value %.2f",
                      scene->exposure, options.grade.saturation,
                      options.grade.warmth, options.grade.contrast,
                      options.grade.hueShift, options.grade.value);
        rep.notes.emplace_back(buf);
        std::snprintf(buf, sizeof(buf),
                      "sun (%.2f, %.2f, %.2f) over %.2f deg; fog density %.4f; "
                      "%d lamps",
                      scene->sun.color.r, scene->sun.color.g, scene->sun.color.b,
                      scene->sun.angularRadiusDeg, scene->fog.density,
                      static_cast<int>(scene->lamps.size()));
        rep.notes.emplace_back(buf);
    }

    rep.seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
    if (report) *report = std::move(rep);
    return scene;
}

} // namespace pathcapture
