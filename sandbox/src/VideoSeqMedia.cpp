#include "VideoSeqMedia.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include "VideoImport.hpp"   // ffmpegPath
#include "VideoSeq.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace vseq {

namespace fs = std::filesystem;
using json = nlohmann::json;

// --- ffmpeg ----------------------------------------------------------------------------

std::string ffmpegExe() { return videoimport::ffmpegPath(); }

std::string ffprobeExe() {
#ifdef _WIN32
    const char* name = "ffprobe.exe";
#else
    const char* name = "ffprobe";
#endif
    const fs::path ff(ffmpegExe());
    std::error_code ec;
    if (ff.has_parent_path()) {
        const fs::path beside = ff.parent_path() / name;
        if (fs::exists(beside, ec)) return beside.string();
    }
    return "ffprobe";
}

const char* ffmpegInstallHint() {
#if defined(_WIN32)
    return "winget install Gyan.FFmpeg";
#elif defined(__APPLE__)
    return "brew install ffmpeg";
#else
    return "sudo apt install ffmpeg";
#endif
}

bool ffmpegReady(std::string* version) {
    static std::once_flag once;
    static bool        ready = false;
    static std::string first;
    std::call_once(once, [] {
        Process p;
        if (!p.start(ffmpegExe(), {"-hide_banner", "-version"}, false, true)) return;
        const std::string out = p.readAll();
        ready = p.wait() == 0;
        first = out.substr(0, out.find('\n'));
    });
    if (version) *version = first;
    return ready;
}

// --- Process -----------------------------------------------------------------------------

namespace {
std::atomic<int> g_logSerial{0};

fs::path scratchLog() {
    std::error_code ec;
#ifdef _WIN32
    const unsigned long pid = GetCurrentProcessId();
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif
    return fs::temp_directory_path(ec) /
           ("fitzel-vseq-" + std::to_string(pid) + "-" + std::to_string(g_logSerial.fetch_add(1)) + ".log");
}
} // namespace

struct Process::Impl {
    fs::path log;
    bool     started = false, waited = false;
    int      code = -1;
#ifdef _WIN32
    HANDLE proc = nullptr, in = nullptr, out = nullptr;
#else
    pid_t pid = -1;
    int   in = -1, out = -1;
#endif
};

Process::Process() : m(std::make_unique<Impl>()) {}

Process::~Process() {
    if (m->started && !m->waited) kill();
    std::error_code ec;
    if (!m->log.empty()) fs::remove(m->log, ec);
}

bool Process::started() const { return m->started; }

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// One argument as CommandLineToArgvW will read it back: quoted, with the
// backslashes before a quote (and before the closing one) doubled.
void appendArg(std::wstring& cmd, const std::wstring& a) {
    if (!cmd.empty()) cmd += L' ';
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        cmd += a;
        return;
    }
    cmd += L'"';
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < a.size() && a[i] == L'\\') { ++i; ++slashes; }
        if (i == a.size()) {
            cmd.append(slashes * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            cmd.append(slashes * 2 + 1, L'\\');
            cmd += L'"';
        } else {
            cmd.append(slashes, L'\\');
            cmd += a[i];
        }
    }
    cmd += L'"';
}

// Inheritable handles are inherited by EVERY process started while they exist:
// two decoders starting at once would each hold the other's pipe open and
// neither would ever see its end. One start at a time.
std::mutex g_spawn;

} // namespace

bool Process::start(const std::string& exe, const std::vector<std::string>& args, bool pipeIn, bool pipeOut,
                    std::string* error) {
    std::lock_guard<std::mutex> lk(g_spawn);
    m->log = scratchLog();
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE inRd = nullptr, inWr = nullptr, outRd = nullptr, outWr = nullptr;
    if (pipeIn && !CreatePipe(&inRd, &inWr, &sa, 1 << 20)) {
        if (error) *error = "no pipe";
        return false;
    }
    if (pipeOut && !CreatePipe(&outRd, &outWr, &sa, 1 << 20)) {
        if (inRd) { CloseHandle(inRd); CloseHandle(inWr); }
        if (error) *error = "no pipe";
        return false;
    }
    if (inWr) SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    if (outRd) SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr);
    HANDLE log = CreateFileW(m->log.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, &sa,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    STARTUPINFOW si{};
    si.cb         = sizeof si;
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = pipeIn ? inRd : nul;
    si.hStdOutput = pipeOut ? outWr : nul;
    si.hStdError  = log != INVALID_HANDLE_VALUE ? log : nul;
    std::wstring cmd;
    appendArg(cmd, widen(exe));
    for (const std::string& a : args) appendArg(cmd, widen(a));
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                                   &si, &pi);
    if (inRd) CloseHandle(inRd);
    if (outWr) CloseHandle(outWr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) {
        if (inWr) CloseHandle(inWr);
        if (outRd) CloseHandle(outRd);
        if (error) *error = "could not start " + exe;
        return false;
    }
    CloseHandle(pi.hThread);
    m->proc    = pi.hProcess;
    m->in      = inWr;
    m->out     = outRd;
    m->started = true;
    return true;
}

