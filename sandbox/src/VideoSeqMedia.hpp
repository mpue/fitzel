#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fitzel/audio/SampleData.hpp>
#include <fitzel/audio/Waveform.hpp>

// The Video Sequencer's sources: video, pictures and sound, read through
// whatever ffmpeg the machine has.
//
// FFMPEG IS A PROGRAM HERE, NOT A LIBRARY -- the reason the video importer and
// the recorder give (VideoImport.hpp): no CMake build to fetch, no LGPL/GPL in
// the tree, and every codec the user's ffmpeg was built with for free. The
// price is a process per stream and frames through a pipe. A decoder reads
// ahead on its own thread so the pipe is never what a frame waits for, and a
// seek is a new process started at the time wanted (-ss before -i: ffmpeg
// jumps to the keyframe and decodes on to the exact frame).
//
// Nothing of this reaches the game. A film leaves the editor as a rendered file
// (VideoSeqRender.hpp); the player knows only .fvid.
namespace vseq {

// --- ffmpeg ----------------------------------------------------------------------------

std::string ffmpegExe();     // next to the editor if the build put it there, else the PATH's
std::string ffprobeExe();    // beside that ffmpeg, else the PATH's
// Does ffmpeg run? Asked once, then remembered. `version`: its first line.
bool ffmpegReady(std::string* version = nullptr);
// How to get ffmpeg on this platform, one line.
const char* ffmpegInstallHint();

// A child process with pipes to its stdin and/or stdout; its stderr goes to a
// scratch file that errors() reads back. Not the shell: arguments are passed as
// they are, so a path with spaces or quotes in it needs no escaping.
class Process {
public:
    Process();
    ~Process();   // kills it if it still runs
    Process(const Process&)            = delete;
    Process& operator=(const Process&) = delete;

    bool start(const std::string& exe, const std::vector<std::string>& args, bool pipeIn, bool pipeOut,
               std::string* error = nullptr);
    // Exactly `n` bytes; false when the stream ends (or breaks) first.
    bool readFully(void* dst, std::size_t n);
    // Everything up to the end of the stream.
    std::string readAll();
    bool write(const void* src, std::size_t n);
    void closeIn();
    // Waits for it to end: its exit code, -1 when it was killed or never ran.
    int  wait();
    void kill();
    bool started() const;
    // The tail of what it said on stderr (a few KB), once it has ended.
    std::string errors() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m;
};

// --- What a file is ----------------------------------------------------------------------

struct MediaInfo {
    bool        ok = false;
    std::string error;
    bool        video = false;      // has pictures that move
    bool        still = false;      // a picture (an image file, or a one-frame video)
    bool        audio = false;
    int         width = 0, height = 0;   // as shown (a phone video's rotation applied)
    double      fps = 0.0;
    double      duration = 0.0;     // seconds; 0 for a still
    int         audioRate = 0, audioChannels = 0;
};
// Asks ffprobe (pictures: stb). Slow-ish (a process): use Library::info.
MediaInfo probe(const std::string& path);

// --- Frames -------------------------------------------------------------------------------

// RGBA8, straight alpha, top row first.
struct Image {
    int w = 0, h = 0;
    std::vector<std::uint8_t> px;
    bool empty() const { return w <= 0 || h <= 0; }
};
using ImagePtr = std::shared_ptr<const Image>;

// One video file decoded at one size and rate. Frame i is the source at time
// i / rate. ffmpeg runs on the source's own thread and keeps a few frames ahead
// of the last one asked for, so playing forward costs a lookup; a frame behind
// the window or far ahead of it restarts ffmpeg there.
class FrameSource {
public:
    FrameSource(std::string path, int w, int h, double rate);
    ~FrameSource();
    FrameSource(const FrameSource&)            = delete;
    FrameSource& operator=(const FrameSource&) = delete;

