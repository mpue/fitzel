// videoseqcheck: the Video Sequencer's insides (VideoSeq.cpp, VideoSeqMedia.cpp,
// VideoSeqRender.cpp) against files ffmpeg makes for it, so every answer is known:
//   - a video of four seconds, one solid colour each (red, green, blue, white),
//     with a 440 Hz tone; a WAV that is a constant 0.25; a picture half yellow,
//     half blue;
//   - the probe reads their sizes, rates and lengths;
//   - a FrameSource hands out the right colour for the right frame, forward,
//     backward, far ahead, past the end, and after a seek mid-read;
//   - a frame of a film: a clip's in-point, speed, opacity, fade, a dissolve
//     between two clips overlapping on a track, a track over another, Fit and
//     Fill, a turned picture, a title, a hidden track;
//   - the mixdown: volume, fades, the equal-power cross-fade, speed, a muted
//     video's sound, a muted track;
//   - edits: split (and the halves joining up seamlessly), ripple delete; the
//     JSON round trip;
//   - an export to MP4 read back by ffprobe and ffmpeg (length, size, sound,
//     the colours where they belong), to .fvid read by the engine's own
//     VideoTexture, to WAV; a cancelled one leaves no file.
//   build/linux/bin/videoseqcheck            (needs ffmpeg; exit 3 when there is none)
//   build/linux/bin/videoseqcheck --bench    (what a 1080p frame of three layers costs)

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <fitzel/audio/SampleData.hpp>

#include "../src/VideoSeq.hpp"
#include "../src/VideoSeqMedia.hpp"
#include "../src/VideoSeqRender.hpp"

namespace fs = std::filesystem;
using namespace vseq;

namespace {

int g_fails = 0;
void check(bool ok, const std::string& what) {
    std::printf(ok ? "  ok   %s\n" : "[FAIL] %s\n", what.c_str());
    if (!ok) ++g_fails;
}

struct Px { int r, g, b; };
Px at(const Image& im, int x, int y) {
    const std::uint8_t* p = &im.px[(static_cast<std::size_t>(y) * im.w + x) * 4];
    return {p[0], p[1], p[2]};
}
bool near(Px p, int r, int g, int b, int tol = 28) {
    return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol;
}
std::string str(Px p) {
    char b[48];
    std::snprintf(b, sizeof b, "(%d,%d,%d)", p.r, p.g, p.b);
    return b;
}

bool run(const std::vector<std::string>& args) {
    Process p;
    if (!p.start(ffmpegExe(), args, false, false)) return false;
    const int code = p.wait();
    if (code != 0) std::printf("ffmpeg: %s\n", p.errors().c_str());
    return code == 0;
}

void writeDcWav(const fs::path& p, float seconds, float value) {
    fitzel::SampleData d;
    d.channels = 2;
    d.sampleRate = kRate;
    d.samples.assign(static_cast<std::size_t>(seconds * kRate) * 2, value);
    std::string err;
    fitzel::saveWav(p.string(), d, fitzel::WavFormat::Float32, &err);
}

void writePicture(const fs::path& p) {
    std::vector<unsigned char> px(64 * 32 * 4);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 64; ++x) {
            unsigned char* q = &px[(static_cast<std::size_t>(y) * 64 + x) * 4];
            if (x < 32) { q[0] = 255; q[1] = 230; q[2] = 0; }
            else        { q[0] = 0; q[1] = 60; q[2] = 255; }
            q[3] = 255;
        }
    stbi_write_png(p.string().c_str(), 64, 32, 4, px.data(), 64 * 4);
}

float sampleAt(const std::vector<float>& pcm, double t, double from = 0.0) {
    const std::size_t i = static_cast<std::size_t>(std::llround((t - from) * kRate)) * 2;
    return i < pcm.size() ? pcm[i] : -99.0f;
}