bool Process::readFully(void* dst, std::size_t n) {
    if (!m->out) return false;
    auto* p = static_cast<char*>(dst);
    while (n > 0) {
        DWORD got = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30));
        if (!ReadFile(m->out, p, want, &got, nullptr) || got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

bool Process::write(const void* src, std::size_t n) {
    if (!m->in) return false;
    auto* p = static_cast<const char*>(src);
    while (n > 0) {
        DWORD put = 0;
        const DWORD want = static_cast<DWORD>(std::min<std::size_t>(n, 1u << 30));
        if (!WriteFile(m->in, p, want, &put, nullptr) || put == 0) return false;
        p += put;
        n -= put;
    }
    return true;
}

void Process::closeIn() {
    if (m->in) CloseHandle(m->in);
    m->in = nullptr;
}

int Process::wait() {
    if (!m->started) return -1;
    if (m->waited) return m->code;
    closeIn();
    WaitForSingleObject(m->proc, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(m->proc, &code);
    CloseHandle(m->proc);
    if (m->out) CloseHandle(m->out);
    m->proc = m->out = nullptr;
    m->waited = true;
    m->code   = static_cast<int>(code);
    return m->code;
}

void Process::kill() {
    if (!m->started || m->waited) return;
    TerminateProcess(m->proc, 1);
    wait();
    m->code = -1;
}

#else

bool Process::start(const std::string& exe, const std::vector<std::string>& args, bool pipeIn, bool pipeOut,
                    std::string* error) {
    // A pipe ffmpeg stops reading from must fail the write, not end the editor.
    static std::once_flag once;
    std::call_once(once, [] { std::signal(SIGPIPE, SIG_IGN); });
    m->log = scratchLog();
    // Close-on-exec: the next process started (another decoder) must not hold
    // this one's pipes open, or this one never sees its stdin end.
    int in[2] = {-1, -1}, out[2] = {-1, -1};
    auto cloexecPipe = [](int fds[2]) {
#ifdef __linux__
        return pipe2(fds, O_CLOEXEC) == 0;
#else
        // No pipe2 (macOS): the flag a moment later, under the same lock as
        // every other start -- no spawn can come between.
        if (pipe(fds) != 0) return false;
        fcntl(fds[0], F_SETFD, FD_CLOEXEC);
        fcntl(fds[1], F_SETFD, FD_CLOEXEC);
        return true;
#endif
    };
    static std::mutex spawnLock;
    std::lock_guard<std::mutex> lk(spawnLock);
    if ((pipeIn && !cloexecPipe(in)) || (pipeOut && !cloexecPipe(out))) {
        for (int fd : {in[0], in[1], out[0], out[1]})
            if (fd >= 0) close(fd);
        if (error) *error = "no pipe";
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (pipeIn) posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    else        posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (pipeOut) posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    else         posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, m->log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    std::vector<std::string> all{exe};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    pid_t pid = -1;
    // spawnp: a bare "ffmpeg" is looked up on the PATH.
    const int err = posix_spawnp(&pid, exe.c_str(), &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (pipeIn) close(in[0]);
    if (pipeOut) close(out[1]);
    if (err != 0) {
        if (pipeIn) close(in[1]);
        if (pipeOut) close(out[0]);
        if (error) *error = "could not start " + exe + ": " + std::strerror(err);
        return false;
    }
    m->pid     = pid;
    m->in      = pipeIn ? in[1] : -1;
    m->out     = pipeOut ? out[0] : -1;
    m->started = true;
    return true;
}

bool Process::readFully(void* dst, std::size_t n) {
    if (m->out < 0) return false;
    auto* p = static_cast<char*>(dst);
    while (n > 0) {
        const ssize_t got = ::read(m->out, p, n);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        p += got;
        n -= static_cast<std::size_t>(got);
    }
    return true;
}

bool Process::write(const void* src, std::size_t n) {
    if (m->in < 0) return false;
    auto* p = static_cast<const char*>(src);
    while (n > 0) {
        const ssize_t put = ::write(m->in, p, n);
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        p += put;
        n -= static_cast<std::size_t>(put);
    }
    return true;
}

void Process::closeIn() {
    if (m->in >= 0) close(m->in);
    m->in = -1;
}

int Process::wait() {
    if (!m->started) return -1;
    if (m->waited) return m->code;
    closeIn();
    int status = 0;
    while (waitpid(m->pid, &status, 0) < 0 && errno == EINTR) {}
    if (m->out >= 0) close(m->out);
    m->out    = -1;
    m->waited = true;
    m->code   = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return m->code;
}

void Process::kill() {
    if (!m->started || m->waited) return;
    ::kill(m->pid, SIGKILL);
    wait();
    m->code = -1;
}

#endif

std::string Process::readAll() {
    std::string s;
    char buf[65536];
    for (;;) {
#ifdef _WIN32
        if (!m->out) break;
        DWORD got = 0;
        if (!ReadFile(m->out, buf, sizeof buf, &got, nullptr) || got == 0) break;
#else
        if (m->out < 0) break;
        const ssize_t got = ::read(m->out, buf, sizeof buf);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
#endif
        s.append(buf, static_cast<std::size_t>(got));
    }
    return s;
}

std::string Process::errors() const {
    std::ifstream f(m->log, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const std::streamoff size = f.tellg();
    const std::streamoff from = std::max<std::streamoff>(0, size - 4096);
    f.seekg(from);
    std::string s(static_cast<std::size_t>(size - from), '\0');
    f.read(s.data(), static_cast<std::streamsize>(s.size()));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}

// --- Probe ------------------------------------------------------------------------------------

namespace {

double rational(const std::string& s) {
    const auto slash = s.find('/');
    try {
        if (slash == std::string::npos) return std::stod(s);
        const double num = std::stod(s.substr(0, slash)), den = std::stod(s.substr(slash + 1));
        return den != 0.0 ? num / den : 0.0;
    } catch (...) {
        return 0.0;
    }
}

double numberOf(const json& j, const char* key) {
    if (!j.contains(key)) return 0.0;
    const json& v = j[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) return rational(v.get<std::string>());
    return 0.0;
}

// The last line of what ffmpeg said: the one that says what went wrong.
std::string lastLine(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    const auto nl = s.find_last_of('\n');
    return nl == std::string::npos ? s : s.substr(nl + 1);
}

// No ffprobe: what `ffmpeg -i` prints about the file, picked apart.
MediaInfo probeWithFfmpeg(const std::string& path) {
    MediaInfo mi;
    Process p;
    if (!p.start(ffmpegExe(), {"-hide_banner", "-nostdin", "-i", path}, false, false, &mi.error)) return mi;
    p.wait();   // "At least one output file must be specified": exit 1, as meant
    std::istringstream in(p.errors());
    std::string line;
    while (std::getline(in, line)) {
        if (const auto d = line.find("Duration: "); d != std::string::npos) {
            int h = 0, mnt = 0;
            double s = 0.0;
            if (std::sscanf(line.c_str() + d + 10, "%d:%d:%lf", &h, &mnt, &s) == 3) mi.duration = h * 3600.0 + mnt * 60.0 + s;
        }
        if (line.find("Stream #") == std::string::npos) continue;
        if (line.find("Video:") != std::string::npos && line.find("attached pic") == std::string::npos && !mi.video) {
            mi.video = true;
            for (std::size_t i = 0; i + 3 < line.size(); ++i) {
                int w = 0, h = 0;
                if ((i == 0 || line[i - 1] == ' ') && std::sscanf(line.c_str() + i, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                    mi.width = w;
                    mi.height = h;
                    break;
                }
            }
            if (const auto f = line.find(" fps"); f != std::string::npos) {
                const auto c = line.rfind(", ", f);
                if (c != std::string::npos) mi.fps = rational(line.substr(c + 2, f - c - 2));
            }
        }
        if (line.find("Audio:") != std::string::npos && !mi.audio) {
            mi.audio = true;
            if (const auto hz = line.find(" Hz"); hz != std::string::npos) {
                const auto c = line.rfind(", ", hz);
                if (c != std::string::npos) mi.audioRate = static_cast<int>(rational(line.substr(c + 2, hz - c - 2)));
            }
            mi.audioChannels = line.find("mono") != std::string::npos ? 1 : 2;
        }
    }
    mi.ok = mi.video || mi.audio;
    if (!mi.ok) mi.error = lastLine(p.errors());
    return mi;
}

} // namespace

MediaInfo probe(const std::string& path) {
    MediaInfo mi;
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        mi.error = "missing: " + path;
        return mi;
    }
    if (isImageFile(path)) {
        int w = 0, h = 0, ch = 0;
        if (!stbi_info(path.c_str(), &w, &h, &ch)) {
            mi.error = "not a picture stb can read";
            return mi;
        }
        mi.ok = mi.still = true;
        mi.width  = w;
        mi.height = h;
        return mi;
    }
    Process p;
    if (!p.start(ffprobeExe(), {"-v", "error", "-print_format", "json", "-show_format", "-show_streams", path}, false,
                 true)) {
        if (ffmpegReady()) return probeWithFfmpeg(path);
        // No ffmpeg at all: a sound the engine decodes itself still works.
        const float secs = fitzel::soundFileSeconds(path);
        if (isAudioFile(path) && secs > 0.0f) {
            mi.ok = mi.audio = true;
            mi.duration = secs;
            mi.audioChannels = 2;
            return mi;
        }
        mi.error = std::string("needs ffmpeg -- ") + ffmpegInstallHint();
        return mi;
    }
    const std::string out = p.readAll();
    const int code = p.wait();
    const json j = json::parse(out, nullptr, false);
    if (code != 0 || j.is_discarded()) {
        mi.error = lastLine(p.errors());
        if (mi.error.empty()) mi.error = "ffprobe could not read it";
        return mi;
    }
    if (j.contains("format")) mi.duration = numberOf(j["format"], "duration");
    if (j.contains("streams"))
        for (const json& s : j["streams"]) {
            const std::string type = s.value("codec_type", std::string());
            if (type == "video" && !mi.video) {
                // A sound file's cover art is a "video" stream too.
                if (s.contains("disposition") && s["disposition"].value("attached_pic", 0) == 1) continue;
                mi.video  = true;
                mi.width  = s.value("width", 0);
                mi.height = s.value("height", 0);
                mi.fps    = rational(s.value("avg_frame_rate", std::string("0/0")));
                if (mi.fps <= 0.0 || mi.fps > 1000.0) mi.fps = rational(s.value("r_frame_rate", std::string("0/0")));
                // Pixels that are not square (anamorphic DV): shown wider.
                const std::string sar = s.value("sample_aspect_ratio", std::string("1:1"));
                if (const auto c = sar.find(':'); c != std::string::npos) {
                    const double a = rational(sar.substr(0, c) + "/" + sar.substr(c + 1));
                    if (a > 0.1 && a < 10.0 && std::abs(a - 1.0) > 1e-3)
                        mi.width = static_cast<int>(std::lround(mi.width * a));
                }
                // A phone's portrait video is landscape pixels with a turn on
                // them; ffmpeg turns them as it decodes.
                int rot = 0;
                if (s.contains("side_data_list"))
                    for (const json& sd : s["side_data_list"])
                        if (sd.contains("rotation")) rot = static_cast<int>(numberOf(sd, "rotation"));
                if (rot == 0 && s.contains("tags") && s["tags"].contains("rotate"))
                    rot = static_cast<int>(rational(s["tags"]["rotate"].get<std::string>()));
                if (std::abs(rot) % 180 == 90) std::swap(mi.width, mi.height);
                if (mi.duration <= 0.0) mi.duration = numberOf(s, "duration");
                const std::string frames = s.value("nb_frames", std::string());
                if (frames == "1") mi.still = true;
            } else if (type == "audio" && !mi.audio) {
                mi.audio         = true;
                mi.audioRate     = static_cast<int>(numberOf(s, "sample_rate"));
                mi.audioChannels = s.value("channels", 2);
                if (mi.duration <= 0.0) mi.duration = numberOf(s, "duration");
            }
        }
    if (mi.video && (mi.fps <= 0.0 || !std::isfinite(mi.fps))) mi.fps = 25.0;
    if (mi.video && mi.duration <= 0.0 && !mi.still) mi.still = true;   // one picture in a container
    mi.ok = (mi.video && mi.width > 0 && mi.height > 0) || mi.audio;
    if (!mi.ok && mi.error.empty()) mi.error = "neither pictures nor sound in it";
    return mi;
}

// --- FrameSource ---------------------------------------------------------------------------

FrameSource::FrameSource(std::string path, int w, int h, double rate)
    : m_path(std::move(path)), m_w(std::max(2, w & ~1)), m_h(std::max(2, h & ~1)), m_rate(std::clamp(rate, 0.1, 480.0)) {
    m_bytes = static_cast<std::size_t>(m_w) * m_h * 4;
    // Ahead by a fraction of a second, within a memory budget (48 MB a source):
    // a quarter-size preview reads twelve ahead, a 4K export two.
    m_ahead  = static_cast<int>(std::clamp<std::size_t>((48u << 20) / m_bytes, 2, 12));
    m_thread = std::thread([this] { run(); });
}

FrameSource::~FrameSource() {
    {
        std::lock_guard<std::mutex> lk(m_mx);
        m_stop = true;
    }
    m_cv.notify_all();
    // A read blocked on the pipe ends when the process does.
    {
        std::lock_guard<std::mutex> lk(m_mx);
        if (m_proc) m_proc->kill();
    }
    if (m_thread.joinable()) m_thread.join();
}

bool FrameSource::spawn(std::int64_t at) {
    char vf[256];
    // fps first: the frame grid is the source's time at this rate, whatever
    // rate the file was shot at (and whatever gaps a phone video has).
    std::snprintf(vf, sizeof vf, "fps=%lld/1000000,scale=%d:%d:flags=bicubic,format=rgba",
                  static_cast<long long>(std::llround(m_rate * 1e6)), m_w, m_h);
    std::vector<std::string> args{"-v", "error", "-nostdin"};
    if (at > 0) {
        char ss[64];
        std::snprintf(ss, sizeof ss, "%.6f", static_cast<double>(at) / m_rate);
        args.insert(args.end(), {"-ss", ss});
    }
    args.insert(args.end(), {"-i", m_path, "-map", "0:v:0", "-an", "-sn", "-dn", "-vf", vf, "-f", "rawvideo",
                             "-pix_fmt", "rgba", "pipe:1"});
    auto p = std::make_unique<Process>();
    std::string err;
    const bool ok = p->start(ffmpegExe(), args, false, true, &err);
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_proc) m_proc->kill();
    m_proc = std::move(p);
    if (!ok) {
        m_failed = true;
        m_error  = err;
    }
    return ok;
}

void FrameSource::run() {
    std::int64_t streamStart = 0;
    std::unique_lock<std::mutex> lk(m_mx);
    for (;;) {
        m_cv.wait(lk, [&] { return m_stop || m_seek >= 0 || (m_running && !m_eof && m_next <= m_want + m_ahead); });
        if (m_stop) break;
        if (m_seek >= 0) {
            const std::int64_t at = m_seek;
            // Running from here on, before ffmpeg is even up: a get() looking
            // in meanwhile must not take the last stream's end for this one's.
            m_seek      = -1;
            streamStart = at;
            m_next      = at;
            m_running   = true;
            m_eof       = false;
            lk.unlock();
            const bool ok = spawn(at);
            lk.lock();
            if (!ok) {
                m_running = false;
                m_eof     = true;
            }
            m_cv.notify_all();
            continue;
        }
        const std::int64_t idx = m_next;
        Process* proc = m_proc.get();
        lk.unlock();
        auto img = std::make_shared<Image>();
        img->w = m_w;
        img->h = m_h;
        img->px.resize(m_bytes);
        const bool ok = proc && proc->readFully(img->px.data(), m_bytes);
        lk.lock();
        if (m_stop) break;
        if (proc != m_proc.get()) continue;   // restarted meanwhile
        if (!ok) {
            m_eof     = true;
            m_running = false;
            const int code = m_proc->wait();
            if (idx == streamStart && code != 0) {
                // Not one frame and an error: the file is not readable.
                m_failed = true;
                m_error  = lastLine(m_proc->errors());
                if (m_error.empty()) m_error = "ffmpeg could not decode " + fs::path(m_path).filename().string();
            } else if (idx == streamStart && streamStart > 0 && !m_last) {
                // Not one frame because the seek landed past the last one (a
                // duration a frame longer than the pictures): a second
                // earlier, read to the end, and that end stands for the rest.
                m_seek = std::max<std::int64_t>(0, streamStart - static_cast<std::int64_t>(std::ceil(m_rate)));
            }
            m_cv.notify_all();
            continue;
        }
        m_frames[idx] = img;
        m_last = img;
        m_next = idx + 1;
        // Keep what is around the frame wanted: a little behind (a scrub back),
        // the read-ahead in front.
        const std::int64_t keepFrom = m_want - std::max<std::int64_t>(2, m_ahead / 2);
        while (!m_frames.empty() && m_frames.begin()->first < keepFrom) m_frames.erase(m_frames.begin());
        while (static_cast<int>(m_frames.size()) > m_ahead * 2 + 4) m_frames.erase(std::prev(m_frames.end()));
        m_cv.notify_all();
    }
    if (m_proc) m_proc->kill();
}

ImagePtr FrameSource::get(std::int64_t i, int waitMs) {
    i = std::max<std::int64_t>(0, i);
    std::unique_lock<std::mutex> lk(m_mx);
    auto have = [&]() -> ImagePtr {
        if (auto it = m_frames.find(i); it != m_frames.end()) return it->second;
        // The stream ran out before `i`: the video's last frame stands.
        if (m_seek < 0 && m_eof && !m_failed && i >= m_next && m_last) return m_last;
        return nullptr;
    };
    if (ImagePtr f = have()) {
        m_want = std::max(m_want, i);
        m_cv.notify_all();
        return m_latest = f;
    }
    if (m_failed) return nullptr;
    m_want = i;
    // Close ahead of where ffmpeg is: reading on gets there sooner than a new
    // process would. Behind it, or far ahead: start over there.
    const bool near = m_running && !m_eof && i >= m_next && i <= m_next + std::max<std::int64_t>(m_ahead * 2, static_cast<std::int64_t>(m_rate));
    const bool pending = m_seek >= 0 && m_seek <= i && i <= m_seek + m_ahead;
    if (!near && !pending) m_seek = i;
    m_cv.notify_all();
    // Done waiting, too, when the stream ended short of `i` with nothing to
    // show for it.
    auto ready = [&] { return m_stop || m_failed || have() != nullptr || (m_seek < 0 && m_eof && i >= m_next && !m_running); };
    if (waitMs < 0) m_cv.wait(lk, ready);
    else            m_cv.wait_for(lk, std::chrono::milliseconds(waitMs), ready);
    ImagePtr f = have();
    if (f) m_latest = f;
    return f;
}

void FrameSource::prefetch(std::int64_t i) {
    i = std::max<std::int64_t>(0, i);
    std::lock_guard<std::mutex> lk(m_mx);
    if (m_frames.count(i) || m_failed) return;
    if (m_running && !m_eof && i >= m_next && i <= m_next + m_ahead) return;
    if (m_seek == i) return;
    m_want = i;
    m_seek = i;
    m_cv.notify_all();
}

ImagePtr FrameSource::latest() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_latest;
}

std::string FrameSource::error() const {
    std::lock_guard<std::mutex> lk(m_mx);
    return m_error;
}

// --- Sound -----------------------------------------------------------------------------------

void measure(Sound& s) {
    fitzel::Waveform& w = s.wave;
    w = {};
    w.rate = 100.0f;
    const std::int64_t frames = s.frames();
    w.seconds = static_cast<float>(frames) / kRate;
    const std::int64_t per = kRate / 100;
    const std::int64_t buckets = (frames + per - 1) / per;
    w.peak.resize(static_cast<std::size_t>(buckets));
    w.rms.resize(static_cast<std::size_t>(buckets));
    for (std::int64_t b = 0; b < buckets; ++b) {
        const std::int64_t f0 = b * per, f1 = std::min(frames, f0 + per);
        float peak = 0.0f;
        double sum = 0.0;
        for (std::int64_t f = f0; f < f1; ++f) {
            const float v = 0.5f * (s.pcm[static_cast<std::size_t>(f * 2)] + s.pcm[static_cast<std::size_t>(f * 2 + 1)]);
            peak = std::max(peak, std::abs(v));
            sum += static_cast<double>(v) * v;
        }
        w.peak[static_cast<std::size_t>(b)] = std::min(1.0f, peak);
        w.rms[static_cast<std::size_t>(b)] = std::min(1.0f, static_cast<float>(std::sqrt(sum / std::max<std::int64_t>(1, f1 - f0))));
    }
}

namespace {

// Any channel count and rate to 48 kHz stereo (linear; only for when there is
// no ffmpeg to do it properly).
std::vector<float> toFilmRate(const fitzel::SampleData& d) {
    std::vector<float> out;
    if (d.channels <= 0 || d.sampleRate <= 0) return out;
    const std::int64_t n = d.frames();
    const double step = static_cast<double>(d.sampleRate) / kRate;
    const std::int64_t m = static_cast<std::int64_t>(std::floor(n / step));
    out.resize(static_cast<std::size_t>(m) * 2);
    auto at = [&](std::int64_t f, int ch) {
        f = std::clamp<std::int64_t>(f, 0, n - 1);
        return d.samples[static_cast<std::size_t>(f * d.channels + std::min(ch, d.channels - 1))];
    };
    for (std::int64_t i = 0; i < m; ++i) {
        const double s = i * step;
        const std::int64_t f = static_cast<std::int64_t>(s);
        const float k = static_cast<float>(s - f);
        for (int ch = 0; ch < 2; ++ch)
            out[static_cast<std::size_t>(i * 2 + ch)] = at(f, ch) + (at(f + 1, ch) - at(f, ch)) * k;
    }
    return out;
}

std::int64_t mtimeOf(const std::string& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}

// Halve a picture (2x2 box).
Image halve(const Image& a) {
    Image b;
    b.w = std::max(1, a.w / 2);
    b.h = std::max(1, a.h / 2);
    b.px.resize(static_cast<std::size_t>(b.w) * b.h * 4);
    for (int y = 0; y < b.h; ++y)
        for (int x = 0; x < b.w; ++x) {
            const int x0 = std::min(a.w - 1, x * 2), x1 = std::min(a.w - 1, x * 2 + 1);
            const int y0 = std::min(a.h - 1, y * 2), y1 = std::min(a.h - 1, y * 2 + 1);
            const std::uint8_t* p[4] = {&a.px[(static_cast<std::size_t>(y0) * a.w + x0) * 4],
                                        &a.px[(static_cast<std::size_t>(y0) * a.w + x1) * 4],
                                        &a.px[(static_cast<std::size_t>(y1) * a.w + x0) * 4],
                                        &a.px[(static_cast<std::size_t>(y1) * a.w + x1) * 4]};
            std::uint8_t* q = &b.px[(static_cast<std::size_t>(y) * b.w + x) * 4];
            // Weighted by alpha, so a transparent pixel's colour does not bleed.
            const int aSum = p[0][3] + p[1][3] + p[2][3] + p[3][3];
            for (int c = 0; c < 3; ++c) {
                const int v = aSum > 0 ? (p[0][c] * p[0][3] + p[1][c] * p[1][3] + p[2][c] * p[2][3] + p[3][c] * p[3][3] + aSum / 2) / aSum
                                       : (p[0][c] + p[1][c] + p[2][c] + p[3][c] + 2) / 4;
                q[c] = static_cast<std::uint8_t>(v);
            }
            q[3] = static_cast<std::uint8_t>((aSum + 2) / 4);
        }
    return b;
}

} // namespace

// --- Library ---------------------------------------------------------------------------------

struct Library::Impl {
    mutable std::mutex mx;
    std::condition_variable cv;
    std::function<std::string(const std::string&)> resolver;

    struct Entry {
        std::shared_ptr<const MediaInfo>  info;
        std::shared_ptr<const Sound>      sound;
        std::shared_ptr<const ThumbStrip> thumbs;
        bool probing = false, sounding = false, thumbing = false;
        std::int64_t songStamp = 0;          // a put sound: when its file was written
        std::vector<Image> levels;           // a picture: full size, then halved
        bool pictureTried = false;
    };
    std::map<std::string, Entry> files;      // by resolved path

    // Workers: probes, sounds and thumbnails run here, a few at a time.
    std::deque<std::function<void()>> jobs;
    std::vector<std::thread> workers;
    int  runningJobs = 0;
    bool stop = false;
    std::set<Process*> live;                 // killed when the library goes

    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lk(mx);
            jobs.push_back(std::move(job));
        }
        cv.notify_one();
    }

    void work() {
        std::unique_lock<std::mutex> lk(mx);
        for (;;) {
            cv.wait(lk, [&] { return stop || !jobs.empty(); });
            if (stop) return;
            auto job = std::move(jobs.front());
            jobs.pop_front();
            ++runningJobs;
            lk.unlock();
            job();
            lk.lock();
            --runningJobs;
            cv.notify_all();
        }
    }

    // A process the library can kill on its way out.
    bool runTracked(Process& p, const std::vector<std::string>& args, std::string& out) {
        {
            std::lock_guard<std::mutex> lk(mx);
            if (stop) return false;
            if (!p.start(ffmpegExe(), args, false, true)) return false;
            live.insert(&p);
        }
        out = p.readAll();
        const int code = p.wait();
        std::lock_guard<std::mutex> lk(mx);
        live.erase(&p);
        return code == 0;
    }

    MediaInfo probeNow(const std::string& path) {
        MediaInfo mi = probe(path);
        std::lock_guard<std::mutex> lk(mx);
        Entry& e = files[path];
        e.info = std::make_shared<MediaInfo>(mi);
        e.probing = false;
        cv.notify_all();
        return mi;
    }

    void soundJob(const std::string& path) {
        auto s = std::make_shared<Sound>();
        const MediaInfo mi = waitInfo(path);
        if (!mi.ok) {
            s->failed = true;
            s->error  = mi.error;
        } else if (!mi.audio) {
            s->ready = true;   // silent: a video without a sound track
        } else if (ffmpegReady()) {
            Process p;
            std::string out;
            // A mono source is read as mono and put on both sides here: ffmpeg's
            // own upmix takes 3 dB off (a pan law), and a voice recorded mono
            // should be as loud in the film as in the file.
            const bool mono = mi.audioChannels == 1;
            if (runTracked(p, {"-v", "error", "-nostdin", "-i", path, "-map", "0:a:0", "-vn", "-ac", mono ? "1" : "2",
                               "-ar", std::to_string(kRate), "-f", "f32le", "pipe:1"},
                           out)) {
                const std::size_t n = out.size() / sizeof(float);
                s->pcm.resize(mono ? n * 2 : n);
                if (mono) {
                    for (std::size_t i = 0; i < n; ++i) {
                        float v;
                        std::memcpy(&v, out.data() + i * sizeof(float), sizeof v);
                        s->pcm[i * 2] = s->pcm[i * 2 + 1] = v;
                    }
                } else {
                    std::memcpy(s->pcm.data(), out.data(), n * sizeof(float));
                }
                s->ready = true;
            } else {
                s->failed = true;
                s->error  = lastLine(p.errors());
            }
        } else {
            fitzel::SampleData d;
            std::string err;
            if (fitzel::loadSampleData(path, d, &err)) {
                s->pcm   = toFilmRate(d);
                s->ready = true;
            } else {
                s->failed = true;
                s->error  = err;
            }
        }
        if (s->ready) measure(*s);
        std::lock_guard<std::mutex> lk(mx);
        Entry& e = files[path];
        if (e.songStamp == 0) e.sound = s;   // a song put in meanwhile wins
        e.sounding = false;
        cv.notify_all();
    }

    void thumbJob(const std::string& path) {
        auto t = std::make_shared<ThumbStrip>();
        const MediaInfo mi = waitInfo(path);
        const int H = 60;
        auto pack = [&](std::vector<Image>& frames) {
            t->count  = static_cast<int>(frames.size());
            t->perRow = std::max(1, std::min(t->count, 4096 / std::max(1, t->w)));
            const int rows = (t->count + t->perRow - 1) / t->perRow;
            t->strip.w = t->perRow * t->w;
            t->strip.h = rows * t->h;
            t->strip.px.assign(static_cast<std::size_t>(t->strip.w) * t->strip.h * 4, 0);
            for (int i = 0; i < t->count; ++i) {
                const int ox = (i % t->perRow) * t->w, oy = (i / t->perRow) * t->h;
                for (int y = 0; y < t->h; ++y)
                    std::memcpy(&t->strip.px[(static_cast<std::size_t>(oy + y) * t->strip.w + ox) * 4],
                                &frames[static_cast<std::size_t>(i)].px[static_cast<std::size_t>(y) * t->w * 4],
                                static_cast<std::size_t>(t->w) * 4);
            }
            t->ready = t->count > 0;
            t->failed = !t->ready;
        };
        if (!mi.ok || mi.width <= 0 || mi.height <= 0) {
            t->failed = true;
        } else {
            t->h = H;
            t->w = std::clamp(static_cast<int>(std::lround(H * static_cast<double>(mi.width) / mi.height)) & ~1, 8, 480);
            if (mi.still) {
                // A picture (or one frame in a video file): one thumbnail.
                std::vector<Image> one(1);
                if (isImageFile(path)) {
                    int w = 0, h = 0, ch = 0;
                    if (unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4)) {
                        Image full;
                        full.w = w;
                        full.h = h;
                        full.px.assign(px, px + static_cast<std::size_t>(w) * h * 4);
                        stbi_image_free(px);
                        while (full.w / 2 >= t->w && full.h / 2 >= t->h) full = halve(full);
                        one[0].w = t->w;
                        one[0].h = t->h;
                        one[0].px.resize(static_cast<std::size_t>(t->w) * t->h * 4);
                        for (int y = 0; y < t->h; ++y)
                            for (int x = 0; x < t->w; ++x) {
                                const int sx = std::min(full.w - 1, x * full.w / t->w), sy = std::min(full.h - 1, y * full.h / t->h);
                                std::memcpy(&one[0].px[(static_cast<std::size_t>(y) * t->w + x) * 4],
                                            &full.px[(static_cast<std::size_t>(sy) * full.w + sx) * 4], 4);
                            }
                        pack(one);
                    } else {
                        t->failed = true;
                    }
                } else {
                    Process p;
                    std::string out;
                    char vf[128];
                    std::snprintf(vf, sizeof vf, "scale=%d:%d,format=rgba", t->w, t->h);
                    if (runTracked(p, {"-v", "error", "-nostdin", "-i", path, "-map", "0:v:0", "-frames:v", "1", "-vf", vf,
                                       "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"},
                                   out) &&
                        out.size() >= static_cast<std::size_t>(t->w) * t->h * 4) {
                        one[0].w = t->w;
                        one[0].h = t->h;
                        one[0].px.assign(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(t->w) * t->h * 4);
                        pack(one);
                    } else {
                        t->failed = true;
                    }
                }
            } else {
                // One every `interval`, up to about 150 along the video; a long
                // one is read keyframes only (seconds instead of minutes).
                t->interval = std::clamp(mi.duration / 150.0, 0.25, 60.0);
                char vf[160];
                std::snprintf(vf, sizeof vf, "fps=1000000/%lld,scale=%d:%d,format=rgba",
                              static_cast<long long>(std::llround(t->interval * 1e6)), t->w, t->h);
                std::vector<std::string> args{"-v", "error", "-nostdin"};
                if (mi.duration > 90.0) args.insert(args.end(), {"-skip_frame", "nokey"});
                args.insert(args.end(), {"-i", path, "-map", "0:v:0", "-an", "-vf", vf, "-f", "rawvideo", "-pix_fmt", "rgba", "pipe:1"});
                Process p;
                std::string out;
                runTracked(p, args, out);
                const std::size_t fb = static_cast<std::size_t>(t->w) * t->h * 4;
                std::vector<Image> frames(out.size() / fb);
                for (std::size_t i = 0; i < frames.size(); ++i) {
                    frames[i].w = t->w;
                    frames[i].h = t->h;
                    frames[i].px.assign(out.begin() + static_cast<std::ptrdiff_t>(i * fb),
                                        out.begin() + static_cast<std::ptrdiff_t>((i + 1) * fb));
                }
                pack(frames);
            }
        }
        std::lock_guard<std::mutex> lk(mx);
        Entry& e = files[path];
        e.thumbs   = t;
        e.thumbing = false;
        cv.notify_all();
    }

    // Probed here and now when nobody has yet. Never waits for a queued probe:
    // the workers waiting on probes still in the queue behind them would be a
    // deadlock, and a file probed twice costs a process.
    MediaInfo waitInfo(const std::string& path) {
        {
            std::lock_guard<std::mutex> lk(mx);
            Entry& e = files[path];
            if (e.info) return *e.info;
            e.probing = true;
        }
        return probeNow(path);
    }
};

