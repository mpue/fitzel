#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef __linux__
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

// A hardware MIDI input for the Song Sequencer (SongPanel.hpp): Linux's raw
// MIDI devices, /dev/snd/midiC<card>D<device>, read on a thread of their own.
// Note-ons and note-offs are parsed out of the byte stream -- running status
// kept, real-time bytes (clock, active sensing) and SysEx skipped, a note-on
// of velocity 0 taken as the note-off it is -- and handed to the frame.
//
// Elsewhere open() says no and nothing is read.
namespace songui {

struct MidiIn {
    struct Ev {
        int    pitch = 60, vel = 0;
        bool   on = false;
        // Filled by onEvent, on the reader's thread: where the song was and
        // which track sounded it, for recording it on the frame.
        double tick  = 0.0;
        int    track = -1;
    };
    // Called on the reader's thread for every note, as it arrives -- the
    // panel sounds it there, without waiting for the editor's next frame
    // (which may be half a second away in an idle editor). Set before open().
    std::function<void(Ev&)> onEvent;

    ~MidiIn() { close(); }

    bool open(const std::string& path) {
#ifdef __linux__
        close();
        m_fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK);
        if (m_fd < 0) return false;
        m_quit = false;
        m_thread = std::thread([this] { run(); });
        return true;
#else
        (void)path;
        return false;
#endif
    }
    void close() {
        m_quit = true;
        if (m_thread.joinable()) m_thread.join();
#ifdef __linux__
        if (m_fd >= 0) ::close(m_fd);
#endif
        m_fd = -1;
    }
    // What arrived since the last call, in order.
    std::vector<Ev> take() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<Ev> out;
        out.swap(m_events);
        return out;
    }
    // The parser on its own, a byte at a time.
    void feed(unsigned char b) {
        if (b >= 0xF8) return;                           // clock, active sensing
        if (b == 0xF0) { m_sysex = true; return; }
        if (b == 0xF7) { m_sysex = false; return; }
        if (b & 0x80) { m_sysex = false; m_status = b; m_have = 0; return; }
        if (m_sysex || !m_status || m_status >= 0xF0) return;
        m_data[m_have++] = b;
        const int kind = m_status & 0xF0;
        const int need = (kind == 0xC0 || kind == 0xD0) ? 1 : 2;
        if (m_have < need) return;
        m_have = 0;
        if (kind == 0x90 || kind == 0x80) {
            Ev e;
            e.pitch = m_data[0] & 0x7F;
            e.vel   = m_data[1] & 0x7F;
            e.on    = kind == 0x90 && m_data[1] > 0;
            if (onEvent) onEvent(e);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_events.push_back(e);
        }
    }

private:
    void run() {
#ifdef __linux__
        while (!m_quit) {
            pollfd p{m_fd, POLLIN, 0};
            if (::poll(&p, 1, 50) <= 0) continue;
            unsigned char buf[256];
            const ssize_t n = ::read(m_fd, buf, sizeof buf);
            if (n <= 0) {
                // Unplugged (or a pipe with nobody writing): not a reason to spin.
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            for (ssize_t i = 0; i < n; ++i) feed(buf[i]);
        }
#endif
    }

    std::thread       m_thread;
    std::atomic<bool> m_quit{false};
    std::mutex        m_mutex;
    std::vector<Ev>   m_events;
    int               m_fd = -1;
    unsigned char     m_status = 0, m_data[2] = {0, 0};
    int               m_have = 0;
    bool              m_sysex = false;
};

} // namespace songui
