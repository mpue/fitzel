// vst3check -- the VST3 host (fitzel/audio/Vst3.hpp), with and without a mixer.
//
// Run as it is, it hosts vst3fixture (a gain built next to it, see there) and
// checks what a host gets wrong in practice: that the scan finds the plugin both
// by loading it and from a moduleinfo.json, that the processor and the
// controller found each other through a message the host made, that a block
// longer than the host promised is cut up rather than overrun, that a parameter
// set from the host reaches the audio, that the state survives a trip into a
// second instance and through the desk's JSON, that the mixer's audio thread
// really runs it (when there is a sound device) and stops when it is bypassed,
// and that the editor opens inside an X window, is resized on request, is kept
// alive by the run loop and that a knob turned in it reaches the audio (when
// there is a display). Exit code 0 when every check passed.
//
//   build/linux/bin/vst3check                 the checks above
//   build/linux/bin/vst3check --list          every plugin in the search paths
//   build/linux/bin/vst3check --plugin DL-1 [--editor SECONDS]
//                                             load one, run a tone through it,
//                                             print its parameters, and with
//                                             --editor keep its editor open

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <dlfcn.h>

#include <nlohmann/json.hpp>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/audio/Vst3.hpp>

#include "../src/MixerPanel.hpp"

namespace fs = std::filesystem;
using fitzel::vst3::Instance;
using fitzel::vst3::PluginInfo;

