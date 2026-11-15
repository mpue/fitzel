// strahl: render a Strahl scene from the command line.
//
//   strahl scene.strahl                     -> scene.png beside it
//   strahl scene.strahl -o shot.png -s 512 -w 1920 -h 1080
//   strahl scene.strahl -D quality=4 -D variant=\"night\"
//   strahl scene.strahl --frames 120        -> scene-0001.png ... (time runs 0..1)
//   strahl scene.strahl --check             -> build only: errors and what it made
//   strahl scene.strahl --3mf print.3mf     -> its isosurfaces (or every solid) for a 3D printer
//   strahl scene.strahl --pieces            -> the solids it has, numbered for --piece
//
// The same build the editor's Strahl window runs (Strahl.hpp) and the same
// tracer the Render panel drives (PathTrace.hpp); no GL, no window. Writes a
// linear .exr beside the PNG with --exr, since a PNG has already been through
// the tonemap and cannot be graded back out of it.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#include <tinyexr.h>

#include "../src/PathTrace.hpp"
#include "../src/Strahl.hpp"
#include "../src/StrahlPrint.hpp"

namespace fs = std::filesystem;

namespace {

void usage() {
    std::fprintf(stderr,
        "usage: strahl <scene.strahl> [options]\n"
        "  -o <file.png>     where the picture goes (default: render { output } or <scene>.png)\n"
        "  -s <samples>      samples per pixel (overrides render { samples })\n"
        "  -w <px> -h <px>   size (overrides render { width / height })\n"
        "  -b <bounces>      bounces per path\n"
        "  -t <threads>      render threads (default: all but one)\n"
        "  -D name=value     set a top-level let before the script runs (a Strahl expression)\n"
        "  -L <dir>          another library folder for import\n"
        "  --frames <n>      an animation: n frames, `time` running 0..1, numbered PNGs\n"
        "  --frame <k>       render only frame k of --frames\n"
        "  --exr             also write the linear image as .exr\n"
        "  --show <mode>     base | normal | depth: a diagnostic instead of the render\n"
        "  --check           build the scene and report, render nothing\n"
        "  --3mf <file.3mf>  mesh the solids for a 3D printer instead of rendering:\n"
        "                    the isosurfaces, or every solid when there are none\n"
        "  --pieces          list the solids --3mf can take, numbered\n"
        "  --piece <n,...>   take these (from --pieces; repeatable, or \"all\")\n"
        "  --mm <n>          the print's longest side in millimetres (default 100)\n"
        "  --scale <n>       millimetres per scene unit instead (1000: 1 m = 1 m)\n"
        "  --cell <mm>       the meshing grid on the print (default 0.25)\n"
        "  -q                quiet: no progress\n");
}

std::string fmtTime(double s) {
    char b[32];
    if (s < 60.0) std::snprintf(b, sizeof b, "%.1fs", s);
    else if (s < 3600.0) std::snprintf(b, sizeof b, "%dm%02ds", static_cast<int>(s) / 60, static_cast<int>(s) % 60);
    else std::snprintf(b, sizeof b, "%dh%02dm", static_cast<int>(s) / 3600, (static_cast<int>(s) / 60) % 60);
    return b;
}

// The bundled library: strahl-lib/ beside the binary, or sandbox/strahl in the source tree.
std::vector<fs::path> libraryDirs(const char* argv0) {
    std::vector<fs::path> out;
    std::error_code ec;
    const fs::path exe = fs::weakly_canonical(fs::path(argv0), ec);
    for (const fs::path& p : {exe.parent_path() / "strahl-lib",
                              exe.parent_path() / ".." / ".." / ".." / "sandbox" / "strahl"})
        if (fs::is_directory(p, ec)) out.push_back(fs::weakly_canonical(p, ec));
    return out;
}

// --pieces and --3mf: the scene's solids, listed or meshed into a print.
bool print(const strahl::Result& r, const fs::path& out, bool list, bool all, const std::vector<int>& pick,
           strahl::print::Options o, bool quiet) {
    namespace sp = strahl::print;
    const std::vector<sp::Piece> pieces = sp::pieces(r);
    if (pieces.empty()) {
        std::fprintf(stderr, "strahl: the scene has no solid to print (planes, meshes and lamps are not)\n");
        return false;
    }
    std::vector<int> picked;
    if (all) for (int i = 0; i < static_cast<int>(pieces.size()); ++i) picked.push_back(i);
    else if (!pick.empty()) {
        for (int i : pick) {
            if (i < 1 || i > static_cast<int>(pieces.size())) {
                std::fprintf(stderr, "strahl: no piece %d (there are %zu: --pieces lists them)\n", i, pieces.size());
                return false;
            }
            picked.push_back(i - 1);
        }
    } else picked = sp::defaultPick(pieces);
    if (list) {
        for (int i = 0; i < static_cast<int>(pieces.size()); ++i) {
            const sp::Piece& p = pieces[static_cast<std::size_t>(i)];
            const glm::dvec3 d = p.hi - p.lo;
            const bool on = std::find(picked.begin(), picked.end(), i) != picked.end();
            std::printf("%c %2d  %-40s %.3g x %.3g x %.3g\n", on ? '*' : ' ', i + 1, p.label().c_str(), d.x, d.y, d.z);
        }
        std::printf("(* = what --3mf takes without --piece)\n");
        if (out.empty()) return true;
    }
    const auto t0 = std::chrono::steady_clock::now();
    if (!quiet)
        o.progress = [](float f) {
            std::fprintf(stderr, "\r  meshing  %3d%%   ", static_cast<int>(f * 100.0f));
            return true;
        };
    export3mf::Shape shape;
    double mm = 0.0;
    std::vector<std::string> notes;
    std::string err;
    const bool ok = sp::mesh(r, pieces, picked, o, shape, mm, notes, err);
    if (!quiet) std::fprintf(stderr, "\r%*s\r", 40, "");
    for (const std::string& n : notes) std::fprintf(stderr, "note: %s\n", n.c_str());
    if (!ok) { std::fprintf(stderr, "strahl: %s\n", err.c_str()); return false; }
    export3mf::Options wo;
    wo.mmPerMetre = static_cast<float>(mm);
    fs::path file = out;
    if (file.extension().empty()) file += ".3mf";
    std::error_code ec;
    if (!file.parent_path().empty()) fs::create_directories(file.parent_path(), ec);
    if (!export3mf::write(file.string(), shape, wo, err)) { std::fprintf(stderr, "strahl: %s\n", err.c_str()); return false; }
    std::size_t tris = 0;
    for (const export3mf::Part& p : shape.parts) tris += p.tris.size();
    const glm::vec3 size = export3mf::printSize(shape, wo);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("wrote %s (%zu part%s, %zu triangles, %zu colour%s, %.1f x %.1f x %.1f mm, %s)\n",
                file.string().c_str(), shape.parts.size(), shape.parts.size() == 1 ? "" : "s", tris,
                shape.colors.size(), shape.colors.size() == 1 ? "" : "s", size.x, size.y, size.z,
                fmtTime(secs).c_str());
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    fs::path scene;
    fs::path out;
    int samples = 0, width = 0, height = 0, bounces = -1, threads = 0;
    int frames = 1, onlyFrame = -1;
    bool exr = false, check = false, quiet = false;
    fs::path print3mf;
    bool listPieces = false, allPieces = false;
    std::vector<int> pickPieces;
    strahl::print::Options printOpts;
    pathtrace::Show show = pathtrace::Show::Full;
    strahl::Options opts;
    opts.libraryDirs = libraryDirs(argv[0]);

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "strahl: %s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "-o") out = next();
        else if (a == "-s") samples = std::atoi(next());
        else if (a == "-w") width = std::atoi(next());
        else if (a == "-h") height = std::atoi(next());
        else if (a == "-b") bounces = std::atoi(next());
        else if (a == "-t") threads = std::atoi(next());
        else if (a == "-L") opts.libraryDirs.insert(opts.libraryDirs.begin(), fs::path(next()));
        else if (a == "-D") {
            const std::string d = next();
            const auto eq = d.find('=');
            if (eq == std::string::npos) { std::fprintf(stderr, "strahl: -D wants name=value\n"); return 2; }
            opts.defines.emplace_back(d.substr(0, eq), d.substr(eq + 1));
        }
        else if (a == "--frames") frames = std::max(1, std::atoi(next()));
        else if (a == "--frame") onlyFrame = std::atoi(next());
        else if (a == "--exr") exr = true;
        else if (a == "--check") check = true;
        else if (a == "--3mf") print3mf = next();
        else if (a == "--pieces") listPieces = true;
        else if (a == "--piece") {
            const std::string v = next();
            if (v == "all") allPieces = true;
            for (std::size_t at = 0; at < v.size() && !allPieces;) {
                const std::size_t comma = v.find(',', at);
                pickPieces.push_back(std::atoi(v.substr(at, comma - at).c_str()));
                at = comma == std::string::npos ? v.size() : comma + 1;
            }
        }
        else if (a == "--mm") printOpts.longestMm = std::atof(next());
        else if (a == "--scale") printOpts.mmPerUnit = std::atof(next());
        else if (a == "--cell") printOpts.cellMm = std::atof(next());
        else if (a == "-q") quiet = true;
        else if (a == "--show") {
            const std::string m = next();
            show = m == "base" ? pathtrace::Show::BaseColor : m == "normal" ? pathtrace::Show::Normal
                 : m == "depth" ? pathtrace::Show::Depth : pathtrace::Show::Full;
        }
        else if (a == "--help" || a == "-?") { usage(); return 0; }
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "strahl: unknown option %s\n", a.c_str()); usage(); return 2; }
        else scene = a;
    }
    if (scene.empty()) { usage(); return 2; }
    opts.print = [](const std::string& s) { std::printf("%s\n", s.c_str()); };
    opts.frames = frames;

    int failures = 0;
    for (int f = 0; f < frames; ++f) {
        if (onlyFrame >= 0 && f != onlyFrame) continue;
        opts.frame = f;
        opts.time  = frames > 1 ? static_cast<double>(f) / static_cast<double>(frames - 1) : 0.0;
        strahl::Result r = strahl::buildFile(scene, opts);
        for (const std::string& l : r.log)
            if (l.find(": isosurface") != std::string::npos || l.find("glass colour") != std::string::npos)
                std::fprintf(stderr, "note: %s\n", l.c_str());
        if (!r.ok) {
            for (const strahl::Diagnostic& d : r.errors) std::fprintf(stderr, "%s\n", d.format().c_str());
            return 1;
        }
        if (!quiet || check)
            std::printf("built in %.2fs: %d shapes, %d meshes (%lld triangles), %d materials, "
                        "%d lights, %d compiled functions\n",
                        r.seconds, r.shapes, r.meshes, r.triangles, r.materials, r.lights, r.functions);
        if (check) continue;
        if (listPieces || !print3mf.empty()) {
            if (!print(r, print3mf, listPieces, allPieces, pickPieces, printOpts, quiet)) ++failures;
            break;
        }

        pathtrace::Settings s = r.settings;
        if (samples > 0) s.samples = samples;
        if (width > 0) s.width = width;
        if (height > 0) s.height = height;
        if (bounces >= 0) s.maxBounces = bounces;
        if (threads > 0) s.threads = threads;
        s.show = show;

        fs::path png = !out.empty() ? out : !r.output.empty() ? r.output
                     : scene.parent_path() / (scene.stem().string() + ".png");
        if (frames > 1) {
            char num[16];
            std::snprintf(num, sizeof num, "-%04d", f + 1);
            png = png.parent_path() / (png.stem().string() + num + png.extension().string());
        }
        if (png.extension().empty()) png += ".png";

        pathtrace::Job job;
        job.start(r.scene, s);
        const auto t0 = std::chrono::steady_clock::now();
        while (job.running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            if (quiet) continue;
            const double el = job.elapsedSeconds();
            const float p = job.progress();
            const double eta = p > 0.01f ? el / p - el : 0.0;
            std::fprintf(stderr, "\r  %s  %3d%%  %d/%d spp  %s  (%s left)   ", png.filename().string().c_str(),
                         static_cast<int>(p * 100.0f), job.samplesDone(), s.samples,
                         fmtTime(el).c_str(), p > 0.01f ? fmtTime(eta).c_str() : "?");
        }
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!quiet) std::fprintf(stderr, "\r%*s\r", 100, "");

        std::vector<unsigned char> px;
        job.snapshotLdr(px);
        std::error_code ec;
        if (!png.parent_path().empty()) fs::create_directories(png.parent_path(), ec);
        if (!stbi_write_png(png.string().c_str(), s.width, s.height, 4, px.data(), s.width * 4)) {
            std::fprintf(stderr, "strahl: cannot write %s\n", png.string().c_str());
            ++failures;
            continue;
        }
        std::printf("wrote %s (%dx%d, %d spp, %s)\n", png.string().c_str(), s.width, s.height,
                    job.samplesDone(), fmtTime(secs).c_str());
        if (exr) {
            std::vector<float> hdr;
            job.snapshotHdr(hdr);
            fs::path e = png;
            e.replace_extension("exr");
            const char* err = nullptr;
            if (SaveEXR(hdr.data(), s.width, s.height, 3, 1, e.string().c_str(), &err) == TINYEXR_SUCCESS)
                std::printf("wrote %s\n", e.string().c_str());
            else {
                std::fprintf(stderr, "strahl: cannot write %s: %s\n", e.string().c_str(), err ? err : "?");
                if (err) FreeEXRErrorMessage(err);
                ++failures;
            }
        }
    }
    return failures ? 1 : 0;
}
