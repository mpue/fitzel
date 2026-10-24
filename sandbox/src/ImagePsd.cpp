// Photoshop files with their layers, for the Image editor (ImageDoc.hpp).
//
// What is read and written: 8-bit RGB documents, one layer record per layer with
// its pixels (raw or PackBits), name (the Unicode one when there is one),
// opacity, visibility, "lock transparent pixels" and blend mode, plus the merged
// picture other programs (and stb_image, which the engine loads textures with)
// show. Groups are flattened into the plain stack -- a hidden group hides what
// is in it -- and layer masks, effects and adjustment layers are not read.
//
// Everything in a PSD is big-endian. The format is Adobe's "Photoshop File
// Formats Specification"; the parts used here have not changed since PS 6.
#include "ImageDoc.hpp"

#include <algorithm>
#include <cstring>

namespace img {

namespace {

struct Reader {
    const unsigned char* p;
    std::size_t n, at = 0;
    bool bad = false;
    bool need(std::size_t k) { if (bad || at + k > n) { bad = true; return false; } return true; }
    std::uint32_t u8() { return need(1) ? p[at++] : 0; }
    std::uint32_t u16() { if (!need(2)) return 0; std::uint32_t v = (p[at] << 8) | p[at + 1]; at += 2; return v; }
    std::uint32_t u32() {
        if (!need(4)) return 0;
        std::uint32_t v = (std::uint32_t(p[at]) << 24) | (p[at + 1] << 16) | (p[at + 2] << 8) | p[at + 3];
        at += 4;
        return v;
    }
    std::int32_t i32() { return std::int32_t(u32()); }
    std::int16_t i16() { return std::int16_t(u16()); }
    void skip(std::size_t k) { if (need(k)) at += k; }
    std::string str(std::size_t k) { if (!need(k)) return {}; std::string s(reinterpret_cast<const char*>(p + at), k); at += k; return s; }
};

struct Writer {
    std::vector<unsigned char>& o;
    void u8(std::uint32_t v) { o.push_back(std::uint8_t(v)); }
    void u16(std::uint32_t v) { u8(v >> 8); u8(v); }
    void u32(std::uint32_t v) { u8(v >> 24); u8(v >> 16); u8(v >> 8); u8(v); }
    void raw(const void* d, std::size_t k) { const auto* b = static_cast<const unsigned char*>(d); o.insert(o.end(), b, b + k); }
    void str(const char* s) { raw(s, std::strlen(s)); }
    std::size_t mark() { const std::size_t m = o.size(); u32(0); return m; }   // a length to fill in later
    void patch(std::size_t m) {
        const std::uint32_t v = std::uint32_t(o.size() - m - 4);
        o[m] = std::uint8_t(v >> 24); o[m + 1] = std::uint8_t(v >> 16); o[m + 2] = std::uint8_t(v >> 8); o[m + 3] = std::uint8_t(v);
    }
};

const char* const kBlendKeys[] = {"norm", "mul ", "scrn", "over", "sLit", "lddg", "fsub", "dark", "lite", "diff", "colr"};
static_assert(sizeof(kBlendKeys) / sizeof(kBlendKeys[0]) == std::size_t(Blend::Count));

Blend blendOfKey(const std::string& k) {
    for (int i = 0; i < int(Blend::Count); ++i) if (k == kBlendKeys[i]) return Blend(i);
    if (k == "lLit" || k == "vLit" || k == "hLit") return Blend::Overlay;      // the light family
    if (k == "idiv" || k == "div ") return Blend::Darken;
    if (k == "lbrn") return Blend::Multiply;
    if (k == "dkCl") return Blend::Darken;
    if (k == "lgCl") return Blend::Lighten;
    if (k == "hue " || k == "sat " || k == "lum ") return Blend::Color;
    return Blend::Normal;
}

// PackBits, as Photoshop and TIFF do it: a run of 2+ equal bytes is (1 - n, b),
// anything else (n - 1, bytes...), at most 128 bytes either way.
void packBits(const std::uint8_t* s, int n, std::vector<unsigned char>& o) {
    int i = 0;
    while (i < n) {
        int run = 1;
        while (i + run < n && run < 128 && s[i + run] == s[i]) ++run;
        if (run >= 2) {
            o.push_back(std::uint8_t(257 - run));
            o.push_back(s[i]);
            i += run;
            continue;
        }
        int lit = 1;
        while (i + lit < n && lit < 128 && !(i + lit + 1 < n && s[i + lit] == s[i + lit + 1])) ++lit;
        o.push_back(std::uint8_t(lit - 1));
        o.insert(o.end(), s + i, s + i + lit);
        i += lit;
    }
}

bool unpackBits(Reader& r, std::size_t bytes, std::uint8_t* out, int n) {
    const std::size_t end = r.at + bytes;
    int k = 0;
    while (r.at < end && k < n && !r.bad) {
        const int h = int(std::int8_t(r.u8()));
        if (h >= 0) {
            const int c = h + 1;
            if (!r.need(std::size_t(c))) return false;
            const int take = std::min(c, n - k);
            std::memcpy(out + k, r.p + r.at, std::size_t(take));
            r.at += std::size_t(c);
            k += take;
        } else if (h != -128) {
            const int c = 1 - h;
            const std::uint8_t b = std::uint8_t(r.u8());
            const int take = std::min(c, n - k);
            std::memset(out + k, b, std::size_t(take));
            k += take;
        }
    }
    r.at = end;
    return !r.bad;
}

// One channel of one layer into a full-canvas buffer (stride 4, byte `slot`).
bool readChannel(Reader& r, std::size_t len, int lw, int lh, int left, int top, int W, int H,
                 std::uint8_t* dst, int slot) {
    const std::size_t start = r.at, end = start + len;
    if (len < 2 || end > r.n) { r.at = std::min(end, r.n); return len == 0; }
    const std::uint32_t comp = r.u16();
    std::vector<std::uint8_t> row(std::size_t(std::max(lw, 0)));
    auto put = [&](int y) {
        const int ty = top + y;
        if (ty < 0 || ty >= H) return;
        for (int x = 0; x < lw; ++x) {
            const int tx = left + x;
            if (tx >= 0 && tx < W) dst[(std::size_t(ty) * W + tx) * 4 + slot] = row[std::size_t(x)];
        }
    };
    bool ok = true;
    if (lw <= 0 || lh <= 0) {
    } else if (comp == 0) {
        for (int y = 0; y < lh && ok; ++y) {
            if (!r.need(std::size_t(lw))) { ok = false; break; }
            std::memcpy(row.data(), r.p + r.at, std::size_t(lw));
            r.at += std::size_t(lw);
            put(y);
        }
    } else if (comp == 1) {
        std::vector<std::uint32_t> counts(static_cast<std::size_t>(lh));
        for (int y = 0; y < lh; ++y) counts[std::size_t(y)] = r.u16();
        for (int y = 0; y < lh && ok; ++y) {
            std::fill(row.begin(), row.end(), std::uint8_t(0));
            ok = unpackBits(r, counts[std::size_t(y)], row.data(), lw);
            put(y);
        }
    } else {
        ok = false;   // ZIP: written by some programs, not read here
    }
    r.at = end;
    return ok && !r.bad;
}

std::string utf8OfUtf16(const std::vector<std::uint16_t>& u) {
    std::string s;
    for (std::size_t i = 0; i < u.size(); ++i) {
        std::uint32_t c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < u.size()) c = 0x10000 + ((c - 0xD800) << 10) + (u[++i] - 0xDC00);
        if (c == 0) break;
        if (c < 0x80) s += char(c);
        else if (c < 0x800) { s += char(0xC0 | (c >> 6)); s += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { s += char(0xE0 | (c >> 12)); s += char(0x80 | ((c >> 6) & 63)); s += char(0x80 | (c & 63)); }
        else { s += char(0xF0 | (c >> 18)); s += char(0x80 | ((c >> 12) & 63)); s += char(0x80 | ((c >> 6) & 63)); s += char(0x80 | (c & 63)); }
    }
    return s;
}

std::vector<std::uint16_t> utf16OfUtf8(const std::string& s) {
    std::vector<std::uint16_t> u;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = c;
        int len = 1;
        if (c >= 0xF0) { cp = c & 7; len = 4; } else if (c >= 0xE0) { cp = c & 15; len = 3; } else if (c >= 0xC0) { cp = c & 31; len = 2; }
        if (i + std::size_t(len) > s.size()) { cp = '?'; len = 1; }
        for (int k = 1; k < len; ++k) cp = (cp << 6) | (static_cast<unsigned char>(s[i + std::size_t(k)]) & 63);
        i += std::size_t(len);
        if (cp >= 0x10000) { cp -= 0x10000; u.push_back(std::uint16_t(0xD800 + (cp >> 10))); u.push_back(std::uint16_t(0xDC00 + (cp & 1023))); }
        else u.push_back(std::uint16_t(cp));
    }
    return u;
}

} // namespace

