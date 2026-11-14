// qrcheck -- QR codes (QrEncode.hpp) and the objects made of them (QrCode.hpp),
// without GL.
//
// A QR code that is wrong by one bit still looks like a QR code -- it just does
// not scan, and nothing in the editor says so. So every code made here is READ
// BACK by a reader written for this check: the format bits found and both
// copies agreeing, the codewords taken off the grid round the function
// patterns, unmasked, de-interleaved, every block's Reed-Solomon syndromes
// zero (a different sum from the division that made them), the text parsed
// and compared. Next to it the standard's own numbers: format and version bits,
// the "HELLO WORLD" codewords of the standard's tutorial, alignment positions,
// capacities -- every version at every level filled to the last byte.
//
// The object: the dark modules closed (every edge crossed once each way, so no
// wall stands between two neighbours and no corner is shared diagonally), its
// volume the modules' volume less the pinched corners, its top seen from above
// the code itself and not its mirror, no two corners within the 3MF export's
// weld; the plate a slab; and through File > Export as 3MF two closed parts, two
// colours, the size it says on Z 0.
//
// With an out dir it writes PNGs (every code, and the object's top as drawn
// from its triangles) and qr-manifest.tsv (file, expected text in hex) for an
// independent reader to decode -- tools/qrdecode.py with zxing-cpp -- and
// qr.3mf for a slicer.
//   build/linux/bin/qrcheck [outdir]

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/EditMesh.hpp"
#include "../src/Export3mf.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/QrCode.hpp"
#include "../src/QrEncode.hpp"
#include "../src/SceneGraph.hpp"

using qrencode::Ecc;

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}
std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}
std::string bin(std::uint32_t v, int n) {
    std::string s;
    for (int i = n - 1; i >= 0; --i) s += ((v >> i) & 1u) ? '1' : '0';
    return s;
}
std::string hex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string h;
    for (unsigned char c : s) { h += d[c >> 4]; h += d[c & 15]; }
    return h;
}

const Ecc kLevels[4] = {Ecc::Low, Ecc::Medium, Ecc::Quartile, Ecc::High};

// --- The reader ------------------------------------------------------------------

struct Read {
    bool        ok = false;
    std::string why;
    std::string text;
    Ecc         ecc = Ecc::Low;
    int         mask = -1;
};

// The modules no data runs through: the finders with their separators and the
// format areas, the timing lines, the alignment patterns, the version blocks.
bool reserved(int x, int y, int n, int version, const std::vector<int>& align) {
    if ((x <= 8 && y <= 8) || (x >= n - 8 && y <= 8) || (x <= 8 && y >= n - 8)) return true;
    if (x == 6 || y == 6) return true;
    const int k = static_cast<int>(align.size());
    for (int i = 0; i < k; ++i)
        for (int j = 0; j < k; ++j) {
            if ((i == 0 && j == 0) || (i == 0 && j == k - 1) || (i == k - 1 && j == 0)) continue;
            if (std::abs(x - align[static_cast<std::size_t>(i)]) <= 2 &&
                std::abs(y - align[static_cast<std::size_t>(j)]) <= 2)
                return true;
        }
    if (version >= 7 && ((x >= n - 11 && x <= n - 9 && y <= 5) || (y >= n - 11 && y <= n - 9 && x <= 5)))
        return true;
    return false;
}

// The standard's masks, written with i the row and j the column as it writes them.
bool masked(int m, int i, int j) {
    switch (m) {
    case 0:  return (i + j) % 2 == 0;
    case 1:  return i % 2 == 0;
    case 2:  return j % 3 == 0;
    case 3:  return (i + j) % 3 == 0;
    case 4:  return ((i / 2) + (j / 3)) % 2 == 0;
    case 5:  return (i * j) % 2 + (i * j) % 3 == 0;
    case 6:  return ((i * j) % 2 + (i * j) % 3) % 2 == 0;
    default: return ((i * j) % 3 + (i + j) % 2) % 2 == 0;
    }
}

// The value of a block at a^k, highest power first.
std::uint8_t evalAt(const std::vector<std::uint8_t>& c, std::uint8_t x) {
    std::uint8_t r = 0;
    for (std::uint8_t b : c) r = static_cast<std::uint8_t>(qrencode::gfMul(r, x) ^ b);
    return r;
}

