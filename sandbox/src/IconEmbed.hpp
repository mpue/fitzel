#pragma once

#include <string>
#include <vector>

// Turning a picture into a Windows application icon, and putting it inside an
// executable that has already been linked.
//
// The icon is deliberately NOT a build-time resource. The exe an export ships is
// a *copy* of the pre-built player, so there is no compile step left to hang an
// .rc file on -- and demanding one would mean the engine has to be rebuilt every
// time a game changes its icon. Windows has an API for exactly this case:
// BeginUpdateResource/UpdateResource rewrites a PE file's resource directory in
// place. So the icon stays what it should be: a project setting.
namespace icoutil {

// Decode `imagePath` (PNG/JPG/BMP/TGA) and build the bytes of a Windows .ico
// holding the usual square sizes, 32-bit with alpha.
//
// A non-square source is padded to a square with transparency rather than
// squashed into one: a logo that is the wrong shape is more obviously wrong than
// a logo that is small. Returns an empty vector and fills `err` on failure.
std::vector<unsigned char> fromImage(const std::string& imagePath, std::string& err);

// Replace the icon resources of the Windows executable at `exePath`, in place.
//
// Two icon groups are written from the same images: one under an integer id --
// what Explorer, the taskbar and Alt-Tab read out of the file -- and one named
// "GLFW_ICON", which is the name GLFW looks up when it registers the window
// class. Without the second the shipped game has the right icon in the folder
// and the default white sheet in its own title bar.
//
// Returns false and fills `err` on failure; on non-Windows builds it always
// fails, because there is no PE resource directory to patch.
bool embed(const std::string& exePath, const std::vector<unsigned char>& ico,
           std::string& err);

} // namespace icoutil
