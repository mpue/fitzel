// mixerspeedcheck -- does a sound play at its own speed through the mixer?
//
// A channel strip is one node with an output per aux bus. miniaudio reads a node
// once per period only while every one of its outputs asks for the same frames
// at the same time; read one output in pieces and then another, and the node
// runs again for the second -- and everything feeding it (a song, a loop) moves
// on that much faster. This plays a tone into a channel with 0, 1 and 2 aux
// buses and compares how far the sound got with how much time went by.
//   build/linux/bin/mixerspeedcheck
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <fitzel/audio/Audio.hpp>
#include <fitzel/audio/Mixer.hpp>
#include <fitzel/audio/MusicPlayer.hpp>

int main() {
    const std::string wav = (std::filesystem::temp_directory_path() / "mixerspeedcheck.wav").string();
    {
        const int rate = 48000, frames = rate * 10;
        std::vector<short> pcm(static_cast<std::size_t>(frames) * 2);
        for (int i = 0; i < frames; ++i)
            pcm[i * 2] = pcm[i * 2 + 1] = static_cast<short>(std::sin(i * 2.0 * 3.14159265 * 330.0 / rate) * 3000);
        FILE* f = std::fopen(wav.c_str(), "wb");
        const int dataBytes = frames * 4, byteRate = rate * 4, fmtLen = 16, riffLen = 36 + dataBytes;
        const short fmtTag = 1, ch = 2, align = 4, bits = 16;
        std::fwrite("RIFF", 1, 4, f); std::fwrite(&riffLen, 4, 1, f);
        std::fwrite("WAVEfmt ", 1, 8, f); std::fwrite(&fmtLen, 4, 1, f);
        std::fwrite(&fmtTag, 2, 1, f); std::fwrite(&ch, 2, 1, f);
        std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f);
        std::fwrite(&align, 2, 1, f); std::fwrite(&bits, 2, 1, f);
        std::fwrite("data", 1, 4, f); std::fwrite(&dataBytes, 4, 1, f);
        std::fwrite(pcm.data(), 2, pcm.size(), f);
        std::fclose(f);
    }
    fitzel::Audio audio;
    if (!audio.ok()) { std::printf("[mixerspeedcheck] no audio device\n"); return 2; }
    int bad = 0;
    for (int auxes : {0, 1, 2}) {
        fitzel::Mixer& m = audio.mixer();
        std::vector<int> made;
        for (int k = 0; k < auxes; ++k) {
            const int a = m.addStrip(fitzel::Mixer::Kind::Aux);
            m.setInserts(a, {fitzel::mixfx::Type::Reverb});
            made.push_back(a);
        }
        const int ch = m.addStrip(fitzel::Mixer::Kind::Channel);
        for (int a : made) m.setSend(ch, a, 0.3f);
        made.push_back(ch);
        fitzel::Sound s = fitzel::Sound::fromFile(audio, wav, false);
        s.setOutput(m, ch);
        s.play();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const float c0 = s.cursorSeconds();
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        const float c1 = s.cursorSeconds();
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double speed = (c1 - c0) / dt;
        const bool ok = std::abs(speed - 1.0) < 0.05;
        std::printf("[mixerspeedcheck] %d aux bus%s: the sound ran %.2fx its speed  %s\n", auxes, auxes == 1 ? "" : "es",
                    speed, ok ? "ok" : "FAIL");
        if (!ok) ++bad;
        s.stop();
        for (int id : made) m.removeStrip(id);
    }
    // The same for a song: MusicPlayer decodes on the audio thread itself.
    for (int auxes : {0, 1, 2}) {
        fitzel::Mixer& m = audio.mixer();
        std::vector<int> made;
        for (int k = 0; k < auxes; ++k) {
            const int a = m.addStrip(fitzel::Mixer::Kind::Aux);
            m.setInserts(a, {fitzel::mixfx::Type::Reverb});
            made.push_back(a);
        }
        const int ch = m.addStrip(fitzel::Mixer::Kind::Channel);
        for (int a : made) m.setSend(ch, a, 0.03f);
        made.push_back(ch);
        std::string err;
        fitzel::MusicPlayer song = fitzel::MusicPlayer::create(audio, &err);
        song.setOutput(m, ch);
        if (!song.load(wav, &err)) { std::printf("[mixerspeedcheck] song: %s\n", err.c_str()); ++bad; break; }
        song.play(0.0);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const double c0 = song.time();
        const auto t0 = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
        const double c1 = song.time();
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double speed = (c1 - c0) / dt;
        const bool ok = std::abs(speed - 1.0) < 0.05;
        std::printf("[mixerspeedcheck] song, %d aux bus%s: it ran %.2fx its speed  %s\n", auxes, auxes == 1 ? "" : "es",
                    speed, ok ? "ok" : "FAIL");
        if (!ok) ++bad;
        song.stop();
        for (int id : made) m.removeStrip(id);
    }
    std::remove(wav.c_str());
    return bad ? 1 : 0;
}
