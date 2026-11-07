#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "VideoSeq.hpp"
#include "VideoSeqMedia.hpp"

// Pictures and sound of a film (VideoSeq.hpp): the one compositor the preview
// and the export both draw with, so what the viewer shows is what the file
// gets -- only smaller. Everything is done on the CPU: a frame is a few layers
// of bilinear sampling, which a handful of threads do in milliseconds, and an
// export never waits for a GPU readback or fights the editor for its context.
namespace vseq {

// Where a film's video clips get their frames: one FrameSource per clip in
// use, decoding at the size the clip is drawn and the rate the film needs its
// frames (the film's fps over the clip's speed: a clip at 2x needs every other
// source frame, so it is decoded at half the rate). Sources nobody asked for in
// a while are closed.
class Frames {
public:
    explicit Frames(Library& lib) : m_lib(lib) {}

    // The frame of video clip `c` at film time `t`, for a `fps` film drawn on a
    // w x h canvas. `waitMs` as FrameSource::get; on a timeout, the clip's
    // last frame (and `exact` false). Null when there is nothing at all yet.
    // `error`: why there will never be a frame (ffmpeg could not read the file).
    ImagePtr video(const Clip& c, const MediaInfo& mi, double t, double fps, int canvasW, int canvasH, int waitMs,
                   bool* exact = nullptr, std::string* error = nullptr);
    // Start decoding clip `c` at film time `t` (it begins soon).
    void prefetch(const Clip& c, const MediaInfo& mi, double t, double fps, int canvasW, int canvasH);
    // Close sources unused for `idle` seconds of `now`, and those of clips not
    // in `alive` (uids).
    void drop(double now, double idle, const std::vector<int>& alive);
    int  open() const { return static_cast<int>(m_src.size()); }
    double clock = 0.0;   // the owner's time, stamped on what is used

private:
    FrameSource* source(const Clip& c, const MediaInfo& mi, double fps, int canvasW, int canvasH, std::int64_t& index,
                        double t);
    Library& m_lib;
    std::map<std::string, std::unique_ptr<FrameSource>> m_src;   // by clip and decode size/rate
};

// What a picture of the film needed and did not get.
struct DrawReport {
    bool complete = true;                 // every layer had its exact frame
    std::vector<std::string> problems;    // "file: why", once each
};

// Film time `t` drawn onto `out` (made w x h, RGBA8, opaque). `waitMs` is how
// long a layer may wait for its frame (-1: as long as it takes -- the export).
void drawFrame(const Film& film, double t, int w, int h, Library& lib, Frames& frames, int waitMs, Image& out,
               DrawReport* report = nullptr);

// Draw `src` onto `dst` with its centre at (cx, cy), `dw` x `dh` pixels big,
// turned `degrees` clockwise, `alpha` solid. Antialiased edges, bilinear
// inside; `dst` is treated as opaque. Parallel over rows.
void drawLayer(Image& dst, const Image& src, float cx, float cy, float dw, float dh, float degrees, float alpha);

// How big clip `c`'s picture is drawn on a w x h canvas at look scale 1, from
// its source's size (sw x sh).
void fitSize(Fit fit, int sw, int sh, int w, int h, float& dw, float& dh);

// A title set for a canvas `canvasH` tall (cached).
ImagePtr textImage(const TextStyle& s, int canvasH);
// The .ttf a title uses when it names none.
std::string defaultFont();

// The film's sound over [from, to) seconds: kRate stereo, interleaved. A
// source whose sound is not decoded yet is silent (`wait` false) or waited for;
// `complete` says whether anything was left out.
std::vector<float> mixdown(const Film& film, double from, double to, Library& lib, bool wait,
                           bool* complete = nullptr);

// --- The preview -----------------------------------------------------------------------

// Draws frames for the viewer on its own thread. The newest request wins: one
// asked for while another is drawn replaces any still waiting, so a scrub never
// queues up behind itself.
class Preview {
public:
    explicit Preview(Library& lib);
    ~Preview();
    Preview(const Preview&)            = delete;
    Preview& operator=(const Preview&) = delete;

    void request(std::shared_ptr<const Film> film, std::uint64_t rev, double t, int w, int h, bool playing);
    // A frame finished since the last call: its picture, the time and the
    // film revision it shows. `complete`: every layer had its exact frame.
    bool take(Image& out, double& t, std::uint64_t& rev, bool& complete);
    bool busy() const;
    std::vector<std::string> problems() const;

private:
    void run();
    struct Ask {
        std::shared_ptr<const Film> film;
        std::uint64_t rev = 0;
        double t = 0.0;
        int w = 0, h = 0;
        bool playing = false;
    };
    Library&                m_lib;
    Frames                  m_frames;
    mutable std::mutex      m_mx;
    std::condition_variable m_cv;
    std::thread             m_thread;
    bool                    m_stop = false, m_drawing = false, m_have = false;
    Ask                     m_ask, m_retry;
    bool                    m_pending = false;
    Image                   m_done;
    double                  m_doneT = 0.0;
    std::uint64_t           m_doneRev = 0;
    bool                    m_doneComplete = true;
    std::vector<std::string> m_problems;
};

// --- The export ------------------------------------------------------------------------

enum class Format { Mp4 = 0, Hevc = 1, WebM = 2, ProRes = 3, Fvid = 4, Wav = 5, Count };
const char* formatName(Format f);        // "MP4 (H.264)" ...
const char* formatExtension(Format f);   // ".mp4" ...
bool        formatNeedsFfmpeg(Format f);

struct ExportSettings {
    Format      format  = Format::Mp4;
    std::string path;
    int         width   = 1920, height = 1080;
    double      fps     = 30.0;
    int         quality = 80;     // 1 (smallest) .. 100 (best)
    double      from    = 0.0, to = -1.0;   // seconds; to -1: the end
    bool        sound   = true;
};

// A film written to a file, on a thread of its own: the sound mixed down to a
// scratch WAV, every frame drawn at full size and piped into ffmpeg (or, for a
// .fvid, JPEG'd here -- the game's own video, which needs no ffmpeg to play).
class Exporter {
public:
    Exporter() = default;
    ~Exporter();
    Exporter(const Exporter&)            = delete;
    Exporter& operator=(const Exporter&) = delete;

    // The film is copied. False (and why) when it cannot even begin.
    bool start(const Film& film, const ExportSettings& s, Library& lib, std::string* error = nullptr);
    void cancel();
    bool running() const { return m_running.load(); }
    // Finished (well or not) since start(); the thread is joined by then.
    bool finished();
    bool ok() const { return m_ok.load(); }
    float progress() const;
    int   framesDone() const { return m_done.load(); }
    int   framesTotal() const { return m_total.load(); }
    std::string message() const;
    std::string stage() const;   // what it is doing ("Mixing the sound", ...)
    const ExportSettings& settings() const { return m_set; }

private:
    void run(Film film, Library* lib);
    void say(const std::string& stage);
    ExportSettings    m_set;
    std::thread       m_thread;
    std::atomic<bool> m_running{false}, m_cancel{false}, m_ok{false};
    std::atomic<int>  m_done{0}, m_total{0};
    mutable std::mutex m_mx;
    std::string       m_message, m_stage;
    std::unique_ptr<Process> m_enc;
};

} // namespace vseq
