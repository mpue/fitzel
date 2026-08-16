#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The one place the engine asks for bytes.
//
// Two backends sit behind it: the plain filesystem (editor, dev runs -- nothing
// mounted, every call falls straight through to disk) and a mounted .fpak
// archive (an exported game, where the content is one encrypted blob next to the
// executable). Loaders call vfs::read() and never learn which it was.
//
// Paths keep the form they already have everywhere in the codebase: relative to
// the executable's directory ("content/models/car.glb", "assets/shaders/lit.vert")
// or absolute. An absolute path under the mount root is folded back to its
// relative form, so an AssetDatabase entry pointing at
// "D:/game/content/x.png" finds "content/x.png" in the archive.
//
// The fallback is what keeps the editor untouched: with no archive mounted this
// whole layer is an ifstream with extra steps.
namespace fitzel::vfs {

// Mount `pakFile`, whose entries are relative to `root` (in practice the
// directory the executable lives in). One archive at a time; mounting replaces
// any previous one. False if the file is missing, corrupt, or was packed with a
// different key.
bool mount(const std::filesystem::path& pakFile, const std::filesystem::path& root);
void unmount();

// True once an archive is mounted -- i.e. this is a packed, shipped build.
// Callers use it for the handful of decisions that genuinely differ (no asset
// hot reload, no writing .meta sidecars), not to pick a read path.
bool packed();

// Bytes of `path`, from the archive when it has them, else from disk. Empty on
// failure -- and an empty file, which no loader can use either way.
std::vector<std::uint8_t> read(const std::string& path);
std::string               readText(const std::string& path);

bool exists(const std::string& path);
bool isDirectory(const std::string& path); // a prefix in the archive counts

// Files under `dir`, as paths in the same shape `dir` came in (relative in,
// relative out). Archive entries and on-disk files are merged, each path once.
std::vector<std::string> listFiles(const std::string& dir, bool recursive);
// Immediate subdirectory names (not paths) under `dir`.
std::vector<std::string> listDirs(const std::string& dir);

// True when `path` is served by the archive. The few callers that need a real
// file on disk (and can't take bytes) check this first.
bool inPak(const std::string& path);

} // namespace fitzel::vfs
