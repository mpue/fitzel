// The icon check: does a picture actually end up on the exported exe?
//
// "Export Game" writes the project's icon into the copied player.exe after the
// fact (see IconEmbed.cpp) -- there is no build step left to hang an .rc on. That
// makes it the one piece of branding nothing verifies: the export reports success
// as long as the two API calls return, and a resource directory Windows quietly
// declines to read looks exactly like one it likes. The result is an exe with the
// default white sheet on it and an export log that says everything went fine.
//
// So this does the whole round trip on a real exe: build the .ico bytes, patch a
// COPY of the given executable, then read the resources back the way the shell
// does -- LookupIconIdFromDirectoryEx picks the image out of the group, and
// CreateIconFromResourceEx makes Windows itself parse it. If those two work, the
// icon is on the file; if they don't, this says which step broke instead of
// leaving it to a folder view and an icon cache.
//
//   build/release/bin/iconcheck.exe [image] [exe]
//   defaults: images/splash.png  build/release/bin/player.exe

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/IconEmbed.hpp"

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  include <shellapi.h>   // ExtractIconEx: the shell's own way in
#endif

namespace fs = std::filesystem;

namespace {

int fail(const std::string& msg) {
    std::fprintf(stderr, "[iconcheck] FAIL: %s\n", msg.c_str());
    return 1;
}

#ifdef _WIN32

std::wstring widen(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n - 1), L' ');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Read one RT_GROUP_ICON back out of the patched file and let Windows build the
// icon it names. `group` is either MAKEINTRESOURCEW(id) or a name.
//
// LOAD_LIBRARY_AS_DATAFILE, so this reads the resource directory of a file it is
// not running -- exactly what the shell does when it draws the folder view.
bool checkGroup(HMODULE mod, const wchar_t* group, const char* label, int want) {
    HRSRC res = FindResourceW(mod, group, MAKEINTRESOURCEW(14)); // RT_GROUP_ICON
    if (!res) {
        std::fprintf(stderr, "[iconcheck]   %s: no RT_GROUP_ICON (error %lu)\n",
                     label, GetLastError());
        return false;
    }
    HGLOBAL h = LoadResource(mod, res);
    auto*   p = static_cast<unsigned char*>(LockResource(h));
    if (!p) { std::fprintf(stderr, "[iconcheck]   %s: group unreadable\n", label); return false; }

    const int count = *reinterpret_cast<const std::uint16_t*>(p + 4);
    if (count != want) {
        std::fprintf(stderr, "[iconcheck]   %s: group names %d images, expected %d\n",
                     label, count, want);
        return false;
    }

    // The shell's own two steps: pick the best image for a size, then parse it.
    bool ok = true;
    for (int px : {256, 48, 32, 16}) {
        const int id = LookupIconIdFromDirectoryEx(p, TRUE, px, px, LR_DEFAULTCOLOR);
        if (id == 0) {
            std::fprintf(stderr, "[iconcheck]   %s: nothing chosen for %dpx\n", label, px);
            ok = false;
            continue;
        }
        HRSRC ires = FindResourceW(mod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(3)); // RT_ICON
        if (!ires) {
            std::fprintf(stderr, "[iconcheck]   %s: group points at RT_ICON %d, "
                                 "which is not in the file\n", label, id);
            ok = false;
            continue;
        }
        HGLOBAL ih   = LoadResource(mod, ires);
        auto*   ip   = static_cast<unsigned char*>(LockResource(ih));
        const DWORD n = SizeofResource(mod, ires);
        HICON icon = CreateIconFromResourceEx(ip, n, TRUE, 0x00030000, px, px,
                                              LR_DEFAULTCOLOR);
        if (!icon) {
            std::fprintf(stderr, "[iconcheck]   %s: Windows rejected the %dpx image "
                                 "(error %lu)\n", label, px, GetLastError());
            ok = false;
            continue;
        }
        DestroyIcon(icon);
        std::printf("[iconcheck]   %s: %3dpx -> RT_ICON %d, %lu bytes, accepted\n",
                    label, px, id, static_cast<unsigned long>(n));
    }
    return ok;
}

#endif // _WIN32

} // namespace

int main(int argc, char** argv) {
    const std::string image = argc > 1 ? argv[1] : "images/splash.png";
    const std::string exe   = argc > 2 ? argv[2] : "build/release/bin/player.exe";

    std::error_code ec;
    if (!fs::exists(image, ec)) return fail("no such image: " + image);
    if (!fs::exists(exe, ec))   return fail("no such exe: " + exe + " (build it first)");

    std::string err;
    const std::vector<unsigned char> ico = icoutil::fromImage(image, err);
    if (ico.empty()) return fail("fromImage: " + err);

    const int count = *reinterpret_cast<const std::uint16_t*>(ico.data() + 4);
    std::printf("[iconcheck] %s -> %d image(s), %zu bytes of .ico\n",
                image.c_str(), count, ico.size());

    // A copy, never the original: this rewrites the file's resource directory,
    // and the exe handed in is usually the one an export is about to ship.
    const fs::path patched = fs::temp_directory_path(ec) / "iconcheck-patched.exe";
    fs::remove(patched, ec);
    fs::copy_file(exe, patched, fs::copy_options::overwrite_existing, ec);
    if (ec) return fail("could not copy " + exe + ": " + ec.message());

    if (!icoutil::embed(patched.generic_string(), ico, err))
        return fail("embed: " + err);
    std::printf("[iconcheck] embedded into %s\n", patched.generic_string().c_str());

#ifdef _WIN32
    HMODULE mod = LoadLibraryExW(widen(patched.generic_string()).c_str(), nullptr,
                                 LOAD_LIBRARY_AS_DATAFILE);
    if (!mod)
        return fail("the patched exe cannot be opened as a resource file (error " +
                    std::to_string(GetLastError()) + ") -- the resource directory "
                    "is broken, which is what Explorer would see too");

    // Both groups: the integer id is what the shell reads out of the file, the
    // named one is what GLFW looks up for the window class. A shipped game needs
    // both, and they fail independently.
    bool ok = checkGroup(mod, MAKEINTRESOURCEW(101), "id 101   ", count);
    ok      = checkGroup(mod, L"GLFW_ICON", "GLFW_ICON", count) && ok;

    // And the way the shell actually asks for it, which is not FindResource at
    // all: it wants the FIRST icon group in the file, whatever it is called.
    HICON shell = nullptr;
    ExtractIconExW(widen(patched.generic_string()).c_str(), 0, &shell, nullptr, 1);
    if (!shell) {
        std::fprintf(stderr, "[iconcheck]   ExtractIconEx found no icon -- this is "
                             "exactly what Explorer does, so the file would show "
                             "the default\n");
        ok = false;
    } else {
        DestroyIcon(shell);
        std::printf("[iconcheck]   ExtractIconEx (what Explorer does): ok\n");
    }

    FreeLibrary(mod);
    if (!ok) return fail("the icon is in the file but Windows will not use it");
    std::printf("[iconcheck] OK -- an export of this exe would carry the icon\n");
    return 0;
#else
    return fail("this check only means anything on Windows");
#endif
}
