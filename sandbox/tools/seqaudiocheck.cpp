// seqaudiocheck -- does a sequence's sound stay whole when the frames do not?
//
// An audio strip plays on the sound card's clock; the sequence runs on the
// frames. When the two disagree the runner has to choose, and the wrong choice
// is audible at once: a voice pulled back to meet a slow frame repeats a
// fragment, over and over -- the stutter this check was written against. So
// this runs a sequence through a real output device with frames that arrive
// unevenly (16 to 90 ms, a busy editor) and measures what a listener would
// hear: how often a playing voice was reseated, and how far picture and sound
// ended up apart.
//
//   build/release/bin/seqaudiocheck [sound file]
// Needs an audio device; you will hear a few seconds of tone. Exits non-zero
// on a reseat or a skew a viewer would notice.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/audio/Mixer.hpp>

#include "../src/SceneTypes.hpp"
#include "../src/Sequencer.hpp"

namespace fs = std::filesystem;

namespace {

void writeTone(const fs::path& p, float seconds) {
    const int rate = 44100, n = static_cast<int>(seconds * rate);
    std::vector<std::int16_t> pcm(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / rate;
        pcm[static_cast<std::size_t>(i)] =
            static_cast<std::int16_t>(std::sin(2.0f * 3.14159265f * 330.0f * t) * 6000.0f);
    }
    std::ofstream f(p, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + n * 2); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    f.write("data", 4); u32(n * 2);
    f.write(reinterpret_cast<const char*>(pcm.data()), n * 2);
}

} // namespace

int main(int argc, char** argv) {
    fitzel::Audio audio;
    if (!audio.ok()) { std::printf("no audio device -- nothing to measure\n"); return 0; }
    // A file given on the command line is played instead of the tone: the
    // formats decode differently (an mp3 is not a wav), and the one that
    // misbehaves is the one somebody is actually using.
    fs::path wav = fs::temp_directory_path() / "fitzel-seqaudiocheck.wav";
    if (argc > 1) wav = argv[1];
    else          writeTone(wav, 12.0f);

    seq::Sequence s;
    s.duration = 20.0f;
    seq::Track& t = s.addTrack(seq::Kind::Audio);
    seq::Strip st;
    st.uid = s.newUid(); st.start = 0.3f; st.length = 10.0f; st.sound = "tone";
    st.fadeIn = 1.0f; st.fadeOut = 1.0f;
    t.strips.push_back(st);
    std::vector<seq::Sequence> seqs{s};
    std::vector<anim::Clip>    clips;
    std::vector<Entity>        entities;

    seq::Runner r;
    r.bindAudio(&audio, [&](const std::string&) { return wav.generic_string(); },
                [&](const std::string&) { return audio.mixer().master(); });
    int cam = -1;
    r.start(0, cam);

    std::mt19937 rng(7);
    std::uniform_int_distribution<int> frameMs(16, 90);
    auto last = std::chrono::steady_clock::now();
    float worst = 0.0f;
    const auto t0 = last;
    while (std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() < 6.0f) {
        std::this_thread::sleep_for(std::chrono::milliseconds(frameMs(rng)));
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        r.tick(seqs, clips, entities, dt, cam);
        if (r.time > 2.0f) worst = std::max(worst, std::fabs(r.skew()));
        if (std::getenv("SEQAUDIO_TRACE"))
            std::printf("t %.3f dt %.3f skew %+.4f reseats %d\n", r.time, dt, r.skew(), r.reseats());
    }
    r.stop(seqs, cam);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    int fails = 0;
    std::printf("  %s  no playing voice was reseated (%d)\n", r.reseats() == 0 ? "ok  " : "FAIL", r.reseats());
    if (r.reseats() != 0) ++fails;
    std::printf("  %s  picture and sound within 60 ms of each other (worst %.1f ms)\n",
                worst < 0.06f ? "ok  " : "FAIL", worst * 1000.0f);
    if (worst >= 0.06f) ++fails;
    std::printf(fails ? "\n%d check(s) FAILED\n" : "\nsequence audio holds together\n", fails);
    return fails ? 1 : 0;
}
