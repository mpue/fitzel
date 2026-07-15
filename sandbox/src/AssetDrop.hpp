#pragma once

#include <string>
#include <vector>

namespace fitzel {
class AssetDatabase;
}

// Files dragged from the OS file manager onto the Assets panel.
//
// A dropped file lives wherever the user keeps it -- a download folder, another
// drive, a network share -- which is nowhere the asset database looks. So a drop
// copies it into the open project, where it gets a GUID and a .meta sidecar like
// every other asset. That is what lets a scene keep referring to it after the
// original is moved or deleted, and it is why a drop needs a project to land in.
namespace assetdrop {

struct Result {
    int         imported = 0; // copied in and registered
    int         existing = 0; // already an asset, or a file of that name is there
    int         skipped  = 0; // not a type the database recognises, or copy failed
    std::string message;      // one line for the panel
};

// Copy `paths` (files, or folders walked recursively) into `projectFolder`, each
// into the subfolder for its type -- textures/, models/, sounds/, materials/ --
// and refresh `db` so they appear in Assets. Files already under a mounted source
// are left where they are: they are assets already, and copying would mint a
// second GUID for the same bytes. Pass "" for `projectFolder` and nothing is
// copied; the message says why.
Result importInto(const std::string& projectFolder,
                  const std::vector<std::string>& paths, fitzel::AssetDatabase& db);

} // namespace assetdrop
