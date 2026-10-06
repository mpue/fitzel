#pragma once

#include <cstdint>
#include <filesystem>

// Shrinking an export to what a browser can sensibly download -- for the web
// export, where every byte of the archive arrives before the game starts and a
// tab has far less memory than a desktop game.
//
// Both work on a staged export folder in place and write each file back under
// the same name in the same format, so the scenes, .meta GUIDs and scripts that
// name it need no change. What is already within budget, and anything that
// fails to decode, is left untouched. Editor-only: the player re-encodes nothing.
namespace webbudget {

struct Report {
    int            shrunk = 0;   // files rewritten smaller
    std::uintmax_t before = 0;   // their bytes before ...
    std::uintmax_t after  = 0;   // ... and after

    Report& operator+=(const Report& o) {
        shrunk += o.shrunk; before += o.before; after += o.after;
        return *this;
    }
};

// Every .png/.jpg/.jpeg/.tga/.bmp/.exr below `root` whose longer side is over
// `maxSize` is halved (a 2x2 box filter, repeatedly) until it fits.
Report shrinkImages(const std::filesystem::path& root, int maxSize);

// Every PCM .wav below `root`: to 16 bit, halved in rate while over 48 kHz, and
// cut to `maxSeconds` when longer -- the cut end crossfaded into the start, so
// a loop (the long ones are ambience loops: an eleven-minute storm) still loops
// without a click.
Report shrinkSounds(const std::filesystem::path& root, float maxSeconds);

} // namespace webbudget
