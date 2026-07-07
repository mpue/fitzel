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

} // namespace ed

#else // other platforms: no native dialog (caller falls back to a text field).

namespace ed {
bool pickFolder(std::string&, const std::string&) { return false; }
} // namespace ed

#endif
