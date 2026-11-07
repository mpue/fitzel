#pragma once

// The file system of a game on GSC's X-Ray engine (S.T.A.L.K.E.R.): the .db
// archives under the install, with loose files in gamedata/ laid over them the
// way the game itself does it (a mod's gamedata/ wins over the archives).
//
// The archives are the Clear Sky / Call of Pripyat layout, which the 2025
// Enhanced Editions use too: a chunked file whose chunk 1 is the file table
// (LZHUF-packed when its id carries the 0x80000000 mark), each entry naming a
// file's offset in the archive and its two sizes; a file whose sizes differ is
// LZO1X-packed. The encrypted tables of the original 2007 Shadow of Chernobyl
// archives are not read -- mount() says so instead of mounting nothing.
//
// Paths are the game's own, relative to gamedata/: "levels\l01_escape\level",
// "textures\mtl\mtl_bochka_01.dds". Lookups ignore case and take either slash.

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace xray {

class FileSystem {
public:
    // Every archive found in `gameDir` (and its subfolders levels/, resources/,
    // patches/, ...), then gamedata/ over them. False with a reason when there
    // is nothing to read.
    bool mount(const std::filesystem::path& gameDir, std::string& error);

    bool exists(const std::string& path) const;
    // The file's bytes; false when it is missing or cannot be unpacked.
    bool read(const std::string& path, std::vector<std::uint8_t>& out) const;

    // The levels: folder names under levels/ that hold a "level" file and its
    // "level.geom".
    std::vector<std::string> levels() const;
    // Every file's normalized path, sorted.
    std::vector<std::string> paths() const;

    std::size_t fileCount() const { return files_.size(); }
    std::size_t archiveCount() const { return archives_.size(); }

    // "Levels\\L01_Escape\\level" -> "levels/l01_escape/level".
    static std::string normalize(const std::string& path);

private:
    struct Entry {
        int           archive = -1;   // -1: a loose file under gamedata/
        std::uint32_t offset = 0;
        std::uint32_t sizeReal = 0;
        std::uint32_t sizeCompressed = 0;
        std::filesystem::path loose;
    };
    bool mountArchive(const std::filesystem::path& file, std::string& error);

    std::vector<std::filesystem::path>     archives_;
    std::unordered_map<std::string, Entry> files_;
};

// The two packers the archives use, exposed for the self-test.
// LZHUF (Yoshizaki's adaptive Huffman LZSS, X-Ray's MAX_FREQ of 0x4000): the
// archive tables and packed chunks. `outSize` is the unpacked size, stored in
// front of the packed bytes. False if the stream ends early.
bool lzhufDecode(const std::uint8_t* src, std::size_t srcSize, std::size_t outSize,
                 std::vector<std::uint8_t>& out);
// LZO1X: packed archive files. False on a malformed stream or a wrong size.
bool lzo1xDecode(const std::uint8_t* src, std::size_t srcSize, std::uint8_t* dst,
                 std::size_t dstSize);

} // namespace xray
