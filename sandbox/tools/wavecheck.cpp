// wavecheck -- does the wave editor do to a sound what it says?
//
//   1. Files: a WAV written at 16, 24 and 32-bit float comes back as written,
//      loop points included (the smpl chunk), and other tools' readers find
//      the loop too (readWavLoop).
//   2. Every edit: lengths, the samples it touches and the ones it must not,
//      loop points following the material they marked, undo/redo restoring the
//      sound exactly.
//   3. Loops: the zero-crossing snap, the best-end search on a periodic sound,
//      and the crossfade leaving no seam at the wrap.
//   4. Analysis: peak, RMS and DC of a known signal; the spectrum's peak at the
//      sine's frequency.
//   5. The preview player (when there is a sound device): it plays, follows the
//      loop round, and stops.
//   6. The window (with --panel out.png): drawn through the editor's own Gui
//      with a sound loaded, a selection and a loop; a picture of it.
//
//   wavecheck [--panel out.png] [--keep dir]

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/audio/SamplePlayer.hpp>

#include "../src/WaveDoc.hpp"

#ifdef WAVECHECK_PANEL
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <fitzel/core/Window.hpp>
#include <fitzel/rhi/Device.hpp>
#include <fitzel/ui/Gui.hpp>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#include "../src/UiStyle.hpp"
#include "../src/WaveEditPanel.hpp"
#endif

namespace {

int g_fail = 0, g_pass = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    if (ok) { ++g_pass; std::printf("ok    %s\n", what.c_str()); return; }
    ++g_fail;
    std::printf("FAIL  %s%s%s\n", what.c_str(), detail.empty() ? "" : "  -- ", detail.c_str());
}
std::string f3(double v) { char b[64]; std::snprintf(b, sizeof b, "%.4f", v); return b; }

constexpr double kPi = 3.14159265358979323846;

// A stereo sine (left) and its octave (right), `seconds` long.
void makeSine(wave::Document& d, int rate, double seconds, double hz, float amp) {
    d.newSilence(2, rate, seconds);
    for (wave::Frames f = 0; f < d.frames(); ++f) {
        const double t = static_cast<double>(f) / rate;
        d.data.samples[static_cast<std::size_t>(2 * f)]     = amp * static_cast<float>(std::sin(2 * kPi * hz * t));
        d.data.samples[static_cast<std::size_t>(2 * f + 1)] = amp * static_cast<float>(std::sin(4 * kPi * hz * t));
    }
    d.touched();
}

float maxDiff(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.size() != b.size()) return 1e9f;
    float m = 0.0f;
    for (std::size_t i = 0; i < a.size(); ++i) m = std::max(m, std::abs(a[i] - b[i]));
    return m;
}

} // namespace

