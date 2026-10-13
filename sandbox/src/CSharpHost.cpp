#include "CSharpHost.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <fitzel/asset/Vfs.hpp>

#include "Component.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cstdlib>
#include <fstream>
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

// --- The C ABI shared with Fitzel.dll -------------------------------------------
// sandbox/dotnet/Fitzel/Internal/Interop.cs mirrors these field for field. Change
// one, change the other, and bump kVersion on both sides.
namespace {
constexpr int kVersion = 2;

struct CsSpawn {
    int   type, physics, parent;
    float mass, scale;
    float pos[3], half[3], rot[3], color[3], vel[3];
    const char *name, *model, *material, *script;
};
struct CsHit {
    float pos[3], normal[3];
    int   id;
    float dist;
};
struct CsHud {
    int      kind;
    float    a[6];
    float    size, align;
    int      bold;
    unsigned col, col2;
};

struct CsNativeApi {
    int version;
    void (*log)(const char*);
    void (*error)(const char*);
    int (*input)(int what, int code);
    int (*mousePos)(float* xy);
    float (*mouseWheel)();
    void (*showCursor)(int);
    int (*mouseRay)(float* origin, float* dir);
    int (*find)(const char*);
    int (*findAll)(const char*, int* out, int cap);
    int (*getVec)(int id, int what, float* out);
    void (*setVec)(int id, int what, float* v);
    void (*impulse)(int id, float* imp, float* at);
    int (*info)(int id, int* typeParentActive, char* name, int cap);
    void (*setActive)(int id, int on);
    void (*setName)(int id, const char*);
    void (*setParent)(int id, int parent);
    void (*destroy)(int id);
    int (*spawn)(CsSpawn*);
    int (*spawnPrefab)(const char*, float* pos, float yaw);
    int (*clone)(int id, const char*);
    int (*castRay)(float* o, float* d, float maxDist, CsHit* out);
    void (*camera)(float* pos, float* dir, float* screen);
    void (*setCamera)(int what, float* v);
    void (*sound)(const char*, float vol, float pitch, float* pos, float nearM, float farM);
    void (*hud)(CsHud*, const char* text);
    float (*terrainHeight)(float x, float z);
    float (*getFloat)(int what);
    void (*setFloat)(int what, float v);
    void (*setHudText)(const char*);
    void (*scene)(int what, const char* name);
    void (*anim)(int id, int what, const char* name, float v);
    const char* (*callApi)(const char* table, const char* fn, const char* args, int* ok);
};
} // namespace

using CsParamSink = void (*)(void* ctx, const char* name, int type, double num, int b,
                             const char* str, const float* vec);
struct CsManagedApi {
    int (*load)(const unsigned char* dll, int dllLen, const unsigned char* pdb, int pdbLen);
    void (*unload)();
    int (*create)(int entity, const char* cls);
    void (*setParam)(int h, const char* name, int type, double num, int b, const char* str,
                     const float* vec);
    int (*call)(int h, int fn, float dt, float time);
    void (*destroy)(int h);
    int (*scan)(const char* cls, void* ctx, CsParamSink sink);
};

