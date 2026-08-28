// capturecheck -- does the offline renderer see the scene the raster path drew?
//
// pathcheck answers whether the tracer computes light correctly, on scenes built
// by hand. This answers the other half, and the half more likely to be quietly
// wrong: whether the SCENE handed to it is the one on screen. Everything in
// PathTraceCapture.cpp is a translation -- vertices out of a GPU buffer, normals
// through an inverse transpose, a material out of a uniform table, a texture out
// of a texture unit -- and a translation that is slightly wrong still produces a
// picture. A sheared normal, a texture read back with the rows swapped, a scale
// applied twice: all of those render, and none of them announces itself.
//
// So this builds a small scene through the real engine types, submits it to a
// real Renderer, harvests it exactly as the editor's Render panel does, and then
// asserts on things that have arithmetic answers -- where a vertex ended up,
// which way a normal points under a non-uniform scale, what colour a texel is,
// what the material said. It needs a GL context for the same reason the harvest
// does: the geometry only exists inside the driver's buffers.
//
//   build/release/bin/capturecheck.exe [outDir]

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fitzel/graphics/EnvironmentIBL.hpp>
#include <fitzel/graphics/Material.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/render/Renderer.hpp>
#include <fitzel/scene/Camera.hpp>

#include "../src/PathTraceCapture.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    std::printf("  [%s] %s%s%s\n", ok ? " ok " : "FAIL", what,
                detail.empty() ? "" : " -- ", detail.c_str());
    if (!ok) ++g_failures;
}

bool near(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }
bool near3(const glm::vec3& a, const glm::vec3& b, float eps = 1e-3f) {
    return near(a.x, b.x, eps) && near(a.y, b.y, eps) && near(a.z, b.z, eps);
}
std::string str3(const glm::vec3& v) {
    char b[96];
    std::snprintf(b, sizeof(b), "(%.3f, %.3f, %.3f)", v.x, v.y, v.z);
    return b;
}

// One flat quad in the XZ plane at y = 0, spanning [-1, 1], normal +Y, UVs
// covering the whole map. Small enough that every assertion below can be
// written out by hand, which is the point.
fitzel::MeshData quadData() {
    fitzel::MeshData d;
    auto v = [](float x, float z, float u, float w) {
        fitzel::Vertex vert;
        vert.position = {x, 0.0f, z};
        vert.normal   = {0.0f, 1.0f, 0.0f};
        vert.uv       = {u, w};
        return vert;
    };
    d.vertices = {v(-1, -1, 0, 0), v(1, -1, 1, 0), v(1, 1, 1, 1), v(-1, 1, 0, 1)};
    d.indices  = {0, 1, 2, 0, 2, 3};
    return d;
}