namespace {

int g_failed = 0;

void check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failed;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Two channels, interleaved: a sine on the left, another on the right.
std::vector<float> tone(unsigned frames) {
    std::vector<float> b(frames * 2);
    for (unsigned f = 0; f < frames; ++f) {
        b[f * 2]     = 0.5f * std::sin(static_cast<float>(f) * 0.0573f);
        b[f * 2 + 1] = 0.25f * std::sin(static_cast<float>(f) * 0.0311f);
    }
    return b;
}

// Through the plugin; the largest difference from `scale` times the input.
float runAndCompare(Instance& p, float scale, unsigned frames = 2600) {
    std::vector<float> in = tone(frames), buf = in;
    p.process(buf.data(), frames, 2);
    float worst = 0.0f;
    for (std::size_t i = 0; i < buf.size(); ++i) worst = std::max(worst, std::fabs(buf[i] - in[i] * scale));
    return worst;
}

using StatFn = int (*)(const char*);
StatFn fixtureStats(const PluginInfo& info) {
    const fs::path so = fs::path(info.path) / "Contents" / "x86_64-linux" / (fs::path(info.path).stem().string() + ".so");
    void* h = dlopen(so.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    return h ? reinterpret_cast<StatFn>(dlsym(h, "fitzelFixtureStat")) : nullptr;
}

int list() {
    for (const std::string& p : fitzel::vst3::searchPaths()) std::printf("looking in %s\n", p.c_str());
    const auto& all = fitzel::vst3::plugins();
    for (const PluginInfo& p : all)
        std::printf("%-24s %-20s %-8s %-28s %s%s\n", p.name.c_str(), p.vendor.c_str(), p.version.c_str(),
                    p.category.c_str(), p.path.c_str(), p.instrument ? "  [instrument]" : "");
    std::printf("%zu plugin(s)\n", all.size());
    return 0;
}

int one(const std::string& name, double editorSeconds) {
    const PluginInfo* info = nullptr;
    for (const PluginInfo& p : fitzel::vst3::plugins())
        if (p.name == name) info = &p;
    if (!info) { std::printf("no plugin called %s (try --list)\n", name.c_str()); return 1; }
    std::string err;
    auto p = Instance::create(*info, 48000.0, &err);
    if (!p) { std::printf("%s would not load: %s\n", name.c_str(), err.c_str()); return 1; }
    std::printf("%s %s by %s (%s)\n", info->name.c_str(), info->version.c_str(), info->vendor.c_str(), info->category.c_str());
    for (const auto& pi : p->params())
        if (!pi.hidden && pi.title.rfind("MIDI CC ", 0) != 0)
            std::printf("  [%u] %-28s %-14s%s\n", pi.id, pi.title.c_str(), p->display(pi.id).c_str(),
                        pi.readOnly ? " (read-only)" : "");
    // A second of tone, in the mixer's block sizes, and what came out.
    double inE = 0.0, outE = 0.0;
    for (int blk = 0; blk < 48000 / 480; ++blk) {
        std::vector<float> in = tone(480), buf = in;
        p->process(buf.data(), 480, 2);
        for (std::size_t i = 0; i < buf.size(); ++i) { inE += in[i] * in[i]; outE += buf[i] * buf[i]; }
    }
    std::printf("tone in %.1f dB RMS, out %.1f dB RMS\n", 10.0 * std::log10(inE / 96000.0 + 1e-20),
                10.0 * std::log10(outE / 96000.0 + 1e-20));
    const std::string st = p->state();
    auto q = Instance::create(*info, 48000.0, &err);
    std::printf("state: %zu bytes, into a second instance: %s\n", st.size(), q && q->setState(st) ? "taken" : "REFUSED");
    if (editorSeconds > 0.0) {
        if (!p->openEditor(name + " (vst3check)")) { std::printf("its editor did not open\n"); return 1; }
        std::printf("editor open for %.0f s\n", editorSeconds);
        const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(editorSeconds);
        while (std::chrono::steady_clock::now() < end && p->editorOpen()) {
            fitzel::vst3::idle();
            sleepMs(16);
        }
        p->closeEditor();
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string plugin;
    double editorSeconds = 0.0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") return list();
        if (a == "--plugin" && i + 1 < argc) plugin = argv[++i];
        else if (a == "--editor" && i + 1 < argc) editorSeconds = std::atof(argv[++i]);
    }
    if (!fitzel::vst3::supported()) { std::printf("no VST3 host on this platform\n"); return 0; }
    if (!plugin.empty()) return one(plugin, editorSeconds);

    // --- the scan: by loading, then from a moduleinfo.json --------------------------
    // Only the fixture's folder is looked in (HOME too, but nothing there is
    // named like the fixture).
    const fs::path fixtureDir = FITZEL_VST3_FIXTURE_DIR;
    setenv("VST3_PATH", fixtureDir.c_str(), 1);
    std::printf("scan\n");
    const std::string uid = "5A1E7A1100F17E107E57A11D00000001";
    const PluginInfo* info = fitzel::vst3::find(uid);
    check(info != nullptr, "the fixture is found (no moduleinfo.json: by loading it)");
    if (!info) return 1;
    check(info->name == "Fitzel Test Gain" && info->vendor == "Fitzel" && info->category == "Fx|Tools" && !info->instrument,
          "name, vendor and sub-categories come from its factory");

    // A copy of the bundle with a moduleinfo.json that names it differently: the
    // scan must take the file's word (and not load anything to find out).
    const fs::path miDir = fs::temp_directory_path() / "fitzel-vst3check";
    std::error_code ec;
    fs::remove_all(miDir, ec);
    fs::create_directories(miDir / "MI.vst3" / "Contents" / "Resources", ec);
    {
        std::ofstream f(miDir / "MI.vst3" / "Contents" / "Resources" / "moduleinfo.json");
        f << "{\n  // JSON5, as the SDK writes it: a comment and trailing commas\n"
             "  \"Name\": \"MI\", \"Factory Info\": {\"Vendor\": \"Fitzel\",},\n"
             "  \"Classes\": [ {\"CID\": \"5A1E7A1100F17E107E57A11D00000009\", \"Category\": \"Audio Module Class\",\n"
             "     \"Name\": \"From moduleinfo\", \"Version\": \"2.0\", \"Sub Categories\": [\"Fx\", \"Delay\",],},\n"
             "    {\"CID\": \"5A1E7A1100F17E107E57A11D0000000A\", \"Category\": \"Component Controller Class\", \"Name\": \"x\"},\n"
             "  ],\n}\n";
    }
    setenv("VST3_PATH", (fixtureDir.string() + ":" + miDir.string()).c_str(), 1);
    fitzel::vst3::plugins(true);
    const PluginInfo* mi = fitzel::vst3::find("5A1E7A1100F17E107E57A11D00000009");
    check(mi && mi->name == "From moduleinfo" && mi->category == "Fx|Delay" && mi->vendor == "Fitzel" && mi->version == "2.0",
          "a bundle's moduleinfo.json is read (comments, trailing commas)");
    info = fitzel::vst3::find(uid);   // the rescan replaced the list
    fs::remove_all(miDir, ec);

    // --- an instance ------------------------------------------------------------------
    std::printf("instance\n");
    std::string err;
    std::shared_ptr<Instance> a = Instance::create(*info, 48000.0, &err);
    check(a != nullptr, ("it loads" + (err.empty() ? std::string() : ": " + err)).c_str());
    if (!a) return 1;
    StatFn stat = fixtureStats(*info);
    check(stat != nullptr, "the fixture's counters can be read");
    if (!stat) return 1;
    check(stat("messages") == 1, "processor and controller talked through a message the host made");
    const auto& ps = a->params();
    check(ps.size() == 2 && ps[0].title == "Gain" && ps[0].units == "dB" && ps[1].steps == 1,
          "its two parameters, with titles, units and steps");
    check(a->display(0) == "0.00", "the plugin prints its own value (0.5 = 0.00 dB)");
    check(runAndCompare(*a, 1.0f) < 1e-6f, "unity: what goes in comes out (2600 frames, cut into blocks)");
    a->setParam(0, 0.25);
    check(runAndCompare(*a, 0.5f) < 1e-6f, "a parameter set by the host reaches the audio (-6 dB)");
    check(std::fabs(a->param(0) - 0.25) < 1e-9, "and the controller has it");
    a->setParam(1, 1.0);
    check(runAndCompare(*a, -0.5f) < 1e-6f, "a stepped one too (polarity)");

    // --- state ------------------------------------------------------------------------
    std::printf("state\n");
    const std::string st = a->state();
    std::shared_ptr<Instance> b = Instance::create(*info, 44100.0, &err);
    check(b && b->setState(st), "the state goes into a second instance");
    if (!b) return 1;
    check(runAndCompare(*b, -0.5f) < 1e-6f, "and it sounds the same");
    check(std::fabs(b->param(0) - 0.25) < 1e-9 && b->param(1) > 0.5, "its controller was given the processor's half");
    check(!b->setState("garbage"), "bytes that are not a state are refused");

    // The desk saves it as base64 with the scene and gives it back.
    {
        mixerui::Desk desk;
        mixerui::Insert in = mixerui::makePluginInsert(uid, info->name);
        in.plugin = b;
        desk.strips[0].inserts.push_back(in);
        const nlohmann::json j = desk.toJson();
        mixerui::Desk back;
        back.fromJson(nlohmann::json::parse(j.dump()));
        const auto& ins = back.strips[0].inserts;
        check(ins.size() == 1 && ins[0].type == fitzel::mixfx::Type::Plugin && ins[0].pluginUid == uid &&
              ins[0].pluginName == "Fitzel Test Gain" && ins[0].pluginState == st,
              "a plugin insert survives the desk's JSON, state and all");
        mixerui::Insert lost = mixerui::makePluginInsert("00000000000000000000000000000000", "Gone");
        lost.pluginTried = true;
        check(lost.label() == "Gone (missing)", "one that is not installed says so on its slot");
    }

    // --- in the mixer -------------------------------------------------------------------
    std::printf("mixer\n");
    {
        fitzel::Audio audio;
        if (!audio.ok()) {
            std::printf("  --    no sound device: the mixer's audio thread is not checked\n");
        } else {
            mixerui::Desk desk;
            desk.strips[0].inserts.push_back(mixerui::makePluginInsert(uid, info->name));
            desk.sync(audio.mixer());
            const mixerui::Insert& in = desk.strips[0].inserts[0];
            check(in.plugin != nullptr, "Desk::sync starts the plugin insert");
            auto during = [&](int ms) {
                const int before = stat("processed");
                for (int t = 0; t < ms; t += 20) { desk.sync(audio.mixer()); sleepMs(20); }
                return stat("processed") - before;
            };
            check(during(400) > 4, "the audio thread runs it");
            desk.strips[0].inserts[0].bypass = true;
            desk.sync(audio.mixer());
            during(100);   // a block already under way may still finish
            check(during(300) == 0, "and stops when it is bypassed");
            desk.strips[0].inserts.clear();
            desk.sync(audio.mixer());
        }
    }

    // --- the editor ---------------------------------------------------------------------
    std::printf("editor\n");
    if (!std::getenv("DISPLAY")) {
        std::printf("  --    no DISPLAY: the editor is not checked\n");
    } else {
        const bool opened = a->openEditor("vst3check");
        check(opened && a->editorOpen() && stat("attached") == 1, "it opens, attached to an X window");
        for (int i = 0; i < 30; ++i) { fitzel::vst3::idle(); sleepMs(10); }
        check(stat("onSize") >= 1, "its request to grow was followed (onSize after resizeView)");
        check(stat("timer") > 3, "its timer runs on the host's run loop");
        check(stat("fd") > 0, "its X connection is watched by the host's run loop");
        check(stat("edits") == 1 && std::fabs(a->param(0) - 0.75) < 1e-9,
              "a knob turned in it reaches the host");
        check(runAndCompare(*a, -1.5f) < 1e-6f, "and the audio (+3.5 dB, still inverted)");
        a->closeEditor();
        check(!a->editorOpen() && stat("removed") == 1, "it closes");
    }

    // --- going away ---------------------------------------------------------------------
    const int terminatedBefore = stat("terminated");
    a.reset();
    b.reset();
    check(stat("terminated") - terminatedBefore == 4, "both halves of both instances are terminated");

    std::printf(g_failed ? "%d check(s) FAILED\n" : "all checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
