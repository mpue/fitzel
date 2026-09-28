#include "ThumbCache.hpp"

#include <fstream>

#include <fitzel/graphics/Texture.hpp>

namespace thumbcache {

// --- Thumbnail disk cache --------------------------------------------------
// Decoding a 4K texture (or a huge EXR) down to a 128px preview is expensive and
// hammers the disk -- opening the Assets browser would otherwise re-read every
// source texture in full. We cache each decoded preview to a tiny file keyed by
// the asset GUID and tagged with the source's last-write time (so edits
// invalidate it); the thumbnail worker loads these instead of re-decoding.
std::filesystem::path thumbCacheDir() {
    std::error_code ec;
    std::filesystem::path d =
        std::filesystem::temp_directory_path(ec) / "fitzel_thumbs";
    std::filesystem::create_directories(d, ec);
    return d;
}

long long sourceMtime(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<long long>(t.time_since_epoch().count());
}

// Cache file layout: magic 'FTH1' | int64 srcMtime | int32 w,h,ch | raw pixels.
bool loadThumbCache(const std::filesystem::path& file, long long srcMtime,
                    fitzel::ImagePixels& out) {
    std::ifstream f(file, std::ios::binary);
    if (!f) return false;
    char magic[4] = {};
    f.read(magic, 4);
    if (!f || magic[0] != 'F' || magic[1] != 'T' ||
        magic[2] != 'H' || magic[3] != '1') return false;
    long long mt = 0; int w = 0, h = 0, ch = 0;
    f.read(reinterpret_cast<char*>(&mt), sizeof mt);
    f.read(reinterpret_cast<char*>(&w),  sizeof w);
    f.read(reinterpret_cast<char*>(&h),  sizeof h);
    f.read(reinterpret_cast<char*>(&ch), sizeof ch);
    if (!f || mt != srcMtime ||
        w < 0 || h < 0 || ch < 0 || w > 4096 || h > 4096 || ch > 4) return false;
    const std::size_t n = static_cast<std::size_t>(w) * h * ch;
    if (n == 0) { out = {}; return true; } // negative cache: source has no usable preview
    out.pixels.resize(n);
    f.read(reinterpret_cast<char*>(out.pixels.data()),
           static_cast<std::streamsize>(n));
    if (!f) { out.pixels.clear(); return false; }
    out.width = w; out.height = h; out.channels = ch;
    return true;
}

// Always writes -- an invalid image is stored as a zero-size "negative" entry so a
// source that can't produce a preview is not re-decoded on every session.
void saveThumbCache(const std::filesystem::path& file, long long srcMtime,
                    const fitzel::ImagePixels& img) {
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    if (!f) return;
    f.write("FTH1", 4);
    f.write(reinterpret_cast<const char*>(&srcMtime), sizeof srcMtime);
    const int w = img.width, h = img.height, ch = img.channels;
    f.write(reinterpret_cast<const char*>(&w),  sizeof w);
    f.write(reinterpret_cast<const char*>(&h),  sizeof h);
    f.write(reinterpret_cast<const char*>(&ch), sizeof ch);
    f.write(reinterpret_cast<const char*>(img.pixels.data()),
            static_cast<std::streamsize>(img.pixels.size()));
}

} // namespace thumbcache
