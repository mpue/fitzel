#include "IconEmbed.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <stb_image.h>

// The resizer is only ever needed here, so its implementation lives here rather
// than in the engine's shared stb translation unit -- nothing in the runtime
// scales an image on the CPU.
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace icoutil {

namespace {

// The sizes Windows actually asks for: 256 in the extra-large view, 48 in the
// large one, 32 on the taskbar and in Alt-Tab, 16 in the title bar and in lists.
// The rest fill the gaps so nothing has to be scaled at display time.
constexpr int kSizes[] = {256, 128, 64, 48, 32, 24, 16};

// The integer id the icon group is written under. 101 is the id an .rc-based
// build would have used (IDI_ICON1), and it is the lowest group id in the file,
// which is how Explorer decides which icon is "the" application icon.
constexpr int kGroupId = 101;

#pragma pack(push, 1)
struct IconDir      { std::uint16_t reserved, type, count; };
struct IconDirEntry { std::uint8_t  w, h, colors, reserved;
                      std::uint16_t planes, bits;
                      std::uint32_t bytes, offset; };
// The same entry as it appears inside a PE: the file offset is replaced by the
// resource id of the RT_ICON holding that image. 14 bytes, not 16.
struct GroupEntry   { std::uint8_t  w, h, colors, reserved;
                      std::uint16_t planes, bits;
                      std::uint32_t bytes;
                      std::uint16_t id; };
struct DibHeader    { std::uint32_t size; std::int32_t w, h;
                      std::uint16_t planes, bits;
                      std::uint32_t compression, imageSize;
                      std::int32_t  xppm, yppm;
                      std::uint32_t used, important; };
#pragma pack(pop)

void put(std::vector<unsigned char>& v, const void* p, std::size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    v.insert(v.end(), b, b + n);
}

// One icon image in the shape an .ico entry wants: a BITMAPINFOHEADER whose
// height is doubled (colour bitmap plus AND mask), then bottom-up BGRA rows,
// then the mask.
//
// The mask is written as all zeros -- "opaque everywhere" -- because a 32-bit
// icon carries its transparency in the alpha channel. It is pure legacy, and
// Windows still refuses the entry if it is not there.
std::vector<unsigned char> dibImage(const unsigned char* rgba, int size) {
    const int         maskStride = ((size + 31) / 32) * 4;
    const std::size_t xorBytes   = static_cast<std::size_t>(size) * size * 4;
    const std::size_t maskBytes  = static_cast<std::size_t>(maskStride) * size;

    DibHeader h{};
    h.size        = sizeof(DibHeader);
    h.w           = size;
    h.h           = size * 2;
    h.planes      = 1;
    h.bits        = 32;
    h.compression = 0; // BI_RGB
    h.imageSize   = static_cast<std::uint32_t>(xorBytes + maskBytes);

    std::vector<unsigned char> out;
    out.reserve(sizeof h + xorBytes + maskBytes);
    put(out, &h, sizeof h);
    for (int y = size - 1; y >= 0; --y) {           // bottom-up, like every DIB
        const unsigned char* row = rgba + static_cast<std::size_t>(y) * size * 4;
        for (int x = 0; x < size; ++x) {
            const unsigned char* p       = row + x * 4;
            const unsigned char  bgra[4] = {p[2], p[1], p[0], p[3]};
            put(out, bgra, 4);
        }
    }
    out.insert(out.end(), maskBytes, 0);
    return out;
}

} // namespace

std::vector<unsigned char> fromImage(const std::string& imagePath, std::string& err) {
    int            w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load(imagePath.c_str(), &w, &h, &comp, 4);
    if (!px) {
        const char* why = stbi_failure_reason();
        err = "could not read " + imagePath + (why ? std::string(": ") + why : "");
        return {};
    }

    // Square the source by padding, never by stretching. Centring the picture on
    // a transparent canvas keeps a wide logo wide, instead of turning it into a
    // squashed one that only looks wrong once it is on somebody's desktop.
    const int                  side = std::max(w, h);
    std::vector<unsigned char> square(static_cast<std::size_t>(side) * side * 4, 0);
    const int                  ox = (side - w) / 2, oy = (side - h) / 2;
    for (int y = 0; y < h; ++y)
        std::memcpy(square.data() + (static_cast<std::size_t>(y + oy) * side + ox) * 4,
                    px + static_cast<std::size_t>(y) * w * 4,
                    static_cast<std::size_t>(w) * 4);
    stbi_image_free(px);

    // Never scale a picture UP into the icon: a 64px logo blown up to 256 is
    // just a blurry 64px logo taking a quarter megabyte in every shipped exe.
    // The 16px entry is the one exception -- something has to be in the list.
    std::vector<int> sizes;
    for (int s : kSizes)
        if (s <= side) sizes.push_back(s);
    if (sizes.empty()) sizes.push_back(16);

    std::vector<std::vector<unsigned char>> images;
    images.reserve(sizes.size());
    for (int s : sizes) {
        std::vector<unsigned char> scaled(static_cast<std::size_t>(s) * s * 4);
        if (s == side) {
            scaled = square;
        } else if (!stbir_resize_uint8_srgb(square.data(), side, side, 0,
                                            scaled.data(), s, s, 0, STBIR_RGBA)) {
            err = "could not scale the icon to " + std::to_string(s) + " px";
            return {};
        }
        images.push_back(dibImage(scaled.data(), s));
    }

    const auto count = static_cast<std::uint16_t>(images.size());
    const IconDir dir{0, 1, count};

    std::vector<unsigned char> ico;
    put(ico, &dir, sizeof dir);
    auto offset = static_cast<std::uint32_t>(sizeof(IconDir) +
                                             sizeof(IconDirEntry) * images.size());
    for (std::size_t i = 0; i < images.size(); ++i) {
        IconDirEntry e{};
        // 0 means 256: the field is a single byte, which is exactly why 256 is
        // the largest icon Windows has ever had.
        e.w      = static_cast<std::uint8_t>(sizes[i] == 256 ? 0 : sizes[i]);
        e.h      = e.w;
        e.planes = 1;
        e.bits   = 32;
        e.bytes  = static_cast<std::uint32_t>(images[i].size());
        e.offset = offset;
        offset += e.bytes;
        put(ico, &e, sizeof e);
    }
    for (const auto& img : images) put(ico, img.data(), img.size());
    return ico;
}

#ifdef _WIN32

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<std::size_t>(n - 1), L' ');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string lastError(const char* what) {
    return std::string(what) + " failed (error " +
           std::to_string(static_cast<unsigned>(GetLastError())) + ")";
}

} // namespace

