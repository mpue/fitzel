#pragma once

// The chunked layout every X-Ray file shares (archives, levels, models,
// motions), and a bounds-checked reader over a chunk. Internal to the
// stalkerimport readers.

#include <cstdint>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "XrArchive.hpp"

namespace xray::detail {

// --- chunked files ----------------------------------------------------------

struct Chunk {
    std::uint32_t       id = 0;
    const std::uint8_t* data = nullptr;
    std::size_t         size = 0;
};

// The chunks of a block. Packed ones (the 0x80000000 mark) are unpacked into
// `store`, which must outlive the result.
inline std::vector<Chunk> chunksOf(const std::uint8_t* p, std::size_t n,
                            std::deque<std::vector<std::uint8_t>>& store) {
    std::vector<Chunk> out;
    std::size_t pos = 0;
    while (pos + 8 <= n) {
        std::uint32_t id, size;
        std::memcpy(&id, p + pos, 4);
        std::memcpy(&size, p + pos + 4, 4);
        pos += 8;
        if (size > n - pos) break;
        Chunk c{id & 0x7fffffffu, p + pos, size};
        if ((id & 0x80000000u) && size >= 4) {
            std::uint32_t real;
            std::memcpy(&real, p + pos, 4);
            store.emplace_back();
            if (lzhufDecode(p + pos + 4, size - 4, real, store.back())) {
                c.data = store.back().data();
                c.size = store.back().size();
            }
        }
        out.push_back(c);
        pos += size;
    }
    return out;
}

inline const Chunk* find(const std::vector<Chunk>& cs, std::uint32_t id) {
    for (const Chunk& c : cs)
        if (c.id == id) return &c;
    return nullptr;
}

class Reader {
public:
    Reader(const std::uint8_t* p, std::size_t n) : p_(p), n_(n) {}
    explicit Reader(const Chunk& c) : p_(c.data), n_(c.size) {}
    bool ok() const { return ok_; }
    std::size_t left() const { return ok_ ? n_ - pos_ : 0; }
    std::size_t tell() const { return pos_; }
    template <class T> T get() {
        T v{};
        if (!ok_ || n_ - pos_ < sizeof(T)) { ok_ = false; return v; }
        std::memcpy(&v, p_ + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    glm::vec3 vec3() {
        const float x = get<float>(), y = get<float>(), z = get<float>();
        return {x, y, z};
    }
    std::string str() {   // zero-terminated
        std::string s;
        while (ok_ && pos_ < n_ && p_[pos_]) s.push_back(static_cast<char>(p_[pos_++]));
        if (pos_ < n_) ++pos_;
        else ok_ = false;
        return s;
    }
    const std::uint8_t* skip(std::size_t n) {
        if (!ok_ || n_ - pos_ < n) { ok_ = false; return nullptr; }
        const std::uint8_t* at = p_ + pos_;
        pos_ += n;
        return at;
    }

private:
    const std::uint8_t* p_;
    std::size_t n_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

struct SlideWindow {
    std::uint32_t offset = 0;
    std::uint16_t tris = 0;
    std::uint16_t verts = 0;
};

// An SWI block: four reserved words, a count, the windows. The first window is
// the full detail (most triangles, every vertex).
inline std::optional<SlideWindow> firstWindow(Reader& r) {
    for (int i = 0; i < 4; ++i) r.get<std::uint32_t>();
    const std::uint32_t n = r.get<std::uint32_t>();
    if (!r.ok() || n == 0 || r.left() < static_cast<std::size_t>(n) * 8) return std::nullopt;
    SlideWindow w;
    w.offset = r.get<std::uint32_t>();
    w.tris   = r.get<std::uint16_t>();
    w.verts  = r.get<std::uint16_t>();
    r.skip((n - 1) * 8);
    return w;
}

} // namespace xray::detail
