#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

// Writing rendered frames out as a video file -- the end of the game recorder
// (GameRecorder.hpp): a take is played back at a fixed rate and every finished
// frame comes through here.
//
// Two ways out, because one of them must always work:
//  - Mp4: H.264 through ffmpeg as a child process, fed raw RGBA on its stdin.
//    Small files that play everywhere -- when ffmpeg is there (the same one the
//    video importer uses, see VideoImport.hpp).
//  - Avi: motion-JPEG in an AVI 1.0 container, written here with stb's JPEG
//    encoder. Needs nothing; VLC, mpv and every editor read it. Big files, and
//    AVI 1.0 stops at 4 GB, which at 1080p is a few minutes.
namespace videoout {

enum class Format { Mp4, Avi };

// Is ffmpeg actually runnable (not just named)? Asked once, then remembered.
bool ffmpegAvailable();

class Writer {
public:
    Writer() = default;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // `width` and `height` are rounded down to even (4:2:0 chroma needs it);
    // frames must still be handed over at the size given here, and the odd
    // last row/column is dropped. `quality` 1 (worst) .. 100 (best).
    bool open(const std::string& path, int width, int height, double fps, Format fmt,
              int quality, std::string& err);
    // One frame, RGBA8, rows bottom-up as glReadPixels returns them.
    bool frame(const unsigned char* rgbaBottomUp);
    // Finishes the file. `msg`: what was written, or what went wrong.
    bool close(std::string& msg);

    bool isOpen() const { return m_open; }
    int  frames() const { return m_frames; }
    int  width()  const { return m_w; }
    int  height() const { return m_h; }

private:
    bool aviFrame(const unsigned char* rgbaBottomUp);
    bool aviClose(std::string& msg);

    Format      m_fmt = Format::Avi;
    std::string m_path;
    FILE*       m_fp = nullptr;      // the AVI file, or the pipe into ffmpeg
    bool        m_open = false;
    bool        m_failed = false;
    int         m_srcW = 0, m_srcH = 0;   // the frames handed in
    int         m_w = 0, m_h = 0;         // the video (even)
    double      m_fps = 30.0;
    int         m_quality = 90;
    int         m_frames = 0;

    // AVI bookkeeping: where the sizes go once known, and the index.
    long                       m_moviStart = 0;   // offset of the 'movi' fourcc
    std::uint32_t              m_maxChunk = 0;
    std::vector<std::uint32_t> m_index;           // offset, size per frame
    std::vector<unsigned char> m_rgb, m_jpg;      // scratch
};

} // namespace videoout
