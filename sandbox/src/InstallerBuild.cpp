#include "InstallerBuild.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace installer {

namespace fs = std::filesystem;

namespace {

// A name that is safe as a directory, a shortcut file and an output file name.
// Not a beautification pass: anything Windows would reject becomes a dash, and
// that is all. The pretty name still travels in the script and is what people
// see in the wizard.
std::string safeName(const std::string& s) {
    std::string out;
    for (char c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        out.push_back(ok ? c : '-');
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("Game") : out;
}

// Text going into a .iss value. A brace opens a constant like {app} in Inno, so
// a literal one has to be doubled; a semicolon or a quote would end a parameter
// in the [Files]/[Icons] sections, and no product name needs either badly enough
// to be worth the parser bug.
std::string iss(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '{') out += "{{";
        else if (c == ';' || c == '"') out += ' ';
        else out.push_back(c);
    }
    return out;
}

// A GUID that is the same every time for the same product name. Inno uses AppId
// to decide whether a setup is an UPDATE of something already installed: derive
// it from the name and version 2 of a game replaces version 1 instead of sitting
// next to it in the Programs list forever.
std::string appId(const std::string& name) {
    auto fnv = [&](std::uint64_t seed) {
        std::uint64_t h = seed;
        for (unsigned char c : name) { h ^= c; h *= 1099511628211ull; }
        return h;
    };
    const std::uint64_t a = fnv(14695981039346656037ull);
    const std::uint64_t b = fnv(1469598103934665603ull);
    char buf[64];
    std::snprintf(buf, sizeof buf, "%08X-%04X-%04X-%04X-%012llX",
                  static_cast<unsigned>(a >> 32), static_cast<unsigned>((a >> 16) & 0xFFFF),
                  static_cast<unsigned>(a & 0xFFFF), static_cast<unsigned>(b >> 48),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
    return buf;
}

#ifdef _WIN32

std::wstring regString(HKEY root, const wchar_t* key, const wchar_t* value, DWORD view) {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD   size              = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ | view, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};
    return buf;
}

// Find ISCC.exe. The file association is asked first because it is the only
// place that is version-independent: Inno registers it whatever major version
// gets installed, so this keeps working across the next upgrade without an
// edit here.
fs::path findCompiler() {
    std::error_code ec;

    const wchar_t* assoc = L"SOFTWARE\\Classes\\InnoSetupScriptFile\\Shell\\Compile\\Command";
    for (DWORD view : {RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY}) {
        const std::wstring cmd = regString(HKEY_LOCAL_MACHINE, assoc, nullptr, view);
        const std::size_t  a   = cmd.find(L'"');
        const std::size_t  b   = cmd.find(L'"', a + 1);
        if (a == std::wstring::npos || b == std::wstring::npos) continue;
        const fs::path iscc = fs::path(cmd.substr(a + 1, b - a - 1)).parent_path() / "ISCC.exe";
        if (fs::exists(iscc, ec)) return iscc;
    }

    // Newest first: a machine with two versions should compile with the one that
    // understands the newer directives, not the one that has been there longest.
    for (int v = 9; v >= 5; --v) {
        const std::wstring key = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
                                 L"Inno Setup " + std::to_wstring(v) + L"_is1";
        for (DWORD view : {RRF_SUBKEY_WOW6464KEY, RRF_SUBKEY_WOW6432KEY}) {
            const std::wstring loc = regString(HKEY_LOCAL_MACHINE, key.c_str(),
                                               L"InstallLocation", view);
            if (loc.empty()) continue;
            const fs::path iscc = fs::path(loc) / "ISCC.exe";
            if (fs::exists(iscc, ec)) return iscc;
        }
        for (const wchar_t* pf : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
            wchar_t base[MAX_PATH] = {};
            if (!GetEnvironmentVariableW(pf, base, MAX_PATH)) continue;
            const fs::path iscc =
                fs::path(base) / (L"Inno Setup " + std::to_wstring(v)) / "ISCC.exe";
            if (fs::exists(iscc, ec)) return iscc;
        }
    }

    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"ISCC.exe", nullptr, MAX_PATH, found, nullptr))
        return fs::path(found);
    return {};
}

// Run the compiler and bring its output back as text. A pipe rather than a
// console: the editor is a windowed program, so a shelled-out compiler would
// either flash a black box over the screen or -- worse -- print the one line
// explaining the failure somewhere nobody is looking.
bool run(const fs::path& exe, const std::wstring& args, std::string& output, DWORD& code) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE              rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = nullptr;
    PROCESS_INFORMATION pi{};

    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    const BOOL   started =
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, exe.parent_path().wstring().c_str(), &si, &pi);
    CloseHandle(wr); // the child owns the only writing end now, or nobody does
    if (!started) { CloseHandle(rd); return false; }

    char  buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0)
        output.append(buf, n);
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

#else  // !_WIN32

fs::path findCompiler() { return {}; }

#endif