Library::Library() : m(std::make_unique<Impl>()) {
    for (int i = 0; i < 3; ++i) m->workers.emplace_back([this] { m->work(); });
}

Library::~Library() {
    {
        std::lock_guard<std::mutex> lk(m->mx);
        m->stop = true;
        for (Process* p : m->live) p->kill();
    }
    m->cv.notify_all();
    for (std::thread& t : m->workers) t.join();
}

void Library::setResolver(std::function<std::string(const std::string&)> r) {
    std::lock_guard<std::mutex> lk(m->mx);
    m->resolver = std::move(r);
}

std::string Library::resolve(const std::string& file) const {
    std::function<std::string(const std::string&)> r;
    {
        std::lock_guard<std::mutex> lk(m->mx);
        r = m->resolver;
    }
    return r ? r(file) : file;
}

std::shared_ptr<const MediaInfo> Library::info(const std::string& file) {
    const std::string path = resolve(file);
    std::lock_guard<std::mutex> lk(m->mx);
    Impl::Entry& e = m->files[path];
    if (!e.info && !e.probing) {
        e.probing = true;
        m->jobs.push_back([this, path] { m->probeNow(path); });
        m->cv.notify_one();
    }
    return e.info;
}

MediaInfo Library::infoNow(const std::string& file) { return m->waitInfo(resolve(file)); }