int main(int argc, char** argv) {
    std::string panelOut, keep;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--panel" && i + 1 < argc) panelOut = argv[++i];
        else if (a == "--keep" && i + 1 < argc) keep = argv[++i];
    }
    const std::filesystem::path tmp =
        keep.empty() ? std::filesystem::temp_directory_path() / "wavecheck" : std::filesystem::path(keep);
    std::filesystem::create_directories(tmp);

    // --- 1. Files ------------------------------------------------------------
    {
        wave::Document d;
        makeSine(d, 44100, 0.5, 440.0, 0.5f);
        d.data.loopStart = 1000;
        d.data.loopEnd   = 20000;
        struct Fmt { fitzel::WavFormat f; const char* name; float tol; int bits; };
        for (const Fmt& f : {Fmt{fitzel::WavFormat::Pcm16, "16-bit", 1.0f / 16000.0f, 16},
                             Fmt{fitzel::WavFormat::Pcm24, "24-bit", 1.0f / 4000000.0f, 24},
                             Fmt{fitzel::WavFormat::Float32, "32-bit float", 0.0f, 32}}) {
            const std::string p = (tmp / (std::string("sine_") + std::to_string(f.bits) + ".wav")).string();
            std::string err;
            check(d.save(p, f.f, &err), std::string(f.name) + ": saved", err);
            wave::Document b;
            check(b.load(p, &err), std::string(f.name) + ": loads back", err);
            check(b.channels() == 2 && b.rate() == 44100 && b.frames() == d.frames(),
                  std::string(f.name) + ": same shape", std::to_string(b.frames()));
            const float md = maxDiff(b.data.samples, d.data.samples);
            check(md <= f.tol + 1e-7f, std::string(f.name) + ": same samples", f3(md));
            check(b.data.loopStart == 1000 && b.data.loopEnd == 20000,
                  std::string(f.name) + ": loop points survive",
                  std::to_string(b.data.loopStart) + ".." + std::to_string(b.data.loopEnd));
            check(b.data.sourceBits == f.bits && b.data.sourceFloat == (f.f == fitzel::WavFormat::Float32),
                  std::string(f.name) + ": knows what it was written as");
            std::int64_t s = 0, e = 0;
            check(fitzel::readWavLoop(p, s, e) && s == 1000 && e == 20000,
                  std::string(f.name) + ": readWavLoop finds the loop (what a looping Sound uses)");
        }
        wave::Document n;
        n.newSilence(1, 48000, 0.1);
        const std::string p = (tmp / "noloop.wav").string();
        n.save(p, fitzel::WavFormat::Pcm16, nullptr);
        std::int64_t s = 0, e = 0;
        check(!fitzel::readWavLoop(p, s, e), "a WAV without loop points has none");
        wave::Document m;
        std::string err;
        check(!m.load((tmp / "missing.wav").string(), &err) && !err.empty(),
              "a missing file is an error, with a reason");
    }

    // --- 2. Edits ---------------------------------------------------------------
    {
        wave::Document d;
        makeSine(d, 48000, 1.0, 100.0, 0.5f);
        d.data.loopStart = 20000;
        d.data.loopEnd   = 30000;
        const auto orig = d.data.samples;
        const wave::Frames n = d.frames();

        d.checkpoint("Delete");
        wave::erase(d, 1000, 3000);
        check(d.frames() == n - 2000, "delete: 2000 frames shorter");
        check(d.data.loopStart == 18000 && d.data.loopEnd == 28000, "delete before the loop moves it");
        check(d.sample(1000, 0) == orig[2 * 3000], "delete: what followed closes up");
        d.checkpoint("Delete through");
        wave::erase(d, 27000, 29000);
        check(d.data.loopStart == 18000 && d.data.loopEnd == 27000, "delete through the loop end shortens it");
        d.undo();
        d.undo();
        check(d.data.samples == orig && d.data.loopStart == 20000 && d.data.loopEnd == 30000,
              "two undos restore the sound and the loop exactly");
        d.redo();
        check(d.frames() == n - 2000 && d.canRedo(), "redo does it again (one left to redo)");
        d.undo();

        const wave::Clip c = wave::copy(d, 100, 600);
        check(c.frames() == 500 && c.channels == 2, "copy: 500 frames, both channels");
        d.checkpoint("Paste");
        wave::insert(d, 10000, c);
        check(d.frames() == n + 500 && d.sample(10000, 0) == orig[200] && d.sample(10499, 1) == orig[2 * 599 + 1],
              "paste: inserted at the cursor, sample for sample");
        check(d.data.loopStart == 20500, "paste before the loop moves it");
        d.undo();

        wave::Clip mono;
        mono.channels = 1;
        mono.rate = 24000;
        mono.samples.assign(100, 0.25f);
        d.checkpoint("Paste mono");
        wave::insert(d, 0, mono);
        check(d.frames() == n + 200 && std::abs(d.sample(100, 1) - 0.25f) < 1e-3f,
              "paste: a mono clip at half the rate comes in stereo, at the document's rate",
              std::to_string(d.frames() - n));
        d.undo();

        d.checkpoint("Crop");
        wave::crop(d, 15000, 35000);
        check(d.frames() == 20000 && d.data.loopStart == 5000 && d.data.loopEnd == 15000,
              "crop keeps the selection and the loop inside it");
        d.undo();
        d.checkpoint("Crop away");
        wave::crop(d, 0, 10000);
        check(!d.data.hasLoop(), "crop that leaves the loop out takes it away");
        d.undo();

        d.checkpoint("Silence");
        wave::silence(d, 100, 200, 1u);
        check(d.sample(150, 0) == 0.0f && d.sample(150, 1) == orig[2 * 150 + 1],
              "silence on the left channel only leaves the right");
        d.undo();

        d.checkpoint("Gain");
        wave::gain(d, 0, n, 6.0206f, wave::kAllChannels);
        wave::gain(d, 0, n, -6.0206f, wave::kAllChannels);
        check(maxDiff(d.data.samples, orig) < 1e-5f, "+6 dB then -6 dB is where it started");
        d.undo();

        d.checkpoint("Normalize");
        const float applied = wave::normalize(d, 0, n, -1.0f, wave::kAllChannels);
        const wave::Stats st = wave::stats(d, 0, n);
        check(std::abs(std::max(st.peakDb[0], st.peakDb[1]) + 1.0f) < 0.01f && std::abs(applied - 5.02f) < 0.05f,
              "normalize to -1 dB: peak at -1 dB", f3(st.peakDb[0]) + " / " + f3(applied));
        d.undo();

        d.checkpoint("Fade in");
        wave::fade(d, 0, 4800, true, wave::FadeCurve::Linear, wave::kAllChannels);
        check(d.sample(0, 0) == 0.0f && std::abs(d.sample(2400, 0) - orig[2 * 2400] * 0.5f) < 1e-3f &&
                  d.sample(4799, 0) == orig[2 * 4799],
              "fade in: silent at the start, half way at the middle, whole at the end");
        d.undo();
        for (int k = 0; k < static_cast<int>(wave::FadeCurve::Count); ++k) {
            d.checkpoint("Fade out");
            wave::fade(d, 0, 4800, false, static_cast<wave::FadeCurve>(k), wave::kAllChannels);
            check(d.sample(0, 1) == orig[1] && d.sample(4799, 1) == 0.0f,
                  std::string("fade out (") + wave::fadeCurveName(static_cast<wave::FadeCurve>(k)) +
                      "): whole, then silent");
            d.undo();
        }

        d.checkpoint("Reverse");
        wave::reverse(d, 0, n, wave::kAllChannels);
        check(d.sample(0, 0) == orig[2 * (n - 1)], "reverse: the last frame first");
        wave::reverse(d, 0, n, wave::kAllChannels);
        check(d.data.samples == orig, "reverse twice is the original");
        d.undo();

        d.checkpoint("Invert");
        wave::invert(d, 0, n, 2u);
        check(d.sample(10, 1) == -orig[21] && d.sample(10, 0) == orig[20], "invert the right channel only");
        d.undo();

        d.checkpoint("DC");
        for (auto& v : d.data.samples) v += 0.1f;
        d.touched();
        wave::removeDc(d, 0, n, wave::kAllChannels);
        check(std::abs(wave::stats(d, 0, n).dc[0]) < 1e-4f, "remove DC: the mean is zero again");
        d.undo();

        d.checkpoint("Mono");
        wave::toMono(d);
        check(d.channels() == 1 && std::abs(d.sample(5, 0) - 0.5f * (orig[10] + orig[11])) < 1e-6f,
              "to mono: one channel, the mean of both");
        wave::toStereo(d);
        check(d.channels() == 2 && d.sample(5, 0) == d.sample(5, 1), "to stereo: both channels the same");
        d.undo();
        check(d.channels() == 2 && d.data.samples == orig, "undo brings the stereo back");

        d.checkpoint("Resample");
        wave::resample(d, 24000);
        check(d.rate() == 24000 && std::abs(d.frames() - n / 2) <= 1 && d.data.loopStart == 10000,
              "resample to 24 kHz: half the frames, the loop with it");
        int zc = 0;
        for (wave::Frames f = 1; f < d.frames(); ++f)
            if ((d.sample(f - 1, 0) < 0.0f) != (d.sample(f, 0) < 0.0f)) ++zc;
        check(std::abs(zc - 200) <= 2, "...and a 100 Hz sine is still 100 Hz", std::to_string(zc) + " crossings");
        d.undo();

        d.checkpoint("Trim");
        wave::insertSilence(d, 0, 3000);
        wave::insertSilence(d, d.frames(), 2000);
        check(d.frames() == n + 5000, "insert silence at both ends");
        const wave::Frames cut = wave::trimSilence(d, -60.0f);
        check(cut >= 3000 && cut <= 3001 && d.frames() <= n && d.frames() >= n - 2,
              "trim silence takes it off again", std::to_string(cut) + " / " + std::to_string(d.frames()));
        d.undo();
        check(d.data.samples == orig, "undo of a run of edits is one step back to the original");
    }

    // --- 3. Loops ---------------------------------------------------------------
    {
        wave::Document d;
        makeSine(d, 48000, 1.0, 100.0, 0.5f);   // period 480 frames
        const wave::Frames zc = wave::nearestZeroCrossing(d, 1000);
        check(std::abs(d.sample(zc, 0)) < 0.02f, "zero-crossing snap lands where the wave crosses",
              std::to_string(zc));
        float score = 0.0f;
        const wave::Frames end = wave::bestLoopEnd(d, 2400, 20000, 30000, 2048, &score);
        check(end >= 0 && (end - 2400) % 480 == 0 && score > 0.99f,
              "best loop end on a 100 Hz sine is a whole number of periods",
              std::to_string(end) + " score " + f3(score));
        d.data.loopStart = 2400;
        d.data.loopEnd   = 25000;   // NOT a whole period: a seam
        auto jump = [&] {
            return std::abs(d.sample(d.data.loopEnd - 1, 0) - d.sample(d.data.loopStart - 1, 0));
        };
        const float before = jump();
        check(wave::crossfadeLoop(d, 2000), "crossfade the loop");
        check(jump() < 1e-5f && before > 0.1f, "after the crossfade the wrap continues the wave",
              f3(before) + " -> " + f3(jump()));
        d.data.loopStart = 100;
        check(!wave::crossfadeLoop(d, 2000), "no crossfade without room before the loop start");
    }

    // --- 4. Analysis ------------------------------------------------------------
    {
        wave::Document d;
        makeSine(d, 48000, 1.0, 1000.0, 0.5f);
        const wave::Stats s = wave::stats(d, 0, d.frames());
        check(std::abs(s.peakDb[0] + 6.02f) < 0.05f && std::abs(s.rmsDb[0] + 9.03f) < 0.05f &&
                  std::abs(s.dc[0]) < 1e-3f && s.clipped == 0,
              "stats of a half-scale sine: -6 dB peak, -9 dB RMS, no DC",
              f3(s.peakDb[0]) + " " + f3(s.rmsDb[0]));
        wave::Document l;
        l.newSilence(1, 48000, 1.0);
        for (wave::Frames f = 0; f < l.frames(); ++f)
            l.data.samples[static_cast<std::size_t>(f)] = 0.5f * static_cast<float>(std::sin(2 * kPi * 1000.0 * f / 48000.0));
        l.touched();
        const auto sp = wave::spectrum(l, 0, l.frames(), 64);
        int top = 0;
        for (int i = 1; i < 64; ++i) if (sp[i] > sp[top]) top = i;
        const double f0 = 20.0 * std::pow(24000.0 / 20.0, top / 64.0), f1 = 20.0 * std::pow(24000.0 / 20.0, (top + 1) / 64.0);
        check(f0 <= 1000.0 && f1 >= 1000.0 && std::abs(sp[top] + 6.0f) < 1.5f,
              "the spectrum peaks at 1 kHz, at the sine's level", f3(f0) + "-" + f3(f1) + " Hz, " + f3(sp[top]) + " dB");
    }

    // --- 5. The preview player ----------------------------------------------------
    {
        fitzel::Audio audio;
        if (!audio.ok()) {
            std::printf("skip  no sound device: the preview player is not tested\n");
        } else {
            wave::Document d;
            makeSine(d, 48000, 0.5, 220.0, 0.05f);
            fitzel::SamplePlayer p = fitzel::SamplePlayer::create(audio, 2, 48000);
            check(p.isValid(), "player: created");
            p.setSamples(d.data.samples);
            p.setLoop(true, 6000, 12000);
            p.play(0);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const auto pos = p.position();
            check(p.isPlaying() && pos >= 6000 && pos <= 12000,
                  "player: past 0.4 s it is going round the loop (0.125-0.25 s)", std::to_string(pos));
            float l = 0, r = 0;
            p.takePeak(l, r);
            check(l > 0.03f && r > 0.03f, "player: the meter sees it", f3(l));
            p.stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            check(!p.isPlaying(), "player: stops");
            p.setLoop(false, 0, 0);
            p.play(20000, 22400);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            check(!p.isPlaying(), "player: a range plays once and ends");
            // A new buffer while playing, and the old one freed: no crash.
            p.play(0);
            for (int i = 0; i < 20; ++i) {
                p.setSamples(d.data.samples);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            p.stop();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            check(true, "player: buffers swapped while playing");

            // The game's side: a looping Sound made from a WAV with loop points
            // plays the start once, then only the loop.
            wave::Document g;
            makeSine(g, 48000, 0.4, 220.0, 0.02f);
            g.data.loopStart = 9600;    // 0.2 s
            g.data.loopEnd   = 14400;   // 0.3 s
            const std::string gp = (tmp / "game_loop.wav").string();
            g.save(gp, fitzel::WavFormat::Pcm16, nullptr);
            fitzel::Sound snd = fitzel::Sound::fromFile(audio, gp, true);
            check(snd.isValid(), "game sound: loads");
            snd.play();
            float lo = 1e9f, hi = -1e9f;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));   // past the file's 0.4 s
            for (int i = 0; i < 40; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const float c = snd.cursorSeconds();
                lo = std::min(lo, c); hi = std::max(hi, c);
            }
            check(snd.isPlaying() && lo >= 0.19f && hi <= 0.31f,
                  "game sound: after the start it stays in the loop points (0.2-0.3 s)",
                  f3(lo) + " .. " + f3(hi));
            snd.stop();
        }
    }