namespace {

// The one instance the C functions talk to. There is one ScriptHost and one
// runtime per process; a second CSharpScripts would only replace this pointer.
CSharpScripts* g_cs = nullptr;

ScriptHost* H() { return g_cs ? g_cs->host() : nullptr; }
std::string S(const char* s) { return s ? std::string(s) : std::string(); }
glm::vec3   V(const float* v) { return {v[0], v[1], v[2]}; }
void        put(float* out, const glm::vec3& v) { out[0] = v.x; out[1] = v.y; out[2] = v.z; }

// Debug.Log: into the Log window (it shows stderr), marked as C#'s.
void csLog(const char* s) { std::fprintf(stderr, "[C#] %s\n", s ? s : ""); }
void csError(const char* s) {
    if (g_cs) g_cs->reportError(S(s));
}
int csInput(int what, int code) {
    ScriptHost* h = H();
    if (!h || (g_cs->inputBlocked && g_cs->inputBlocked())) return 0;
    switch (what) {
        case 0: return h->keyDown && h->keyDown(code);
        case 1: return h->keyPressed && h->keyPressed(code);
        case 2: return h->mouseDown && h->mouseDown(code);
        case 3: return h->mousePressed && h->mousePressed(code);
        default: return 0;
    }
}
int csMousePos(float* xy) {
    ScriptHost* h = H();
    glm::vec2 p(0.0f);
    const bool over = h && h->mousePos && h->mousePos(p);
    xy[0] = p.x; xy[1] = p.y;
    return over ? 1 : 0;
}
float csMouseWheel() {
    ScriptHost* h = H();
    return h && h->mouseWheel ? h->mouseWheel() : 0.0f;
}
void csShowCursor(int on) {
    if (ScriptHost* h = H(); h && h->showCursor) h->showCursor(on != 0);
}
int csMouseRay(float* o, float* d) {
    ScriptHost* h = H();
    glm::vec3 a, b;
    if (!h || !h->mouseRay || !h->mouseRay(a, b)) return 0;
    put(o, a); put(d, b);
    return 1;
}
int csFind(const char* name) {
    ScriptHost* h = H();
    return h && h->findEntity ? h->findEntity(S(name)) : -1;
}
int csFindAll(const char* name, int* out, int cap) {
    ScriptHost* h = H();
    if (!h || !h->findEntities) return 0;
    const std::vector<int> ids = h->findEntities(S(name));
    const int n = static_cast<int>(ids.size());
    for (int i = 0; i < n && i < cap; ++i) out[i] = ids[i];
    return n;
}
// what: 0 local position, 1 local rotation, 2 half extents, 3 world position,
// 4 world rotation, 5 velocity, 6 angular velocity (set only).
int csGetVec(int id, int what, float* out) {
    Entity* e = g_cs ? g_cs->entity(id) : nullptr;
    ScriptHost* h = H();
    switch (what) {
        case 0: if (!e) return 0; put(out, e->localCenter);   return 1;
        case 1: if (!e) return 0; put(out, e->localRotation); return 1;
        case 2: if (!e) return 0; put(out, e->half);          return 1;
        case 3: if (!e) return 0; put(out, e->center);        return 1;
        case 4: if (!e) return 0; put(out, e->rotation);      return 1;
        case 5: {
            glm::vec3 v(0.0f);
            if (!h || !h->getVelocity || !h->getVelocity(id, v)) return 0;
            put(out, v);
            return 1;
        }
        default: return 0;
    }
}
void csSetVec(int id, int what, float* v) {
    Entity* e = g_cs ? g_cs->entity(id) : nullptr;
    ScriptHost* h = H();
    switch (what) {
        case 0: if (e) e->localCenter = V(v);   break;
        case 1: if (e) e->localRotation = V(v); break;
        case 2: if (e) e->half = glm::max(V(v), glm::vec3(0.001f)); break;
        case 3: if (h && h->setPos) h->setPos(id, V(v)); break;
        case 4: if (h && h->setRot) h->setRot(id, V(v)); break;
        case 5: if (h && h->setVelocity) h->setVelocity(id, V(v)); break;
        case 6: if (h && h->setAngularVelocity) h->setAngularVelocity(id, V(v)); break;
        default: break;
    }
}
void csImpulse(int id, float* imp, float* at) {
    ScriptHost* h = H();
    if (!h || !h->applyImpulse) return;
    const glm::vec3 p = at ? V(at) : glm::vec3(0.0f);
    h->applyImpulse(id, V(imp), at ? &p : nullptr);
}
int csInfo(int id, int* tpa, char* name, int cap) {
    Entity* e = g_cs ? g_cs->entity(id) : nullptr;
    if (!e) return 0;
    tpa[0] = static_cast<int>(e->type);
    tpa[1] = e->parent;
    tpa[2] = e->active ? 1 : 0;
    if (cap > 0) {
        const std::size_t n = std::min<std::size_t>(e->name.size(), static_cast<std::size_t>(cap - 1));
        std::memcpy(name, e->name.data(), n);
        name[n] = '\0';
    }
    return 1;
}
void csSetActive(int id, int on) {
    if (ScriptHost* h = H(); h && h->setActive) h->setActive(id, on != 0);
}
void csSetName(int id, const char* n) {
    if (ScriptHost* h = H(); h && h->setName) h->setName(id, S(n));
}
void csSetParent(int id, int parent) {
    if (ScriptHost* h = H(); h && h->setParent) h->setParent(id, parent);
}
void csDestroy(int id) {
    if (ScriptHost* h = H(); h && h->destroy) h->destroy(id);
}
int csSpawn(CsSpawn* b) {
    ScriptHost* h = H();
    if (!h || !h->spawn) return 0;
    ScriptSpawn s;
    s.type = b->type; s.physics = b->physics; s.parent = b->parent;
    s.mass = b->mass; s.scale = b->scale;
    s.pos = V(b->pos); s.half = V(b->half); s.rot = V(b->rot);
    s.color = V(b->color); s.vel = V(b->vel);
    s.name = S(b->name); s.model = S(b->model); s.material = S(b->material); s.script = S(b->script);
    return h->spawn(s);
}
int csSpawnPrefab(const char* name, float* pos, float yaw) {
    ScriptHost* h = H();
    return h && h->spawnPrefab ? h->spawnPrefab(S(name), V(pos), yaw) : 0;
}
int csClone(int id, const char* name) {
    ScriptHost* h = H();
    return h && h->clone ? h->clone(id, S(name)) : 0;
}
int csCastRay(float* o, float* d, float maxDist, CsHit* out) {
    ScriptHost* h = H();
    ScriptRayHit hit;
    if (!h || !h->castRay || !h->castRay(V(o), V(d), maxDist, hit)) return 0;
    put(out->pos, hit.pos);
    put(out->normal, hit.normal);
    out->id   = hit.id;
    out->dist = hit.dist;
    return 1;
}
void csCamera(float* pos, float* dir, float* screen) {
    ScriptHost* h = H();
    if (!h) return;
    put(pos, h->camPos);
    put(dir, h->camDir);
    screen[0] = h->screen.x;
    screen[1] = h->screen.y;
}
void csSetCamera(int what, float* v) {
    ScriptHost* h = H();
    if (!h) return;
    if (what == 0 && h->setCamPos) h->setCamPos(V(v));
    if (what == 1 && h->setCamDir) h->setCamDir(V(v));
    if (what == 2 && h->setCamFov) h->setCamFov(v[0]);
}
void csSound(const char* name, float vol, float pitch, float* pos, float nearM, float farM) {
    ScriptHost* h = H();
    if (!h || !h->playSoundEx) return;
    const glm::vec3 p = pos ? V(pos) : glm::vec3(0.0f);
    h->playSoundEx(S(name), vol, pitch, pos ? &p : nullptr, nearM, farM, std::string());
}
void csHud(CsHud* b, const char* text) {
    ScriptHost* h = H();
    if (!h) return;
    ScriptHudCmd c;
    c.kind = static_cast<ScriptHudCmd::Kind>(std::clamp(b->kind, 0, 7));
    for (int i = 0; i < 6; ++i) c.a[i] = b->a[i];
    c.size  = b->size;
    c.align = b->align;
    c.bold  = b->bold != 0;
    c.col   = b->col;
    c.col2  = b->col2;
    if (text) c.text = text;
    h->hudCmds.push_back(std::move(c));
}
float csTerrainHeight(float x, float z) {
    ScriptHost* h = H();
    return h && h->terrainHeight ? h->terrainHeight(x, z) : 0.0f;
}
float csGetFloat(int what) {
    ScriptHost* h = H();
    if (!h) return 0.0f;
    if (what == 0) return h->getTimeOfDay ? h->getTimeOfDay() : 0.0f;
    if (what == 1) return h->getDayLength ? h->getDayLength() : 0.0f;
    if (what == 2) return static_cast<float>(h->score);
    return 0.0f;
}
void csSetFloat(int what, float v) {
    ScriptHost* h = H();
    if (!h) return;
    if (what == 0 && h->setTimeOfDay) h->setTimeOfDay(v);
    if (what == 1 && h->setDayLength) h->setDayLength(v);
    if (what == 2) h->score = static_cast<int>(v);
    if (what == 3) h->crosshair = v != 0.0f;
}
void csSetHudText(const char* t) {
    if (ScriptHost* h = H()) h->hud = S(t);
}
void csScene(int what, const char* name) {
    ScriptHost* h = H();
    if (!h) return;
    if (what == 0 && h->loadScene) h->loadScene(S(name));
    if (what == 1 && h->restart) h->restart();
    if (what == 2 && h->quit) h->quit();
}
// The rest of the API, through Lua's own functions (ScriptSystem::callApi). The
// answer stays valid until the next call -- Fitzel.dll copies it at once.
const char* csCallApi(const char* table, const char* fn, const char* args, int* ok) {
    static std::string result;
    result.clear();
    *ok = 0;
    if (!g_cs || !g_cs->callLua) {
        result = "the Lua API is not available here";
        return result.c_str();
    }
    *ok = g_cs->callLua(S(table), S(fn), S(args), g_cs->currentScript(), result) ? 1 : 0;
    return result.c_str();
}

void csAnim(int id, int what, const char* name, float v) {
    ScriptHost* h = H();
    if (!h) return;
    if (what == 0 && h->animTrigger) h->animTrigger(id, S(name));
    if (what == 1 && h->animSetBool) h->animSetBool(id, S(name), v != 0.0f);
    if (what == 2 && h->animSetNumber) h->animSetNumber(id, S(name), v);
}

CsNativeApi g_native{
    kVersion,   csLog,      csError,     csInput,        csMousePos,   csMouseWheel,
    csShowCursor, csMouseRay, csFind,    csFindAll,      csGetVec,     csSetVec,
    csImpulse,  csInfo,     csSetActive, csSetName,      csSetParent,  csDestroy,
    csSpawn,    csSpawnPrefab, csClone,  csCastRay,      csCamera,     csSetCamera,
    csSound,    csHud,      csTerrainHeight, csGetFloat, csSetFloat,   csSetHudText,
    csScene,    csAnim,     csCallApi,
};
CsManagedApi g_managed{};

std::string keyOf(int id, const std::string& file) { return std::to_string(id) + "|" + file; }

std::string classOf(const std::string& file) { return fs::path(file).stem().string(); }

} // namespace