// The last few lines of the compiler's output. A failed ISCC run prints a page
// of banner before the one line that matters, and the whole page does not fit
// in a status line under the File menu.
std::string tail(const std::string& s, std::size_t lines) {
    std::size_t cut = s.size();
    for (std::size_t i = 0; i < lines && cut > 0; ++i) {
        const std::size_t nl = s.find_last_of('\n', cut - 1);
        if (nl == std::string::npos) return s;
        cut = nl;
    }
    std::string out = s.substr(cut);
    while (!out.empty() && (out.front() == '\n' || out.front() == '\r')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

} // namespace

bool available() { return !findCompiler().empty(); }

std::string compilerPath() { return findCompiler().generic_string(); }

bool build(const std::string& payloadDir, const Info& info,
           const std::vector<unsigned char>& icon, std::string& setupPath,
           std::string& err) {
    std::error_code ec;

    const fs::path iscc = findCompiler();
    if (iscc.empty()) {
        err = "no Inno Setup compiler found - install Inno Setup (innosetup.com) "
              "and export again";
        return false;
    }

    const std::string base   = safeName(info.name) + "-setup";
    const fs::path    outDir = fs::absolute(fs::path(payloadDir), ec);

    // The script and the .ico are build inputs, not shipped files, so they live
    // outside the folder being packed. Anything left in there would be installed
    // on somebody's machine along with the game.
    const fs::path work = fs::temp_directory_path(ec) / "fitzel-installer";
    fs::create_directories(work, ec);
    if (ec) { err = "could not create a work folder for the setup: " + ec.message(); return false; }

    std::string iconLine;
    if (!icon.empty()) {
        const fs::path ico = work / "setup.ico";
        std::ofstream  f(ico, std::ios::binary);
        if (f) {
            f.write(reinterpret_cast<const char*>(icon.data()),
                    static_cast<std::streamsize>(icon.size()));
            f.close();
            iconLine = "SetupIconFile=" + ico.string() + "\n";
        }
    }

    const std::string name = iss(info.name);
    const std::string exe  = iss(info.exeName);

    std::ofstream s(work / "setup.iss");
    if (!s) { err = "could not write the setup script"; return false; }
    s << "; Generated by the Fitzel editor on export -- edits here are overwritten.\n"
      << "[Setup]\n"
      << "AppId={{" << appId(info.name) << "}\n"
      << "AppName=" << name << "\n"
      << "AppVersion=" << iss(info.version.empty() ? std::string("1.0.0") : info.version) << "\n"
      << (info.publisher.empty() ? "" : "AppPublisher=" + iss(info.publisher) + "\n")
      << "DefaultDirName={autopf}\\" << safeName(info.name) << "\n"
      << "DefaultGroupName=" << name << "\n"
      << "UninstallDisplayName=" << name << "\n"
      << "UninstallDisplayIcon={app}\\" << exe << "\n"
      << "OutputDir=" << outDir.string() << "\n"
      << "OutputBaseFilename=" << base << "\n"
      << iconLine
      // No admin rights, and therefore no UAC prompt: {autopf} resolves to the
      // per-user Programs folder. A game is not a system component, and the one
      // thing standing between somebody and playing it should not be a consent
      // dialog they have to read.
      << "PrivilegesRequired=lowest\n"
      << "ArchitecturesAllowed=x64compatible\n"
      << "ArchitecturesInstallIn64BitMode=x64compatible\n"
      << "Compression=lzma2/max\n"
      << "SolidCompression=yes\n"
      << "WizardStyle=modern\n"
      << "DisableProgramGroupPage=yes\n"
      << "\n[Languages]\n"
      << "Name: \"de\"; MessagesFile: \"compiler:Languages\\German.isl\"\n"
      << "Name: \"en\"; MessagesFile: \"compiler:Default.isl\"\n"
      << "\n[Tasks]\n"
      << "Name: \"desktopicon\"; Description: \"{cm:CreateDesktopIcon}\"; "
         "GroupDescription: \"{cm:AdditionalIcons}\"\n"
      << "\n[Files]\n"
      // The setup writes itself into the folder it is packing, so a second
      // export would otherwise bundle the first one's setup inside the second.
      << "Source: \"" << outDir.string() << "\\*\"; DestDir: \"{app}\"; "
         "Excludes: \"" << base << ".exe\"; "
         "Flags: ignoreversion recursesubdirs createallsubdirs\n"
      << "\n[Icons]\n"
      << "Name: \"{group}\\" << name << "\"; Filename: \"{app}\\" << exe << "\"\n"
      << "Name: \"{autodesktop}\\" << name << "\"; Filename: \"{app}\\" << exe
      << "\"; Tasks: desktopicon\n"
      << "\n[Run]\n"
      << "Filename: \"{app}\\" << exe << "\"; Description: \"{cm:LaunchProgram," << name
      << "}\"; Flags: nowait postinstall skipifsilent\n";
    s.close();

#ifdef _WIN32
    std::string output;
    DWORD       code = 1;
    if (!run(iscc, L"/Q \"" + (work / "setup.iss").wstring() + L"\"", output, code)) {
        err = "could not start " + iscc.generic_string();
        return false;
    }
    if (code != 0) {
        err = "Inno Setup failed: " + tail(output, 4) +
              " (script: " + (work / "setup.iss").generic_string() + ")";
        return false;
    }
    setupPath = (outDir / (base + ".exe")).generic_string();
    if (!fs::exists(setupPath, ec)) {
        err = "Inno Setup reported success but wrote no " + base + ".exe";
        return false;
    }
    return true;
#else
    (void)setupPath;
    err = "installers can only be built on Windows";
    return false;
#endif
}

} // namespace installer