#ifdef WAVECHECK_PANEL
    if (!panelOut.empty()) {
        fitzel::Window window(fitzel::WindowConfig{
            .width = 1400, .height = 860, .title = "wavecheck", .vsync = false, .maximized = false});
        fitzel::Gui gui(window);
        fitzel::rhi::Device& gpu = window.device();
        ui::setBoldFont(gui.boldFont());
        ImGui::GetIO().IniFilename = nullptr;
        std::string status, project;
        waveui::WaveEditor ed({project, status, nullptr});
        // A sound with character: a decaying chord, a click, and silence.
        wave::Document& d = ed.newDocument(2, 48000, 3.0);
        for (wave::Frames f = 0; f < d.frames(); ++f) {
            const double t = static_cast<double>(f) / 48000.0;
            const double env = std::exp(-1.5 * t) * (t < 2.4 ? 1.0 : 0.0);
            const double v = env * (0.4 * std::sin(2 * kPi * 220 * t) + 0.25 * std::sin(2 * kPi * 330 * t) +
                                    0.15 * std::sin(2 * kPi * 440 * t * (1.0 + 0.002 * std::sin(6 * t))));
            d.data.samples[static_cast<std::size_t>(2 * f)]     = static_cast<float>(v);
            d.data.samples[static_cast<std::size_t>(2 * f + 1)] = static_cast<float>(v * (0.8 + 0.2 * std::sin(3 * t)));
        }
        d.data.loopStart = 48000;
        d.data.loopEnd   = 96000;
        d.touched();
        ed.setSelection(30000, 60000);
        ImGuiIO& io = ImGui::GetIO();
        ImVec2 mouse(5.0f, 5.0f);
        bool mouseOurs = false;
        auto frame = [&] {
            // Injected input: the position is sent again every frame, or the
            // GLFW backend puts the real cursor back.
            if (mouseOurs) io.AddMousePosEvent(mouse.x, mouse.y);
            window.pollEvents();
            int fw = 0, fh = 0;
            window.framebufferSize(fw, fh);
            gpu.setViewport(0, 0, fw, fh);
            gpu.setClearColor(glm::vec4(0.10f, 0.11f, 0.13f, 1.0f));
            gpu.clear(fitzel::rhi::ClearColor);
            gui.beginFrame();
            // The panel sets its own first size; take the whole window instead.
            ImGui::SetWindowPos("Wave editor", ImVec2(0, 0));
            ImGui::SetWindowSize("Wave editor", ImVec2(static_cast<float>(fw), static_cast<float>(fh)));
            bool show = true;
            ed.panel(show);
            gui.endFrame();
            window.swapBuffers();
        };
        for (int i = 0; i < 8; ++i) frame();
        int fw = 0, fh = 0;
        window.framebufferSize(fw, fh);
        std::vector<unsigned char> px(static_cast<std::size_t>(fw) * fh * 4);
        gpu.readPixels({}, 0, fitzel::rhi::Rect{0, 0, fw, fh}, fitzel::rhi::Format::RGBA8, px.data());
        stbi_flip_vertically_on_write(1);
        check(stbi_write_png(panelOut.c_str(), fw, fh, 4, px.data(), fw * 4) != 0, "panel: picture written");

        // --- By hand: the mouse and the keys, as a person would.
        // The real mouse is unplugged: a hand on the desk moving over this
        // window would otherwise put its own positions between ours.
        ImGui_ImplGlfw_CursorEnterCallback(window.nativeHandle(), 1);
        glfwSetCursorPosCallback(window.nativeHandle(), nullptr);
        glfwSetMouseButtonCallback(window.nativeHandle(), nullptr);
        glfwSetScrollCallback(window.nativeHandle(), nullptr);
        glfwSetCursorEnterCallback(window.nativeHandle(), nullptr);
        mouseOurs = true;
        ed.setSnap(false);
        const double fpp = ed.framesPerPixel();
        const wave::Frames slack = static_cast<wave::Frames>(fpp * 1.5) + 1;
        auto near = [&](wave::Frames a, wave::Frames b) { return std::abs(a - b) <= slack; };
        auto at = [&](wave::Frames f, float lane = 0.5f) { mouse = ed.toScreen(f, lane); frame(); };
        auto down = [&] { io.AddMouseButtonEvent(0, true); frame(); };
        auto up = [&] { io.AddMouseButtonEvent(0, false); frame(); };
        auto click = [&](wave::Frames f) { at(f); down(); up(); frame(); frame(); };
        auto key = [&](ImGuiKey k, bool ctrl = false) {
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, true);
            io.AddKeyEvent(k, true); frame();
            io.AddKeyEvent(k, false);
            if (ctrl) io.AddKeyEvent(ImGuiMod_Ctrl, false);
            frame();
        };
        wave::Frames a = 0, b = 0;

        click(24000);
        ed.selection(a, b);
        check(near(ed.cursorFrame(), 24000) && a == b, "a click sets the cursor (no selection)",
              std::to_string(ed.cursorFrame()));

        io.AddKeyEvent(ImGuiMod_Shift, true);
        io.AddKeyEvent(ImGuiKey_LeftShift, true);
        click(48000);   // on the loop start marker: Shift still means the selection
        io.AddKeyEvent(ImGuiKey_LeftShift, false);
        io.AddKeyEvent(ImGuiMod_Shift, false);
        frame();
        ed.selection(a, b);
        check(near(a, 24000) && near(b, 48000), "Shift+click selects from the cursor to there",
              std::to_string(a) + ".." + std::to_string(b));

        at(60000); down();
        for (int i = 1; i <= 6; ++i) at(60000 + 5000 * i);
        up();
        ed.selection(a, b);
        check(near(a, 60000) && near(b, 90000), "a drag selects too", std::to_string(a) + ".." + std::to_string(b));

        const wave::Frames n0 = d.frames();
        key(ImGuiKey_Delete);
        check(d.frames() == n0 - (b - a), "Del deletes the selection", std::to_string(n0 - d.frames()));
        key(ImGuiKey_Z, true);
        check(d.frames() == n0, "Ctrl+Z brings it back");

        // The loop end marker, dragged.
        at(96000); down();
        for (int i = 1; i <= 4; ++i) at(96000 + 3500 * i);
        up();
        check(near(d.data.loopEnd, 110000) && d.data.loopStart == 48000,
              "the loop end marker drags along",
              std::to_string(d.data.loopStart) + ".." + std::to_string(d.data.loopEnd));
        key(ImGuiKey_Z, true);
        check(d.data.loopEnd == 96000, "...and one undo puts it back", std::to_string(d.data.loopEnd));

        // Zoom with the wheel: the frame under the mouse stays under it.
        at(72000);
        const float x0 = mouse.x;
        io.AddMouseWheelEvent(0.0f, 2.0f);
        frame(); frame();
        check(ed.framesPerPixel() < fpp * 0.7 && std::abs(ed.toScreen(72000, 0.5f).x - x0) < 2.0f,
              "the wheel zooms in round the mouse");
        key(ImGuiKey_F);
        check(std::abs(ed.framesPerPixel() - fpp) < fpp * 0.01, "F shows the whole sound again");

        // A double-click selects everything.
        at(30000); down(); up(); down(); up(); frame();
        ed.selection(a, b);
        check(a == 0 && b == d.frames(), "a double-click selects it all");

        // Snap on: a click lands on a zero crossing.
        ed.setSnap(true);
        click(30017);
        const float z = std::abs(d.sample(ed.cursorFrame(), 0)) + std::abs(d.sample(ed.cursorFrame(), 1));
        check(z < 0.05f, "with Snap a click lands on a zero crossing", f3(z));

        // Space without a sound device says so rather than doing nothing.
        status.clear();
        key(ImGuiKey_Space);
        check(status.find("No sound device") != std::string::npos, "Space without a device explains itself",
              status);
    }
#else
    (void)panelOut;
#endif

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
