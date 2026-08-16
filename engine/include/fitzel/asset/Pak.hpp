#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace fitzel::pak {

// Result of packing an archive. `error` is empty when ok.
struct WriteResult {
    bool          ok    = false;
    std::size_t   files = 0;
    std::uint64_t bytes = 0;   // total payload written, before the index
    std::string   error;
};

// Pack every file under `root` into one encrypted `.fpak` archive.
//
// Entry paths are stored relative to `root` with '/' separators -- which is
// exactly the form the runtime asks for once the archive is mounted at the
// executable's directory, so "content/models/car.glb" resolves in a packed build
// the same way it does against a loose folder. Nothing is compressed: the
// content is already JPG/PNG/GLB, so deflate would buy a percent or two and cost
// the load time of decompressing gigabytes.
//
// `skip` sees each candidate's relative path and can drop it from the archive.
WriteResult write(const std::filesystem::path& root,
                  const std::filesystem::path& outFile,
                  const std::function<bool(const std::string&)>& skip = {});

} // namespace fitzel::pak
