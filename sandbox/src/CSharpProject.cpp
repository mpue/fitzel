#include "CSharpProject.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <regex>
#include <sstream>

#include <imgui.h>

#include "CSharpHost.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace csproject {
namespace {

const char* kCsproj = R"(<Project Sdk="Microsoft.NET.Sdk">
  <!--
    The game's C# scripts, written by fitzel when the project was made. Every .cs
    under scripts/ goes into one assembly, Assembly-CSharp.dll, which the editor
    loads when Play starts. fitzel builds this project itself whenever a script
    changes; building it here in Visual Studio does the same. Edit freely: fitzel
    does not write this file again.
  -->
  <PropertyGroup>
    <TargetFramework>net10.0</TargetFramework>
    <AssemblyName>Assembly-CSharp</AssemblyName>
    <RootNamespace></RootNamespace>
    <LangVersion>latest</LangVersion>
    <Nullable>disable</Nullable>
    <ImplicitUsings>enable</ImplicitUsings>
    <EnableDefaultItems>false</EnableDefaultItems>
    <OutputPath>Library\ScriptAssemblies\</OutputPath>
    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>
    <AppendRuntimeIdentifierToOutputPath>false</AppendRuntimeIdentifierToOutputPath>
    <DebugType>portable</DebugType>
    <GenerateDependencyFile>false</GenerateDependencyFile>
    <ProduceReferenceAssembly>false</ProduceReferenceAssembly>
  </PropertyGroup>
  <ItemGroup>
    <Compile Include="scripts\**\*.cs" />
    <Using Include="Fitzel" />
    <Using Include="System.Numerics" />
  </ItemGroup>
  <ItemGroup>
    <!-- The engine's API. Library\Fitzel.dll is kept up to date by the editor. -->
    <Reference Include="Fitzel">
      <HintPath>Library\Fitzel.dll</HintPath>
      <Private>false</Private>
    </Reference>
  </ItemGroup>
</Project>
)";

// Before the SDK is imported, which is the only place the intermediate folder
// can be moved: obj\ would otherwise sit in the project root beside the scenes.
const char* kProps = R"(<Project>
  <PropertyGroup>
    <BaseIntermediateOutputPath>Library\obj\</BaseIntermediateOutputPath>
  </PropertyGroup>
</Project>
)";

const char* kSln = R"(
Microsoft Visual Studio Solution File, Format Version 12.00
# Visual Studio Version 17
VisualStudioVersion = 17.0.31903.59
MinimumVisualStudioVersion = 10.0.40219.1
Project("{9A19103F-16F7-4668-BE54-9A1E7A4F7556}") = "Assembly-CSharp", "Assembly-CSharp.csproj", "{%GUID%}"
EndProject
Global
	GlobalSection(SolutionConfigurationPlatforms) = preSolution
		Debug|Any CPU = Debug|Any CPU
		Release|Any CPU = Release|Any CPU
	EndGlobalSection
	GlobalSection(ProjectConfigurationPlatforms) = postSolution
		{%GUID%}.Debug|Any CPU.ActiveCfg = Debug|Any CPU
		{%GUID%}.Debug|Any CPU.Build.0 = Debug|Any CPU
		{%GUID%}.Release|Any CPU.ActiveCfg = Release|Any CPU
		{%GUID%}.Release|Any CPU.Build.0 = Release|Any CPU
	EndGlobalSection
	GlobalSection(SolutionProperties) = preSolution
		HideSolutionNode = FALSE
	EndGlobalSection
EndGlobal
)";

const char* kGitignore = R"(# C# build output and Visual Studio's own state (fitzel)
Library/
.vs/
*.csproj.user
)";

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    for (std::size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
        s.replace(at, from.size(), to);
    return s;
}

