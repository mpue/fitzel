#pragma once

#include <string>

#include <fitzel/graphics/Mesh.hpp>
#include <fitzel/graphics/Shader.hpp>

// Booting: the handful of things that must happen before anything is loaded,
// and the few objects every run makes first. Out of main() so the top of that
// function reads as a list of what booting means rather than two hundred lines
// of how. Compiled into the player too -- a shipped game boots the same way.
namespace startup {

// How this run starts. An exported/player build ships a game.json next to the exe
// that boots straight into the game with the editor hidden; `--play <project>`
// does the same from a command line. An empty project means the editor.
struct BootConfig {
    std::string project;
    std::string scene;             // start scene stem ("" = default scene)
    bool        fullscreen = true;
    // --- Benchmark mode ------------------------------------------------------
    // `--profile <file>` plays the project for a few seconds, writes what the
    // frame cost -- every CPU and GPU zone, and what the vegetation submitted --
    // to that file, and quits.
    //
    // It exists because the alternative is reading numbers off a screen: the
    // Performance window is the right tool while you are in there working, and
    // the wrong one for "is this change faster than that one", which needs the
    // same scene measured twice under the same conditions and the two numbers
    // side by side. This is that.
    std::string profilePath;
    // Where to drop a PNG of the last measured frame. A benchmark that only
    // reports milliseconds cannot tell you whether the change that bought them
    // also removed a shadow -- which, for anything in this area, is the more
    // likely outcome of the two. Same run, same camera, one picture.
    //
    // Editor build only: the PNG writer's implementation lives in the editor's
    // Render panel, and the player has no reason to carry an image encoder for
    // a development flag.
    std::string profileShot;
    double      profileSeconds = 8.0;
    // `--shots <list>`: photograph a list of fixed views in one run and quit
    // (see ShotList.hpp). `--shots-out <dir>` says where the PNGs go.
    std::string shotsPath;
    std::string shotsOut;
    // `--shots-trace <samples>`: a path-traced still of every shot as well.
    int         shotsTrace = 0;
    bool        shotsTraceGpu = false;   // `--shots-trace-gpu 1`: the GPU tracer too
    // `--open <project>`: the editor starts with this project open (not Play).
    std::string editorOpen;
    // `--export <dir>` / `--export-web <dir>` (with --open): export the project
    // as File > Export does, then quit -- non-zero when the export failed.
    std::string exportDir;
    bool        exportWeb = false;
};

// Where this build's content lives. A portable/exported build ships a `content/`
// next to the exe; a dev run falls back to the compile-time tree CMake injected.
struct ContentRoots {
    std::string content;
    std::string models;
    std::string textures;
    std::string sounds;
};

// The engine's own shader programs -- the ones every frame goes through.
struct CoreShaders {
    fitzel::Shader lit;     // scene geometry + terrain
    fitzel::Shader water;   // the water surface
    // Running water (brooks, rivers, canals). Its own program rather than the
    // one above because the lake's planar reflection is rendered for ONE height
    // and a river is at a different one every ten metres -- see river.frag.
    fitzel::Shader river;
    fitzel::Shader sky;     // sky + volumetric clouds (fullscreen raymarch pass)
    fitzel::Shader skybox;  // HDRI background (reuses the fullscreen sky vertex shader)
};

void adoptParentConsole();
void setWorkingDirToExe(int argc, char** argv);
void mountGameArchive();
BootConfig   loadBootConfig(int argc, char** argv);
ContentRoots resolveContentRoots();
bool         loadCoreShaders(CoreShaders& out);
fitzel::Mesh makeWaterGrid(int n);
fitzel::Mesh makeFullscreenQuad();

} // namespace startup