Read readBack(const qrencode::Code& c) {
    Read r;
    const int n = c.size;
    const int v = (n - 17) / 4;
    if (n < 21 || (n - 17) % 4 || static_cast<int>(c.dark.size()) != n * n) { r.why = "not a symbol"; return r; }
    auto dark = [&](int x, int y) { return c.at(x, y); };

    // Finders, timing, the dark module.
    for (const glm::ivec2 o : {glm::ivec2(0, 0), glm::ivec2(n - 7, 0), glm::ivec2(0, n - 7)})
        for (int y = -1; y <= 7; ++y)
            for (int x = -1; x <= 7; ++x) {
                const int px = o.x + x, py = o.y + y;
                if (px < 0 || py < 0 || px >= n || py >= n) continue;
                const int d = std::max(std::abs(x - 3), std::abs(y - 3));
                if (dark(px, py) != (d != 2 && d != 4)) { r.why = "finder pattern"; return r; }
            }
    for (int k = 8; k < n - 8; ++k)
        if (dark(k, 6) != (k % 2 == 0) || dark(6, k) != (k % 2 == 0)) { r.why = "timing"; return r; }
    if (!dark(8, n - 8)) { r.why = "the dark module"; return r; }

    // Format: both copies, MSB first as the standard draws them.
    std::uint32_t f1 = 0, f2 = 0;
    const glm::ivec2 copy1[15] = {{0, 8}, {1, 8}, {2, 8}, {3, 8}, {4, 8}, {5, 8}, {7, 8}, {8, 8},
                                  {8, 7}, {8, 5}, {8, 4}, {8, 3}, {8, 2}, {8, 1}, {8, 0}};
    for (const glm::ivec2 p : copy1) f1 = f1 << 1 | (dark(p.x, p.y) ? 1u : 0u);
    for (int k = 0; k < 7; ++k) f2 = f2 << 1 | (dark(8, n - 1 - k) ? 1u : 0u);   // bits 14..8, up
    for (int k = 0; k < 8; ++k) f2 = f2 << 1 | (dark(n - 8 + k, 8) ? 1u : 0u);   // bits 7..0, right
    if (f1 != f2) { r.why = "the format copies differ: " + bin(f1, 15) + " " + bin(f2, 15); return r; }
    bool found = false;
    for (Ecc e : kLevels)
        for (int m = 0; m < 8; ++m)
            if (qrencode::formatBits(e, m) == f1) { r.ecc = e; r.mask = m; found = true; }
    if (!found) { r.why = "no format matches " + bin(f1, 15); return r; }

    if (v >= 7) {
        std::uint32_t a = 0, b = 0;
        for (int k = 17; k >= 0; --k) {
            a = a << 1 | (dark(k / 3, n - 11 + k % 3) ? 1u : 0u);   // bottom-left block
            b = b << 1 | (dark(n - 11 + k % 3, k / 3) ? 1u : 0u);   // top-right block
        }
        if (a != b || a != qrencode::versionBits(v)) { r.why = "version bits"; return r; }
    }

    // The codewords, up and down the column pairs from the bottom right.
    const std::vector<int> align = qrencode::alignmentPositions(v);
    std::vector<std::uint8_t> cw;
    std::uint8_t cur = 0;
    int nbits = 0, extra = 0, extraDark = 0;
    const int total = qrencode::rawCodewords(v);
    bool up = true;
    for (int col = n - 1; col > 0; col -= 2) {
        if (col == 6) --col;
        for (int s = 0; s < n; ++s) {
            const int y = up ? n - 1 - s : s;
            for (int x : {col, col - 1}) {
                if (reserved(x, y, n, v, align)) continue;
                const bool bit = dark(x, y) != masked(r.mask, y, x);
                if (static_cast<int>(cw.size()) < total) {
                    cur = static_cast<std::uint8_t>(cur << 1 | (bit ? 1 : 0));
                    if (++nbits == 8) { cw.push_back(cur); cur = 0; nbits = 0; }
                } else {
                    ++extra;
                    extraDark += bit ? 1 : 0;
                }
            }
        }
        up = !up;
    }
    if (static_cast<int>(cw.size()) != total) { r.why = "codeword count " + std::to_string(cw.size()); return r; }
    if (extraDark) { r.why = "remainder bits not light"; return r; }

    // Blocks: de-interleave, check the syndromes, gather the data.
    int blocks = 0, eccLen = 0;
    qrencode::blockLayout(v, r.ecc, blocks, eccLen);
    const int shortN = blocks - total % blocks, shortLen = total / blocks;
    std::vector<std::vector<std::uint8_t>> bs(static_cast<std::size_t>(blocks));
    std::size_t k = 0;
    for (int i = 0; i < shortLen - eccLen + 1; ++i)
        for (int b = 0; b < blocks; ++b)
            if (i < shortLen - eccLen || b >= shortN) bs[static_cast<std::size_t>(b)].push_back(cw[k++]);
    for (int i = 0; i < eccLen; ++i)
        for (int b = 0; b < blocks; ++b) bs[static_cast<std::size_t>(b)].push_back(cw[k++]);
    std::vector<std::uint8_t> data;
    for (int b = 0; b < blocks; ++b) {
        const auto& blk = bs[static_cast<std::size_t>(b)];
        std::uint8_t ak = 1;
        for (int s = 0; s < eccLen; ++s, ak = qrencode::gfMul(ak, 2))
            if (evalAt(blk, ak) != 0) { r.why = "syndrome " + std::to_string(s) + " of block " + std::to_string(b); return r; }
        data.insert(data.end(), blk.begin(), blk.end() - eccLen);
    }

    // The segments.
    std::size_t pos = 0;
    auto take = [&](int bits) {
        int val = 0;
        for (int i = 0; i < bits; ++i, ++pos) {
            if (pos >= data.size() * 8) return -1;
            val = val << 1 | ((data[pos >> 3] >> (7 - (pos & 7))) & 1);
        }
        return val;
    };
    const int range = v <= 9 ? 0 : v <= 26 ? 1 : 2;
    while (pos + 4 <= data.size() * 8) {
        const int mode = take(4);
        if (mode == 0) break;
        if (mode == 1) {
            int len = take(range == 0 ? 10 : range == 1 ? 12 : 14);
            for (; len >= 3; len -= 3) { char b[8]; std::snprintf(b, sizeof b, "%03d", take(10)); r.text += b; }
            if (len == 2) { char b[8]; std::snprintf(b, sizeof b, "%02d", take(7)); r.text += b; }
            if (len == 1) r.text += static_cast<char>('0' + take(4));
        } else if (mode == 2) {
            static const char* al = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";
            int len = take(range == 0 ? 9 : range == 1 ? 11 : 13);
            for (; len >= 2; len -= 2) { const int p = take(11); r.text += al[p / 45]; r.text += al[p % 45]; }
            if (len == 1) r.text += al[take(6)];
        } else if (mode == 4) {
            const int len = take(range == 0 ? 8 : 16);
            for (int i = 0; i < len; ++i) r.text += static_cast<char>(take(8));
        } else {
            r.why = "mode " + std::to_string(mode);
            return r;
        }
    }
    r.ok = true;
    return r;
}