std::string newGuid() {
    std::random_device rd;
    std::mt19937_64    rng((static_cast<std::uint64_t>(rd()) << 32) ^ rd());
    const std::uint64_t a = rng(), b = rng();
    char buf[40];
    std::snprintf(buf, sizeof buf, "%08X-%04X-%04X-%04X-%012llX", static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xFFFF), static_cast<unsigned>((a & 0x0FFF) | 0x4000),
                  static_cast<unsigned>(((b >> 48) & 0x3FFF) | 0x8000),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
    return buf;
}

bool writeIfMissing(const fs::path& p, const std::string& text, std::string& err) {
    std::error_code ec;
    if (fs::exists(p, ec)) return true;
    std::ofstream out(p, std::ios::binary);
    out << text;
    if (!out) { err = "could not write " + p.generic_string(); return false; }
    return true;
}

bool sameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (!fs::exists(a, ec) || !fs::exists(b, ec)) return false;
    if (fs::file_size(a, ec) != fs::file_size(b, ec)) return false;
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    return std::equal(std::istreambuf_iterator<char>(x), std::istreambuf_iterator<char>(),
                      std::istreambuf_iterator<char>(y));
}

std::string slnPath(const std::string& projectDir) {
    std::error_code ec;
    for (const auto& de : fs::directory_iterator(projectDir, ec))
        if (de.path().extension() == ".sln") return de.path().generic_string();
    return {};
}

double nowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The sources a build depends on, as one number: every .cs under scripts/ (path,
// size, time) and the .csproj. Equal numbers = nothing to build.
std::uint64_t sourceStamp(const std::string& projectDir) {
    std::uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, std::size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ull; }
    };
    std::error_code ec;
    std::vector<fs::path> files;
    const fs::path scripts = fs::path(projectDir) / "scripts";
    for (fs::recursive_directory_iterator it(scripts, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".cs") files.push_back(it->path());
    files.push_back(fs::path(projectDir) / "Assembly-CSharp.csproj");
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        const std::string s = f.generic_string();
        mix(s.data(), s.size());
        const auto t  = fs::last_write_time(f, ec).time_since_epoch().count();
        const auto sz = fs::file_size(f, ec);
        mix(&t, sizeof t);
        mix(&sz, sizeof sz);
    }
    return h;
}

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

// Run a command line hidden, its output (stdout + stderr) into `out`. Returns
// the exit code, -1 when it would not start.
int runCapture(const std::wstring& cmdline, const fs::path& cwd, std::string& out) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                   cwd.empty() ? nullptr : cwd.wstring().c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    char  buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n > 0) out.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return static_cast<int>(code);
}

fs::path dotnetExe() {
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"dotnet.exe", nullptr, MAX_PATH, found, nullptr)) return fs::path(found);
    for (const char* env : {"ProgramW6432", "ProgramFiles"})
        if (const char* base = std::getenv(env)) {
            const fs::path p = fs::path(base) / "dotnet" / "dotnet.exe";
            std::error_code ec;
            if (fs::exists(p, ec)) return p;
        }
    return {};
}

// Visual Studio's devenv.exe, as vswhere (installed with every VS since 2017)
// names it; the newest, preview channels included.
fs::path devenvExe() {
    static fs::path cached;
    static bool     looked = false;
    if (looked) return cached;
    looked = true;
    const char* pf = std::getenv("ProgramFiles(x86)");
    if (!pf) return {};
    const fs::path vswhere = fs::path(pf) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    std::error_code ec;
    if (!fs::exists(vswhere, ec)) return {};
    std::string out;
    runCapture(L"\"" + vswhere.wstring() + L"\" -latest -prerelease -property productPath", {}, out);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    const std::size_t nl = out.find_first_of("\r\n");
    if (nl != std::string::npos) out = out.substr(0, nl);
    if (!out.empty() && fs::exists(fs::path(widen(out)), ec)) cached = fs::path(widen(out));
    return cached;
}
#endif

} // namespace

