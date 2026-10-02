#include "Blender.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#  pragma comment(lib, "version.lib")
#endif

namespace fs = std::filesystem;

namespace blender {

namespace {

// The script Blender runs: open the file, export everything as one .glb with
// every action baked at the scene's frame rate. Options this Blender does not
// know are left out rather than failing the export (they move between
// versions). The last line it prints, "FITZEL {json}", is what the editor reads.
const char* kScript = R"PY(
import bpy, sys, os, json
argv = sys.argv[sys.argv.index("--") + 1:]
src, dst = argv[0], argv[1]
ext = os.path.splitext(src)[1].lower()
def say(**kw):
    print("FITZEL " + json.dumps(kw), flush=True)
try:
    if ext != ".blend":
        bpy.ops.wm.read_factory_settings(use_empty=True)
        if ext == ".fbx":
            bpy.ops.import_scene.fbx(filepath=src, use_anim=True)
        elif ext == ".bvh":
            bpy.ops.import_anim.bvh(filepath=src, update_scene_fps=True, update_scene_duration=True)
        elif ext == ".dae":
            bpy.ops.wm.collada_import(filepath=src)
        elif ext in (".usd", ".usda", ".usdc", ".usdz"):
            bpy.ops.wm.usd_import(filepath=src)
        elif ext in (".glb", ".gltf"):
            bpy.ops.import_scene.gltf(filepath=src)
        else:
            raise RuntimeError("Blender cannot open " + ext + " files")
    arms = [o for o in bpy.data.objects if o.type == "ARMATURE"]
    if not arms:
        raise RuntimeError("there is no skeleton (armature) in the file")
    acts = [a.name for a in bpy.data.actions]
    for o in arms:
        if bpy.data.actions and (o.animation_data is None or o.animation_data.action is None):
            o.animation_data_create()
            o.animation_data.action = bpy.data.actions[0]
    opts = dict(filepath=dst, export_format="GLB", export_animations=True,
                export_animation_mode="ACTIONS", export_force_sampling=True,
                export_optimize_animation_size=False, export_frame_step=1,
                export_anim_slide_to_zero=True, export_skins=True, export_morph=False,
                export_def_bones=False, export_yup=True, export_apply=False,
                export_materials="EXPORT", export_image_format="AUTO",
                export_reset_pose_bones=True, use_selection=False)
    known = bpy.ops.export_scene.gltf.get_rna_type().properties.keys()
    bpy.ops.export_scene.gltf(**{k: v for k, v in opts.items() if k in known})
    say(ok=True, armatures=len(arms), actions=acts, fps=bpy.context.scene.render.fps,
        version=bpy.app.version_string)
except Exception as e:
    say(ok=False, error=str(e))
    sys.exit(1)
)PY";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const char* kSettings = "blender.json";

std::string chosenPath() {
    std::ifstream f(kSettings);
    if (!f) return {};
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return {};
    return j.value("path", std::string());
}

// "5.1.0" -> comparable number.
long versionKey(const std::string& v) {
    int a = 0, b = 0, c = 0;
    std::sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c);
    return a * 1000000L + b * 1000L + c;
}

#ifdef _WIN32


std::string fileVersion(const fs::path& exe) {
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.wstring().c_str(), &dummy);
    if (size == 0) return {};
    std::vector<std::uint8_t> buf(size);
    if (!GetFileVersionInfoW(exe.wstring().c_str(), 0, size, buf.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return {};
    char out[48];
    std::snprintf(out, sizeof out, "%u.%u.%u", HIWORD(info->dwProductVersionMS),
                  LOWORD(info->dwProductVersionMS), HIWORD(info->dwProductVersionLS));
    return out;
}

std::wstring regString(HKEY root, const std::wstring& key, const wchar_t* value) {
    wchar_t buf[2048] = {};
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, buf, &size) !=
        ERROR_SUCCESS)
        return {};
    return buf;
}

// The program a registry command line starts: the quoted (or first) word.
fs::path commandExe(const std::wstring& cmd) {
    if (cmd.empty()) return {};
    if (cmd[0] == L'"') {
        const std::size_t e = cmd.find(L'"', 1);
        return e == std::wstring::npos ? fs::path() : fs::path(cmd.substr(1, e - 1));
    }
    const std::size_t sp = cmd.find(L' ');
    return fs::path(cmd.substr(0, sp));
}

