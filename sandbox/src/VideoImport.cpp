#include "VideoImport.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <stb_image.h>

#include <fitzel/graphics/VideoTexture.hpp>

namespace videoimport {
namespace {

namespace fs = std::filesystem;

std::string lowerExt(const std::string& ext) {
    std::string e;
    for (char c : ext) {
        if (c == '.' && e.empty()) continue;
        e.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return e;
}

// Little-endian writes, matching VideoTexture's reader. Byte by byte rather than
// memcpy from a struct so the layout can't drift with compiler padding.
void putU32(std::vector<unsigned char>& out, std::uint32_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
}

void putF32(std::vector<unsigned char>& out, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof bits);
    putU32(out, bits);
}

// Wrap an argument for the shell std::system hands the command to.
std::string quoted(const std::string& s) { return "\"" + s + "\""; }

// cmd.exe strips the outermost pair of quotes from the command it is given, so a
// command that both starts with a quoted program and contains quoted arguments
// needs one extra pair around the whole thing or the parse comes apart. Harmless
// on a POSIX shell, where the extra quotes just concatenate.
int runCommand(const std::string& cmd) {
#ifdef _WIN32
    return std::system(("\"" + cmd + "\"").c_str());
#else
    return std::system(cmd.c_str());
#endif
}

} // namespace

bool isSourceVideo(const std::string& ext) {
    const std::string e = lowerExt(ext);
    return e == "mp4" || e == "mov" || e == "mkv" || e == "webm" || e == "avi" ||
           e == "m4v" || e == "mpg" || e == "mpeg" || e == "wmv";
}

std::string ffmpegPath() {
    std::error_code ec;
    // main() chdir's to the executable's directory, so this is the bin dir the
    // build copies ffmpeg into.
#ifdef _WIN32
    const fs::path local = fs::current_path(ec) / "ffmpeg.exe";
#else
    const fs::path local = fs::current_path(ec) / "ffmpeg";
#endif
    if (!ec && fs::exists(local, ec)) return local.string();
    return "ffmpeg"; // let the PATH resolve it
}

Result transcode(const std::string& srcFile, const std::string& dstFvid,
                 const Options& opt) {
    Result r;
    std::error_code ec;

    if (!fs::exists(srcFile, ec)) {
        r.message = "Video source is gone: " + srcFile;
        return r;
    }
    const std::string ffmpeg = ffmpegPath();
    if (ffmpeg.empty()) {
        r.message = "No ffmpeg found -- install it (winget install Gyan.FFmpeg) "
                    "or drop ffmpeg.exe next to the editor.";
        return r;
    }

    // Frames go to a scratch directory rather than a pipe: splitting a JPEG
    // stream means parsing markers, and numbered files are something you can
    // look at when an import comes out wrong. Named after the destination stem,
    // so two concurrent imports can't collide.
    const fs::path work =
        fs::temp_directory_path(ec) /
        ("fitzel_video_" + fs::path(dstFvid).stem().string());
    fs::remove_all(work, ec);
    if (!fs::create_directories(work, ec)) {
        r.message = "Couldn't create a scratch folder for the transcode.";
        return r;
    }
    // Whatever happens below, the scratch frames go away.
    struct Cleanup {
        const fs::path& p;
        ~Cleanup() { std::error_code e; fs::remove_all(p, e); }
    } cleanup{work};

    // -v error: ffmpeg's normal banner is several screens of build config.
    // scale=min(maxWidth,iw):-2 downscales without ever upscaling and rounds the
    // height to an even number, which the JPEG encoder's 4:2:0 chroma needs.
    const std::string pattern = (work / "f_%05d.jpg").string();
    const std::string cmd =
        quoted(ffmpeg) + " -y -v error -i " + quoted(srcFile) +
        " -t " + std::to_string(opt.maxSeconds) +
        " -vf \"fps=" + std::to_string(opt.fps) +
        ",scale='min(" + std::to_string(opt.maxWidth) + ",iw)':-2\"" +
        " -q:v " + std::to_string(opt.quality) +
        " -f image2 " + quoted(pattern);

    if (runCommand(cmd) != 0) {
        r.message = "ffmpeg couldn't read " + fs::path(srcFile).filename().string() +
                    " (see the console for its error).";
        return r;
    }

    std::vector<fs::path> frames;
    for (const auto& de : fs::directory_iterator(work, ec))
        if (de.is_regular_file(ec)) frames.push_back(de.path());
    // Zero-padded names, so lexicographic order is playback order.
    std::sort(frames.begin(), frames.end());
    if (frames.empty()) {
        r.message = "ffmpeg produced no frames from " +
                    fs::path(srcFile).filename().string() + ".";
        return r;
    }

    // Load every frame into one buffer, building the index as we go. The payload
    // comes first and the header is prepended at the end, once the frame count
    // and dimensions are known.
    std::vector<unsigned char> payload;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> index;
    index.reserve(frames.size());
    for (const fs::path& f : frames) {
        std::ifstream in(f, std::ios::binary | std::ios::ate);
        if (!in) continue;
        const std::streamoff n = in.tellg();
        if (n <= 0) continue;
        in.seekg(0);
        const std::size_t at = payload.size();
        payload.resize(at + static_cast<std::size_t>(n));
        if (!in.read(reinterpret_cast<char*>(payload.data() + at), n)) {
            payload.resize(at);
            continue;
        }
        index.emplace_back(static_cast<std::uint32_t>(at),
                           static_cast<std::uint32_t>(n));
        if (r.width == 0) {
            int w = 0, h = 0, ch = 0;
            if (stbi_info_from_memory(payload.data() + at, static_cast<int>(n),
                                      &w, &h, &ch)) {
                r.width  = w;
                r.height = h;
            }
        }
    }
    if (index.empty() || r.width <= 0 || r.height <= 0) {
        r.message = "Frames from " + fs::path(srcFile).filename().string() +
                    " couldn't be read back.";
        return r;
    }

    const std::uint32_t count = static_cast<std::uint32_t>(index.size());
    const std::uint32_t base  = 4 + 4 * 5 + count * 8; // header + index
    std::vector<unsigned char> head;
    head.reserve(base);
    head.insert(head.end(), fitzel::VideoTexture::kMagic,
                fitzel::VideoTexture::kMagic + 4);
    putU32(head, fitzel::VideoTexture::kVersion);
    putU32(head, static_cast<std::uint32_t>(r.width));
    putU32(head, static_cast<std::uint32_t>(r.height));
    putU32(head, count);
    putF32(head, static_cast<float>(opt.fps));
    // Payload offsets were relative while collecting; rebase them onto the file.
    for (const auto& [off, size] : index) {
        putU32(head, base + off);
        putU32(head, size);
    }

    fs::create_directories(fs::path(dstFvid).parent_path(), ec);
    std::ofstream out(dstFvid, std::ios::binary | std::ios::trunc);
    if (!out) {
        r.message = "Couldn't write " + dstFvid;
        return r;
    }
    out.write(reinterpret_cast<const char*>(head.data()),
              static_cast<std::streamsize>(head.size()));
    out.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.size()));
    if (!out) {
        r.message = "Writing " + dstFvid + " failed part-way.";
        return r;
    }

    r.ok      = true;
    r.frames  = static_cast<int>(count);
    r.outPath = dstFvid;
    r.message = fs::path(dstFvid).filename().string() + ": " +
                std::to_string(r.width) + "x" + std::to_string(r.height) + ", " +
                std::to_string(r.frames) + " frames.";
    return r;
}

} // namespace videoimport