bool isCSharp(const std::string& projectDir) {
    std::error_code ec;
    return !projectDir.empty() && fs::exists(fs::path(projectDir) / "Assembly-CSharp.csproj", ec);
}

std::string scriptTemplate(const std::string& className) {
    return "using Fitzel;\n"
           "using System.Numerics;\n"
           "\n"
           "// Add a Script component to an object and pick " + className + ".cs.\n"
           "// Public fields show up on the Script card, one value per object.\n"
           "public class " + className + " : Behaviour\n"
           "{\n"
           "    public float Speed = 45f;   // degrees per second\n"
           "\n"
           "    // Once, before the first Update.\n"
           "    void Start()\n"
           "    {\n"
           "        Debug.Log($\"{Name} starts turning\");\n"
           "    }\n"
           "\n"
           "    // Every frame while the game plays.\n"
           "    void Update()\n"
           "    {\n"
           "        Rotation += new Vector3(0f, Speed * Time.DeltaTime, 0f);\n"
           "    }\n"
           "}\n";
}

void syncApi(const std::string& projectDir) {
    if (!isCSharp(projectDir)) return;
    const fs::path src = CSharpScripts::engineDir();
    const fs::path lib = fs::path(projectDir) / "Library";
    std::error_code ec;
    fs::create_directories(lib, ec);
    for (const char* f : {"Fitzel.dll", "Fitzel.xml"}) {
        if (!fs::exists(src / f, ec) || sameFile(src / f, lib / f)) continue;
        fs::copy_file(src / f, lib / f, fs::copy_options::overwrite_existing, ec);
        if (ec) std::fprintf(stderr, "[C#] could not update %s: %s\n", (lib / f).generic_string().c_str(),
                             ec.message().c_str());
    }
}

bool create(const std::string& projectDir, const std::string& name, std::string& err) {
    const fs::path dir(projectDir);
    std::error_code ec;
    fs::create_directories(dir / "scripts", ec);
    fs::create_directories(dir / "Library" / "ScriptAssemblies", ec);
    const std::string sln = (name.empty() ? std::string("Game") : name) + ".sln";
    if (slnPath(projectDir).empty() &&
        !writeIfMissing(dir / sln, replaceAll(kSln, "%GUID%", newGuid()), err))
        return false;
    if (!writeIfMissing(dir / "Assembly-CSharp.csproj", kCsproj, err)) return false;
    if (!writeIfMissing(dir / "Directory.Build.props", kProps, err)) return false;
    // .gitignore: added to, never replaced.
    {
        const fs::path gi = dir / ".gitignore";
        std::string have;
        if (std::ifstream in(gi, std::ios::binary); in) {
            std::stringstream ss;
            ss << in.rdbuf();
            have = ss.str();
        }
        if (have.find("Library/") == std::string::npos) {
            std::ofstream out(gi, std::ios::binary | std::ios::app);
            if (!have.empty() && have.back() != '\n') out << '\n';
            out << kGitignore;
        }
    }
    // A first script, unless there are C# scripts already.
    bool anyCs = false;
    for (fs::recursive_directory_iterator it(dir / "scripts", ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".cs") { anyCs = true; break; }
    if (!anyCs && !writeIfMissing(dir / "scripts" / "Rotator.cs", scriptTemplate("Rotator"), err)) return false;
    syncApi(projectDir);
    return true;
}

bool openInIde(const std::string& projectDir, const std::string& file) {
#ifdef _WIN32
    if (!file.empty()) {
        const fs::path devenv = devenvExe();
        if (!devenv.empty()) {
            // /edit: into the Visual Studio already running (the solution's,
            // usually), or a new one with just this file.
            std::wstring cmd = L"\"" + devenv.wstring() + L"\" /edit \"" + widen(file) + L"\"";
            STARTUPINFOW si{};
            si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return true;
            }
        }
        return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", widen(file).c_str(), nullptr,
                                                       nullptr, SW_SHOWNORMAL)) > 32;
    }
    const std::string sln = slnPath(projectDir);
    if (sln.empty()) return false;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", widen(sln).c_str(), nullptr,
                                                   widen(projectDir).c_str(), SW_SHOWNORMAL)) > 32;