// --- CSharpScripts ----------------------------------------------------------------
CSharpScripts::CSharpScripts() { g_cs = this; }

CSharpScripts::~CSharpScripts() {
    // The runtime cannot be shut down and started again in one process; at exit
    // it goes with the process. The instances are dropped so OnDestroy runs.
    if (m_api && m_loaded) m_api->unload();
    if (g_cs == this) g_cs = nullptr;
}

fs::path CSharpScripts::engineDir() {
    std::error_code ec;
    return fs::absolute("dotnet", ec);
}

std::string CSharpScripts::assemblyFor(const std::string& projectDir) {
    if (projectDir.empty()) return {};
    return (fs::path(projectDir) / "Library" / "ScriptAssemblies" / "Assembly-CSharp.dll").generic_string();
}

void CSharpScripts::setAssembly(const std::string& dllPath) {
    if (dllPath == m_dllPath) return;
    reset();
    m_dllPath = dllPath;
}

Entity* CSharpScripts::entity(int id) {
    if (!m_entities) return nullptr;
    std::vector<Entity>& list = *m_entities;
    // The script's own object, usually -- the one the last lookup found, or
    // the next one along, since the tick walks the list in order.
    for (std::size_t k : {m_hint, m_hint + 1})
        if (k < list.size() && list[k].id == id) { m_hint = k; return &list[k]; }
    for (std::size_t k = 0; k < list.size(); ++k)
        if (list[k].id == id) { m_hint = k; return &list[k]; }
    return nullptr;
}