// --- Pictures --------------------------------------------------------------------

struct Png {
    std::string dir;
    std::FILE*  manifest = nullptr;
    void put(const std::string& name, int w, int h, const std::vector<unsigned char>& px,
             const std::string& text) {
        if (dir.empty()) return;
        const std::string f = dir + "/" + name + ".png";
        stbi_write_png(f.c_str(), w, h, 1, px.data(), w);
        if (manifest) std::fprintf(manifest, "%s.png\t%s\n", name.c_str(), hex(text).c_str());
    }
    // A code with a four-module quiet zone, `scale` pixels a module.
    void code(const std::string& name, const qrencode::Code& c, const std::string& text, int scale = 4) {
        const int w = (c.size + 8) * scale;
        std::vector<unsigned char> px(static_cast<std::size_t>(w * w), 255);
        for (int y = 0; y < w; ++y)
            for (int x = 0; x < w; ++x)
                if (c.at(x / scale - 4, y / scale - 4)) px[static_cast<std::size_t>(y * w + x)] = 0;
        put(name, w, w, px, text);
    }
};

// The object's top, seen from above (x right, -z up): the faces that face
// up, filled where a pixel's middle falls inside, `k` pixels a metre from `lo`
// on both axes. Light where nothing.
std::vector<unsigned char> topView(const EditMesh& m, float lo, float width, float k, int& w) {
    w = static_cast<int>(std::ceil(width * k));
    std::vector<unsigned char> px(static_cast<std::size_t>(w * w), 255);
    for (const std::vector<int>& f : m.faces) {
        std::vector<glm::vec2> p;
        for (int i : f) p.emplace_back((m.verts[i].x - lo) * k, (m.verts[i].z - lo) * k);
        const glm::vec3 a = m.verts[f[0]], b = m.verts[f[1]], c = m.verts[f[2]];
        if (glm::cross(b - a, c - a).y <= 0.0f) continue;   // not facing up
        glm::vec2 mn(1e9f), mx(-1e9f);
        for (const glm::vec2& q : p) { mn = glm::min(mn, q); mx = glm::max(mx, q); }
        for (int y = std::max(0, static_cast<int>(mn.y)); y < std::min(w, static_cast<int>(mx.y) + 1); ++y)
            for (int x = std::max(0, static_cast<int>(mn.x)); x < std::min(w, static_cast<int>(mx.x) + 1); ++x) {
                const glm::vec2 s(x + 0.5f, y + 0.5f);
                // Seen from above with z down the picture the faces turn
                // clockwise: inside is right of every edge.
                bool in = true;
                for (std::size_t i = 0; i < p.size() && in; ++i) {
                    const glm::vec2 e = p[(i + 1) % p.size()] - p[i], d = s - p[i];
                    in = e.x * d.y - e.y * d.x <= 0.0f;
                }
                if (in) px[static_cast<std::size_t>(y * w + x)] = 0;
            }
    }
    return px;
}

