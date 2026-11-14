#include "QrEncode.hpp"

#include <algorithm>
#include <climits>
#include <cstdlib>

namespace qrencode {

namespace {

// --- The standard's tables (ISO/IEC 18004, table 9), by level then version ------

// Error correction codewords per block.
constexpr std::int8_t kEccPerBlock[4][41] = {
    {-1,  7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28,
         28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},   // L
    {-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26,
         26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28},   // M
    {-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30,
         28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},   // Q
    {-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28,
         30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30},   // H
};

// Error correction blocks.
constexpr std::int8_t kBlocks[4][41] = {
    {-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8,
         8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25},       // L
    {-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16,
         17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49},    // M
    {-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20,
         23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68},    // Q
    {-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25,
         25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81},    // H
};

int lvl(Ecc e) { return std::clamp(static_cast<int>(e), 0, 3); }

// The two format bits of a level: not in the order of the enum.
int formatLevel(Ecc e) {
    static constexpr int bits[4] = {1, 0, 3, 2};   // L M Q H
    return bits[lvl(e)];
}

const char* kAlnum = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

int alnumValue(char c) {
    for (int i = 0; i < 45; ++i) if (kAlnum[i] == c) return i;
    return -1;
}

Mode modeFor(const std::string& s) {
    bool digits = true, alnum = true;
    for (char c : s) {
        if (c < '0' || c > '9') digits = false;
        if (alnumValue(c) < 0) alnum = false;
    }
    if (digits) return Mode::Numeric;
    if (alnum) return Mode::Alphanumeric;
    return Mode::Byte;
}

int modeIndicator(Mode m) {
    switch (m) {
    case Mode::Numeric:      return 0x1;
    case Mode::Alphanumeric: return 0x2;
    default:                 return 0x4;
    }
}

// Bits of the character count, by mode and version range.
int countBits(Mode m, int version) {
    const int r = version <= 9 ? 0 : version <= 26 ? 1 : 2;
    static constexpr int bits[3][3] = {{10, 12, 14}, {9, 11, 13}, {8, 16, 16}};
    return bits[static_cast<int>(m)][r];
}

// Bits the text itself takes, header not counted.
long payloadBits(Mode m, std::size_t n) {
    const long len = static_cast<long>(n);
    switch (m) {
    case Mode::Numeric:      return len / 3 * 10 + (len % 3 == 2 ? 7 : len % 3 == 1 ? 4 : 0);
    case Mode::Alphanumeric: return len / 2 * 11 + (len % 2) * 6;
    default:                 return len * 8;
    }
}

struct Bits {
    std::vector<bool> v;
    void put(std::uint32_t val, int n) {
        for (int i = n - 1; i >= 0; --i) v.push_back(((val >> i) & 1u) != 0);
    }
};

// --- The symbol under construction --------------------------------------------

struct Grid {
    int n;
    std::vector<std::uint8_t> dark, fixed;   // fixed: a function pattern, not data
    explicit Grid(int size) : n(size), dark(static_cast<std::size_t>(size * size), 0),
                              fixed(static_cast<std::size_t>(size * size), 0) {}
    void set(int x, int y, bool d) {
        const std::size_t i = static_cast<std::size_t>(y * n + x);
        dark[i]  = d ? 1 : 0;
        fixed[i] = 1;
    }
    bool isFixed(int x, int y) const { return fixed[static_cast<std::size_t>(y * n + x)] != 0; }
};

void finder(Grid& g, int cx, int cy) {
    for (int dy = -4; dy <= 4; ++dy)
        for (int dx = -4; dx <= 4; ++dx) {
            const int x = cx + dx, y = cy + dy;
            if (x < 0 || y < 0 || x >= g.n || y >= g.n) continue;
            const int d = std::max(std::abs(dx), std::abs(dy));
            g.set(x, y, d != 2 && d != 4);   // 4 is the light separator
        }
}

void alignment(Grid& g, int cx, int cy) {
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            g.set(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
}

void drawFormat(Grid& g, Ecc ecc, int mask) {
    const std::uint32_t bits = formatBits(ecc, mask);
    auto bit = [&](int i) { return ((bits >> i) & 1u) != 0; };
    const int n = g.n;
    // Round the top-left finder: up column 8, then left along row 8.
    for (int i = 0; i <= 5; ++i) g.set(8, i, bit(i));
    g.set(8, 7, bit(6));
    g.set(8, 8, bit(7));
    g.set(7, 8, bit(8));
    for (int i = 9; i < 15; ++i) g.set(14 - i, 8, bit(i));
    // The second copy, split between the other two finders.
    for (int i = 0; i < 8; ++i) g.set(n - 1 - i, 8, bit(i));
    for (int i = 8; i < 15; ++i) g.set(8, n - 15 + i, bit(i));
    g.set(8, n - 8, true);   // the one module that is always dark
}

void drawVersion(Grid& g, int version) {
    if (version < 7) return;
    const std::uint32_t bits = versionBits(version);
    for (int i = 0; i < 18; ++i) {
        const bool d = ((bits >> i) & 1u) != 0;
        const int a = g.n - 11 + i % 3, b = i / 3;
        g.set(a, b, d);   // above the bottom-left finder ...
        g.set(b, a, d);   // ... and left of the top-right one
    }
}

void drawFunctions(Grid& g, int version, Ecc ecc) {
    const int n = g.n;
    for (int i = 0; i < n; ++i) {
        g.set(6, i, i % 2 == 0);   // timing
        g.set(i, 6, i % 2 == 0);
    }
    finder(g, 3, 3);
    finder(g, n - 4, 3);
    finder(g, 3, n - 4);
    const std::vector<int> pos = alignmentPositions(version);
    const int k = static_cast<int>(pos.size());
    for (int i = 0; i < k; ++i)
        for (int j = 0; j < k; ++j) {
            // Not on the three finders.
            if ((i == 0 && j == 0) || (i == 0 && j == k - 1) || (i == k - 1 && j == 0)) continue;
            alignment(g, pos[static_cast<std::size_t>(i)], pos[static_cast<std::size_t>(j)]);
        }
    drawFormat(g, ecc, 0);   // reserves the format area; the real bits come with the mask
    drawVersion(g, version);
}

// The data and error correction codewords, split into blocks, each block given
// its error correction, and read out interleaved.
std::vector<std::uint8_t> interleave(const std::vector<std::uint8_t>& data, int version, Ecc ecc) {
    const int blocks   = kBlocks[lvl(ecc)][version];
    const int eccLen   = kEccPerBlock[lvl(ecc)][version];
    const int raw      = rawCodewords(version);
    const int shortN   = blocks - raw % blocks;   // blocks one data codeword shorter
    const int shortLen = raw / blocks;             // a short block, ecc included
    std::vector<std::vector<std::uint8_t>> bs;
    std::size_t k = 0;
    for (int i = 0; i < blocks; ++i) {
        const int dl = shortLen - eccLen + (i < shortN ? 0 : 1);
        std::vector<std::uint8_t> b(data.begin() + static_cast<long>(k),
                                    data.begin() + static_cast<long>(k + static_cast<std::size_t>(dl)));
        k += static_cast<std::size_t>(dl);
        const std::vector<std::uint8_t> e = reedSolomon(b, eccLen);
        if (i < shortN) b.push_back(0);   // a hole, skipped below
        b.insert(b.end(), e.begin(), e.end());
        bs.push_back(std::move(b));
    }
    std::vector<std::uint8_t> out;
    out.reserve(static_cast<std::size_t>(raw));
    for (std::size_t i = 0; i < bs[0].size(); ++i)
        for (int j = 0; j < blocks; ++j)
            if (i != static_cast<std::size_t>(shortLen - eccLen) || j >= shortN)
                out.push_back(bs[static_cast<std::size_t>(j)][i]);
    return out;
}

// Up and down the two-module columns from the bottom right, round every
// function pattern, skipping the vertical timing column.
void placeData(Grid& g, const std::vector<std::uint8_t>& cw) {
    const int n = g.n;
    std::size_t i = 0;
    const std::size_t total = cw.size() * 8;
    for (int right = n - 1; right >= 1; right -= 2) {
        if (right == 6) right = 5;
        const bool upward = ((right + 1) & 2) == 0;
        for (int v = 0; v < n; ++v)
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const int y = upward ? n - 1 - v : v;
                if (g.isFixed(x, y)) continue;
                // Left over past the last codeword: the remainder bits, light.
                bool d = false;
                if (i < total) d = ((cw[i >> 3] >> (7 - (i & 7))) & 1) != 0;
                ++i;
                g.dark[static_cast<std::size_t>(y * n + x)] = d ? 1 : 0;
            }
    }
}

bool maskBit(int mask, int x, int y) {
    switch (mask) {
    case 0:  return (x + y) % 2 == 0;
    case 1:  return y % 2 == 0;
    case 2:  return x % 3 == 0;
    case 3:  return (x + y) % 3 == 0;
    case 4:  return (x / 3 + y / 2) % 2 == 0;
    case 5:  return x * y % 2 + x * y % 3 == 0;
    case 6:  return (x * y % 2 + x * y % 3) % 2 == 0;
    default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
    }
}

void applyMask(Grid& g, int mask) {
    for (int y = 0; y < g.n; ++y)
        for (int x = 0; x < g.n; ++x)
            if (!g.isFixed(x, y) && maskBit(mask, x, y))
                g.dark[static_cast<std::size_t>(y * g.n + x)] ^= 1;
}

} // namespace

// --- Names ----------------------------------------------------------------------

const char* eccName(Ecc e) {
    static const char* n[4] = {"L (7 %)", "M (15 %)", "Q (25 %)", "H (30 %)"};
    return n[lvl(e)];
}

char eccLetter(Ecc e) { return "LMQH"[lvl(e)]; }

const char* modeName(Mode m) {
    switch (m) {
    case Mode::Numeric:      return "digits";
    case Mode::Alphanumeric: return "alphanumeric";
    default:                 return "bytes";
    }
}

// --- Sizes ----------------------------------------------------------------------

int rawCodewords(int v) {
    int bits = (16 * v + 128) * v + 64;
    if (v >= 2) {
        const int a = v / 7 + 2;
        bits -= (25 * a - 10) * a - 55;   // alignment patterns and their timing overlap
        if (v >= 7) bits -= 36;           // two version blocks
    }
    return bits / 8;
}

int dataCodewords(int v, Ecc ecc) {
    if (v < 1 || v > 40) return 0;
    return rawCodewords(v) - kEccPerBlock[lvl(ecc)][v] * kBlocks[lvl(ecc)][v];
}

void blockLayout(int v, Ecc ecc, int& blocks, int& eccPerBlock) {
    v = std::clamp(v, 1, 40);
    blocks      = kBlocks[lvl(ecc)][v];
    eccPerBlock = kEccPerBlock[lvl(ecc)][v];
}

int byteCapacity(int v, Ecc ecc) {
    if (v < 1 || v > 40) return 0;
    return (dataCodewords(v, ecc) * 8 - 4 - countBits(Mode::Byte, v)) / 8;
}

std::vector<int> alignmentPositions(int v) {
    if (v < 2) return {};
    const int a    = v / 7 + 2;
    const int size = 17 + 4 * v;
    const int step = v == 32 ? 26 : (v * 4 + a * 2 + 1) / (a * 2 - 2) * 2;
    std::vector<int> out(static_cast<std::size_t>(a));
    out[0] = 6;
    for (int i = a - 1, p = size - 7; i >= 1; --i, p -= step) out[static_cast<std::size_t>(i)] = p;
    return out;
}

// --- Bits with their BCH checks ---------------------------------------------------

std::uint32_t formatBits(Ecc ecc, int mask) {
    const std::uint32_t data = static_cast<std::uint32_t>(formatLevel(ecc) << 3 | (mask & 7));
    std::uint32_t rem = data;
    for (int i = 0; i < 10; ++i) rem = (rem << 1) ^ ((rem >> 9) * 0x537u);
    return ((data << 10) | (rem & 0x3FFu)) ^ 0x5412u;
}

std::uint32_t versionBits(int version) {
    const std::uint32_t v = static_cast<std::uint32_t>(version);
    std::uint32_t rem = v;
    for (int i = 0; i < 12; ++i) rem = (rem << 1) ^ ((rem >> 11) * 0x1F25u);
    return (v << 12) | (rem & 0xFFFu);
}

// --- Reed-Solomon over GF(256) -----------------------------------------------------

std::uint8_t gfMul(std::uint8_t a, std::uint8_t b) {
    unsigned r = 0;
    for (int i = 7; i >= 0; --i) {
        r = (r << 1) ^ ((r >> 7) * 0x11Du);
        r ^= ((b >> i) & 1u) * a;
    }
    return static_cast<std::uint8_t>(r);
}

std::vector<std::uint8_t> reedSolomon(const std::vector<std::uint8_t>& data, int degree) {
    // The generator (x - a^0)(x - a^1)...(x - a^(degree-1)), leading 1 left out,
    // highest power first.
    std::vector<std::uint8_t> gen(static_cast<std::size_t>(degree), 0);
    gen.back() = 1;
    std::uint8_t root = 1;
    for (int i = 0; i < degree; ++i) {
        for (int j = 0; j < degree; ++j) {
            gen[static_cast<std::size_t>(j)] = gfMul(gen[static_cast<std::size_t>(j)], root);
            if (j + 1 < degree) gen[static_cast<std::size_t>(j)] ^= gen[static_cast<std::size_t>(j + 1)];
        }
        root = gfMul(root, 0x02);
    }
    // Long division; what is left is the error correction.
    std::vector<std::uint8_t> rem(static_cast<std::size_t>(degree), 0);
    for (std::uint8_t b : data) {
        const std::uint8_t f = b ^ rem.front();
        rem.erase(rem.begin());
        rem.push_back(0);
        for (std::size_t i = 0; i < rem.size(); ++i) rem[i] ^= gfMul(gen[i], f);
    }
    return rem;
}

// --- The text as codewords ---------------------------------------------------------

std::vector<std::uint8_t> dataCodewordsOf(const std::string& s, Ecc ecc, int version, Mode& mode) {
    mode = modeFor(s);
    Bits b;
    b.put(static_cast<std::uint32_t>(modeIndicator(mode)), 4);
    b.put(static_cast<std::uint32_t>(s.size()), countBits(mode, version));
    switch (mode) {
    case Mode::Numeric:
        for (std::size_t i = 0; i < s.size(); i += 3) {
            const std::size_t n = std::min<std::size_t>(3, s.size() - i);
            b.put(static_cast<std::uint32_t>(std::atoi(s.substr(i, n).c_str())), n == 3 ? 10 : n == 2 ? 7 : 4);
        }
        break;
    case Mode::Alphanumeric:
        for (std::size_t i = 0; i + 1 < s.size(); i += 2)
            b.put(static_cast<std::uint32_t>(alnumValue(s[i]) * 45 + alnumValue(s[i + 1])), 11);
        if (s.size() % 2) b.put(static_cast<std::uint32_t>(alnumValue(s.back())), 6);
        break;
    default:
        for (char c : s) b.put(static_cast<std::uint8_t>(c), 8);
        break;
    }
    const std::size_t cap = static_cast<std::size_t>(dataCodewords(version, ecc)) * 8;
    // Terminator (up to four zeros), to a whole byte, then the two pad bytes in turn.
    for (int i = 0; i < 4 && b.v.size() < cap; ++i) b.v.push_back(false);
    while (b.v.size() % 8) b.v.push_back(false);
    for (std::uint8_t pad = 0xEC; b.v.size() < cap; pad ^= 0xEC ^ 0x11) b.put(pad, 8);
    std::vector<std::uint8_t> out(b.v.size() / 8, 0);
    for (std::size_t i = 0; i < b.v.size(); ++i)
        if (b.v[i]) out[i >> 3] |= static_cast<std::uint8_t>(0x80 >> (i & 7));
    return out;
}

// --- Penalty --------------------------------------------------------------------

int penalty(const Code& c) {
    const int n = c.size;
    int score = 0;
    auto at = [&](int x, int y) { return c.dark[static_cast<std::size_t>(y * n + x)]; };
    // 1: five or more of a colour in a row or a column.
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 0; a < n; ++a) {
            int run = 1;
            for (int b = 1; b <= n; ++b) {
                const bool same = b < n && (pass ? at(a, b) == at(a, b - 1) : at(b, a) == at(b - 1, a));
                if (same) { ++run; continue; }
                if (run >= 5) score += 3 + run - 5;
                run = 1;
            }
        }
    // 2: every 2 x 2 block of one colour.
    for (int y = 0; y + 1 < n; ++y)
        for (int x = 0; x + 1 < n; ++x) {
            const int v = at(x, y);
            if (v == at(x + 1, y) && v == at(x, y + 1) && v == at(x + 1, y + 1)) score += 3;
        }
    // 3: what looks like a finder -- 1:1:3:1:1 with four light on one side.
    static constexpr int pa[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0};
    static constexpr int pb[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
    for (int pass = 0; pass < 2; ++pass)
        for (int a = 0; a < n; ++a)
            for (int b = 0; b + 11 <= n; ++b) {
                bool ma = true, mb = true;
                for (int k = 0; k < 11 && (ma || mb); ++k) {
                    const int v = pass ? at(a, b + k) : at(b + k, a);
                    ma = ma && v == pa[k];
                    mb = mb && v == pb[k];
                }
                score += (ma ? 40 : 0) + (mb ? 40 : 0);
            }
    // 4: how far the share of dark is from half, in steps of 5 %.
    int dark = 0;
    for (std::uint8_t d : c.dark) dark += d;
    const int total = n * n;
    const int k = (std::abs(dark * 20 - total * 10) + total - 1) / total - 1;
    score += std::max(k, 0) * 10;
    return score;
}

// --- Encode -----------------------------------------------------------------------

Code encode(const std::string& text, Ecc ecc, int minVersion, int mask) {
    Code out;
    out.ecc = ecc;
    const Mode mode = modeFor(text);
    int version = 0;
    for (int v = std::clamp(minVersion, 1, 40); v <= 40; ++v) {
        const long need = 4 + countBits(mode, v) + payloadBits(mode, text.size());
        // The count has to fit its field, too (a 256th byte at version 9 does not).
        if (static_cast<long>(text.size()) >= (1L << countBits(mode, v))) continue;
        if (need <= static_cast<long>(dataCodewords(v, ecc)) * 8) { version = v; break; }
    }
    if (version == 0) return out;

    Mode m;
    const std::vector<std::uint8_t> data = dataCodewordsOf(text, ecc, version, m);
    const std::vector<std::uint8_t> all  = interleave(data, version, ecc);
    Grid base(17 + 4 * version);
    drawFunctions(base, version, ecc);
    placeData(base, all);

    out.version   = version;
    out.size      = base.n;
    out.mode      = mode;
    out.capacity  = dataCodewords(version, ecc);
    out.dataBytes = static_cast<int>((4 + countBits(mode, version) + payloadBits(mode, text.size()) + 7) / 8);

    int best = INT_MAX;
    for (int k = 0; k < 8; ++k) {
        if (mask >= 0 && k != (mask & 7)) continue;
        Grid g = base;
        applyMask(g, k);
        drawFormat(g, ecc, k);
        Code c = out;
        c.mask = k;
        c.dark = std::move(g.dark);
        const int p = mask >= 0 ? 0 : penalty(c);
        if (p < best) {
            best = p;
            out.mask = k;
            out.dark = std::move(c.dark);
        }
    }
    return out;
}

} // namespace qrencode
