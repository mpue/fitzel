#include "FolderDialog.hpp"

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>   // IFileOpenDialog
#include <shlobj.h>     // SHCreateItemFromParsingName

#include <filesystem>

namespace ed {

namespace {

std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

std::string fromWide(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0'); // n includes the null
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace

bool pickFolder(std::string& out, const std::string& initialDir) {
    // Ensure COM is available on this (main) thread. GLFW may already have
    // initialised it: S_OK means we did, S_FALSE means it was already up (both
    // succeed), RPC_E_CHANGED_MODE means a different apartment -- we still try
    // the dialog but must not uninitialise what we didn't initialise.
    const HRESULT hrInit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialised = (hrInit == S_OK || hrInit == S_FALSE);

    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                        FOS_PATHMUSTEXIST);

        if (!initialDir.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(initialDir, ec)) {
                IShellItem* startItem = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(
                        toWide(initialDir).c_str(), nullptr,
                        IID_PPV_ARGS(&startItem)))) {
                    dlg->SetFolder(startItem);
                    startItem->Release();
                }
            }
        }

        if (SUCCEEDED(dlg->Show(nullptr))) { // parent HWND unknown; modal to desktop
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    // Normalise to forward slashes to match the rest of the app.
                    out = std::filesystem::path(fromWide(path)).generic_string();
                    ok = !out.empty();
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }

    if (weInitialised) CoUninitialize();
    return ok;
}

bool pickFile(std::string& out, const std::string& initialDir,
              const std::string& filterName, const std::string& filterSpec) {
    const HRESULT hrInit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialised = (hrInit == S_OK || hrInit == S_FALSE);

    bool ok = false;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);

        // Optional single-type filter (plus an "All files" catch-all).
        std::wstring fname = toWide(filterName), fspec = toWide(filterSpec);
        if (!fspec.empty()) {
            COMDLG_FILTERSPEC specs[2] = {
                {fname.empty() ? L"Files" : fname.c_str(), fspec.c_str()},
                {L"All files", L"*.*"},
            };
            dlg->SetFileTypes(2, specs);
        }

        if (!initialDir.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(initialDir, ec)) {
                IShellItem* startItem = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(
                        toWide(initialDir).c_str(), nullptr,
                        IID_PPV_ARGS(&startItem)))) {
                    dlg->SetFolder(startItem);
                    startItem->Release();
                }
            }
        }

        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = std::filesystem::path(fromWide(path)).generic_string();
                    ok = !out.empty();
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }

    if (weInitialised) CoUninitialize();
    return ok;
}

bool saveFile(std::string& out, const std::string& initialDir,
              const std::string& defaultName, const std::string& filterName,
              const std::string& filterSpec, const std::string& defaultExt) {
    const HRESULT hrInit =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool weInitialised = (hrInit == S_OK || hrInit == S_FALSE);

    bool ok = false;
    IFileSaveDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);

        std::wstring fname = toWide(filterName), fspec = toWide(filterSpec);
        if (!fspec.empty()) {
            COMDLG_FILTERSPEC specs[1] = {
                {fname.empty() ? L"Files" : fname.c_str(), fspec.c_str()},
            };
            dlg->SetFileTypes(1, specs);
        }
        if (!defaultExt.empty()) dlg->SetDefaultExtension(toWide(defaultExt).c_str());
        if (!defaultName.empty()) dlg->SetFileName(toWide(defaultName).c_str());

        if (!initialDir.empty()) {
            std::error_code ec;
            if (std::filesystem::exists(initialDir, ec)) {
                IShellItem* startItem = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(
                        toWide(initialDir).c_str(), nullptr,
                        IID_PPV_ARGS(&startItem)))) {
                    dlg->SetFolder(startItem);
                    startItem->Release();
                }
            }
        }

        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    out = std::filesystem::path(fromWide(path)).generic_string();
                    ok = !out.empty();
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }

    if (weInitialised) CoUninitialize();
    return ok;
}

} // namespace ed

#elif defined(__APPLE__)

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>

namespace ed {

namespace {

// Wrap a string as a single-quoted shell token, escaping embedded single quotes
// so the whole thing survives being passed through /bin/sh via popen().
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}

} // namespace