void CSharpScripts::reportError(const std::string& msg) {
    m_lastError = msg;
    // The first line on the card; the whole trace in the log.
    std::fprintf(stderr, "[C#] error: %s\n", msg.c_str());
}

void CSharpScripts::fail(const std::string& key) { m_failed.insert(key); }

void CSharpScripts::reset() {
    if (m_api && m_loaded) m_api->unload();   // OnDestroy on every live instance
    m_loaded = false;
    m_handles.clear();
    m_failed.clear();
    m_lastError.clear();
}

void CSharpScripts::removeEntity(int id) {
    const std::string prefix = std::to_string(id) + "|";
    for (auto it = m_handles.begin(); it != m_handles.end();) {
        if (it->first.rfind(prefix, 0) == 0) {
            if (m_api) m_api->destroy(it->second);
            it = m_handles.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_failed.begin(); it != m_failed.end();)
        it = (it->rfind(prefix, 0) == 0) ? m_failed.erase(it) : std::next(it);
}

fs::file_time_type CSharpScripts::assemblyTime() const {
    std::error_code ec;
    if (m_dllPath.empty()) return {};
    const auto t = fs::last_write_time(m_dllPath, ec);
    return ec ? fs::file_time_type{} : t;
}

// --- hostfxr --------------------------------------------------------------------------
// The few declarations of hostfxr.h / coreclr_delegates.h this needs, written out
// rather than included: they are a stable C ABI, and declaring them here keeps the
// build free of the .NET SDK's packs and the exe free of nethost -- a Lua game
// must not fail to start because a C# helper library is missing.
namespace {
#ifdef _WIN32
using char_t = wchar_t;
#define FZ_CDECL   __cdecl
#define FZ_STDCALL __stdcall
#define FZ_STR(s)  L##s
constexpr const char* kHostfxrName = "hostfxr.dll";
std::wstring native(const fs::path& p) { return p.wstring(); }
#else
using char_t = char;
#define FZ_CDECL
#define FZ_STDCALL
#define FZ_STR(s) s
constexpr const char* kHostfxrName = "libhostfxr.so";
std::string native(const fs::path& p) { return p.string(); }
#endif
using hostfxr_handle = void*;
constexpr int kLoadAssemblyAndGetFunctionPointer = 5;   // hdt_load_assembly_and_get_function_pointer
const char_t* const kUnmanagedCallersOnly = reinterpret_cast<const char_t*>(-1);

using hostfxr_error_writer_fn  = void(FZ_CDECL*)(const char_t*);
using hostfxr_set_error_writer_fn = hostfxr_error_writer_fn(FZ_CDECL*)(hostfxr_error_writer_fn);
using hostfxr_initialize_for_runtime_config_fn = int(FZ_CDECL*)(const char_t*, const void*, hostfxr_handle*);
using hostfxr_get_runtime_delegate_fn = int(FZ_CDECL*)(hostfxr_handle, int, void**);
using hostfxr_close_fn = int(FZ_CDECL*)(hostfxr_handle);
using load_assembly_and_get_function_pointer_fn =
    int(FZ_STDCALL*)(const char_t*, const char_t*, const char_t*, const char_t*, void*, void**);

// The .NET installs to look in, where nethost would look.
std::vector<fs::path> dotnetRoots() {
    std::vector<fs::path> roots;
#ifdef _WIN32
    // DOTNET_ROOT, the installer's registry entry, then Program Files.
    if (const wchar_t* r = _wgetenv(L"DOTNET_ROOT")) roots.emplace_back(r);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\dotnet\\Setup\\InstalledVersions\\x64", 0,
                      KEY_READ | KEY_WOW64_32KEY, &key) == ERROR_SUCCESS) {
        wchar_t buf[MAX_PATH];
        DWORD   len = sizeof buf, type = 0;
        if (RegQueryValueExW(key, L"InstallLocation", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &len) ==
                ERROR_SUCCESS && type == REG_SZ)
            roots.emplace_back(std::wstring(buf));
        RegCloseKey(key);
    }
    for (const wchar_t* env : {L"ProgramW6432", L"ProgramFiles"})
        if (const wchar_t* base = _wgetenv(env)) roots.push_back(fs::path(base) / "dotnet");
#else
    // DOTNET_ROOT, the install location the distribution's package records,
    // wherever `dotnet` on the PATH really lives, then the usual places
    // (Microsoft's packages, Ubuntu's, dotnet-install.sh's).
    if (const char* r = std::getenv("DOTNET_ROOT")) roots.emplace_back(r);
    for (const char* f : {"/etc/dotnet/install_location_x64", "/etc/dotnet/install_location_arm64",
                          "/etc/dotnet/install_location"}) {
        std::ifstream in(f);
        std::string line;
        if (in && std::getline(in, line)) {
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
            if (!line.empty()) roots.emplace_back(line);
        }
    }
    std::error_code ec;
    if (const char* path = std::getenv("PATH")) {
        std::string p = path;
        for (std::size_t at = 0; at <= p.size();) {
            const std::size_t colon = std::min(p.find(':', at), p.size());
            const fs::path exe = fs::path(p.substr(at, colon - at)) / "dotnet";
            if (colon > at && fs::exists(exe, ec)) roots.push_back(fs::canonical(exe, ec).parent_path());
            at = colon + 1;
        }
    }
    for (const char* d : {"/usr/lib/dotnet", "/usr/share/dotnet", "/usr/local/share/dotnet", "/opt/dotnet"})
        roots.emplace_back(d);
    if (const char* home = std::getenv("HOME")) roots.push_back(fs::path(home) / ".dotnet");
#endif
    return roots;
}

