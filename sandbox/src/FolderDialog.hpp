#pragma once

#include <string>

namespace ed {

// Open a native "choose folder" dialog. On success sets `out` to the chosen
// absolute path (forward slashes) and returns true; returns false if the user
// cancels or no native dialog is available (non-Windows builds). `initialDir`
// (if it exists) selects the starting folder.
bool pickFolder(std::string& out, const std::string& initialDir = {});

// Open a native "choose file" dialog. On success sets `out` to the chosen
// absolute path (forward slashes) and returns true; returns false if the user
// cancels or no native dialog is available (non-Windows builds). `initialDir`
// (if it exists) selects the starting folder. `filterName`/`filterSpec` build a
// single file-type filter, e.g. ("Images", "*.png;*.jpg"); leave empty for all
// files.
bool pickFile(std::string& out, const std::string& initialDir = {},
              const std::string& filterName = {},
              const std::string& filterSpec = {});

// Open a native "save as" dialog, offering `defaultName` in `initialDir`. The
// system asks before overwriting an existing file. On success sets `out` to the
// chosen absolute path (forward slashes, `defaultExt` added when the name was
// typed without one) and returns true; false if cancelled or no native dialog is
// available. `filterName`/`filterSpec` as for pickFile; `defaultExt` without the
// dot ("zip").
bool saveFile(std::string& out, const std::string& initialDir,
              const std::string& defaultName, const std::string& filterName,
              const std::string& filterSpec, const std::string& defaultExt);

} // namespace ed