// --- Looking at a panorama --------------------------------------------------
// Not a test: a way of SEEING what the offline renderer sees when a scene lights
// from an HDRI. "The colours of the map are wrong" is a claim nothing in a
// finished render can settle, because the map is only ever visible through the
// tonemap, the grade and whatever the light did on the way. This writes it out
// twice -- straight from the buffer, and through the tracer's own direction
// lookup -- so the two can be put side by side and a disagreement between them
// has nowhere left to hide.
void dumpPanorama(const std::string& path, const std::filesystem::path& outDir) {
    std::printf("\npanorama: %s\n", path.c_str());
    fitzel::EnvironmentIBL::Panorama pano =
        fitzel::EnvironmentIBL::loadPanorama(path);
    if (!pano.valid()) {
        std::printf("  [FAIL] could not be read\n");
        ++g_failures;
        return;
    }
    std::printf("  %dx%d, rescaled by x%.4g on load\n",
                pano.width, pano.height, pano.exposureScale);

    // What is actually in the buffer. Reported because the whole question is
    // whether these numbers are the ones the viewport lights from.
    double sum[3] = {0, 0, 0};
    float  mx[3]  = {0, 0, 0};
    for (std::size_t i = 0; i + 2 < pano.pixels.size(); i += 3)
        for (int c = 0; c < 3; ++c) {
            sum[c] += pano.pixels[i + c];
            mx[c]   = std::max(mx[c], pano.pixels[i + c]);
        }
    const double n = static_cast<double>(pano.pixels.size() / 3);
    std::printf("  mean rgb (%.4f, %.4f, %.4f), peak (%.1f, %.1f, %.1f)\n",
                sum[0] / n, sum[1] / n, sum[2] / n, mx[0], mx[1], mx[2]);

    pathtrace::Environment env;
    env.pixels    = pano.pixels;
    env.width     = pano.width;
    env.height    = pano.height;
    env.intensity = 1.0f;

    // 1: the buffer as it lies, at a size that can be looked at. Row 0 of the
    // buffer is the GROUND (the loader hands panoramas over bottom-up), so this
    // writes the last row first -- upright, the way the file looked.
    constexpr int kW = 800, kH = 400;
    std::vector<unsigned char> direct(static_cast<std::size_t>(kW) * kH * 4, 255);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const int sx = x * pano.width / kW;
            const int sy = (kH - 1 - y) * pano.height / kH;
            const std::size_t o =
                (static_cast<std::size_t>(sy) * pano.width + sx) * 3;
            const glm::vec3 c = pathtrace::tonemap(
                {pano.pixels[o], pano.pixels[o + 1], pano.pixels[o + 2]}, 1.0f, {});
            const std::size_t d = (static_cast<std::size_t>(y) * kW + x) * 4;
            for (int k = 0; k < 3; ++k)
                direct[d + k] = static_cast<unsigned char>(c[k] * 255.0f + 0.5f);
        }
    const std::filesystem::path f1 = outDir / "panorama-buffer.png";
    stbi_write_png(f1.string().c_str(), kW, kH, 4, direct.data(), kW * 4);
    std::printf("  wrote %s (the buffer, upright)\n", f1.string().c_str());

    // 2: the same image rebuilt by asking the tracer's lookup for a direction
    // per pixel. If the lookup agrees with the mapping the engine uses, this is
    // the picture above. If it does not, the difference IS the bug.
    std::vector<unsigned char> viaLookup(static_cast<std::size_t>(kW) * kH * 4, 255);
    for (int y = 0; y < kH; ++y) {
        const float elev = 3.14159265f * (0.5f - (static_cast<float>(y) + 0.5f) / kH);
        for (int x = 0; x < kW; ++x) {
            const float phi = 6.2831853f * ((static_cast<float>(x) + 0.5f) / kW - 0.5f);
            const glm::vec3 dir(std::cos(elev) * std::cos(phi), std::sin(elev),
                                std::cos(elev) * std::sin(phi));
            const glm::vec3 c = pathtrace::tonemap(env.sample(dir), 1.0f, {});
            const std::size_t d = (static_cast<std::size_t>(y) * kW + x) * 4;
            for (int k = 0; k < 3; ++k)
                viaLookup[d + k] = static_cast<unsigned char>(c[k] * 255.0f + 0.5f);
        }
    }
    const std::filesystem::path f2 = outDir / "panorama-lookup.png";
    stbi_write_png(f2.string().c_str(), kW, kH, 4, viaLookup.data(), kW * 4);
    std::printf("  wrote %s (through the tracer's direction lookup)\n",
                f2.string().c_str());

    // Compared in BLOCKS, not pixel by pixel. Both pictures are a 4K panorama
    // squeezed to a fifth of its width, and a corrugated roof at that scale
    // differs wildly between any two ways of choosing which texels to look at.
    // Per-pixel this reads as a 20% failure and means nothing; what the mapping
    // has to get right is WHERE the content lands, and that survives averaging.
    constexpr int kBlock = 16;
    long long differ = 0, blocks = 0;
    for (int by = 0; by + kBlock <= kH; by += kBlock)
        for (int bx = 0; bx + kBlock <= kW; bx += kBlock) {
            int accA[3] = {0, 0, 0}, accB[3] = {0, 0, 0};
            for (int y = by; y < by + kBlock; ++y)
                for (int x = bx; x < bx + kBlock; ++x) {
                    const std::size_t i =
                        (static_cast<std::size_t>(y) * kW + x) * 4;
                    for (int c = 0; c < 3; ++c) {
                        accA[c] += direct[i + c];
                        accB[c] += viaLookup[i + c];
                    }
                }
            const int n = kBlock * kBlock;
            bool bad = false;
            for (int c = 0; c < 3; ++c)
                if (std::abs(accA[c] - accB[c]) / n > 16) bad = true;
            if (bad) ++differ;
            ++blocks;
        }
    const double frac = static_cast<double>(differ) /
                        static_cast<double>(std::max<long long>(blocks, 1));
    check(frac < 0.02, "the direction lookup puts the panorama where it belongs",
          std::to_string(frac * 100.0) + "% of blocks disagree");

    // 3: the panorama as the RENDERER sees it -- traced, not merely looked
    // up. This is the one that can reproduce "the brightest areas come out
    // yellow and cyan", because that artefact needs the whole chain: the
    // importance sampler's own idea of where the light is, the division by
    // its density, and the tonemap at the end. Rendered at a deliberately
    // generous exposure, since the complaint is always about the bright end.
    {
        auto sky = std::make_shared<pathtrace::Scene>();
        sky->materials.emplace_back();
        sky->sun.enabled = false;
        sky->env      = env;
        sky->exposure = 2.0f;
        sky->camera.position = glm::vec3(0.0f);
        sky->camera.forward  = glm::normalize(glm::vec3(0.0f, 0.15f, -1.0f));
        sky->camera.right    = glm::normalize(glm::cross(sky->camera.forward,
                                                         glm::vec3(0, 1, 0)));
        sky->camera.up       = glm::normalize(glm::cross(sky->camera.right,
                                                         sky->camera.forward));
        sky->camera.fovDegrees = 70.0f;

        pathtrace::Settings s;
        s.width = 640; s.height = 360; s.samples = 16; s.batch = 8;
        s.maxBounces = 2; s.tonemap = true;
        pathtrace::Job job;
        job.start(sky, s);
        while (job.running())
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::vector<unsigned char> px;
        if (job.snapshotLdr(px)) {
            const std::filesystem::path f3 = outDir / "panorama-traced.png";
            stbi_write_png(f3.string().c_str(), s.width, s.height, 4,
                           px.data(), s.width * 4);
            std::printf("  wrote %s (the sky as the renderer traces it)\n",
                        f3.string().c_str());

            // A saturated channel sitting next to a missing one is what
            // yellow, cyan and black actually ARE. Counted rather than
            // looked at, so this can fail on a build server too.
            long long broken = 0;
            for (std::size_t i = 0; i + 2 < px.size(); i += 4) {
                const int hi = std::max(px[i], std::max(px[i + 1], px[i + 2]));
                const int lo = std::min(px[i], std::min(px[i + 1], px[i + 2]));
                if (hi > 230 && lo < 40) ++broken;
            }
            const double bfrac = static_cast<double>(broken) /
                                 static_cast<double>(s.width * s.height);
            check(bfrac < 0.005,
                  "no pixel is a saturated channel beside a missing one",
                  std::to_string(bfrac * 100.0) + "% of pixels");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path outDir = argc > 1 ? argv[1] : ".";
    std::error_code ec;
    std::filesystem::create_directories(outDir, ec);

    if (!glfwInit()) {
        std::printf("[FAIL] glfwInit\n");
        return 2;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "capturecheck", nullptr, nullptr);
    if (!win) {
        std::printf("[FAIL] no GL 3.3 core context\n");
        glfwTerminate();
        return 2;
    }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[FAIL] glad\n");
        glfwTerminate();
        return 2;
    }
    std::printf("capturecheck -- %s\n\n",
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    // --- A scene, built the way the editor builds one --------------------
    // The shader is never used (nothing is drawn here), but a Material needs
    // one and a Material is what carries the surface parameters.
    fitzel::Shader shader = fitzel::Shader::fromSource(
        "#version 330 core\nvoid main(){gl_Position=vec4(0);}",
        "#version 330 core\nout vec4 c;\nvoid main(){c=vec4(1);}");

    const fitzel::MeshData data = quadData();
    fitzel::Mesh quad = fitzel::Mesh::create(data);

    // A 2x2 texture with four distinguishable texels, so a readback that
    // flipped or swizzled it cannot pass.
    const unsigned char texels[] = {
        255,   0,   0, 255,    0, 255,   0, 255,   // row 0: red,  green
          0,   0, 255, 255,  255, 255, 255, 128,   // row 1: blue, half-alpha white
    };
    fitzel::Texture tex = fitzel::Texture::fromPixels(texels, 2, 2, 4);

    fitzel::Material plain(shader);
    plain.set("uColorMode", 0)
         .set("uAlbedo", glm::vec3(0.8f, 0.1f, 0.05f))
         .set("uRoughness", 0.25f)
         .set("uReflectivity", 0.6f)
         .set("uEmission", glm::vec3(0.0f, 0.5f, 0.0f))
         .set("uEmissionStrength", 3.0f);

    fitzel::Material textured(shader);
    textured.set("uColorMode", 2).set("uTint", glm::vec3(0.5f, 1.0f, 1.0f));
    textured.setTexture("uTexture", tex, 0);

    // An OPAQUE material carrying a texture whose alpha channel is not 1. This
    // is the ordinary case, not a corner one -- a vehicle's livery atlas has an
    // alpha channel and the material is solid paint -- and reading that alpha
    // as transparency puts holes through the bodywork, which shows up as the
    // wrong colour and a great deal of noise rather than as anything that looks
    // like transparency.
    const unsigned char halfAlpha[] = {
        200, 100,  40, 128,   200, 100,  40, 128,
        200, 100,  40, 128,   200, 100,  40, 128,
    };
    fitzel::Texture paintTex = fitzel::Texture::fromPixels(halfAlpha, 2, 2, 4);
    fitzel::Material paint(shader);
    paint.set("uColorMode", 2).set("uTint", glm::vec3(1.0f)).set("uAlphaCutout", 0);
    paint.setTexture("uTexture", paintTex, 0);

    fitzel::Camera camera(glm::vec3(0.0f, 3.0f, 8.0f));
    fitzel::Renderer renderer;
    fitzel::DirectionalLight light;
    light.direction = glm::normalize(glm::vec3(0.3f, 1.0f, 0.2f));
    light.color     = glm::vec3(2.0f, 1.9f, 1.7f);
    light.ambient   = glm::vec3(0.20f, 0.22f, 0.26f);

    renderer.begin(camera, 16.0f / 9.0f, light);

    // 1: the quad, moved and scaled NON-UNIFORMLY. The scale is the point --
    // an inverse transpose and a plain mat3 agree on every uniform scale in
    // the world, so a bug there is invisible until something is squashed.
    const glm::mat4 squashed =
        glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 0.0f)),
                   glm::vec3(4.0f, 1.0f, 0.5f));
    renderer.submit(quad, plain, squashed);

    // 2: the same mesh again, elsewhere. One readback must serve both.
    const glm::mat4 moved = glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, 0.0f, 0.0f));
    renderer.submit(quad, textured, moved);

    // 3: far away, to be culled by the distance limit.
    const glm::mat4 distant = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -900.0f));
    renderer.submit(quad, plain, distant);

    // A rotated quad, so the normal test has an answer that is not +Y. Rotating
    // 90 degrees about +X is right-handed, so the +Y normal goes to +Z.
    const glm::mat4 tipped = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f),
                                         glm::vec3(1.0f, 0.0f, 0.0f));
    renderer.submit(quad, plain, tipped);

    // Submitted as the editor submits an opaque material: full opacity, and
    // forceTransparent FALSE, which is the engine saying "the alpha in that
    // map is not transparency".
    const glm::mat4 paintXform =
        glm::translate(glm::mat4(1.0f), glm::vec3(6.0f, 0.0f, 0.0f));
    renderer.submit(quad, paint, paintXform, true, false, 1.0f, false);

    fitzel::PointLight lamp;
    lamp.position = {1.0f, 2.0f, 1.0f};
    lamp.color    = glm::vec3(4.0f, 2.0f, 1.0f);
    lamp.range    = 15.0f;
    renderer.setPointLights({lamp});

    fitzel::SpotLight spot;
    spot.position  = {-2.0f, 3.0f, 2.0f};
    spot.direction = glm::normalize(glm::vec3(0.0f, -1.0f, 0.0f));
    spot.color     = glm::vec3(3.0f);
    spot.cosInner  = 0.9f;
    spot.cosOuter  = 0.8f;
    renderer.setSpotLights({spot});

    fitzel::Fog fog;
    fog.density = 0.004f;
    fog.color   = glm::vec3(0.7f, 0.8f, 0.95f);
    renderer.setFog(fog);
    renderer.setExposure(1.3f);

    // --- Harvest ----------------------------------------------------------
    std::printf("harvesting the render queue\n");
    pathcapture::Options opt;
    opt.maxDistance = 100.0f;   // the distant quad is 900 away
    opt.sunAngleDeg = 1.0f;
    opt.lampRadius  = 0.2f;
    pathcapture::Report rep;
    std::shared_ptr<pathtrace::Scene> scene =
        pathcapture::capture(renderer, camera, opt, &rep);
    std::printf("  %s\n", rep.summary().c_str());

    // Four quads survive the distance cull, two triangles each.
    check(scene->triangles.size() == 8, "every submitted mesh arrived",
          std::to_string(scene->triangles.size()) + " triangles");
    check(rep.meshes == 1, "the shared mesh was read back once, not once per draw",
          std::to_string(rep.meshes) + " readbacks for " +
          std::to_string(rep.instances) + " draws");
    check(rep.culled == 2, "the distant quad was culled",
          std::to_string(rep.culled) + " triangles culled");

    // --- Geometry ---------------------------------------------------------
    std::printf("\nvertices and normals\n");
    {
        // The squashed quad: local (-1, 0, -1) scaled by (4, 1, 0.5) and moved
        // to (2, 1, 0) lands at (-2, 1, -0.5). If the readback lost the vertex
        // stride, or the transform was applied in the wrong order, it will not.
        bool found = false;
        for (const pathtrace::Triangle& t : scene->triangles)
            for (const glm::vec3& p : {t.p0, t.p1, t.p2})
                if (near3(p, glm::vec3(-2.0f, 1.0f, -0.5f))) found = true;
        check(found, "a vertex landed where the model matrix says it should",
              "looking for (-2.000, 1.000, -0.500)");

        // Under a non-uniform scale the +Y normal must stay +Y. A plain mat3
        // would give the same answer here, so the test that matters is the
        // rotated quad below -- this one catches a normal that was scaled.
        bool flatNormalsUp = true;
        for (const pathtrace::Triangle& t : scene->triangles) {
            const glm::vec3 n = glm::normalize(t.n0);
            if (near3(t.p0, glm::vec3(-2.0f, 1.0f, -0.5f), 4.0f) &&
                std::fabs(n.y) < 0.99f && std::fabs(n.z) < 0.99f)
                flatNormalsUp = false;
        }
        check(flatNormalsUp, "a non-uniform scale did not shear the normals");

        // The tipped quad: rotating +Y by 90 degrees about +X gives -Z.
        bool tippedFound = false;
        for (const pathtrace::Triangle& t : scene->triangles) {
            const glm::vec3 n = glm::normalize(t.n0);
            if (near3(n, glm::vec3(0.0f, 0.0f, 1.0f), 1e-3f)) tippedFound = true;
        }
        check(tippedFound, "a rotated mesh's normal was rotated with it",
              "expecting a (0, 0, 1) normal from the 90 degree quad");
    }

    // --- Alpha modes ------------------------------------------------------
    std::printf("\nalpha modes\n");
    {
        const pathtrace::Material* paintMat = nullptr;
        for (const pathtrace::Material& m : scene->materials)
            if (m.texture >= 0 && m.tint.g > 0.99f && m.tint.b > 0.99f &&
                m.tint.r > 0.99f)
                paintMat = &m;
        check(paintMat != nullptr, "the opaque textured material came through");
        if (paintMat) {
            check(paintMat->alphaMode == 0,
                  "a texture's alpha channel alone does not make a material "
                  "transparent",
                  "alphaMode " + std::to_string(paintMat->alphaMode));
            check(paintMat->opacity >= 0.999f, "and it kept full opacity",
                  std::to_string(paintMat->opacity));
        }
    }

    // --- Materials --------------------------------------------------------
    std::printf("\nmaterials\n");
    {
        const pathtrace::Material* flat = nullptr;
        const pathtrace::Material* mapped = nullptr;
        for (const pathtrace::Material& m : scene->materials) {
            // By its tint, not merely by having a texture: there are two
            // textured materials now and picking whichever came last is how a
            // test starts checking something other than what it says.
            if (m.texture >= 0 && near(m.tint.r, 0.5f)) mapped = &m;
            else if (m.texture < 0 && near(m.reflectivity, 0.6f)) flat = &m;
        }
        check(flat != nullptr, "the flat material came through");
        if (flat) {
            check(near3(flat->albedo, glm::vec3(0.8f, 0.1f, 0.05f)),
                  "albedo survived the trip", str3(flat->albedo));
            check(near(flat->roughness, 0.25f), "roughness survived",
                  std::to_string(flat->roughness));
            check(near3(flat->emission, glm::vec3(0.0f, 0.5f, 0.0f)) &&
                  near(flat->emissionStrength, 3.0f),
                  "emission kept its colour and its strength separately",
                  str3(flat->emission) + " x" + std::to_string(flat->emissionStrength));
        }
        check(mapped != nullptr, "the textured material kept its map");
        if (mapped)
            check(near3(mapped->tint, glm::vec3(0.5f, 1.0f, 1.0f)),
                  "the tint came with it", str3(mapped->tint));
    }

    // --- Textures ---------------------------------------------------------
    std::printf("\ntexture readback\n");
    {
        check(scene->textures.size() == 2, "both textures, each read back once",
              std::to_string(scene->textures.size()));
        if (!scene->textures.empty()) {
            const pathtrace::Image& img = scene->textures[0];
            check(img.width == 2 && img.height == 2, "size preserved");
            // Sampled at the texel centres. Bilinear with wrapping means the
            // centre of a texel is still exactly that texel.
            const glm::vec4 tl = img.sample(0.25f, 0.25f);
            const glm::vec4 tr = img.sample(0.75f, 0.25f);
            const glm::vec4 bl = img.sample(0.25f, 0.75f);
            const glm::vec4 br = img.sample(0.75f, 0.75f);
            check(near3(glm::vec3(tl), glm::vec3(1, 0, 0), 0.02f),
                  "top-left texel is the red one it was uploaded as",
                  str3(glm::vec3(tl)));
            check(near3(glm::vec3(tr), glm::vec3(0, 1, 0), 0.02f),
                  "the rows were not swapped", str3(glm::vec3(tr)));
            check(near3(glm::vec3(bl), glm::vec3(0, 0, 1), 0.02f),
                  "nor the columns", str3(glm::vec3(bl)));
            check(near(br.a, 128.0f / 255.0f, 0.02f),
                  "alpha survived (this is what cutout foliage depends on)",
                  std::to_string(br.a));
        }
    }

    // --- Lights, fog, camera ----------------------------------------------
    std::printf("\nlights, fog and the eye\n");
    {
        check(scene->lamps.size() == 2, "both lamps arrived",
              std::to_string(scene->lamps.size()));
        bool haveSpot = false, havePoint = false;
        for (const pathtrace::Lamp& l : scene->lamps) {
            if (l.isSpot()) haveSpot = true; else havePoint = true;
        }
        check(havePoint && haveSpot, "a point light and a spot, told apart");
        check(near3(scene->sun.color, glm::vec3(2.0f, 1.9f, 1.7f)),
              "the sun is the renderer's own, not a reconstruction",
              str3(scene->sun.color));
        check(near(scene->sun.angularRadiusDeg, 1.0f), "the sun's disc was set");
        check(near(scene->fog.density, 0.004f), "fog came across",
              std::to_string(scene->fog.density));
        check(near(scene->exposure, 1.3f), "exposure came across",
              std::to_string(scene->exposure));
        check(near3(scene->camera.position, glm::vec3(0.0f, 3.0f, 8.0f)),
              "the eye is where the viewport's is", str3(scene->camera.position));
        check(near(glm::length(scene->camera.forward), 1.0f) &&
              near(glm::dot(scene->camera.forward, scene->camera.up), 0.0f, 1e-3f),
              "the camera basis is orthonormal");
    }

    // --- End to end -------------------------------------------------------
    // The harvested scene actually traces. Not a picture anybody wants, but it
    // is the one assertion that covers the join between the two halves.
    std::printf("\ntracing the harvested scene\n");
    {
        pathtrace::Settings s;
        s.width = 240; s.height = 135; s.samples = 32; s.batch = 8;
        s.maxBounces = 3;
        pathtrace::Job job;
        job.start(scene, s);
        while (job.running()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::vector<unsigned char> px;
        const bool got = job.snapshotLdr(px);
        check(got && px.size() == 240u * 135u * 4u, "it rendered");
        if (got) {
            long long lit = 0;
            for (std::size_t i = 0; i < px.size(); i += 4)
                if (px[i] > 8 || px[i + 1] > 8 || px[i + 2] > 8) ++lit;
            check(lit > 240 * 135 / 10,
                  "the frame is not black -- geometry and light both arrived",
                  std::to_string(lit) + " lit pixels of " + std::to_string(240 * 135));
            const std::filesystem::path f = outDir / "capturecheck.png";
            stbi_write_png(f.string().c_str(), 240, 135, 4, px.data(), 240 * 4);
            std::printf("  wrote %s\n", f.string().c_str());
        }
    }

    for (const std::string& n : rep.notes) std::printf("  note: %s\n", n.c_str());

    // Optional: a panorama to look at, for when a render's sky is in question.
    if (argc > 2) dumpPanorama(argv[2], outDir);

    glfwDestroyWindow(win);
    glfwTerminate();
    std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "passed",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
