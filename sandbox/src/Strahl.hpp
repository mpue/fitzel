#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "PathTrace.hpp"

// Strahl: the path tracer's scene language. A scene written for the tracer
// rather than harvested from the game -- in the line of POV-Ray's scene
// description language, with what thirty years of languages since have
// learned: real functions and closures, expressions everywhere, one syntax for
// building an object and for modifying one, and procedural textures and
// isosurfaces that are plain functions of the point.
//
//     camera { position: [0, 1.6, 5], look_at: [0, 0.8, 0], fov: 40 }
//     sun    { direction: [-1, 2, 1], intensity: 3 }
//     sky    { zenith: #4a6fa5, horizon: #cdd9e5 }
//
//     plane  { material: matte(checker(#ddd, #333, size: 0.5)) }
//
//     difference {
//         box    { size: 1.6 }
//         sphere { radius: 1.05 }
//         material: gold
//         rotate: [0, 30, 0]
//         translate: [0, 0.8, 0]
//     }
//
//     for i in 0..12 {
//         sphere { center: [3 * cos(i * 30), 0.3, 3 * sin(i * 30)], radius: 0.3,
//                  material: glass(hsv(i * 30, 0.4, 1)) }
//     }
//
// docs/strahl.md is the language reference. The scene it builds is an
// ordinary pathtrace::Scene: exact shapes and CSG go into Scene::shapes
// (TraceShapes.hpp), meshes into Scene::meshes, and every function a material
// or an isosurface is given is compiled to the tracer's VM (TraceFunc.hpp), so
// nothing of the language runs while the picture is traced.
//
// No GL, no editor: the editor window (StrahlPanel.hpp), the command-line
// renderer (tools/strahl.cpp) and the checks (tools/strahlcheck.cpp) all go
// through build() below.
namespace strahl {

struct Diagnostic {
    std::string file;
    int         line = 0, col = 0;
    std::string message;

    std::string format() const;   // file:line:col: message
};

struct Options {
    // The script's own path: relative imports, images and HDRIs resolve
    // against its folder, and errors name it. Empty for a buffer with no file.
    std::filesystem::path file;
    // Where `import "name"` looks when the name is not beside the script --
    // the bundled library (assets/strahl) comes first in the editor and the CLI.
    std::vector<std::filesystem::path> libraryDirs;
    // The animation clock: `time` runs 0..1 over `frames` frames, `frame` counts.
    double time   = 0.0;
    int    frame  = 0;
    int    frames = 1;
    // name=value pairs from the command line, bound before the script runs and
    // evaluated as Strahl expressions ("samples=64", "variant=\"night\"").
    std::vector<std::pair<std::string, std::string>> defines;
    // Where print() goes as it runs, besides Result::log.
    std::function<void(const std::string&)> print;
    // Checked now and then while the script runs; true stops it with an error.
    // A script with a loop of a hundred million iterations is a script.
    std::function<bool()> cancelled;
};

struct Result {
    bool ok = false;
    std::shared_ptr<pathtrace::Scene> scene;
    pathtrace::Settings               settings;
    // render { output: "..." }, resolved against the script's folder; empty
    // when the script did not say.
    std::filesystem::path             output;
    std::vector<Diagnostic>           errors;   // the first one stopped the build
    std::vector<std::string>          log;      // print() and warnings, in order
    double seconds = 0.0;                       // how long the script took
    // What the scene came to, for the panel and the CLI to report.
    int shapes = 0, meshes = 0, materials = 0, lights = 0, functions = 0;
    long long triangles = 0;
    // Where the camera came from, for the window that moves it with the mouse
    // and writes it back into the script (complete::setCamera): the camera
    // block's place -- line 0 when the script made none -- the point it turns
    // around, and the transforms it and the groups around it put on it.
    struct CameraSource {
        std::string file;               // as Diagnostic::file names it
        int         line = 0, col = 0;  // 1-based, the block's first word
        glm::dvec3  target{0.0};        // world space: its look_at, or a point ahead
        glm::dmat4  xf{1.0};            // world from the block's own values
        double      fov = 45.0;
        bool        focusFollows = true;   // no focus: given -- the lens focuses on the target
    } camera;
};

// Run a script and build the scene it describes.
Result build(const std::string& source, const Options& options);
// The same, from a file (Options::file is set from it).
Result buildFile(const std::filesystem::path& file, Options options = {});

// The words the language knows -- keywords, constructors, properties and
// built-in functions -- for the editor's highlighting and completion.
struct Vocabulary {
    std::vector<std::string> keywords;
    std::vector<std::string> objects;     // sphere, material, camera, ...
    std::vector<std::string> properties;  // radius, look_at, ...
    std::vector<std::string> functions;   // sin, noise, checker, ...
};
const Vocabulary& vocabulary();

} // namespace strahl