std::shared_ptr<const Sound> Library::sound(const std::string& file) {
    const std::string path = resolve(file);
    std::lock_guard<std::mutex> lk(m->mx);
    Impl::Entry& e = m->files[path];
    if (!e.sound && !e.sounding && !isSongFile(path)) {
        e.sounding = true;
        m->jobs.push_back([this, path] { m->soundJob(path); });
        m->cv.notify_one();
    }
    return e.sound;
}

std::shared_ptr<const Sound> Library::soundNow(const std::string& file) {
    const std::string path = resolve(file);
    sound(file);
    std::unique_lock<std::mutex> lk(m->mx);
    if (isSongFile(path) && !m->files[path].sound) return nullptr;
    m->cv.wait(lk, [&] { return m->stop || m->files[path].sound != nullptr; });
    return m->files[path].sound;
}

std::shared_ptr<const ThumbStrip> Library::thumbs(const std::string& file) {
    const std::string path = resolve(file);
    std::lock_guard<std::mutex> lk(m->mx);
    Impl::Entry& e = m->files[path];
    if (!e.thumbs && !e.thumbing && !isAudioFile(path)) {
        e.thumbing = true;
        m->jobs.push_back([this, path] { m->thumbJob(path); });
        m->cv.notify_one();
    }
    return e.thumbs;
}