// The newest host/fxr/<version>/hostfxr of the first install that has one.
fs::path findHostfxr() {
    auto versionOf = [](const fs::path& p) {
        std::vector<int> v;
        std::string s = p.filename().string();
        for (std::size_t i = 0; i < s.size();) {
            if (!std::isdigit(static_cast<unsigned char>(s[i]))) { ++i; continue; }
            int n = 0;
            while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) n = n * 10 + (s[i++] - '0');
            v.push_back(n);
        }
        return v;
    };
    std::error_code ec;
    for (const fs::path& root : dotnetRoots()) {
        fs::path best;
        std::vector<int> bestV;
        for (const auto& de : fs::directory_iterator(root / "host" / "fxr", ec)) {
            if (!fs::exists(de.path() / kHostfxrName, ec)) continue;
            const std::vector<int> v = versionOf(de.path());
            if (best.empty() || v > bestV) { best = de.path() / kHostfxrName; bestV = v; }
        }
        if (!best.empty()) return best;
    }
    return {};
}

std::string g_hostError;   // what hostfxr said while starting, for runtimeProblem()

void FZ_CDECL hostErrorWriter(const char_t* msg) {
#ifdef _WIN32
    const int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, msg, -1, s.data(), n, nullptr, nullptr);
#else
    std::string s = msg ? msg : "";
