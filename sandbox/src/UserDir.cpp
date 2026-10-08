#include "UserDir.hpp"

#include <cstdio>
#include <fstream>
#include <system_error>

#if defined(_WIN32) && !defined(FITZEL_PLAYER)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>   // SHGetKnownFolderPath
#endif

namespace fs = std::filesystem;

namespace userdir {

namespace {

#ifndef FITZEL_PLAYER
// Everything older builds of the editor wrote beside the exe. A file here that the
// new folder already has is left alone: that one is newer.
constexpr const char* kLegacyFiles[] = {
    "editor.json", "imgui.ini", "graphics.json", "difficulty.json", "scores.json",
    "blender.json", "retarget.json", "campath.txt", "road.txt", "grass.txt",
};

// A copy the user can write to. fs::copy_file and fs::copy carry the source's
// permissions over, and a source in /opt or Program Files is read-only -- the
// copied editor.json could then never be saved again.
bool copyWritable(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (!fs::copy_file(from, to, ec)) return false;
    fs::permissions(to, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::add, ec);
    return true;
}

void migrateFromExeDir(const fs::path& to) {
    std::error_code ec;
    // Only into a fresh folder: once editor.json is there, this has happened.
    if (fs::exists(to / "editor.json", ec)) return;
    if (!fs::exists("editor.json", ec)) return;
    if (fs::equivalent(".", to, ec)) return;
    int copied = 0;
    for (const char* name : kLegacyFiles) {
        if (!fs::exists(name, ec) || fs::exists(to / name, ec)) continue;
        if (copyWritable(name, to / name)) ++copied;
    }
    // The pending crash snapshot, file by file: the folders are created here
    // with this user's defaults rather than copied, read-only, from the source.
    if (fs::is_directory("recovery", ec) && !fs::exists(to / "recovery", ec)) {
        for (const auto& e : fs::recursive_directory_iterator("recovery", ec)) {
            const fs::path dst = to / e.path();
            if (e.is_directory(ec)) fs::create_directories(dst, ec);
            else if (e.is_regular_file(ec)) {
                fs::create_directories(dst.parent_path(), ec);
                copyWritable(e.path(), dst);
            }
        }
        ++copied;
    }
    if (copied)
        std::fprintf(stderr, "[Fitzel] settings now in %s (%d copied from %s)\n",
                     to.generic_string().c_str(), copied,
                     fs::current_path(ec).generic_string().c_str());
}
#endif

#if !defined(_WIN32) && !defined(FITZEL_PLAYER)
// XDG_DOCUMENTS_DIR from user-dirs.dirs, the file the desktop writes when it
// names the folder in the user's language. One line of it looks like
//   XDG_DOCUMENTS_DIR="$HOME/Dokumente"
fs::path xdgDocuments(const fs::path& home) {
    fs::path config;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) config = xdg;
    else config = home / ".config";
    std::ifstream f(config / "user-dirs.dirs");
    const std::string key = "XDG_DOCUMENTS_DIR=\"";
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind(key, 0) != 0) continue;
        std::string v = line.substr(key.size());
        if (const auto q = v.find('"'); q != std::string::npos) v.resize(q);
        if (v.rfind("$HOME", 0) == 0) return home / v.substr(v.size() > 5 ? 6 : 5);
        if (!v.empty() && v[0] == '/') return v;
    }
    return {};
}
#endif

} // namespace

void prepare() {
    const fs::path dir = stateDir();
    std::error_code ec;
    fs::create_directories(dir, ec);
#ifndef FITZEL_PLAYER
    migrateFromExeDir(dir);
#endif
}

fs::path defaultProjectsDir() {
    fs::path docs;
#if defined(FITZEL_PLAYER)
    // No New Project wizard to offer it to -- and no shell32 linked to ask.
#elif defined(_WIN32)
    // The known folder, not %USERPROFILE%\Documents: OneDrive and group policy
    // both move it.
    PWSTR p = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p))) docs = p;
    CoTaskMemFree(p);
#else
    if (const char* home = std::getenv("HOME")) {
        docs = xdgDocuments(home);
        std::error_code ec;
        if (docs.empty() || !fs::is_directory(docs, ec)) {
            docs = fs::path(home) / "Documents";
            if (!fs::is_directory(docs, ec)) docs = home;
        }
    }
#endif
    if (docs.empty()) return fs::absolute("projects");
    return docs / "Fitzel";
}

} // namespace userdir
