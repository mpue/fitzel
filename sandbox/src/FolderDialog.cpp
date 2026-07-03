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

#else // non-Windows: no native dialog (caller falls back to a path text field).

namespace ed {
bool pickFolder(std::string&, const std::string&) { return false; }
} // namespace ed

#endif