double rmsOver(const std::vector<float>& pcm, double t0, double t1) {
    double s = 0.0;
    std::size_t n = 0;
    for (std::size_t i = static_cast<std::size_t>(t0 * kRate) * 2; i < std::min(pcm.size(), static_cast<std::size_t>(t1 * kRate) * 2); i += 2) {
        s += pcm[i] * pcm[i];
        ++n;
    }
    return n ? std::sqrt(s / n) : 0.0;
}

Clip videoClip(const std::string& file, double start, double length, double in = 0.0) {
    Clip c;
    c.kind = ClipKind::Video;
    c.file = file;
    c.name = fs::path(file).stem().string();
    c.start = start;
    c.length = length;
    c.in = in;
    return c;
}

int bench(const fs::path& dir) {
    Library lib;
    lib.setResolver([dir](const std::string& f) { return (dir / f).string(); });
    Frames frames(lib);
    Film film;
    film.width = 1920;
    film.height = 1080;
    const int v1 = film.addTrack(TrackKind::Video), v2 = film.addTrack(TrackKind::Video);
    film.addClip(v1, videoClip("colors.mp4", 0, 4));
    Clip pic;
    pic.kind = ClipKind::Image;
    pic.file = "pic.png";
    pic.start = 0;
    pic.length = 4;
    pic.look.scale = 0.5f;
    pic.look.rotation = 12.0f;
    film.addClip(v2, pic);
    Clip t;
    t.kind = ClipKind::Text;
    t.start = 0;
    t.length = 4;
    t.text.text = "Coming soon\nto a screen near you";
    t.text.outline = 0.05f;
    film.addClip(film.addTrack(TrackKind::Video), t);
    Image img;
    drawFrame(film, 0.0, 1920, 1080, lib, frames, -1, img);
    const int n = 60;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < n; ++i) drawFrame(film, i / 30.0, 1920, 1080, lib, frames, -1, img);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / n;
    std::printf("1080p frame, video + turned picture + title: %.1f ms (%.0f fps)\n", ms, 1000.0 / ms);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string version;
    if (!ffmpegReady(&version)) {
        std::printf("[videoseqcheck] no ffmpeg (%s) -- %s\n", ffmpegExe().c_str(), ffmpegInstallHint());
        return 3;
    }
    std::printf("[videoseqcheck] %s\n", version.c_str());

    std::error_code ec;
    const fs::path dir = fs::temp_directory_path() / "fitzel-videoseqcheck";
    fs::create_directories(dir, ec);
    const fs::path colors = dir / "colors.mp4";
    if (!fs::exists(colors, ec)) {
        const bool made = run({"-y", "-v", "error", "-f", "lavfi", "-i", "color=c=red:s=320x180:r=30:d=1", "-f", "lavfi", "-i",
                               "color=c=lime:s=320x180:r=30:d=1", "-f", "lavfi", "-i", "color=c=blue:s=320x180:r=30:d=1", "-f",
                               "lavfi", "-i", "color=c=white:s=320x180:r=30:d=1", "-f", "lavfi", "-i", "sine=f=440:d=4:r=48000",
                               "-filter_complex", "[0][1][2][3]concat=n=4:v=1:a=0[v]", "-map", "[v]", "-map", "4:a", "-c:v",
                               "libx264", "-crf", "8", "-g", "15", "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "128k",
                               colors.string()});
        check(made, "ffmpeg made the test video");
    }
    writeDcWav(dir / "dc.wav", 3.0f, 0.25f);
    writePicture(dir / "pic.png");
    if (argc > 1 && std::string(argv[1]) == "--bench") return bench(dir);

    Library lib;
    lib.setResolver([dir](const std::string& f) { return fs::path(f).is_absolute() ? f : (dir / f).string(); });

    std::printf("-- probe\n");
    {
        const MediaInfo v = lib.infoNow("colors.mp4");
        check(v.ok && v.video && v.audio && !v.still, "the video has pictures and sound");
        check(v.width == 320 && v.height == 180, "its size is 320x180 (" + std::to_string(v.width) + "x" + std::to_string(v.height) + ")");
        check(std::abs(v.fps - 30.0) < 0.01, "its rate is 30 fps (" + std::to_string(v.fps) + ")");
        check(std::abs(v.duration - 4.0) < 0.1, "it is 4 s long (" + std::to_string(v.duration) + ")");
        const MediaInfo w = lib.infoNow("dc.wav");
        check(w.ok && w.audio && !w.video && std::abs(w.duration - 3.0) < 0.01, "the WAV: sound only, 3 s");
        const MediaInfo p = lib.infoNow("pic.png");
        check(p.ok && p.still && p.width == 64 && p.height == 32, "the picture: a still, 64x32");
        const MediaInfo gone = lib.infoNow("nothing-here.mp4");
        check(!gone.ok && !gone.error.empty(), "a missing file says so (" + gone.error + ")");
    }

    std::printf("-- frames\n");
    {
        FrameSource src(colors.string(), 160, 90, 30.0);
        auto colorOf = [&](std::int64_t i) {
            ImagePtr f = src.get(i, 5000);
            return f ? at(*f, 80, 45) : Px{-1, -1, -1};
        };
        Px p = colorOf(15);
        check(near(p, 255, 0, 0), "frame 15 is red " + str(p));
        p = colorOf(75);
        check(near(p, 0, 0, 255), "frame 75 (a seek ahead) is blue " + str(p));
        p = colorOf(45);
        check(near(p, 0, 255, 0), "frame 45 (a seek back) is green " + str(p));
        int got = 0;
        bool order = true;
        for (std::int64_t i = 46; i < 120; ++i) {
            ImagePtr f = src.get(i, 5000);
            if (!f) { order = false; break; }
            ++got;
            const Px q = at(*f, 80, 45);
            const int sec = static_cast<int>(i / 30);
            const bool right = sec == 1 ? near(q, 0, 255, 0) : sec == 2 ? near(q, 0, 0, 255) : near(q, 255, 255, 255);
            // The frames on a colour change may be either side of it.
            if (!right && i % 30 != 0 && i % 30 != 29) order = false;
        }
        check(got == 74 && order, "frames 46..119 played on, each the colour of its second");
        p = colorOf(119);
        check(near(p, 255, 255, 255), "frame 119 is white " + str(p));
        p = colorOf(500);
        check(near(p, 255, 255, 255), "past the end: the last frame " + str(p));
        // A fresh source asked straight past the end still finds the last frame.
        FrameSource late(colors.string(), 160, 90, 30.0);
        ImagePtr f = late.get(200, 8000);
        check(f && near(at(*f, 80, 45), 255, 255, 255), "a fresh source seeked past the end shows the last frame");
        FrameSource bad((dir / "pic-not.mp4").string(), 160, 90, 30.0);
        const bool none = !bad.get(0, 5000);
        check(none && !bad.error().empty(), "an unreadable file: no frame and a reason (" + bad.error() + ")");
        // Prefetch, then the frame is there without waiting long.
        FrameSource pre(colors.string(), 160, 90, 30.0);
        pre.prefetch(60);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        f = pre.get(60, 5);
        check(f && near(at(*f, 80, 45), 0, 0, 255), "a prefetched frame is ready when asked");
    }

    std::printf("-- a frame of a film\n");
    Frames frames(lib);
    auto draw = [&](const Film& film, double t) {
        Image img;
        DrawReport rep;
        drawFrame(film, t, film.width, film.height, lib, frames, -1, img, &rep);
        for (const auto& pr : rep.problems) std::printf("       problem: %s\n", pr.c_str());
        return img;
    };
    {
        Film film;
        film.width = 320;
        film.height = 180;
        const int v1 = film.addTrack(TrackKind::Video);
        const int cu = film.addClip(v1, videoClip("colors.mp4", 0.0, 2.0, 1.0));
        Px p = at(draw(film, 0.5), 160, 90);
        check(near(p, 0, 255, 0), "a clip from 1 s in: green at 0.5 " + str(p));
        p = at(draw(film, 1.5), 160, 90);
        check(near(p, 0, 0, 255), "...and blue at 1.5 " + str(p));
        p = at(draw(film, 2.5), 160, 90);
        check(near(p, 0, 0, 0, 4), "after it: the background " + str(p));
        film.clip(cu)->look.opacity = 0.5f;
        p = at(draw(film, 0.5), 160, 90);
        check(near(p, 0, 128, 0, 12), "half opacity over black: half green " + str(p));
        film.clip(cu)->look.opacity = 1.0f;
        film.clip(cu)->fadeIn = 1.0f;
        p = at(draw(film, 0.25), 160, 90);
        check(near(p, 0, 64, 0, 12), "a quarter into a 1 s fade: a quarter green " + str(p));
        film.clip(cu)->fadeIn = 0.0f;
        film.clip(cu)->speed = 2.0;
        film.clip(cu)->in = 0.0;
        p = at(draw(film, 0.75), 160, 90);
        check(near(p, 0, 255, 0), "at 2x speed, 0.75 s is the source's 1.5 s: green " + str(p));
        p = at(draw(film, 1.25), 160, 90);
        check(near(p, 0, 0, 255), "...and 1.25 s its 2.5 s: blue " + str(p));
        film.clip(cu)->speed = 1.0;

        // Two clips overlapping by a second on one track: a dissolve.
        Film x;
        x.width = 320;
        x.height = 180;
        const int t1 = x.addTrack(TrackKind::Video);
        x.addClip(t1, videoClip("colors.mp4", 0.0, 2.0, 0.0));   // red, green
        x.addClip(t1, videoClip("colors.mp4", 1.0, 2.0, 2.0));   // blue, white
        p = at(draw(x, 1.5), 160, 90);
        check(near(p, 0, 128, 128, 16), "half-way through the dissolve: green and blue in equal parts " + str(p));
        p = at(draw(x, 1.02), 160, 90);
        check(near(p, 0, 250, 5, 14), "its start: nearly all the first clip " + str(p));

        // A track over another, moved right by a quarter of the frame.
        Film o;
        o.width = 320;
        o.height = 180;
        const int b = o.addTrack(TrackKind::Video, "V1");
        const int top = o.addTrack(TrackKind::Video, "V2");
        check(o.rowOf(top) < o.rowOf(b), "a new video track goes on top of the pile");
        o.addClip(b, videoClip("colors.mp4", 0.0, 4.0));
        Clip col;
        col.kind = ClipKind::Color;
        col.color = {1, 1, 0, 1};
        col.start = 0;
        col.length = 4;
        col.look.x = 0.25f;
        const int cc = o.addClip(top, col);
        const Image oi = draw(o, 0.5);
        check(near(at(oi, 40, 90), 255, 0, 0), "left of the moved colour: the red underneath " + str(at(oi, 40, 90)));
        check(near(at(oi, 200, 90), 255, 255, 0, 4), "under it: yellow " + str(at(oi, 200, 90)));
        check(near(at(oi, 79, 90), 255, 0, 0) && near(at(oi, 80, 90), 255, 255, 0, 4),
              "its edge sits at a quarter of the width " + str(at(oi, 79, 90)) + str(at(oi, 80, 90)));
        o.track(top)->hidden = true;
        check(near(at(draw(o, 0.5), 200, 90), 255, 0, 0), "the top track hidden: red again");
        o.track(top)->hidden = false;
        o.clip(cc)->look = {};
        o.clip(cc)->look.scale = 0.5f;
        o.clip(cc)->look.rotation = 90.0f;
        const Image ri = draw(o, 0.5);
        // 160x90 turned a quarter: 90 wide, 160 tall, centred.
        check(near(at(ri, 160, 20), 255, 255, 0, 4) && near(at(ri, 100, 90), 255, 0, 0),
              "a half-size colour turned 90 degrees stands upright " + str(at(ri, 160, 20)) + str(at(ri, 100, 90)));

        // A picture: Fit (bars), Fill (cut), turned 180 (halves swapped).
        Film pf;
        pf.width = 320;
        pf.height = 180;
        const int pt = pf.addTrack(TrackKind::Video);
        Clip pic;
        pic.kind = ClipKind::Image;
        pic.file = "pic.png";
        pic.start = 0;
        pic.length = 2;
        const int pu = pf.addClip(pt, pic);
        Image pi = draw(pf, 0.5);
        // 64x32 fitted into 320x180: 320x160, bars of 10 above and below.
        check(near(at(pi, 80, 5), 0, 0, 0, 4) && near(at(pi, 80, 20), 255, 230, 0, 8) && near(at(pi, 240, 90), 0, 60, 255, 8),
              "Fit: yellow left, blue right, bars above and below " + str(at(pi, 80, 5)) + str(at(pi, 80, 20)));
        pf.clip(pu)->fit = Fit::Fill;
        pi = draw(pf, 0.5);
        check(near(at(pi, 80, 3), 255, 230, 0, 8), "Fill: no bars " + str(at(pi, 80, 3)));
        pf.clip(pu)->fit = Fit::Fit;
        pf.clip(pu)->look.rotation = 180.0f;
        pi = draw(pf, 0.5);
        check(near(at(pi, 80, 90), 0, 60, 255, 8) && near(at(pi, 240, 90), 255, 230, 0, 8), "turned 180: the halves swap sides");
        pf.clip(pu)->look.rotation = 0.0f;
        pf.clip(pu)->animate = true;
        pf.clip(pu)->lookEnd = pf.clip(pu)->look;
        pf.clip(pu)->lookEnd.x = 0.5f;
        pi = draw(pf, 1.0);
        // 320 wide, its centre at 240: the bar left of it, yellow to 240, blue after.
        check(near(at(pi, 60, 90), 0, 0, 0, 4) && near(at(pi, 100, 90), 255, 230, 0, 8) && near(at(pi, 250, 90), 0, 60, 255, 8),
              "animated half-way to x 0.5: moved right by a quarter " + str(at(pi, 60, 90)) + str(at(pi, 100, 90)));

        // A title: white words in the middle, nothing at the corners.
        Film tf;
        tf.width = 640;
        tf.height = 360;
        Clip t;
        t.kind = ClipKind::Text;
        t.start = 0;
        t.length = 2;
        t.text.text = "HELLO";
        t.text.size = 0.25f;
        t.text.shadow = false;
        tf.addClip(tf.addTrack(TrackKind::Video), t);
        const Image ti = draw(tf, 0.5);
        int white = 0;
        for (int y = 140; y < 220; ++y)
            for (int x = 200; x < 440; ++x) white += near(at(ti, x, y), 255, 255, 255, 30) ? 1 : 0;
        check(white > 2000, "a title sets white words in the middle (" + std::to_string(white) + " px)");
        check(near(at(ti, 5, 5), 0, 0, 0, 2) && !defaultFont().empty(), "...and leaves the corner alone (font " + defaultFont() + ")");
        const ImagePtr t1img = textImage(t.text, 360);
        t.text.text = "HELLO\nWORLD";
        const ImagePtr t2img = textImage(t.text, 360);
        check(t1img && t2img && t2img->h > t1img->h * 1.6, "two lines are twice as tall as one");
    }

    std::printf("-- sound\n");
    {
        Film film;
        const int a1 = film.addTrack(TrackKind::Audio);
        Clip c;
        c.kind = ClipKind::Audio;
        c.file = "dc.wav";
        c.start = 1.0;
        c.length = 2.0;
        c.volume = 0.5f;
        const int cu = film.addClip(a1, c);
        std::vector<float> pcm = mixdown(film, 0.0, 4.0, lib, true);
        check(std::abs(sampleAt(pcm, 0.5)) < 1e-6f, "before the clip: silence");
        check(std::abs(sampleAt(pcm, 2.0) - 0.125f) < 1e-4f, "0.25 at half volume: 0.125 (" + std::to_string(sampleAt(pcm, 2.0)) + ")");
        check(std::abs(sampleAt(pcm, 3.5)) < 1e-6f, "after it: silence");
        film.clip(cu)->fadeIn = 1.0f;
        pcm = mixdown(film, 0.0, 4.0, lib, true);
        check(std::abs(sampleAt(pcm, 1.5) - 0.0625f) < 1e-3f, "half-way into a fade in: half (" + std::to_string(sampleAt(pcm, 1.5)) + ")");
        film.clip(cu)->fadeIn = 0.0f;
        film.track(a1)->volume = 2.0f;
        pcm = mixdown(film, 0.0, 4.0, lib, true);
        check(std::abs(sampleAt(pcm, 2.0) - 0.25f) < 1e-4f, "the track's volume multiplies");
        film.track(a1)->volume = 1.0f;
        film.track(a1)->muted = true;
        pcm = mixdown(film, 0.0, 4.0, lib, true);
        check(std::abs(sampleAt(pcm, 2.0)) < 1e-6f, "a muted track: silence");
        film.track(a1)->muted = false;
        film.clip(cu)->volume = 1.0f;
        // A second clip overlapping the first by a second: equal power.
        Clip d = c;
        d.uid = 0;
        d.volume = 1.0f;
        d.start = 2.0;
        film.addClip(a1, d);
        pcm = mixdown(film, 0.0, 4.5, lib, true);
        const float mid = sampleAt(pcm, 2.5);
        check(std::abs(mid - 0.25f * 1.41421f) < 2e-3f, "the cross-fade's middle: 0.25 x (sin+cos)(45 deg) = 0.354 (" + std::to_string(mid) + ")");
        check(std::abs(sampleAt(pcm, 2.0) - 0.25f) < 2e-3f && std::abs(sampleAt(pcm, 2.999) - 0.25f) < 3e-3f,
              "the cross-fade's ends: one clip at full level");
        // A slice of the mix is the same samples as the whole.
        const std::vector<float> part = mixdown(film, 2.0, 3.0, lib, true);
        check(std::abs(sampleAt(part, 2.5, 2.0) - mid) < 1e-6f, "a slice of the mixdown matches the whole");

        Film vf;
        const int v1 = vf.addTrack(TrackKind::Video);
        const int vu = vf.addClip(v1, videoClip("colors.mp4", 0.0, 4.0));
        pcm = mixdown(vf, 0.0, 4.0, lib, true);
        const double tone = rmsOver(pcm, 0.5, 3.5);
        // lavfi's sine is 1/8 full scale: an rms of 0.088, mono, on both sides at that.
        check(tone > 0.08 && tone < 0.1, "a video clip's own (mono) sound plays at its level (rms " + std::to_string(tone) + ")");
        vf.clip(vu)->muted = true;
        pcm = mixdown(vf, 0.0, 4.0, lib, true);
        check(rmsOver(pcm, 0.5, 3.5) < 1e-6, "...and not once it is muted");
        vf.clip(vu)->muted = false;
        vf.clip(vu)->speed = 2.0;
        vf.clip(vu)->length = 2.0;
        pcm = mixdown(vf, 0.0, 4.0, lib, true);
        check(rmsOver(pcm, 0.2, 1.8) > 0.08 && rmsOver(pcm, 2.1, 3.9) < 1e-6, "at 2x speed: 4 s of sound in 2");
    }

    std::printf("-- edits and files\n");
    {
        Film film;
        const int v1 = film.addTrack(TrackKind::Video);
        const int u = film.addClip(v1, videoClip("colors.mp4", 0.0, 4.0));
        film.clip(u)->fadeIn = 0.5f;
        film.clip(u)->fadeOut = 0.5f;
        const int r = film.split(u, 1.5);
        check(r > 0 && film.clip(u)->length == 1.5 && film.clip(r)->start == 1.5 && film.clip(r)->in == 1.5,
              "split at 1.5: two clips, the right one 1.5 s into the source");
        check(film.clip(u)->fadeOut == 0.0f && film.clip(r)->fadeIn == 0.0f && film.clip(u)->fadeIn == 0.5f && film.clip(r)->fadeOut == 0.5f,
              "...the fades stay on the outer ends");
        check(film.split(u, 1.49) < 0, "no split a hair from an end");
        film.width = 320;
        film.height = 180;
        const Image a = draw(film, 1.4), b = draw(film, 1.6);
        check(near(at(a, 160, 90), 0, 255, 0) && near(at(b, 160, 90), 0, 255, 0), "the halves join up: green either side of the cut");
        // Ripple: three clips, the middle one out, the last moves up.
        Film rp;
        const int t = rp.addTrack(TrackKind::Video);
        const int c1 = rp.addClip(t, videoClip("x", 0, 1)), c2 = rp.addClip(t, videoClip("x", 1, 2)), c3 = rp.addClip(t, videoClip("x", 3, 1));
        rp.rippleDelete({c2});
        check(!rp.clip(c2) && rp.clip(c3)->start == 1.0 && rp.clip(c1)->start == 0.0, "ripple delete closes the gap");
        const int c4 = rp.addClip(t, videoClip("x", 4, 1));
        rp.rippleDelete({c4});
        check(rp.clip(c3)->start == 1.0, "...and moves nothing when nothing comes after");

        Film j = Film::starter();
        j.name = "Trailer";
        j.fps = 25.0;
        j.markIn = 1.0;
        j.markOut = 3.0;
        j.media = {"colors.mp4", "pic.png"};
        Clip tc;
        tc.kind = ClipKind::Text;
        tc.text.text = "Line one\nLine \"two\"";
        tc.text.align = 2;
        tc.text.outline = 0.1f;
        tc.animate = true;
        tc.lookEnd.scale = 2.0f;
        tc.start = 1.0;
        tc.length = 2.0;
        j.addClip(j.tracks[0].uid, tc);
        Clip ac;
        ac.kind = ClipKind::Audio;
        ac.file = "dc.wav";
        ac.speed = 1.5;
        ac.volume = 0.7f;
        j.addClip(j.tracks[1].uid, ac);
        Film back;
        std::string err;
        check(Film::fromJson(j.toJson(), back, &err), "the film reads back from its JSON " + err);
        check(back.toJson() == j.toJson(), "...unchanged");
        check(back.tracks.size() == 2 && back.tracks[0].clips.size() == 1 && back.tracks[0].clips[0].text.text == tc.text.text &&
                  back.tracks[0].clips[0].animate && back.tracks[1].clips[0].speed == 1.5,
              "...with its title, animation and speed");
        check(!Film::fromJson("{\"format\":\"something\"}", back, &err), "a JSON that is not a film is refused (" + err + ")");
    }

    std::printf("-- export\n");
    {
        Film film;
        film.width = 320;
        film.height = 180;
        film.fps = 30.0;
        const int v1 = film.addTrack(TrackKind::Video);
        film.addClip(v1, videoClip("colors.mp4", 0.0, 2.0, 1.0));   // green, blue
        Clip tail;
        tail.kind = ClipKind::Color;
        tail.color = {1, 0, 1, 1};
        tail.start = 2.0;
        tail.length = 1.0;
        film.addClip(v1, tail);
        const int a1 = film.addTrack(TrackKind::Audio);
        Clip dc;
        dc.kind = ClipKind::Audio;
        dc.file = "dc.wav";
        dc.start = 0.0;
        dc.length = 3.0;
        film.addClip(a1, dc);

        auto exportTo = [&](Format f, const fs::path& out, double from = 0.0, double to = -1.0) {
            Exporter ex;
            ExportSettings s;
            s.format = f;
            s.path = out.string();
            s.width = film.width;
            s.height = film.height;
            s.fps = film.fps;
            s.from = from;
            s.to = to;
            std::string err;
            if (!ex.start(film, s, lib, &err)) {
                std::printf("       %s\n", err.c_str());
                return false;
            }
            while (!ex.finished()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            std::printf("       %s\n", ex.message().c_str());
            return ex.ok() && ex.framesDone() == ex.framesTotal();
        };
        const fs::path mp4 = dir / "out.mp4";
        fs::remove(mp4, ec);
        check(exportTo(Format::Mp4, mp4), "an MP4 export finishes");
        const MediaInfo mi = probe(mp4.string());
        check(mi.ok && mi.video && mi.audio && mi.width == 320 && mi.height == 180, "it has pictures (320x180) and sound");
        check(std::abs(mi.duration - 3.0) < 0.1, "it is 3 s long (" + std::to_string(mi.duration) + ")");
        FrameSource back(mp4.string(), 320, 180, 30.0);
        auto px = [&](std::int64_t i) {
            ImagePtr f = back.get(i, 5000);
            return f ? at(*f, 160, 90) : Px{-1, -1, -1};
        };
        check(near(px(10), 0, 255, 0) && near(px(45), 0, 0, 255) && near(px(75), 255, 0, 255),
              "its frames: green, blue, magenta where they belong " + str(px(10)) + str(px(45)) + str(px(75)));
        Library fresh;
        const auto snd = fresh.soundNow(mp4.string());
        check(snd && snd->ready && snd->frames() > kRate * 2, "its sound decodes");
        if (snd && snd->ready && snd->frames() > kRate * 2) {
            // AAC is not exact: near 0.25 in the middle.
            const float v = snd->pcm[static_cast<std::size_t>(kRate * 1.5) * 2];
            check(std::abs(v - 0.25f) < 0.03f, "...a level of 0.25 (" + std::to_string(v) + ")");
        }

        const fs::path part = dir / "part.mp4";
        check(exportTo(Format::Mp4, part, 1.0, 2.0), "a range export finishes");
        const MediaInfo pm = probe(part.string());
        check(std::abs(pm.duration - 1.0) < 0.1, "the range is 1 s (" + std::to_string(pm.duration) + ")");

        const fs::path fv = dir / "out.fvid";
        check(exportTo(Format::Fvid, fv), "a .fvid export finishes");
        // No GL here for VideoTexture::open to make its texture in: the header
        // is read by hand, field by field as the engine reads it.
        std::ifstream in(fv, std::ios::binary);
        char magic[4] = {};
        std::uint32_t ver = 0, w = 0, h = 0, count = 0;
        float fps = 0;
        in.read(magic, 4);
        in.read(reinterpret_cast<char*>(&ver), 4);
        in.read(reinterpret_cast<char*>(&w), 4);
        in.read(reinterpret_cast<char*>(&h), 4);
        in.read(reinterpret_cast<char*>(&count), 4);
        in.read(reinterpret_cast<char*>(&fps), 4);
        check(std::memcmp(magic, "FVID", 4) == 0 && ver == 1 && w == 320 && h == 180 && count == 90 && fps == 30.0f,
              "the .fvid header: FVID v1, 320x180, 90 frames at 30");

        const fs::path wav = dir / "out.wav";
        check(exportTo(Format::Wav, wav), "a sound-only export finishes");
        fitzel::SampleData sd;
        check(fitzel::loadSampleData(wav.string(), sd) && sd.sampleRate == kRate && std::abs(sd.seconds() - 3.0) < 0.01 &&
                  std::abs(sd.samples[static_cast<std::size_t>(kRate) * 2] - 0.25f) < 1e-3f,
              "the WAV: 3 s at 48 kHz, 0.25");

        // A long film, cancelled at once: no file left behind.
        Film longf = film;
        longf.tracks[0].clips[1].length = 120.0;
        const fs::path big = dir / "cancelled.mp4";
        fs::remove(big, ec);
        Exporter ex;
        ExportSettings s;
        s.path = big.string();
        s.width = 320;
        s.height = 180;
        check(ex.start(longf, s, lib), "a long export starts");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        ex.cancel();
        while (!ex.finished()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        check(!ex.ok() && ex.message().find("cancel") != std::string::npos && !fs::exists(big, ec),
              "cancelled: no file left (" + ex.message() + ")");
    }

    std::printf(g_fails ? "[videoseqcheck] %d FAILED\n" : "[videoseqcheck] all passed\n", g_fails);
    return g_fails ? 1 : 0;
}