ImagePtr Library::picture(const std::string& file, int maxW, int maxH) {
    const std::string path = resolve(file);
    {
        std::lock_guard<std::mutex> lk(m->mx);
        Impl::Entry& e = m->files[path];
        if (e.pictureTried) {
            if (e.levels.empty()) return nullptr;
            std::size_t l = 0;
            while (l + 1 < e.levels.size() && e.levels[l + 1].w >= maxW && e.levels[l + 1].h >= maxH) ++l;
            return std::make_shared<Image>(e.levels[l]);
        }
    }
    // Decoded outside the lock: a big PNG takes a while.
    std::vector<Image> levels;
    int w = 0, h = 0, ch = 0;
    if (unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4)) {
        Image full;
        full.w = w;
        full.h = h;
        full.px.assign(px, px + static_cast<std::size_t>(w) * h * 4);
        stbi_image_free(px);
        levels.push_back(std::move(full));
        while (levels.back().w > 64 && levels.back().h > 64) levels.push_back(halve(levels.back()));
    }
    std::lock_guard<std::mutex> lk(m->mx);
    Impl::Entry& e = m->files[path];
    e.pictureTried = true;
    e.levels = std::move(levels);
    if (e.levels.empty()) return nullptr;
    std::size_t l = 0;
    while (l + 1 < e.levels.size() && e.levels[l + 1].w >= maxW && e.levels[l + 1].h >= maxH) ++l;
    return std::make_shared<Image>(e.levels[l]);
}

