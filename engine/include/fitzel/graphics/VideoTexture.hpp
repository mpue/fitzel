#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fitzel {

class Texture;

// A looping video played into a Texture.
//
// The engine deliberately understands exactly one video format: `.fvid`, a
// motion-JPEG container the editor writes at import time (see VideoImport). Every
// frame is a standalone JPEG, so playback needs no decoder beyond stb_image --
// which is already linked -- and no new dependency follows the video feature into
// the player or the web build. The price is size: without inter-frame
// compression a clip is roughly five times a comparable MPEG file. That is the
// right trade for what this exists for (short, looping billboard ads), and it is
// the reason to keep imports small and low-framerate.
//
// The file is:
//
//     char     magic[4]   "FVID"
//     uint32   version    1
//     uint32   width, height
//     uint32   frameCount
//     float    fps
//     { uint32 offset; uint32 size; } * frameCount   -- offsets from file start
//     ... the JPEG payloads, in order ...
//
// Playback holds a shared Texture whose pixels are rewritten in place, so a
// MaterialDef can bind it once and never think about video again.
class VideoTexture {
public:
    // Container constants, shared with the importer that writes the file.
    static constexpr char     kMagic[4]  = {'F', 'V', 'I', 'D'};
    static constexpr uint32_t kVersion   = 1;

    VideoTexture() = default;

    // Read `path` and allocate the playback texture. The whole file is held in
    // memory -- an import-sized clip is a few MB, and paging it in from disk mid-
    // frame would cost far more than the RAM does. Invalid on any failure
    // (missing file, bad magic, unsupported version); check isValid().
    static std::shared_ptr<VideoTexture> open(const std::string& path);

    bool isValid() const { return m_tex != nullptr; }

    // The texture the frames are decoded into. Stable for this object's lifetime:
    // the GL name never changes, only its pixels.
    const std::shared_ptr<Texture>& texture() const { return m_tex; }

    int   width()      const { return m_width; }
    int   height()     const { return m_height; }
    float fps()        const { return m_fps; }
    int   frameCount() const { return static_cast<int>(m_frames.size()); }
    float duration()   const;

    // Advance playback by `dt` seconds, looping at the end, and decode+upload the
    // frame if that moved us to a new one. Costs nothing on the frames where the
    // index doesn't change -- at a 15 fps import and a 60 fps render that is three
    // calls out of four. Must run on the thread owning the GL context.
    void advance(float dt);

    // Jump to a time in seconds, wrapped into the clip. The frame is decoded on
    // the next advance(). Note that one VideoTexture serves every surface bound
    // to it, so this moves all of them together.
    void seek(float seconds);

private:
    struct FrameRef { std::uint32_t offset = 0, size = 0; };

    void decodeCurrentFrame();

    std::vector<unsigned char> m_bytes;   // the whole .fvid
    std::vector<FrameRef>      m_frames;
    std::shared_ptr<Texture>   m_tex;
    int                        m_width   = 0;
    int                        m_height  = 0;
    float                      m_fps     = 15.0f;
    float                      m_time    = 0.0f;
    int                        m_current = -1;  // frame index currently uploaded
};

} // namespace fitzel
