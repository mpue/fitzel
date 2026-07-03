#pragma once

#include <string>

namespace ed {

// Open a native "choose folder" dialog. On success sets `out` to the chosen
// absolute path (forward slashes) and returns true; returns false if the user
// cancels or no native dialog is available (non-Windows builds). `initialDir`
// (if it exists) selects the starting folder.
bool pickFolder(std::string& out, const std::string& initialDir = {});

} // namespace ed