void Library::putSound(const std::string& file, fitzel::SampleData data) {
    const std::string path = resolve(file);
    auto s = std::make_shared<Sound>();
    if (data.sampleRate == kRate && data.channels == 2) s->pcm = std::move(data.samples);
    else                                                s->pcm = toFilmRate(data);
    s->ready = true;
    measure(*s);
    std::lock_guard<std::mutex> lk(m->mx);
    Impl::Entry& e = m->files[path];
    e.sound     = s;
    e.songStamp = std::max<std::int64_t>(1, mtimeOf(path));
    m->cv.notify_all();
}

bool Library::songStale(const std::string& file) {
    const std::string path = resolve(file);
    const std::int64_t now = mtimeOf(path);
    std::lock_guard<std::mutex> lk(m->mx);
    auto it = m->files.find(path);
    return it == m->files.end() || !it->second.sound || it->second.songStamp != std::max<std::int64_t>(1, now);
}

void Library::forget(const std::string& file) {
    const std::string path = resolve(file);
    std::lock_guard<std::mutex> lk(m->mx);
    auto it = m->files.find(path);
    if (it != m->files.end() && !it->second.probing && !it->second.sounding && !it->second.thumbing) m->files.erase(it);
}

void Library::clear() {
    std::lock_guard<std::mutex> lk(m->mx);
    for (auto it = m->files.begin(); it != m->files.end();) {
        if (!it->second.probing && !it->second.sounding && !it->second.thumbing) it = m->files.erase(it);
        else ++it;
    }
}

bool Library::busy() const {
    std::lock_guard<std::mutex> lk(m->mx);
    return m->runningJobs > 0 || !m->jobs.empty();
}

} // namespace vseq