    // Frame `i`, waiting up to `waitMs` for it (-1: as long as it takes). Past
    // the end of the video: its last frame. Null on a timeout, or when ffmpeg
    // could not read the file (error() says why).
    ImagePtr get(std::int64_t i, int waitMs = -1);
    // Begin decoding at `i` without waiting: a clip that starts soon.
    void prefetch(std::int64_t i);
    // The frame handed out last (what to show while a new one is coming).
    ImagePtr latest() const;
    std::string error() const;

    const std::string& path() const { return m_path; }
    int    width() const { return m_w; }
    int    height() const { return m_h; }
    double rate() const { return m_rate; }
    double lastUsed = 0.0;   // the owner's clock, for dropping idle sources

private:
    void run();
    bool spawn(std::int64_t at);

    std::string m_path;
    int         m_w, m_h;
    double      m_rate;
    std::size_t m_bytes;
    int         m_ahead;      // frames kept decoded ahead of the one wanted

    mutable std::mutex      m_mx;
    std::condition_variable m_cv;
    std::thread             m_thread;
    bool                    m_stop = false;
    std::int64_t            m_want = -1;     // the last frame asked for
    std::int64_t            m_seek = -1;     // restart ffmpeg at this frame
    std::int64_t            m_next = 0;      // the frame ffmpeg delivers next
    bool                    m_running = false, m_eof = false, m_failed = false;
    std::string             m_error;
    std::map<std::int64_t, ImagePtr> m_frames;   // decoded, around m_want
    ImagePtr                m_latest, m_last;    // handed out last; the stream's last frame
    std::unique_ptr<Process> m_proc;
};

// --- Thumbnails and sound ------------------------------------------------------------------------

// Small pictures along a video, one every `interval` seconds, for the clips on
// the timeline. All in one Image, a row of `count` frames each `w` x `h`.
struct ThumbStrip {
    bool   ready = false, failed = false;
    double interval = 1.0;
    int    count = 0, w = 0, h = 0;
    Image  strip;   // count*w wide (wrapped onto rows of `perRow`)
    int    perRow = 0;
};

// A source's sound, decoded once: 48 kHz stereo floats, interleaved.
struct Sound {
    bool             ready = false, failed = false;
    std::string      error;
    std::vector<float> pcm;
    fitzel::Waveform wave;
    std::int64_t frames() const { return static_cast<std::int64_t>(pcm.size() / 2); }
};
constexpr int kRate = 48000;   // the film's sound runs at this

// Fill `s.wave` from `s.pcm` (100 buckets a second).
void measure(Sound& s);

// Everything decoded once and shared: what each file is, its sound, its
// thumbnails, its pictures at the sizes they are drawn. Thread-safe -- the panel,
// the preview and an export all ask it. The slow parts (ffprobe, ffmpeg) run on
// worker threads; until they are done the answer is "not yet" (null, or an
// entry whose `ready` is false).
class Library {
public:
    Library();
    ~Library();

    // A film's file name to a path on disk (relative names are the project's).
    void setResolver(std::function<std::string(const std::string&)> r);
    std::string resolve(const std::string& file) const;

    // nullptr while the probe runs.
    std::shared_ptr<const MediaInfo> info(const std::string& file);
    // Waits for the probe.
    MediaInfo infoNow(const std::string& file);
    std::shared_ptr<const Sound>      sound(const std::string& file);
    std::shared_ptr<const Sound>      soundNow(const std::string& file);   // waits
    std::shared_ptr<const ThumbStrip> thumbs(const std::string& file);
    // A picture file, halved until it is no more than twice `maxW` x `maxH`
    // (sampling a picture far bigger than it is drawn shimmers).
    ImagePtr picture(const std::string& file, int maxW, int maxH);

    // Sound from somewhere else -- a Song Sequencer song, bounced -- put in as
    // `file`'s, stamped with when that file was written (a song saved again
    // makes it stale: songStale()).
    void putSound(const std::string& file, fitzel::SampleData data);
    bool songStale(const std::string& file);
    // A file changed on disk (or vanished): forget everything about it.
    void forget(const std::string& file);
    void clear();
    // Probes, sounds or thumbnails still being made.
    bool busy() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace vseq