#else
    (void)projectDir; (void)file;
    return false;
#endif
}

// --- Builder ------------------------------------------------------------------------
Builder::~Builder() {
    if (m_thread.joinable()) m_thread.join();
}

bool Builder::ok() const { std::lock_guard<std::mutex> l(m_lock); return m_ok; }
std::vector<BuildMessage> Builder::messages() const { std::lock_guard<std::mutex> l(m_lock); return m_messages; }
std::string Builder::log() const { std::lock_guard<std::mutex> l(m_lock); return m_log; }
double Builder::finishedAt() const { std::lock_guard<std::mutex> l(m_lock); return m_finishedAt; }

void Builder::wait() {
    if (m_thread.joinable()) m_thread.join();
}

void Builder::poll(const std::string& projectDir) {
    if (projectDir.empty() || !isCSharp(projectDir)) return;
    const double now = nowSeconds();
    if (projectDir != m_dir) {
        // Another project: what was built for the last one says nothing here.
        // A build that is still newer than every source counts as done.
        wait();
        m_dir = projectDir;
        m_builtStamp = 0;
        m_lastPoll = -1e9;
        std::error_code ec;
        const fs::path dll = CSharpScripts::assemblyFor(projectDir);
        if (fs::exists(dll, ec)) {
            const auto dllTime = fs::last_write_time(dll, ec);
            bool newer = true;
            const fs::path scripts = fs::path(projectDir) / "scripts";
            for (fs::recursive_directory_iterator it(scripts, ec), end; !ec && it != end; it.increment(ec))
                if (it->path().extension() == ".cs" && fs::last_write_time(it->path(), ec) > dllTime) newer = false;
            if (fs::last_write_time(fs::path(projectDir) / "Assembly-CSharp.csproj", ec) > dllTime) newer = false;
            if (newer) {
                m_builtStamp = sourceStamp(projectDir);
                std::lock_guard<std::mutex> l(m_lock);
                m_ok = true;
                m_messages.clear();
                m_log = "(up to date)";
                m_finishedAt = now;
            }
        }
    }
    if (m_running.load() || now - m_lastPoll < 1.0) return;
    m_lastPoll = now;
    if (sourceStamp(projectDir) != m_builtStamp) start(projectDir);
}

void Builder::start(const std::string& projectDir) {
    if (m_running.load()) return;
    if (m_thread.joinable()) m_thread.join();
    m_dir = projectDir;
    const std::uint64_t stamp = sourceStamp(projectDir);
    m_builtStamp = stamp;   // a failed build is not retried until something changes
    m_running.store(true);
    m_thread = std::thread(&Builder::run, this, projectDir, stamp);
}