// --- Meshes --------------------------------------------------------------------

// Closed and wound one way: every directed edge once, and its reverse once.
bool closed(const EditMesh& m, std::string& why) {
    std::map<std::pair<int, int>, int> edges;
    for (const std::vector<int>& f : m.faces)
        for (std::size_t k = 0; k < f.size(); ++k) ++edges[{f[k], f[(k + 1) % f.size()]}];
    for (const auto& [e, n] : edges) {
        const auto back = edges.find({e.second, e.first});
        if (n != 1 || back == edges.end() || back->second != 1) {
            why = "edge " + std::to_string(e.first) + "-" + std::to_string(e.second) + " x" + std::to_string(n);
            return false;
        }
    }
    return !m.faces.empty();
}

double volume(const EditMesh& m) {
    double s = 0.0;
    for (const std::vector<int>& f : m.faces)
        for (std::size_t k = 1; k + 1 < f.size(); ++k) {
            const glm::dvec3 a(m.verts[f[0]]), b(m.verts[f[k]]), c(m.verts[f[k + 1]]);
            s += glm::dot(a, glm::cross(b, c));
        }
    return s / 6.0;
}

// The closest two distinct corners.
double closestPair(const std::vector<glm::vec3>& v) {
    std::vector<glm::vec3> s = v;
    std::sort(s.begin(), s.end(), [](const glm::vec3& a, const glm::vec3& b) { return a.x < b.x; });
    double best = 1e30;
    for (std::size_t i = 0; i < s.size(); ++i)
        for (std::size_t j = i + 1; j < s.size() && s[j].x - s[i].x < best; ++j)
            best = std::min(best, static_cast<double>(glm::length(s[j] - s[i])));
    return best;
}

// Corners two dark modules share only diagonally.
int diagonals(const qrencode::Code& c) {
    int k = 0;
    for (int j = 0; j <= c.size; ++j)
        for (int i = 0; i <= c.size; ++i) {
            const bool a = c.at(i - 1, j - 1), b = c.at(i, j - 1), d = c.at(i - 1, j), e = c.at(i, j);
            if ((a && e && !b && !d) || (b && d && !a && !e)) ++k;
        }
    return k;
}

bool closedTris(const std::vector<glm::ivec3>& tris) {
    std::map<std::pair<int, int>, int> edges;
    for (const glm::ivec3& t : tris)
        for (int k = 0; k < 3; ++k) ++edges[{t[k], t[(k + 1) % 3]}];
    for (const auto& [e, n] : edges) {
        const auto back = edges.find({e.second, e.first});
        if (n != 1 || back == edges.end() || back->second != 1) return false;
    }
    return !tris.empty();
}

