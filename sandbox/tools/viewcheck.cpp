// viewcheck -- render a project's scene offscreen and write the picture out.
//
// Every other harness here MEASURES. This one looks. That is not a lesser thing:
// most of what goes wrong in a renderer produces a picture either way, and the
// questions that matter -- did the paint land where the brush was, is that wall
// wearing the brick or the default grey, is the model inside out -- have no
// arithmetic answer. They have a look, and until now the only way to get one was
// to open the editor and be a person.
//
// It draws through the same code the editor draws through: scenesubmit::submit,
// the same materials out of the same .fmat files, the same shader. That is the
// whole point and the reason SceneSubmit.cpp exists. A harness with its own
// submit loop would be right the day it was written and quietly wrong later, and
// a picture that disagrees with the screen is worse than no picture -- it is a
// wrong answer that looks like evidence.
//
// What it does NOT draw, yet: terrain, sky, water, vegetation, roads and the
// post chain, all of which still live in main()'s frame. So this is the objects
// on a flat ground colour, which is exactly the view for a question about a
// material, a mesh or a stroke of paint, and not one for a question about the
// world they stand in.
//
//   viewcheck <projectFolder> [out.png] [--size WxH] [--yaw deg] [--pitch deg]
//             [--shaders dir] [--shade textured|solid|solidlit|wireframe]
//
// With no camera given it frames the scene's own bounds from a three-quarter
// view, so a scene authored anywhere in the world still lands in the picture.
// Exits non-zero only if it could not render at all -- there is nothing here to
// pass or fail.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <glad/gl.h>
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>   // make_mat4
#include <imgui.h>      // ImGuizmo.h leans on ImGui's types; must come first
#include <ImGuizmo.h>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/asset/Vfs.hpp>
#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/scene/Camera.hpp>
#include <fitzel/render/Renderer.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/Primitives.hpp"
#include "../src/ProjectIO.hpp"
#include "../src/SceneGraph.hpp"
#include "../src/SceneSubmit.hpp"
#include "../src/SceneTypes.hpp"

namespace fs = std::filesystem;

namespace {

// The one colour that stands in for a sky here -- both behind the objects and
// inside the env probe, so a mirror reflects what the picture is made of.
constexpr glm::vec3 kSky{0.42f, 0.47f, 0.55f};

// The scene's own composition, shared with the editor rather than reimplemented
// here -- a matrix built any other way is a picture of a slightly different
// scene. See SceneGraph.hpp.
glm::mat4 composeModel(const glm::vec3& t, const glm::vec3& rotDeg, const glm::vec3& s) {
    return scenegraph::compose(t, rotDeg, s);
}

// The scene's own extent, so a camera can be pointed at it without anybody
// having to know where the author happened to build. Entities carry half-extents
// that are kept honest against their geometry, so the boxes are enough -- no
// need to walk a single vertex.
bool sceneBounds(const std::vector<Entity>& es, glm::vec3& lo, glm::vec3& hi) {
    bool any = false;
    for (const Entity& e : es) {
        if (!e.activeInHierarchy) continue;
        if (e.type == EntityType::Sun || e.type == EntityType::Empty) continue;
        const glm::vec3 a = e.center - e.half, b = e.center + e.half;
        if (!any) { lo = a; hi = b; any = true; continue; }
        lo = glm::min(lo, a);
        hi = glm::max(hi, b);
    }
    return any;
}

struct Options {
    std::string project;
    std::string out = "viewcheck.png";
    std::string shaders = "assets/shaders";
    // The engine's own asset root, resolved the way the editor resolves it: a
    // content/ beside the executable if there is one, else the path baked in at
    // build time. Material textures live under it, so getting this wrong renders
    // textured surfaces flat.
    std::string content = fitzel::vfs::isDirectory("content")
                        ? std::string("content") : std::string(FITZEL_CONTENT_DIR);
    int   width = 1280, height = 720;
    float yaw = -35.0f, pitch = -22.0f;
    // The editor's viewport shading ladder, so a question about a mode has a
    // picture too. The same numbers lit.frag uses: 0 textured, 1 solid,
    // 2 solid lit, 3 wireframe.
    int   shade = 0;
    bool  ok = true;
};

Options parse(int argc, char** argv) {
    Options o;
    std::vector<std::string> loose;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--size") {
            const std::string v = next();
            const std::size_t x = v.find('x');
            if (x == std::string::npos) { o.ok = false; break; }
            o.width  = std::max(16, std::atoi(v.substr(0, x).c_str()));
            o.height = std::max(16, std::atoi(v.substr(x + 1).c_str()));
        } else if (a == "--yaw")     { o.yaw   = static_cast<float>(std::atof(next().c_str()));
        } else if (a == "--pitch")   { o.pitch = static_cast<float>(std::atof(next().c_str()));
        } else if (a == "--shade") {
            const std::string v = next();
            o.shade = (v == "wireframe" || v == "wire") ? 3
                    : (v == "solidlit"  || v == "lit")  ? 2
                    : (v == "solid")                    ? 1
                    : (v == "textured"  || v == "tex")  ? 0
                    : std::atoi(v.c_str());
        } else if (a == "--shaders") { o.shaders = next();
        } else if (a == "--content") { o.content = next();
        } else if (!a.empty() && a[0] == '-') { o.ok = false; break;
        } else { loose.push_back(a); }
    }
    if (!loose.empty()) o.project = loose[0];
    if (loose.size() > 1) o.out = loose[1];
    if (o.project.empty()) o.ok = false;
    return o;
}

} // namespace