bool pickFolder(std::string& out, const std::string& initialDir) {
    // macOS has no COM file dialog; drive AppleScript's native "choose folder"
    // via osascript instead. No extra dependency, and it returns the chosen
    // POSIX path on stdout (with a trailing slash). On cancel osascript exits
    // non-zero and prints nothing usable.
    std::string script = "choose folder with prompt \"Select folder\"";
    std::error_code ec;
    if (!initialDir.empty() && std::filesystem::exists(initialDir, ec)) {
        // AppleScript string literal: escape backslash and double-quote.
        std::string esc;
        for (char c : initialDir) {
            if (c == '\\' || c == '"') esc += '\\';
            esc += c;
        }
        script += " default location (POSIX file \"" + esc + "\")";
    }
    script = "POSIX path of (" + script + ")";

    const std::string cmd = "osascript -e " + shellQuote(script) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    std::string result;
    std::array<char, 512> buf;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        result += buf.data();
    const int rc = pclose(pipe);

    // osascript appends a newline; "choose folder" yields a trailing slash.
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    while (result.size() > 1 && result.back() == '/')
        result.pop_back();

    if (rc != 0 || result.empty()) return false; // cancelled or failed
    out = std::filesystem::path(result).generic_string();
    return !out.empty();
}

bool pickFile(std::string& out, const std::string& initialDir,
              const std::string& /*filterName*/, const std::string& /*filterSpec*/) {
    std::string script = "choose file with prompt \"Select file\"";
    std::error_code ec;
    if (!initialDir.empty() && std::filesystem::exists(initialDir, ec)) {
        std::string esc;
        for (char c : initialDir) {
            if (c == '\\' || c == '"') esc += '\\';
            esc += c;
        }
        script += " default location (POSIX file \"" + esc + "\")";
    }
    script = "POSIX path of (" + script + ")";

    const std::string cmd = "osascript -e " + shellQuote(script) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;

    std::string result;
    std::array<char, 512> buf;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        result += buf.data();
    const int rc = pclose(pipe);

    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();

    if (rc != 0 || result.empty()) return false;
    out = std::filesystem::path(result).generic_string();
    return !out.empty();
}

bool saveFile(std::string& out, const std::string& initialDir,
              const std::string& defaultName, const std::string& /*filterName*/,
              const std::string& /*filterSpec*/, const std::string& defaultExt) {
    // AppleScript's "choose file name" asks about replacing an existing file
    // itself, like the Windows dialog does.
    auto esc = [](const std::string& s) {
        std::string e;
        for (char c : s) {
            if (c == '\\' || c == '"') e += '\\';
            e += c;
        }
        return e;
    };
    std::string script = "choose file name with prompt \"Save as\"";
    if (!defaultName.empty()) script += " default name \"" + esc(defaultName) + "\"";
    std::error_code ec;
    if (!initialDir.empty() && std::filesystem::exists(initialDir, ec))
        script += " default location (POSIX file \"" + esc(initialDir) + "\")";
    script = "POSIX path of (" + script + ")";

    const std::string cmd = "osascript -e " + shellQuote(script) + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;
    std::string result;
    std::array<char, 512> buf;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        result += buf.data();
    const int rc = pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    if (rc != 0 || result.empty()) return false;
    std::filesystem::path p(result);
    if (!defaultExt.empty() && p.extension().empty()) p += "." + defaultExt;
    out = p.generic_string();
    return !out.empty();
}

} // namespace ed