void candidates(std::vector<fs::path>& out) {
    std::error_code ec;
    auto add = [&](const fs::path& p) { if (!p.empty() && fs::exists(p, ec)) out.push_back(p); };
    // The .blend file association: the launcher sits beside blender.exe.
    for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        const fs::path exe = commandExe(regString(root, L"SOFTWARE\\Classes\\blendfile\\shell\\open\\command", nullptr));
        if (!exe.empty()) add(exe.parent_path() / "blender.exe");
        add(commandExe(regString(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\blender.exe", nullptr)));
    }
    // Every installer entry that says Blender.
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            HKEY h = nullptr;
            if (RegOpenKeyExW(root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall", 0,
                              KEY_READ | view, &h) != ERROR_SUCCESS)
                continue;
            for (DWORD i = 0;; ++i) {
                wchar_t name[256];
                DWORD len = 256;
                if (RegEnumKeyExW(h, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + name;
                const std::wstring disp = regString(root, key, L"DisplayName");
                if (disp.rfind(L"Blender", 0) != 0) continue;
                const std::wstring loc = regString(root, key, L"InstallLocation");
                if (!loc.empty()) add(fs::path(loc) / "blender.exe");
            }
            RegCloseKey(h);
        }
    // The usual folders: every "Blender x.y" under Program Files, and Steam.
    for (const char* env : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        const char* base = std::getenv(env);
        if (!base) continue;
        for (const fs::path dir : {fs::path(base) / "Blender Foundation", fs::path(base) / "Programs" / "Blender Foundation"}) {
            if (!fs::is_directory(dir, ec)) continue;
            for (const auto& e : fs::directory_iterator(dir, ec)) add(e.path() / "blender.exe");
        }
        add(fs::path(base) / "Steam" / "steamapps" / "common" / "Blender" / "blender.exe");
    }
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"blender.exe", nullptr, MAX_PATH, found, nullptr)) add(fs::path(found));
}

// Run Blender with `args`, its output into `logFile`; false on timeout or if it
// would not start. `code` is its exit code.
bool run(const fs::path& exe, const std::wstring& args, const fs::path& logFile, DWORD& code, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE log = CreateFileW(logFile.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (log == INVALID_HANDLE_VALUE) return false;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = log;
    si.hStdError = log;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\" " + args;
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        exe.parent_path().wstring().c_str(), &si, &pi);
    CloseHandle(log);
    if (!started) return false;
    const DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (w == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return w != WAIT_TIMEOUT;
}

#else

std::string fileVersion(const fs::path&) { return {}; }

void candidates(std::vector<fs::path>& out) {
    std::error_code ec;
    auto add = [&](const fs::path& p) { if (fs::exists(p, ec)) out.push_back(p); };
    if (const char* path = std::getenv("PATH")) {
        std::stringstream ss(path);
        std::string dir;
        while (std::getline(ss, dir, ':'))
            if (!dir.empty()) add(fs::path(dir) / "blender");
    }
    add("/Applications/Blender.app/Contents/MacOS/Blender");
    add("/snap/bin/blender");
    add("/usr/local/bin/blender");
    add("/usr/bin/blender");
}

#endif

std::string versionOf(const fs::path& exe) {
    std::string v = fileVersion(exe);
    if (!v.empty()) return v;
    // "Blender Foundation/Blender 5.1/blender.exe" says it in the folder name.
    const std::string dir = exe.parent_path().filename().string();
    const std::size_t at = dir.find_first_of("0123456789");
    return at == std::string::npos ? std::string() : dir.substr(at);
}

std::mutex g_findLock;
Install    g_found;
bool       g_searched = false;

} // namespace

const Install& find(bool again) {
    std::lock_guard<std::mutex> lock(g_findLock);
    if (g_searched && !again) return g_found;
    g_searched = true;
    g_found = Install{};
    std::error_code ec;
    const std::string mine = chosenPath();
    if (!mine.empty() && fs::exists(fs::path(mine), ec)) {
        g_found.exe = fs::path(mine).generic_string();
        g_found.version = versionOf(fs::path(mine));
        g_found.chosen = true;
        return g_found;
    }
    if (const char* env = std::getenv("FITZEL_BLENDER")) {
        if (fs::exists(fs::path(env), ec)) {
            g_found.exe = fs::path(env).generic_string();
            g_found.version = versionOf(fs::path(env));
            return g_found;
        }
    }
    std::vector<fs::path> all;
    candidates(all);
    long bestKey = -1;
    for (const fs::path& p : all) {
        const std::string v = versionOf(p);
        const long k = versionKey(v);
        if (k > bestKey) {
            bestKey = k;
            g_found.exe = fs::path(p).lexically_normal().generic_string();
            g_found.version = v;
        }
    }
    return g_found;
}

bool choose(const std::string& exe) {
    std::error_code ec;
    if (!exe.empty() && !fs::exists(fs::path(exe), ec)) return false;
    nlohmann::json j = {{"path", exe}};
    std::ofstream f(kSettings, std::ios::trunc);
    f << j.dump(2) << "\n";
    find(true);
    return true;
}

bool canConvert(const std::string& ext) {
    const std::string e = lower(ext);
    return e == ".fbx" || e == ".bvh" || e == ".blend" || e == ".dae" || e == ".usd" || e == ".usda" ||
           e == ".usdc" || e == ".usdz";
}

bool needsBlender(const std::string& file) {
    const std::string e = lower(fs::path(file).extension().string());
    return e != ".glb" && e != ".gltf";
}

std::string cachePath(const std::string& src) {
    std::error_code ec;
    const fs::path p(src);
    const auto size = fs::file_size(p, ec);
    const auto time = fs::last_write_time(p, ec).time_since_epoch().count();
    const std::string key = lower(fs::absolute(p, ec).generic_string()) + "|" + std::to_string(size) + "|" +
                            std::to_string(static_cast<long long>(time));
    std::uint64_t h = 14695981039346656037ull;
    for (unsigned char c : key) { h ^= c; h *= 1099511628211ull; }
    char hex[24];
    std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(h));
    const fs::path dir = fs::temp_directory_path(ec) / "fitzel_retarget";
    return (dir / (p.stem().string() + "-" + hex + ".glb")).generic_string();
}