#endif
    if (!g_hostError.empty()) g_hostError += '\n';
    g_hostError += s;
}

#ifdef _WIN32
void* openLib(const fs::path& p) { return LoadLibraryW(p.wstring().c_str()); }
void* symbol(void* lib, const char* name) {
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
}
#else
void* openLib(const fs::path& p) { return dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL); }
void* symbol(void* lib, const char* name) { return dlsym(lib, name); }
#endif
} // namespace

bool CSharpScripts::startRuntime() {
    if (m_tried) return m_api != nullptr;
    m_tried = true;
    const fs::path dir    = engineDir();
    const fs::path dll    = dir / "Fitzel.dll";
    const fs::path config = dir / "Fitzel.runtimeconfig.json";
    std::error_code ec;
    if (!fs::exists(dll, ec) || !fs::exists(config, ec)) {
        m_runtimeProblem = "Fitzel.dll is missing next to the program (" + dir.generic_string() + ")";
        return false;
    }
    // hostfxr from the installed .NET, found the way `dotnet` itself finds it.
    const fs::path hostfxr = findHostfxr();
    if (hostfxr.empty()) {
        m_runtimeProblem = ".NET is not installed (C# scripts need the .NET 10 runtime: "
                           "https://dotnet.microsoft.com/download)";
        return false;
    }
    void* lib = openLib(hostfxr);
    if (!lib) { m_runtimeProblem = hostfxr.filename().string() + " would not load"; return false; }
    auto setWriter = reinterpret_cast<hostfxr_set_error_writer_fn>(symbol(lib, "hostfxr_set_error_writer"));
    auto init  = reinterpret_cast<hostfxr_initialize_for_runtime_config_fn>(
        symbol(lib, "hostfxr_initialize_for_runtime_config"));
    auto getDelegate = reinterpret_cast<hostfxr_get_runtime_delegate_fn>(
        symbol(lib, "hostfxr_get_runtime_delegate"));
    auto close = reinterpret_cast<hostfxr_close_fn>(symbol(lib, "hostfxr_close"));
    if (!init || !getDelegate || !close) {
        m_runtimeProblem = hostfxr.filename().string() + " is too old";
        return false;
    }
    if (setWriter) setWriter(hostErrorWriter);

    hostfxr_handle cx = nullptr;
    const int rc = init(native(config).c_str(), nullptr, &cx);
    if (rc < 0 || !cx) {
        m_runtimeProblem = "the .NET runtime would not start" +
                           (g_hostError.empty() ? std::string() : ": " + g_hostError);
        if (cx) close(cx);
        return false;
    }
    load_assembly_and_get_function_pointer_fn loadAsm = nullptr;
    getDelegate(cx, kLoadAssemblyAndGetFunctionPointer, reinterpret_cast<void**>(&loadAsm));
    close(cx);
    if (!loadAsm) { m_runtimeProblem = "the .NET runtime gave no loader"; return false; }

    using InitFn = int(FZ_STDCALL*)(CsNativeApi*, CsManagedApi*);
    InitFn entry = nullptr;
    const int lr = loadAsm(native(dll).c_str(), FZ_STR("Fitzel.Internal.Bridge, Fitzel"), FZ_STR("Init"),
                           kUnmanagedCallersOnly, nullptr, reinterpret_cast<void**>(&entry));
    if (lr != 0 || !entry) {
        m_runtimeProblem = "Fitzel.dll would not load" +
                           (g_hostError.empty() ? std::string() : ": " + g_hostError);
        return false;
    }
    const int v = entry(&g_native, &g_managed);
    if (v != kVersion || !g_managed.load || !g_managed.create || !g_managed.call) {
        m_runtimeProblem = "Fitzel.dll is version " + std::to_string(v) + ", the engine speaks " +
                           std::to_string(kVersion);
        return false;
    }
    if (setWriter) setWriter(nullptr);
    m_api = &g_managed;
    std::fprintf(stderr, "[C#] .NET runtime started (%s)\n", dll.generic_string().c_str());
    return true;
}