void Builder::run(std::string projectDir, std::uint64_t /*stamp*/) {
    syncApi(projectDir);
    std::string out;
    int code = -1;
#ifdef _WIN32
    const fs::path dotnet = dotnetExe();
    if (dotnet.empty()) {
        out = "dotnet was not found. C# scripts need the .NET 10 SDK: https://dotnet.microsoft.com/download";
    } else {
        SetEnvironmentVariableW(L"DOTNET_CLI_TELEMETRY_OPTOUT", L"1");
        SetEnvironmentVariableW(L"DOTNET_NOLOGO", L"1");
        const fs::path csproj = fs::path(projectDir) / "Assembly-CSharp.csproj";
        const std::wstring cmd = L"\"" + dotnet.wstring() + L"\" build \"" + csproj.wstring() +
                                 L"\" -c Debug -nologo -v:q -tl:off -clp:NoSummary";
        code = runCapture(cmd, fs::path(projectDir), out);
        if (code < 0) out = "dotnet would not start (" + dotnet.generic_string() + ")";
    }
#else
    out = "C# scripts are built on Windows only for now";
#endif
    // file(line,col): error CS0103: The name 'x' does not exist ... [project]
    static const std::regex re(R"(^\s*(.+?)\((\d+),\d+\):\s+(error|warning)\s+(\w+):\s+(.*?)(\s+\[[^\]]*\])?\s*$)");
    std::vector<BuildMessage> msgs;
    std::istringstream lines(out);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::smatch m;
        if (!std::regex_match(line, m, re)) continue;
        BuildMessage b;
        b.file  = m[1].str();
        b.line  = std::stoi(m[2].str());
        b.error = m[3].str() == "error";
        b.text  = m[4].str() + ": " + m[5].str();
        const bool dup = std::any_of(msgs.begin(), msgs.end(), [&](const BuildMessage& o) {
            return o.file == b.file && o.line == b.line && o.text == b.text;
        });
        if (!dup) msgs.push_back(std::move(b));
    }
    const bool ok = code == 0;
    if (ok) std::fprintf(stderr, "[C#] scripts built\n");
    else {
        std::fprintf(stderr, "[C#] build failed:\n%s\n", out.c_str());
    }
    {
        std::lock_guard<std::mutex> l(m_lock);
        m_ok         = ok;
        m_messages   = std::move(msgs);
        m_log        = std::move(out);
        m_finishedAt = nowSeconds();
    }
    m_generation.fetch_add(1);
    m_running.store(false);
}

// --- Panel --------------------------------------------------------------------------
void drawPanel(bool& open, Builder& b, const std::string& projectDir, const std::string& runtimeProblem) {
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(560.0f, 320.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("C# Scripts", &open)) { ImGui::End(); return; }
    if (!isCSharp(projectDir)) {
        ImGui::TextWrapped("This project has no C# scripts. File > Game Settings sets it up "
                           "(a Visual Studio solution next to the scenes).");
        ImGui::End();
        return;
    }
    if (ImGui::Button("Open in Visual Studio")) openInIde(projectDir);
    ImGui::SameLine();
    ImGui::BeginDisabled(b.running());
    if (ImGui::Button("Rebuild")) b.start(projectDir);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const ImVec4 red(1.0f, 0.42f, 0.36f, 1.0f), green(0.45f, 0.9f, 0.5f, 1.0f), dim(0.6f, 0.6f, 0.6f, 1.0f);
    const auto msgs = b.messages();
    int errors = 0, warnings = 0;
    for (const auto& m : msgs) (m.error ? errors : warnings)++;
    if (b.running()) ImGui::TextColored(dim, "Building...");
    else if (b.generation() == 0 && b.log().empty()) ImGui::TextColored(dim, "Not built yet");
    else if (b.ok()) ImGui::TextColored(green, "Built%s", warnings ? "" : ", no warnings");
    else ImGui::TextColored(red, "Build failed: %d error%s", errors, errors == 1 ? "" : "s");
    if (!runtimeProblem.empty()) ImGui::TextColored(red, "%s", runtimeProblem.c_str());
    ImGui::Separator();
    ImGui::BeginChild("##csmsgs");
    for (std::size_t i = 0; i < msgs.size(); ++i) {
        const BuildMessage& m = msgs[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Text, m.error ? red : ImVec4(1.0f, 0.8f, 0.4f, 1.0f));
        const std::string label = fs::path(m.file).filename().string() + "(" + std::to_string(m.line) + "): " + m.text;
        if (ImGui::Selectable(label.c_str())) openInIde(projectDir, m.file);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open %s in Visual Studio", m.file.c_str());
        ImGui::PopID();
    }
    if (!b.ok() && msgs.empty() && !b.running()) {
        const std::string log = b.log();
        if (!log.empty()) ImGui::TextWrapped("%s", log.c_str());
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace csproject