bool readPsd(const std::vector<unsigned char>& bytes, PsdLayers& out, std::string& err) {
    Reader r{bytes.data(), bytes.size()};
    if (r.str(4) != "8BPS" || r.u16() != 1) { err = "not a PSD (or a PSB)"; return false; }
    r.skip(6);
    r.u16();   // channels of the merged picture
    const int H = int(r.u32()), W = int(r.u32());
    const std::uint32_t depth = r.u16(), mode = r.u16();
    if (r.bad || W <= 0 || H <= 0 || W > 30000 || H > 30000) { err = "a broken PSD header"; return false; }
    if (depth != 8 || mode != 3) { err = "only 8-bit RGB PSDs keep their layers here"; return false; }
    r.skip(r.u32());   // colour mode data
    r.skip(r.u32());   // image resources
    const std::uint32_t lmLen = r.u32();
    if (r.bad || lmLen == 0) { err = "no layers"; return false; }
    const std::size_t liLen = r.u32();
    if (r.bad || liLen == 0) { err = "no layers"; return false; }
    const int count = std::abs(int(r.i16()));

    struct Chan { int id; std::size_t len; };
    struct Rec {
        int top, left, bottom, right;
        std::vector<Chan> ch;
        std::string key, name;
        int opacity = 255, flags = 0, section = 0;
    };
    std::vector<Rec> recs(static_cast<std::size_t>(count));
    for (Rec& L : recs) {
        L.top = r.i32(); L.left = r.i32(); L.bottom = r.i32(); L.right = r.i32();
        const int nch = int(r.u16());
        for (int c = 0; c < nch && !r.bad; ++c) { const int id = r.i16(); L.ch.push_back({id, std::size_t(r.u32())}); }
        r.str(4);   // "8BIM"
        L.key = r.str(4);
        L.opacity = int(r.u8());
        r.u8();     // clipping
        L.flags = int(r.u8());
        r.u8();
        const std::size_t extra = r.u32(), extraEnd = r.at + extra;
        r.skip(r.u32());   // layer mask
        r.skip(r.u32());   // blending ranges
        const std::size_t nameLen = r.u8();
        L.name = r.str(nameLen);
        r.skip((4 - (nameLen + 1) % 4) % 4);
        while (!r.bad && r.at + 12 <= extraEnd) {
            const std::string sig = r.str(4), key = r.str(4);
            if (sig != "8BIM" && sig != "8B64") break;
            const std::size_t len = r.u32(), blockEnd = r.at + len;
            if (key == "luni") {
                const std::uint32_t nc = r.u32();
                std::vector<std::uint16_t> u;
                for (std::uint32_t k = 0; k < nc && !r.bad && r.at + 2 <= blockEnd; ++k) u.push_back(std::uint16_t(r.u16()));
                L.name = utf8OfUtf16(u);
            } else if (key == "lsct" || key == "lsdk") {
                L.section = int(r.u32());
            }
            r.at = blockEnd + (len & 1);   // blocks are padded to even (most writers pad to 4 inside len)
        }
        r.at = extraEnd;
    }
    if (r.bad) { err = "a broken layer record"; return false; }

    out.w = W; out.h = H;
    out.layers.clear();
    std::vector<std::shared_ptr<Pixels>> px(recs.size());
    for (std::size_t i = 0; i < recs.size(); ++i) {
        const Rec& L = recs[i];
        auto p = std::make_shared<Pixels>(std::size_t(W) * H * 4, 0);
        bool hasAlpha = false;
        for (const Chan& c : L.ch) {
            const int slot = c.id == -1 ? 3 : c.id >= 0 && c.id <= 2 ? c.id : -1;
            if (slot < 0) { r.at += c.len; continue; }     // masks
            hasAlpha |= slot == 3;
            if (!readChannel(r, c.len, L.right - L.left, L.bottom - L.top, L.left, L.top, W, H, p->data(), slot)) {
                err = "layer pixels this reader does not know (ZIP-compressed?)";
                return false;
            }
        }
        if (!hasAlpha) {
            // No transparency channel: the layer's rectangle is opaque.
            for (int y = std::max(0, L.top); y < std::min(H, L.bottom); ++y)
                for (int x = std::max(0, L.left); x < std::min(W, L.right); ++x) (*p)[(std::size_t(y) * W + x) * 4 + 3] = 255;
        }
        px[i] = std::move(p);
    }
    // Groups: walk from the top down, a hidden group hides everything in it.
    std::vector<bool> hiddenBy(recs.size(), false);
    std::vector<bool> stack;
    for (int i = int(recs.size()) - 1; i >= 0; --i) {
        const Rec& L = recs[std::size_t(i)];
        const bool parent = !stack.empty() && stack.back();
        if (L.section == 1 || L.section == 2) stack.push_back(parent || (L.flags & 2));
        else if (L.section == 3) { if (!stack.empty()) stack.pop_back(); }
        else hiddenBy[std::size_t(i)] = parent;
    }
    for (std::size_t i = 0; i < recs.size(); ++i) {
        const Rec& L = recs[i];
        if (L.section != 0) continue;
        Layer l;
        l.name = L.name.empty() ? "Layer " + std::to_string(out.layers.size()) : L.name;
        l.px = px[i];
        l.opacity = L.opacity / 255.0f;
        l.visible = !(L.flags & 2) && !hiddenBy[i];
        l.lockAlpha = (L.flags & 1) != 0;
        l.blend = blendOfKey(L.key);
        out.layers.push_back(std::move(l));
    }
    if (out.layers.empty()) { err = "no layers"; return false; }
    return true;
}