Result toGlb(const std::string& src) {
    Result r;
    std::error_code ec;
    const std::string name = fs::path(src).filename().string();
    if (!fs::exists(fs::path(src), ec)) { r.message = name + " is not there."; return r; }
    const std::string ext = fs::path(src).extension().string();
    if (!canConvert(ext)) { r.message = "Blender cannot read " + ext + " files."; return r; }
    r.glb = cachePath(src);
    if (fs::exists(fs::path(r.glb), ec) && fs::file_size(fs::path(r.glb), ec) > 0) {
        r.ok = true;
        r.message = "converted earlier, from the cache";
        return r;
    }
    const Install& bl = find();
    if (bl.exe.empty()) {
        r.message = "Blender is needed for " + ext + " files and was not found -- install it (blender.org) "
                    "or point the editor at blender.exe.";
        return r;
    }
#ifdef _WIN32
    // One Blender at a time: three of them loading three FBX files at once
    // only makes all three slow.
    static std::mutex oneAtATime;
    std::lock_guard<std::mutex> lock(oneAtATime);
    const fs::path dir = fs::path(r.glb).parent_path();
    fs::create_directories(dir, ec);
    const fs::path script = dir / "fitzel_to_glb.py";
    {
        std::ofstream f(script, std::ios::trunc);
        f << kScript;
    }
    // Ends in .glb: the exporter appends one to any other name.
    const fs::path out = fs::path(r.glb).replace_extension(".part.glb");
    const fs::path logFile = fs::path(r.glb + ".log");
    const std::wstring q = L"\"";
    std::wstring args;
    if (lower(ext) == ".blend") args += q + fs::path(src).wstring() + q + L" ";
    args += L"--background ";
    if (lower(ext) != ".blend") args += L"--factory-startup ";
    args += L"--python " + q + script.wstring() + q + L" -- " + q + fs::path(src).wstring() + q + L" " + q +
            out.wstring() + q;
    DWORD code = 0;
    const bool finished = run(fs::path(bl.exe), args, logFile, code, 10u * 60u * 1000u);
    {
        std::ifstream f(logFile, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        r.log = ss.str();
    }
    // What the script said about itself.
    nlohmann::json said;
    if (const std::size_t at = r.log.rfind("FITZEL {"); at != std::string::npos) {
        const std::size_t e = r.log.find('\n', at);
        said = nlohmann::json::parse(r.log.substr(at + 7, e == std::string::npos ? std::string::npos : e - at - 7),
                                     nullptr, false);
    }
    if (!finished) { r.message = "Blender took longer than ten minutes for " + name + " and was stopped."; return r; }
    if (said.is_object() && !said.value("ok", false)) {
        r.message = "Blender could not convert " + name + ": " + said.value("error", std::string("unknown error"));
        return r;
    }
    if (code != 0 || !fs::exists(out, ec)) {
        r.message = "Blender could not convert " + name + " (see " + logFile.filename().string() + ").";
        return r;
    }
    fs::rename(out, fs::path(r.glb), ec);
    if (ec) { r.message = "Could not keep the converted " + name + ": " + ec.message(); return r; }
    r.ok = true;
    r.message = "converted with Blender " + (said.is_object() ? said.value("version", bl.version) : bl.version);
    return r;
#else
    r.message = "Converting with Blender is only wired up on Windows so far.";
    return r;
#endif
}

} // namespace blender