bool embed(const std::string& exePath, const std::vector<unsigned char>& ico,
           std::string& err) {
    if (ico.size() < sizeof(IconDir)) { err = "empty icon"; return false; }

    const auto*       dir  = reinterpret_cast<const IconDir*>(ico.data());
    const auto*       ent  = reinterpret_cast<const IconDirEntry*>(ico.data() + sizeof(IconDir));
    const std::size_t need = sizeof(IconDir) + sizeof(IconDirEntry) * dir->count;
    if (dir->type != 1 || dir->count == 0 || ico.size() < need) {
        err = "malformed icon";
        return false;
    }

    // The group directory: the same entries, with each image's file offset
    // swapped for the resource id it is about to be stored under.
    std::vector<unsigned char> group;
    const IconDir              gdir{0, 1, dir->count};
    put(group, &gdir, sizeof gdir);
    for (int i = 0; i < dir->count; ++i) {
        GroupEntry g{};
        std::memcpy(&g, &ent[i], offsetof(GroupEntry, id)); // everything but the id
        g.id = static_cast<std::uint16_t>(i + 1);
        put(group, &g, sizeof g);
    }

    // FALSE, not TRUE: the linker already put a manifest in this exe, and
    // wiping every resource to make room for an icon would take it with them.
    const std::wstring wpath = widen(exePath);
    HANDLE             upd   = BeginUpdateResourceW(wpath.c_str(), FALSE);
    if (!upd) { err = lastError("BeginUpdateResource"); return false; }

    // Spelled out rather than using RT_ICON / RT_GROUP_ICON: those macros are
    // the ANSI form unless the whole translation unit is built with UNICODE
    // defined, and handing a char* type to the W function does not compile.
    const wchar_t* const rtIcon      = MAKEINTRESOURCEW(3);
    const wchar_t* const rtGroupIcon = MAKEINTRESOURCEW(14);

    const WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    bool       ok   = true;
    for (int i = 0; i < dir->count && ok; ++i) {
        if (static_cast<std::size_t>(ent[i].offset) + ent[i].bytes > ico.size()) {
            err = "truncated icon";
            ok  = false;
            break;
        }
        ok = UpdateResourceW(upd, rtIcon, MAKEINTRESOURCEW(i + 1), lang,
                             const_cast<unsigned char*>(ico.data() + ent[i].offset),
                             ent[i].bytes) != FALSE;
        if (!ok) err = lastError("UpdateResource(icon)");
    }
    // Twice, from the same images: the integer id is what the shell reads out of
    // the file, the name is what GLFW asks for when it builds the window class.
    const wchar_t* groups[] = {MAKEINTRESOURCEW(kGroupId), L"GLFW_ICON"};
    for (const wchar_t* name : groups) {
        if (!ok) break;
        ok = UpdateResourceW(upd, rtGroupIcon, name, lang, group.data(),
                             static_cast<DWORD>(group.size())) != FALSE;
        if (!ok) err = lastError("UpdateResource(icon group)");
    }

    if (!EndUpdateResourceW(upd, ok ? FALSE : TRUE) && ok) {
        err = lastError("EndUpdateResource");
        ok  = false;
    }
    return ok;
}

#else

bool embed(const std::string&, const std::vector<unsigned char>&, std::string& err) {
    err = "icons can only be written into Windows executables";
    return false;
}

#endif

} // namespace icoutil