bool writePsd(const PsdLayers& in, const Pixels& merged, std::vector<unsigned char>& o) {
    const int W = in.w, H = in.h;
    if (W <= 0 || H <= 0 || merged.size() != std::size_t(W) * H * 4) return false;
    o.clear();
    Writer w{o};
    w.str("8BPS");
    w.u16(1);
    w.raw("\0\0\0\0\0\0", 6);
    w.u16(4);   // RGBA
    w.u32(std::uint32_t(H));
    w.u32(std::uint32_t(W));
    w.u16(8);
    w.u16(3);   // RGB
    w.u32(0);   // colour mode data
    w.u32(0);   // image resources

    // One channel of a layer, PackBits row by row, with its byte counts first.
    auto encode = [&](const Pixels& p, int slot, std::vector<unsigned char>& dst) {
        std::vector<unsigned char> data;
        std::vector<std::uint8_t> row(static_cast<std::size_t>(W)), counts;
        dst.clear();
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) row[std::size_t(x)] = p[(std::size_t(y) * W + x) * 4 + slot];
            const std::size_t before = data.size();
            packBits(row.data(), W, data);
            const std::size_t n = data.size() - before;
            counts.push_back(std::uint8_t(n >> 8));
            counts.push_back(std::uint8_t(n));
        }
        dst.insert(dst.end(), counts.begin(), counts.end());
        dst.insert(dst.end(), data.begin(), data.end());
    };

    const std::size_t lm = w.mark();   // layer and mask information
    const std::size_t li = w.mark();   // layer info
    w.u16(std::uint16_t(-std::int16_t(in.layers.size())));   // negative: the merged alpha is transparency
    static const int kChannels[4] = {-1, 0, 1, 2};
    std::vector<std::vector<unsigned char>> chans;
    for (const Layer& l : in.layers) {
        for (int c = 0; c < 4; ++c) {
            chans.emplace_back();
            encode(*l.px, c == 0 ? 3 : c - 1, chans.back());
        }
    }
    for (std::size_t i = 0; i < in.layers.size(); ++i) {
        const Layer& l = in.layers[i];
        w.u32(0); w.u32(0); w.u32(std::uint32_t(H)); w.u32(std::uint32_t(W));
        w.u16(4);
        for (int c = 0; c < 4; ++c) {
            w.u16(std::uint16_t(kChannels[c]));
            w.u32(std::uint32_t(2 + chans[i * 4 + std::size_t(c)].size()));
        }
        w.str("8BIM");
        w.str(kBlendKeys[std::clamp(int(l.blend), 0, int(Blend::Count) - 1)]);
        w.u8(std::uint32_t(std::clamp(l.opacity, 0.0f, 1.0f) * 255.0f + 0.5f));
        w.u8(0);
        w.u8((l.lockAlpha ? 1 : 0) | (l.visible ? 0 : 2) | 8);
        w.u8(0);
        const std::size_t extra = w.mark();
        w.u32(0);   // no mask
        w.u32(0);   // no blending ranges
        std::string ascii;
        for (char ch : l.name) ascii += (static_cast<unsigned char>(ch) < 0x80) ? ch : '?';
        if (ascii.size() > 255) ascii.resize(255);
        w.u8(std::uint32_t(ascii.size()));
        w.str(ascii.c_str());
        for (std::size_t k = (ascii.size() + 1) % 4; k && k < 4; ++k) w.u8(0);
        // The name in Unicode, as Photoshop writes it.
        const std::vector<std::uint16_t> u = utf16OfUtf8(l.name);
        w.str("8BIM");
        w.str("luni");
        std::size_t len = 4 + u.size() * 2;
        const std::size_t pad = (4 - len % 4) % 4;
        w.u32(std::uint32_t(len + pad));
        w.u32(std::uint32_t(u.size()));
        for (std::uint16_t c : u) w.u16(c);
        for (std::size_t k = 0; k < pad; ++k) w.u8(0);
        w.patch(extra);
    }
    for (const auto& c : chans) { w.u16(1); w.raw(c.data(), c.size()); }
    if ((o.size() - li - 4) & 1) w.u8(0);
    w.patch(li);
    w.u32(0);   // global layer mask info
    w.patch(lm);

    // The merged picture, matted on white the way Photoshop stores it.
    Pixels flat = merged;
    for (std::size_t i = 0; i < flat.size(); i += 4) {
        const int a = flat[i + 3];
        for (int c = 0; c < 3; ++c) flat[i + std::size_t(c)] = std::uint8_t((flat[i + std::size_t(c)] * a + 255 * (255 - a) + 127) / 255);
    }
    w.u16(1);
    std::vector<std::vector<unsigned char>> planes(4);
    std::vector<unsigned char> countsAll, dataAll;
    std::vector<std::uint8_t> row(static_cast<std::size_t>(W));
    for (int c = 0; c < 4; ++c)
        for (int y = 0; y < H; ++y) {
            for (int x = 0; x < W; ++x) row[std::size_t(x)] = flat[(std::size_t(y) * W + x) * 4 + std::size_t(c)];
            const std::size_t before = dataAll.size();
            packBits(row.data(), W, dataAll);
            const std::size_t n = dataAll.size() - before;
            countsAll.push_back(std::uint8_t(n >> 8));
            countsAll.push_back(std::uint8_t(n));
        }
    w.raw(countsAll.data(), countsAll.size());
    w.raw(dataAll.data(), dataAll.size());
    return true;
}

} // namespace img