bool CSharpScripts::ensureLoaded() {
    if (!startRuntime()) {
        m_lastError = m_runtimeProblem;
        return false;
    }
    if (m_dllPath.empty()) {
        m_lastError = "no C# project is open";
        return false;
    }
    const fs::file_time_type t = assemblyTime();
    // While instances live, the assembly they came from stays: a build that
    // lands mid-game is picked up when Play starts again.
    if (m_loaded && (t == m_loadedTime || !m_handles.empty())) return true;
    const std::vector<std::uint8_t> dll = fitzel::vfs::read(m_dllPath);
    if (dll.empty()) {
        m_lastError = "the C# scripts are not built yet (" + m_dllPath + ")";
        return false;
    }
    const std::string pdbPath = fs::path(m_dllPath).replace_extension(".pdb").generic_string();
    const std::vector<std::uint8_t> pdb =
        fitzel::vfs::exists(pdbPath) ? fitzel::vfs::read(pdbPath) : std::vector<std::uint8_t>{};
    m_handles.clear();
    if (!m_api->load(dll.data(), static_cast<int>(dll.size()), pdb.empty() ? nullptr : pdb.data(),
                     static_cast<int>(pdb.size()))) {
        m_loaded = false;
        return false;
    }
    m_loaded     = true;
    m_loadedTime = t;
    return true;
}