// One object built and measured: the code read back, the layout adding up,
// the modules (with the frame) closed, their volume, their heights, the plate,
// the corners apart, and the top seen from above -- the code module for
// module, light round it, the frame dark. `pngName`: also as a picture.
void objectChecks(const std::string& what, const qrcode::Params& p, Png& png, const std::string& pngName) {
    std::printf("  %s\n", what.c_str());
    const qrcode::Model m = qrcode::build(p);
    const qrcode::Layout& L = m.layout;
    const int n = m.code.size;
    const Read r = readBack(m.code);
    check(m.code.valid() && r.ok && r.text == p.text, "the code: version " + std::to_string(m.code.version));
    check(std::abs(2.0f * L.frame + L.pitch * static_cast<float>(n + 2 * p.border) - p.size) < 1e-6f &&
              std::abs(L.inset - L.frame - L.pitch * static_cast<float>(p.border)) < 1e-7f,
          "frame, quiet zone and modules fill the width", num(L.pitch * 1000.0) + " mm a module");
    std::string why;
    check(closed(m.modules, why), "the modules" + std::string(p.frame ? " and the frame" : "") +
          " are closed, every edge crossed once each way", why);
    int dark = 0;
    for (std::uint8_t d : m.code.dark) dark += d;
    const int diag = diagonals(m.code);
    const double p2 = static_cast<double>(L.pitch) * L.pitch;
    const double W = p.size, ring = p.frame ? W * W - (W - 2.0 * L.frame) * (W - 2.0 * L.frame) : 0.0;
    const double want = (dark * p2 - 2.0 * diag * qrcode::kPinch * p2 + ring) * p.relief;
    const double got = volume(m.modules);
    check(std::abs(got - want) < want * 1e-4, "their volume: the dark modules less " + std::to_string(2 * diag) +
          " pinched corners" + (p.frame ? ", and the frame" : ""), num(got * 1e9) + " mm3, want " + num(want * 1e9));
    float lo = 1e9f, ylo = 1e9f, yhi = -1e9f;
    for (const glm::vec3& v : m.modules.verts) {
        lo = std::min({lo, v.x, v.z});
        ylo = std::min(ylo, v.y);
        yhi = std::max(yhi, v.y);
    }
    const float base = p.background ? p.plate : 0.0f;
    check(std::abs(ylo - base) < 1e-7f && std::abs(yhi - base - p.relief) < 1e-7f,
          p.background ? "standing on the plate, as high as the relief" : "standing on the ground, as high as the relief",
          num(ylo * 1000.0) + ".." + num(yhi * 1000.0) + " mm");
    if (p.background)
        check(std::abs(volume(m.plate) - W * W * p.plate) < W * W * p.plate * 1e-5 && closed(m.plate, why),
              "the plate: a closed slab of the size", num(volume(m.plate) * 1e9) + " mm3");
    else
        check(m.plate.faces.empty(), "no plate");
    const double gap = closestPair(m.modules.verts);
    check(gap > 1e-6 * 10, "no two corners within ten times the 3MF weld", num(gap * 1e6) + " um");

    // Seen from above.
    const float o = -0.5f * p.size;
    const float k = 8.0f / L.pitch;   // eight pixels a module
    int w = 0;
    const std::vector<unsigned char> px = topView(m.modules, o, p.size, k, w);
    auto darkAt = [&](float x, float z) {
        const int i = static_cast<int>((x - o) * k), j = static_cast<int>((z - o) * k);
        return i >= 0 && j >= 0 && i < w && j < w && px[static_cast<std::size_t>(j * w + i)] == 0;
    };
    int wrong = 0, quiet = 0, rim = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            wrong += darkAt(o + L.inset + (x + 0.5f) * L.pitch, o + L.inset + (y + 0.5f) * L.pitch) != m.code.at(x, y);
    for (int y = -p.border; y < n + p.border; ++y)
        for (int x = -p.border; x < n + p.border; ++x)
            if ((x < 0 || y < 0 || x >= n || y >= n) &&
                darkAt(o + L.inset + (x + 0.5f) * L.pitch, o + L.inset + (y + 0.5f) * L.pitch))
                ++quiet;
    if (p.frame)
        for (int t = 0; t < 40; ++t) {
            const float a = o + (t + 0.5f) / 40.0f * p.size, e = o + 0.5f * L.frame, f = -o - 0.5f * L.frame;
            rim += !darkAt(a, e) + !darkAt(a, f) + !darkAt(e, a) + !darkAt(f, a);
        }
    check(wrong == 0 && quiet == 0 && rim == 0, "seen from above: the code module for module (not mirrored), "
          "the quiet zone light" + std::string(p.frame ? ", the frame dark" : ""),
          std::to_string(wrong) + " wrong, " + std::to_string(quiet) + " dark in the quiet zone, " +
              std::to_string(rim) + " light on the frame");
    if (!pngName.empty()) {
        // On white with a margin, for the outside reader.
        const int q = 32, Wp = w + 2 * q;
        std::vector<unsigned char> out(static_cast<std::size_t>(Wp * Wp), 255);
        for (int y = 0; y < w; ++y)
            for (int x = 0; x < w; ++x)
                out[static_cast<std::size_t>((y + q) * Wp + x + q)] = px[static_cast<std::size_t>(y * w + x)];
        png.put(pngName, Wp, Wp, out, p.text);
    }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Png png;
    if (argc > 1) {
        png.dir = argv[1];
        std::filesystem::create_directories(png.dir);
        png.manifest = std::fopen((png.dir + "/qr-manifest.tsv").c_str(), "w");
    }

    // --- The standard's numbers ------------------------------------------------------
    std::printf("The standard's numbers\n");
    {
        // Format bits as the standard tabulates them.
        const struct { Ecc e; int m; const char* bits; } fmt[] = {
            {Ecc::Low, 0, "111011111000100"},      {Ecc::Low, 1, "111001011110011"},
            {Ecc::Medium, 0, "101010000010010"},   {Ecc::Quartile, 0, "011010101011111"},
            {Ecc::High, 0, "001011010001001"},
        };
        for (const auto& f : fmt)
            check(bin(qrencode::formatBits(f.e, f.m), 15) == f.bits,
                  std::string("format bits ") + qrencode::eccLetter(f.e) + std::to_string(f.m),
                  bin(qrencode::formatBits(f.e, f.m), 15));
        check(qrencode::versionBits(7) == 0x07C94, "version bits of version 7", bin(qrencode::versionBits(7), 18));
        check(qrencode::versionBits(40) == 0x28C69, "version bits of version 40", bin(qrencode::versionBits(40), 18));

        const struct { int v; std::vector<int> pos; } al[] = {
            {1, {}}, {2, {6, 18}}, {7, {6, 22, 38}}, {14, {6, 26, 46, 66}},
            {32, {6, 34, 60, 86, 112, 138}}, {36, {6, 24, 50, 76, 102, 128, 154}},
            {40, {6, 30, 58, 86, 114, 142, 170}},
        };
        for (const auto& a : al)
            check(qrencode::alignmentPositions(a.v) == a.pos, "alignment positions of version " + std::to_string(a.v));

        // "HELLO WORLD" at 1-M: the tutorial's codewords.
        qrencode::Mode mode;
        const std::vector<std::uint8_t> d = qrencode::dataCodewordsOf("HELLO WORLD", Ecc::Medium, 1, mode);
        const std::vector<std::uint8_t> wantD = {32, 91, 11, 120, 209, 114, 220, 77, 67, 64, 236, 17, 236, 17, 236, 17};
        check(mode == qrencode::Mode::Alphanumeric && d == wantD, "HELLO WORLD packed alphanumeric, 1-M");
        const std::vector<std::uint8_t> wantE = {196, 35, 39, 119, 235, 215, 231, 226, 93, 23};
        check(qrencode::reedSolomon(d, 10) == wantE, "its ten error correction codewords");

        const struct { int v; int cap[4]; } caps[] = {
            {1, {17, 14, 11, 7}}, {2, {32, 26, 20, 14}}, {10, {271, 213, 151, 119}},
            {20, {858, 666, 482, 382}}, {30, {1732, 1370, 982, 742}}, {40, {2953, 2331, 1663, 1273}},
        };
        for (const auto& c : caps) {
            bool ok = true;
            std::string got;
            for (int l = 0; l < 4; ++l) {
                const int b = qrencode::byteCapacity(c.v, kLevels[l]);
                ok = ok && b == c.cap[l];
                got += std::to_string(b) + " ";
            }
            check(ok, "bytes in version " + std::to_string(c.v) + " at L M Q H", got);
        }
    }

    // --- Every version at every level, filled to the last byte -------------------------
    std::printf("Every version at every level\n");
    {
        int bad = 0, tried = 0;
        std::string first;
        for (int v = 1; v <= 40; ++v)
            for (Ecc e : kLevels) {
                const int cap = qrencode::byteCapacity(v, e);
                std::string t;
                for (int i = 0; i < cap; ++i) t += static_cast<char>('a' + (i * 7 + v) % 26);
                t[0] = '#';   // not alphanumeric
                const qrencode::Code c = qrencode::encode(t, e);
                const Read r = readBack(c);
                const qrencode::Code over = qrencode::encode(t + "x", e);
                ++tried;
                const bool ok = c.version == v && r.ok && r.text == t && r.ecc == e && r.mask == c.mask &&
                                (over.version == v + 1 || (v == 40 && !over.valid()));
                if (!ok) {
                    ++bad;
                    if (first.empty())
                        first = std::to_string(v) + "-" + qrencode::eccLetter(e) + ": version " +
                                std::to_string(c.version) + ", " + (r.ok ? "read" : r.why);
                }
                char name[32];
                std::snprintf(name, sizeof name, "qr-v%02d-%c", v, qrencode::eccLetter(e));
                png.code(name, c, t, v > 25 ? 3 : 4);
            }
        check(bad == 0, std::to_string(tried) + " codes: the version they fill, read back, one byte more is the next",
              first);
        check(!qrencode::encode(std::string(2954, 'a'), Ecc::Low).valid(), "2954 bytes do not fit at all");
    }

    // --- Texts --------------------------------------------------------------------------
    std::printf("Texts\n");
    {
        const struct { const char* name; std::string text; Ecc e; qrencode::Mode mode; } ts[] = {
            {"hello", "HELLO WORLD", Ecc::Quartile, qrencode::Mode::Alphanumeric},
            {"digits", "0123456789012345", Ecc::Medium, qrencode::Mode::Numeric},
            {"digits1", "7", Ecc::High, qrencode::Mode::Numeric},
            {"url", "https://pueski.de/fitzel/", Ecc::Medium, qrencode::Mode::Byte},
            {"umlauts", "Gr\xC3\xBC\xC3\x9F" "e aus Fitzel \xE2\x80\x93 \xC3\x84\xC3\x96\xC3\x9C \xC3\x9F \xE2\x82\xAC",
             Ecc::Medium, qrencode::Mode::Byte},
            {"wifi", "WIFI:T:WPA;S:Fitzel Gast;P:pa\\;ss;;", Ecc::High, qrencode::Mode::Byte},
            {"lines", "Zeile eins\nZeile zwei\n", Ecc::Low, qrencode::Mode::Byte},
            {"empty", "", Ecc::Medium, qrencode::Mode::Numeric},
        };
        for (const auto& t : ts) {
            const qrencode::Code c = qrencode::encode(t.text, t.e);
            const Read r = readBack(c);
            check(c.valid() && c.mode == t.mode && r.ok && r.text == t.text,
                  std::string(t.name) + ": " + qrencode::modeName(c.mode) + ", version " + std::to_string(c.version) +
                      ", mask " + std::to_string(c.mask),
                  r.ok ? "" : r.why);
            png.code(std::string("qr-") + t.name, c, t.text, 6);
        }
        // Every mask, forced.
        bool all = true;
        for (int m = 0; m < 8; ++m) {
            const qrencode::Code c = qrencode::encode("Fitzel QR mask test", Ecc::Medium, 1, m);
            const Read r = readBack(c);
            all = all && c.mask == m && r.ok && r.mask == m && r.text == "Fitzel QR mask test";
            png.code("qr-mask" + std::to_string(m), c, "Fitzel QR mask test", 6);
        }
        check(all, "each of the eight masks, forced, reads back");
        const qrencode::Code big = qrencode::encode("x", Ecc::Medium, 9);
        check(big.version == 9 && readBack(big).ok, "a minimum version is kept");
        // The mask chosen scores lowest.
        const qrencode::Code c = qrencode::encode("https://pueski.de", Ecc::Medium);
        int best = 1 << 30;
        for (int m = 0; m < 8; ++m) best = std::min(best, qrencode::penalty(qrencode::encode("https://pueski.de", Ecc::Medium, 1, m)));
        check(qrencode::penalty(c) == best, "the mask chosen has the lowest penalty", std::to_string(best));
    }

    // --- The object ----------------------------------------------------------------------
    std::printf("The object\n");
    const qrcode::Params def;
    {
        objectChecks("the default", def, png, "qr-mesh-top");
        qrcode::Params bare = def;
        bare.background = false;
        bare.relief = 0.004f;
        objectChecks("no background", bare, png, "");
        qrcode::Params framed = def;
        framed.frame = true;
        framed.frameWidth = 0.003f;
        objectChecks("a 3 mm frame", framed, png, "qr-mesh-frame");
        framed.border = 0;
        framed.frameWidth = 0.0015f;
        objectChecks("a frame touching the code (no quiet zone)", framed, png, "");
        framed.border = 2;
        framed.background = false;
        objectChecks("a frame, no background", framed, png, "qr-mesh-frame-bare");
        std::string why;
        // A big one stays closed.
        qrcode::Params large = def;
        large.text = std::string(1200, 'q');
        large.ecc = Ecc::High;
        const qrcode::Model l = qrcode::build(large);
        check(l.code.version >= 35 && closed(l.modules, why),
              "version " + std::to_string(l.code.version) + " closed too", num(static_cast<double>(l.modules.faces.size())) + " faces");
        qrcode::Params tooLong = def;
        tooLong.text = std::string(3000, 'q');
        int counter = 1;
        std::vector<MaterialDef> mats;
        check(qrcode::entities(tooLong, qrcode::ensurePalette(mats, tooLong), counter, glm::vec3(0.0f)).empty(),
              "a text too long: no object");
    }

    // --- Saved and loaded ---------------------------------------------------------------
    std::printf("Saved and loaded\n");
    {
        qrcode::Params p;
        p.text = "Fitzel \xE2\x9C\x93";
        p.ecc = Ecc::Quartile;
        p.size = 0.25f;
        p.border = 2;
        p.background = false;
        p.plate = 0.0042f;
        p.relief = 0.0035f;
        p.frame = true;
        p.frameWidth = 0.007f;
        p.plateColor = {0.1f, 0.2f, 0.3f};
        p.inkColor = {0.9f, 0.8f, 0.7f};
        nlohmann::json j;
        qrcode::saveParams(j, p);
        qrcode::Params q;
        qrcode::loadParams(nlohmann::json::parse(j.dump()), q);
        check(q == p, "every parameter comes back", j.dump());
        qrcode::Params d;
        qrcode::loadParams(nlohmann::json::object(), d);
        check(d == qrcode::Params{}, "nothing saved: the defaults");
        qrcode::Params old;
        qrcode::loadParams(nlohmann::json::parse(R"({"plate": 0.0, "relief": 0.002})"), old);
        check(!old.background && old.plate > 0.0f, "a code saved before the switch with a plate of 0: no background");
        std::unique_ptr<ComponentBase> c = components::create("qrCode");
        check(c && dynamic_cast<QrCodeComponent*>(c.get()), "the component is registered as qrCode");
    }

    // --- Through the 3MF export -----------------------------------------------------------
    std::printf("Through the 3MF export\n");
    {
        ModelLibrary models;
        Document doc;
        int counter = 1;
        const qrcode::Palette pal = qrcode::ensurePalette(doc.materials(), def);
        std::vector<Entity> es = qrcode::entities(def, pal, counter, glm::vec3(3.0f, 1.0f, -2.0f));
        check(es.size() == 3 && es[0].components.get<QrCodeComponent>() && es[1].name == "Plate" &&
                  es[2].name == "Code",
              "a root keeping the parameters, a plate and a code", es.empty() ? "" : es[0].name);
        const int root = es.empty() ? -1 : es[0].id;
        for (Entity& e : es) doc.entities().push_back(std::move(e));
        scenegraph::resolve(doc.entities());
        const export3mf::Shape s = export3mf::gather(doc, models, {root});
        bool allClosed = s.parts.size() == 2;
        for (const export3mf::Part& p : s.parts) allClosed = allClosed && closedTris(p.tris);
        check(allClosed, "two parts, both closed after the weld");
        check(s.colors.size() == 2, "two colours, one filament each", std::to_string(s.colors.size()));
        export3mf::Options o;
        const glm::vec3 mm = export3mf::printSize(s, o);
        check(std::abs(mm.x - 60.0f) < 1e-3f && std::abs(mm.y - 60.0f) < 1e-3f && std::abs(mm.z - 3.0f) < 1e-3f,
              "60 x 60 x 3 mm on the bed", num(mm.x) + " x " + num(mm.y) + " x " + num(mm.z));
        if (!png.dir.empty()) {
            std::string err;
            check(export3mf::write(png.dir + "/qr.3mf", s, o, err), "written as " + png.dir + "/qr.3mf", err);
        }
    }

    if (png.manifest) std::fclose(png.manifest);
    std::printf("\n%s (%d failed)\n", failures ? "FAILED" : "all ok", failures);
    return failures ? 1 : 0;
}
