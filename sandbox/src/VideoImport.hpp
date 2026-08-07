#pragma once

#include <string>

// Turning a video file into something the engine can play.
//
// The runtime knows exactly one video format -- the .fvid motion-JPEG container
// described in <fitzel/graphics/VideoTexture.hpp> -- and nothing here is compiled
// into the player. Everything else (mp4, mov, mkv, ...) is a *source* file that
// the editor transcodes on import.
//
// The transcode runs ffmpeg as a child process instead of linking libav*. ffmpeg
// ships no CMake build, so linking it would mean a prebuilt binary blob or vcpkg,
// and this project's dependencies are all FetchContent'd -- one .bat and it
// builds. A separate process also keeps the LGPL/GPL question out of the tree
// entirely. The build copies ffmpeg next to the editor (see sandbox/CMakeLists);
// a PATH install works too.
namespace videoimport {

// Does this extension (with or without the dot) name a video the importer can
// transcode? False for ".fvid" -- that one is already an asset.
bool isSourceVideo(const std::string& ext);

// Where ffmpeg lives: next to the executable if the build put it there, else the
// bare name so the PATH resolves it. Empty when neither is available.
std::string ffmpegPath();

// Transcode settings. The defaults are chosen for what this feature exists for:
// a billboard is a few hundred pixels wide on screen and its clip loops, so
// anything more is bytes and decode time nobody sees.
struct Options {
    int   maxWidth   = 640;    // downscale to this; never upscales, aspect kept
    int   fps        = 15;     // frames per second to sample
    int   quality    = 5;      // ffmpeg -q:v, 2 (best) .. 31 (worst)
    float maxSeconds = 30.0f;  // hard cap, so a feature film can't be dropped in
};

struct Result {
    bool        ok = false;
    std::string outPath;   // the written .fvid
    std::string message;   // one line, suitable for the Assets panel
    int         frames = 0;
    int         width = 0, height = 0;
};

// Transcode `srcFile` into `dstFvid` (its parent directories are created).
// Overwrites an existing destination -- the caller decides whether that is
// allowed. Slow: seconds for a short clip. Runs on the calling thread.
Result transcode(const std::string& srcFile, const std::string& dstFvid,
                 const Options& opt = {});

} // namespace videoimport