#elif defined(__linux__)

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace ed {

namespace {

// As on macOS: a single-quoted shell token for popen().
std::string shellQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}

bool have(const char* tool) {
    const std::string cmd = std::string("command -v ") + tool + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// Linux has no dialog of its own to call; zenity (GNOME, and most others) or
// kdialog (KDE) put up the desktop's own. Asked once which one is there. Both
// print the chosen path on stdout and exit non-zero on cancel.
enum class Tool { None, Zenity, KDialog };
Tool dialogTool() {
    static const Tool t = have("zenity")  ? Tool::Zenity
                        : have("kdialog") ? Tool::KDialog
                                          : Tool::None;
    if (t == Tool::None)
        std::fprintf(stderr, "[Fitzel] no file dialog: install zenity or kdialog\n");
    return t;
}

bool run(const std::string& cmd, std::string& out) {
    FILE* pipe = popen((cmd + " 2>/dev/null").c_str(), "r");
    if (!pipe) return false;
    std::string result;
    std::array<char, 512> buf;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        result += buf.data();
    const int rc = pclose(pipe);
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        result.pop_back();
    if (rc != 0 || result.empty()) return false;   // cancelled or failed
    out = std::filesystem::path(result).generic_string();
    return !out.empty();
}

// The starting folder, or empty when there is none to start in.
std::string startDir(const std::string& initialDir) {
    std::error_code ec;
    if (initialDir.empty() || !std::filesystem::is_directory(initialDir, ec)) return {};
    return initialDir;
}

// "*.png;*.jpg" -> "*.png *.jpg": both tools separate patterns by spaces.
std::string patterns(const std::string& filterSpec) {
    std::string p = filterSpec;
    for (char& c : p)
        if (c == ';') c = ' ';
    return p;
}

std::string filterArgs(Tool tool, const std::string& filterName,
                       const std::string& filterSpec, bool allFiles) {
    if (filterSpec.empty()) return {};
    const std::string name = filterName.empty() ? "Files" : filterName;
    if (tool == Tool::KDialog)
        return " " + shellQuote(name + " (" + patterns(filterSpec) + ")");
    std::string args = " --file-filter=" + shellQuote(name + " | " + patterns(filterSpec));
    if (allFiles) args += " --file-filter=" + shellQuote("All files | *");
    return args;
}

} // namespace

bool pickFolder(std::string& out, const std::string& initialDir) {
    const std::string dir = startDir(initialDir);
    switch (dialogTool()) {
        case Tool::Zenity:
            // A trailing slash makes zenity open INSIDE the folder.
            return run("zenity --file-selection --directory --title='Select folder'" +
                           (dir.empty() ? std::string() : " --filename=" + shellQuote(dir + "/")),
                       out);
        case Tool::KDialog:
            return run("kdialog --getexistingdirectory " + shellQuote(dir.empty() ? "." : dir),
                       out);
        default:
            return false;
    }
}

bool pickFile(std::string& out, const std::string& initialDir,
              const std::string& filterName, const std::string& filterSpec) {
    const std::string dir  = startDir(initialDir);
    const Tool        tool = dialogTool();
    switch (tool) {
        case Tool::Zenity:
            return run("zenity --file-selection --title='Select file'" +
                           (dir.empty() ? std::string() : " --filename=" + shellQuote(dir + "/")) +
                           filterArgs(tool, filterName, filterSpec, true),
                       out);
        case Tool::KDialog:
            return run("kdialog --getopenfilename " + shellQuote(dir.empty() ? "." : dir) +
                           filterArgs(tool, filterName, filterSpec, true),
                       out);
        default:
            return false;
    }
}

bool saveFile(std::string& out, const std::string& initialDir,
              const std::string& defaultName, const std::string& filterName,
              const std::string& filterSpec, const std::string& defaultExt) {
    std::string start = startDir(initialDir);
    if (!defaultName.empty())
        start = (std::filesystem::path(start.empty() ? "." : start) / defaultName).string();
    const Tool tool = dialogTool();
    bool ok = false;
    switch (tool) {
        case Tool::Zenity:
            // zenity 4 asks before overwriting by itself and only warns about
            // --confirm-overwrite; 3.x needs it.
            ok = run("zenity --file-selection --save --confirm-overwrite --title='Save as'" +
                         (start.empty() ? std::string() : " --filename=" + shellQuote(start)) +
                         filterArgs(tool, filterName, filterSpec, false),
                     out);
            break;
        case Tool::KDialog:
            ok = run("kdialog --getsavefilename " + shellQuote(start.empty() ? "." : start) +
                         filterArgs(tool, filterName, filterSpec, false),
                     out);
            break;
        default:
            return false;
    }
    if (!ok) return false;
    std::filesystem::path p(out);
    if (!defaultExt.empty() && p.extension().empty()) p += "." + defaultExt;
    out = p.generic_string();
    return true;
}

} // namespace ed

#else // other platforms: no native dialog (caller falls back to a text field).

namespace ed {
bool pickFolder(std::string&, const std::string&) { return false; }
bool pickFile(std::string&, const std::string&, const std::string&,
              const std::string&) { return false; }
bool saveFile(std::string&, const std::string&, const std::string&, const std::string&,
              const std::string&, const std::string&) { return false; }
} // namespace ed

#endif