void CSharpScripts::update(Entity& e, const ScriptComponent& sc, float dt, float time) {
    const std::string key = keyOf(e.id, sc.file);
    if (m_failed.count(key)) return;
    // While it runs, this script is "the current one" for the Lua side too:
    // game.captureInput holds the keys for it, and keys held by another
    // script are hidden from it.
    struct Current {
        CSharpScripts& s;
        Current(CSharpScripts& c, const std::string& k) : s(c) {
            s.m_current = "cs|" + k;
            if (s.enterScript) s.enterScript(s.m_current);
        }
        ~Current() {
            s.m_current.clear();
            if (s.enterScript) s.enterScript(std::string());
        }
    } current(*this, key);
    auto it = m_handles.find(key);
    if (it == m_handles.end()) {
        if (!ensureLoaded()) {
            std::fprintf(stderr, "[C#] %s on '%s': %s\n", sc.file.c_str(), e.name.c_str(),
                         m_lastError.c_str());
            fail(key);
            return;
        }
        const int h = m_api->create(e.id, classOf(sc.file).c_str());
        if (!h) { fail(key); return; }
        // The card's values win over the field initialisers, before Start sees them.
        for (const ScriptParam& p : sc.params) {
            const float v[3] = {p.vec.x, p.vec.y, p.vec.z};
            m_api->setParam(h, p.name.c_str(), static_cast<int>(p.type), p.num, p.b ? 1 : 0,
                            p.str.c_str(), v);
        }
        it = m_handles.emplace(key, h).first;
        if (!m_api->call(h, 0, dt, time)) { fail(key); return; }
    }
    if (!m_api->call(it->second, 1, dt, time)) fail(key);
}

std::vector<ScriptParam> CSharpScripts::scanParams(const std::string& file, std::string* err) {
    std::vector<ScriptParam> out;
    const std::string before = m_lastError;
    if (!ensureLoaded()) {
        if (err) *err = m_lastError;
        m_lastError = before;   // a scan is not a script failing
        return out;
    }
    auto sink = [](void* ctx, const char* name, int type, double num, int b, const char* str,
                   const float* vec) {
        ScriptParam p;
        p.name = name ? name : "";
        p.type = static_cast<ScriptParam::Type>(std::clamp(type, 0, 4));
        p.num  = num;
        p.b    = b != 0;
        p.str  = str ? str : "";
        p.vec  = glm::vec3(vec[0], vec[1], vec[2]);
        static_cast<std::vector<ScriptParam>*>(ctx)->push_back(std::move(p));
    };
    const std::string cls = classOf(file);
    if (!m_api->scan(cls.c_str(), &out, sink)) {
        if (err) *err = m_lastError != before ? m_lastError
                                              : "no class '" + cls + "' deriving from Fitzel.Behaviour";
        m_lastError = before;
    }
    return out;
}