int main(int argc, char** argv) {
    const Options opt = parse(argc, argv);
    if (!opt.ok) {
        std::printf("usage: viewcheck <projectFolder> [out.png] [--size WxH]\n"
                    "                 [--yaw deg] [--pitch deg] [--shaders dir]\n"
                    "                 [--shade textured|solid|solidlit|wireframe]\n");
        return 2;
    }
    if (!fs::exists(opt.project)) {
        std::printf("[viewcheck] no such project folder: %s\n", opt.project.c_str());
        return 2;
    }

    // --- A context to draw in ------------------------------------------------
    if (!glfwInit()) { std::printf("[viewcheck] glfwInit failed\n"); return 2; }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    GLFWwindow* win = glfwCreateWindow(64, 64, "viewcheck", nullptr, nullptr);
    if (!win) { std::printf("[viewcheck] no GL 3.3 core context\n"); glfwTerminate(); return 2; }
    glfwMakeContextCurrent(win);
    if (!gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress))) {
        std::printf("[viewcheck] glad failed\n"); glfwTerminate(); return 2;
    }

    // --- The project, loaded the way the editor loads it ---------------------
    // Textures come out of the AssetDatabase here, which needs the GL context
    // above -- which is why the window is opened first even though nothing is
    // drawn for a while yet.
    // The Document owns the two lists; everything else takes a reference to
    // them, which is how main() is wired too.
    Document                  document;
    std::vector<Entity>&      entities  = document.entities();
    std::vector<MaterialDef>& materials = document.materials();
    ModelLibrary              models;
    fitzel::AssetDatabase     assetDb{opt.content};
    int         matSel = 0, entityCounter = 1, entitySel = -1;
    std::string currentProject, prefLocation, exportStatus, uiFontFamily;
    char        projName[128] = {};
    std::vector<std::string> recentProjects;
    float       uiFontSize = 16.0f;
    std::function<void(nlohmann::json&)>       writeSettings = [](nlohmann::json&){};
    std::function<void(const nlohmann::json&)> readSettings  = [](const nlohmann::json&){};
    std::function<void()>                      afterSceneLoad = []{};

    projectio::Context ctx{
        entities, materials, matSel, entityCounter, entitySel,
        currentProject, projName, sizeof(projName), prefLocation,
        recentProjects, (fs::path(opt.project) / "editor.json").generic_string(),
        exportStatus, uiFontSize, uiFontFamily,
        assetDb, opt.project, opt.project,
        [&]{ materials.push_back(MaterialDef{}); },
        [&](const std::string& p) { return models.import(p, assetDb, materials); },
        [&](const std::string& p, int n) { return models.importNode(p, n, true, assetDb, materials); },
        [&](int id) { return models.byId(id); },
        [&]{ models.clear(); },
        writeSettings, readSettings, afterSceneLoad,
    };

    // The project has to be MOUNTED before anything in it is loaded. The GUID a
    // material carries comes from assetDb.idForPath, which only answers for paths
    // under a mounted source -- and loadProjectMaterials quietly mints a fresh
    // random GUID when it does not. Every material then matches nothing the scene
    // refers to and every object falls back to material 0, which is how two
    // spheres both came out wearing "Building A Base" instead of Chrome. The
    // editor mounts it inside its open-project path; this loads more directly and
    // so has to do it here.
    assetDb.mountProject(opt.project);
    assetDb.refresh();

    projectio::loadProjectMaterials(ctx, projectio::matsDirIn(opt.project));
    const std::string scenePath = projectio::sceneFileIn(opt.project);
    if (!projectio::loadScene(ctx, scenePath)) {
        std::printf("[viewcheck] could not load the scene: %s\n", scenePath.c_str());
        glfwTerminate();
        return 2;
    }
    // What is actually in the picture, and what is not. A harness whose answer
    // is "one dark blob" should say whether that is the scene or the renderer --
    // and most of a scene is made of things this does not draw yet.
    int drawn = 0, nModels = 0, nMeshes = 0, nPrims = 0, nLights = 0, nSkipped = 0;
    for (const Entity& e : entities) {
        if (!e.activeInHierarchy || e.type == EntityType::Sun ||
            e.type == EntityType::Empty) { ++nSkipped; continue; }
        if (e.components.get<TerrainComponent>()) { ++nSkipped; continue; }
        ++drawn;
        if (e.type == EntityType::Model)            ++nModels;
        else if (e.components.get<MeshComponent>()) ++nMeshes;
        else if (e.type == EntityType::Light)       ++nLights;
        else                                        ++nPrims;
    }
    // A scene file stores LOCAL transforms; everything downstream reads world
    // ones. Without this every object sits at the origin -- which is exactly how
    // this harness's first picture came out, and how it earned its keep.
    scenegraph::resolve(entities);

    std::printf("[viewcheck] %zu entities, %zu materials\n",
                entities.size(), materials.size());
    std::printf("[viewcheck] drawing %d: %d model(s), %d modelled mesh(es), "
                "%d primitive(s), %d light marker(s); "
                "%d not drawn (sun, groups, terrain)\n",
                drawn, nModels, nMeshes, nPrims, nLights, nSkipped);
    // One line per object. When a picture is surprising the first question is
    // always whether the scene is what you think it is, and answering that by
    // opening the scene file by hand is the thing this harness exists to stop.
    for (const Entity& e : entities) {
        if (!e.activeInHierarchy || e.type == EntityType::Sun ||
            e.type == EntityType::Empty) continue;
        if (e.components.get<TerrainComponent>()) continue;
        const auto* mc = e.components.get<MaterialComponent>();
        const MaterialDef& md = materials[document.materialIndex(
            mc ? mc->material : fitzel::AssetId{})];
        std::printf("             %-18s at (%.1f %.1f %.1f) half %.1f  "
                    "mat %-14s albedo(%.2f %.2f %.2f) refl %.2f%s\n",
                    e.name.c_str(), e.center.x, e.center.y, e.center.z,
                    e.half.x, md.name.c_str(),
                    md.albedo.r, md.albedo.g, md.albedo.b, md.reflectivity,
                    md.tex ? "  [textured]" : "");
    }

    // --- What the scene is drawn WITH ---------------------------------------
    fitzel::Shader lit = fitzel::Shader::fromFiles(opt.shaders + "/lit.vert",
                                                   opt.shaders + "/lit.frag");
    const fitzel::Mesh cube     = fitzel::Mesh::create(makeCubeVerts());
    const fitzel::Mesh ramp     = fitzel::Mesh::create(makeRampVerts());
    const fitzel::Mesh cylinder = fitzel::Mesh::create(makeCylinderYVerts());
    const fitzel::Mesh sphere   = fitzel::Mesh::create(makeSphereVerts());
    const fitzel::Mesh plane    = fitzel::Mesh::create(makePlaneVerts());
    EditMeshCache meshCache;

    // --- A camera that finds the scene wherever it was built -----------------
    glm::vec3 lo(0.0f), hi(0.0f);
    const bool haveBounds = sceneBounds(entities, lo, hi);
    const glm::vec3 mid    = haveBounds ? 0.5f * (lo + hi) : glm::vec3(0.0f);
    const float     radius = haveBounds
                           ? std::max(0.5f, 0.5f * glm::length(hi - lo)) : 5.0f;
    // Far enough out that the whole extent fits the vertical field of view, with
    // a little air around it.
    const float dist = radius * 2.6f;
    const float yawR = glm::radians(opt.yaw), pitchR = glm::radians(opt.pitch);
    const glm::vec3 dir(std::cos(pitchR) * std::cos(yawR),
                        std::sin(pitchR),
                        std::cos(pitchR) * std::sin(yawR));
    fitzel::Camera camera(mid - dir * dist, opt.yaw, opt.pitch);

    const float aspect = static_cast<float>(opt.width) / static_cast<float>(opt.height);
    const glm::mat4 view = camera.viewMatrix();
    const glm::mat4 proj = camera.projectionMatrix(aspect);

    // --- Somewhere to render into -------------------------------------------
    GLuint fbo = 0, colour = 0, depth = 0;
    glGenTextures(1, &colour);
    glBindTexture(GL_TEXTURE_2D, colour);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, opt.width, opt.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glGenRenderbuffers(1, &depth);
    glBindRenderbuffer(GL_RENDERBUFFER, depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, opt.width, opt.height);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::printf("[viewcheck] incomplete framebuffer\n");
        glfwTerminate();
        return 2;
    }

    // --- The frame -----------------------------------------------------------
    fitzel::DirectionalLight light;
    fitzel::Renderer renderer;
    renderer.setShadingMode(opt.shade);
    renderer.setViewport(opt.width, opt.height);
    renderer.begin(camera, aspect, light);

    scenesubmit::Scratch scratch;
    scenesubmit::submit({entities, materials, document, models, meshCache,
                         lit, renderer,
                         cube, ramp, cylinder, sphere, plane,
                         composeModel, /*roadWetness=*/0.0f, /*playMode=*/false},
                        scratch);
    renderer.prepareShadows();
    // A reflective material with nothing captured reads the probe as black, and a
    // chrome sphere comes out a black disc -- which is what the first picture from
    // this harness was, and it took reading the scene file to tell that from a bug.
    // There is no sky here, so the probe is given the same flat colour the picture
    // is cleared to: not the editor's sky, but honest about what it is.
    renderer.prepareEnvProbe(mid, [&](const glm::mat4&, const glm::vec3&) {
        glClearColor(kSky.r, kSky.g, kSky.b, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    });

    // prepareShadows renders the cascades into targets of its own and leaves
    // them bound, so the picture's framebuffer is bound again here rather than
    // before the submit.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, opt.width, opt.height);
    // The editor puts a plain mode against a flat dark ground rather than a sky.
    if (opt.shade == 0) glClearColor(kSky.r, kSky.g, kSky.b, 1.0f);
    else                glClearColor(0.055f, 0.060f, 0.070f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    if (opt.shade == 3) glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    renderer.renderScene(view, proj, camera.position(), fitzel::Renderer::kNoClip, false);
    if (opt.shade == 3) glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glFinish();

    // --- Out ------------------------------------------------------------------
    std::vector<unsigned char> px(static_cast<std::size_t>(opt.width) * opt.height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, opt.width, opt.height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    stbi_flip_vertically_on_write(1);          // GL counts rows from the bottom
    const bool wrote = stbi_write_png(opt.out.c_str(), opt.width, opt.height, 4,
                                      px.data(), opt.width * 4) != 0;
    std::printf(wrote ? "[viewcheck] wrote %s (%dx%d)\n"
                      : "[viewcheck] could not write %s\n",
                opt.out.c_str(), opt.width, opt.height);

    glfwTerminate();
    return wrote ? 0 : 1;
}
