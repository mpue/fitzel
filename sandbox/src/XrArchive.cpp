#include "XrArchive.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <set>

namespace xray {

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// LZHUF. Haruyasu Yoshizaki's coder as X-Ray's xrCore carries it: a 4 KiB
// window, matches of 3..60 bytes, an adaptive Huffman tree for the literals and
// lengths, and a fixed code for the upper six bits of a match's distance. The
// one departure from the 1988 original is MAX_FREQ 0x4000 (not 0x8000): the
// tree is rebuilt twice as often, and a decoder with the original constant
// reads the first ~16k symbols right and garbage after them.

namespace {

constexpr int kN        = 4096;
constexpr int kF        = 60;
constexpr int kThresh   = 2;
constexpr int kNChar    = 256 - kThresh + kF;
constexpr int kT        = kNChar * 2 - 1;
constexpr int kR        = kT - 1;
constexpr unsigned kMaxFreq = 0x4000;

struct DistanceCode {
    std::uint8_t code[256];
    std::uint8_t len[256];
    DistanceCode() {
        // (bits in the first byte, how many codes of that length, first code)
        const int groups[6][3] = {{3, 1, 0}, {4, 3, 1}, {5, 8, 4},
                                  {6, 12, 12}, {7, 24, 24}, {8, 16, 48}};
        int i = 0;
        for (const auto& g : groups)
            for (int c = 0; c < g[1]; ++c)
                for (int k = 0; k < (1 << (8 - g[0])); ++k, ++i) {
                    code[i] = static_cast<std::uint8_t>(g[2] + c);
                    len[i]  = static_cast<std::uint8_t>(g[0]);
                }
    }
};

class Lzhuf {
public:
    Lzhuf(const std::uint8_t* src, std::size_t n) : src_(src), n_(n) {
        for (int i = 0; i < kNChar; ++i) {
            freq_[i] = 1;
            son_[i] = i + kT;
            prnt_[i + kT] = i;
        }
        for (int i = 0, j = kNChar; j <= kR; i += 2, ++j) {
            freq_[j] = freq_[i] + freq_[i + 1];
            son_[j] = i;
            prnt_[i] = prnt_[i + 1] = j;
        }
        freq_[kT] = 0xffff;
        prnt_[kR] = 0;
    }

    bool decode(std::size_t outSize, std::vector<std::uint8_t>& out) {
        static const DistanceCode dc;
        std::uint8_t text[kN];
        std::memset(text, ' ', sizeof(text));
        int r = kN - kF;
        out.clear();
        out.reserve(outSize);
        while (out.size() < outSize) {
            if (pos_ > n_ + 2) return false;   // ran dry: the stream is cut short
            int c = son_[kR];
            while (c < kT) c = son_[c + bit()];
            c -= kT;
            update(c);
            if (c < 256) {
                out.push_back(static_cast<std::uint8_t>(c));
                text[r] = static_cast<std::uint8_t>(c);
                r = (r + 1) & (kN - 1);
                continue;
            }
            int i = byte();
            const int hi = dc.code[i] << 6;
            for (int j = dc.len[i] - 2; j > 0; --j) i = (i << 1) + bit();
            const int from = (r - (hi | (i & 0x3f)) - 1) & (kN - 1);
            const int len  = c - 255 + kThresh;
            for (int k = 0; k < len && out.size() < outSize; ++k) {
                const std::uint8_t ch = text[(from + k) & (kN - 1)];
                out.push_back(ch);
                text[r] = ch;
                r = (r + 1) & (kN - 1);
            }
        }
        return true;
    }

private:
    void fill() {
        while (len_ <= 8) {
            const unsigned b = pos_ < n_ ? src_[pos_] : 0;
            ++pos_;
            buf_ |= b << (8 - len_);
            len_ += 8;
        }
    }
    int bit() {
        fill();
        const unsigned v = buf_;
        buf_ = (buf_ << 1) & 0xffff;
        --len_;
        return static_cast<int>((v & 0x8000) >> 15);
    }
    int byte() {
        fill();
        const unsigned v = buf_;
        buf_ = (buf_ << 8) & 0xffff;
        len_ -= 8;
        return static_cast<int>((v & 0xff00) >> 8);
    }

    void reconst() {
        int j = 0;
        for (int i = 0; i < kT; ++i)
            if (son_[i] >= kT) {
                freq_[j] = (freq_[i] + 1) / 2;
                son_[j] = son_[i];
                ++j;
            }
        for (int i = 0, k, jj = kNChar; jj < kT; i += 2, ++jj) {
            const unsigned f = freq_[jj] = freq_[i] + freq_[i + 1];
            for (k = jj - 1; f < freq_[k]; --k) {}
            ++k;
            std::memmove(&freq_[k + 1], &freq_[k], (jj - k) * sizeof(freq_[0]));
            freq_[k] = f;
            std::memmove(&son_[k + 1], &son_[k], (jj - k) * sizeof(son_[0]));
            son_[k] = i;
        }
        for (int i = 0; i < kT; ++i) {
            const int k = son_[i];
            if (k >= kT) prnt_[k] = i;
            else prnt_[k] = prnt_[k + 1] = i;
        }
    }

    void update(int c) {
        if (freq_[kR] == kMaxFreq) reconst();
        c = prnt_[c + kT];
        do {
            const unsigned k = ++freq_[c];
            int l = c + 1;
            if (k > freq_[l]) {
                while (k > freq_[++l]) {}
                --l;
                freq_[c] = freq_[l];
                freq_[l] = k;
                const int i = son_[c];
                prnt_[i] = l;
                if (i < kT) prnt_[i + 1] = l;
                const int j = son_[l];
                son_[l] = i;
                prnt_[j] = c;
                if (j < kT) prnt_[j + 1] = c;
                son_[c] = j;
                c = l;
            }
        } while ((c = prnt_[c]) != 0);
    }

    const std::uint8_t* src_;
    std::size_t n_;
    std::size_t pos_ = 0;
    unsigned buf_ = 0;
    int len_ = 0;
    unsigned freq_[kT + 1];
    int prnt_[kT + kNChar];
    int son_[kT];
};

} // namespace

bool lzhufDecode(const std::uint8_t* src, std::size_t srcSize, std::size_t outSize,
                 std::vector<std::uint8_t>& out) {
    Lzhuf z(src, srcSize);
    return z.decode(outSize, out);
}

// ---------------------------------------------------------------------------
// LZO1X, the decompressor alone (lzo1x_decompress_safe's logic): literal runs
// and four kinds of match, each copy checked against both buffers.

bool lzo1xDecode(const std::uint8_t* src, std::size_t srcSize, std::uint8_t* dst,
                 std::size_t dstSize) {
    const std::uint8_t* ip = src;
    const std::uint8_t* const ipEnd = src + srcSize;
    std::uint8_t* op = dst;
    std::uint8_t* const opEnd = dst + dstSize;

    auto need = [&](std::size_t n) { return static_cast<std::size_t>(ipEnd - ip) >= n; };
    auto literals = [&](std::size_t n) {
        if (!need(n) || static_cast<std::size_t>(opEnd - op) < n) return false;
        std::memcpy(op, ip, n);
        op += n;
        ip += n;
        return true;
    };
    // A match: `n` bytes from `dist` back. Byte by byte, the two may overlap.
    auto match = [&](std::size_t dist, std::size_t n) {
        if (dist > static_cast<std::size_t>(op - dst) ||
            static_cast<std::size_t>(opEnd - op) < n)
            return false;
        const std::uint8_t* from = op - dist;
        for (std::size_t i = 0; i < n; ++i) op[i] = from[i];
        op += n;
        return true;
    };
    // A run length past its field: zero bytes add 255 each, then one more.
    auto longCount = [&](std::size_t base, std::size_t& t) {
        while (need(1) && *ip == 0) {
            t += 255;
            ++ip;
        }
        if (!need(1)) return false;
        t += base + *ip++;
        return true;
    };

    // The reference decoder's labels as states.
    enum class S { Top, FirstRun, Match, MatchDone, MatchNext } s = S::Top;
    std::size_t t = 0;
    if (!need(1)) return false;
    if (*ip > 17) {
        t = *ip++ - 17u;
        if (t < 4) {
            s = S::MatchNext;
        } else {
            if (!literals(t)) return false;
            s = S::FirstRun;
        }
    }
    for (;;) {
        switch (s) {
        case S::Top:
            if (!need(1)) return false;
            t = *ip++;
            if (t >= 16) { s = S::Match; break; }
            if (t == 0 && !longCount(15, t)) return false;
            if (!literals(t + 3)) return false;
            s = S::FirstRun;
            break;
        case S::FirstRun:
            if (!need(1)) return false;
            t = *ip++;
            if (t >= 16) { s = S::Match; break; }
            if (!need(1)) return false;
            // Three bytes from just past an M2 match's reach.
            if (!match(1 + 0x0800 + (t >> 2) + (static_cast<std::size_t>(*ip++) << 2), 3))
                return false;
            s = S::MatchDone;
            break;
        case S::Match:
            if (t >= 64) {                       // M2: 3..8 bytes, up to 2 KiB back
                if (!need(1)) return false;
                const std::size_t d = 1 + ((t >> 2) & 7) + (static_cast<std::size_t>(*ip++) << 3);
                if (!match(d, (t >> 5) + 1)) return false;
            } else if (t >= 32) {                // M3: up to 16 KiB back
                t &= 31;
                if (t == 0 && !longCount(31, t)) return false;
                if (!need(2)) return false;
                const std::size_t d = 1 + (ip[0] >> 2) + (static_cast<std::size_t>(ip[1]) << 6);
                ip += 2;
                if (!match(d, t + 2)) return false;
            } else if (t >= 16) {                // M4: 16..48 KiB back, or the end
                std::size_t d = static_cast<std::size_t>(t & 8) << 11;
                t &= 7;
                if (t == 0 && !longCount(7, t)) return false;
                if (!need(2)) return false;
                d += (ip[0] >> 2) + (static_cast<std::size_t>(ip[1]) << 6);
                ip += 2;
                if (d == 0) return op == opEnd && ip == ipEnd;
                if (!match(d + 0x4000, t + 2)) return false;
            } else {                             // M1: two bytes, close by
                if (!need(1)) return false;
                if (!match(1 + (t >> 2) + (static_cast<std::size_t>(*ip++) << 2), 2))
                    return false;
            }
            s = S::MatchDone;
            break;
        case S::MatchDone:
            t = ip[-2] & 3u;
            s = t == 0 ? S::Top : S::MatchNext;
            break;
        case S::MatchNext:
            if (!literals(t)) return false;
            if (!need(1)) return false;
            t = *ip++;
            s = S::Match;
            break;
        }
    }
}

// ---------------------------------------------------------------------------

namespace {

std::uint32_t rd32(const std::uint8_t* p) {
    std::uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

bool isArchiveName(const fs::path& p) {
    std::string e = p.extension().string();
    for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (e.rfind(".xdb", 0) == 0) e.erase(1, 1);
    if (e.rfind(".db", 0) != 0) return false;
    for (std::size_t i = 3; i < e.size(); ++i)
        if (!std::isalnum(static_cast<unsigned char>(e[i]))) return false;
    return true;
}

} // namespace

std::string FileSystem::normalize(const std::string& path) {
    std::string s;
    s.reserve(path.size());
    for (char c : path) {
        if (c == '\\') c = '/';
        if (c == '/' && !s.empty() && s.back() == '/') continue;
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

bool FileSystem::mountArchive(const fs::path& file, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) { error = "cannot open " + file.string(); return false; }
    in.seekg(0, std::ios::end);
    const std::uint64_t size = static_cast<std::uint64_t>(in.tellg());
    std::uint64_t pos = 0;
    while (pos + 8 <= size) {
        std::uint8_t h[8];
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(reinterpret_cast<char*>(h), 8);
        if (!in) break;
        const std::uint32_t id = rd32(h), len = rd32(h + 4);
        if ((id & 0x7fffffffu) == 1) {
            std::vector<std::uint8_t> raw(len);
            in.read(reinterpret_cast<char*>(raw.data()), len);
            if (!in) { error = file.string() + ": file table cut short"; return false; }
            std::vector<std::uint8_t> table;
            if (id & 0x80000000u) {
                if (raw.size() < 4 ||
                    !lzhufDecode(raw.data() + 4, raw.size() - 4, rd32(raw.data()), table)) {
                    error = file.string() + ": file table does not unpack";
                    return false;
                }
            } else {
                table = std::move(raw);
            }
            const int archive = static_cast<int>(archives_.size());
            archives_.push_back(file);
            std::size_t p = 0;
            while (p + 2 <= table.size()) {
                const std::size_t n = table[p] | (table[p + 1] << 8);
                p += 2;
                if (n < 16 || p + n > table.size()) {
                    error = file.string() + ": file table is not the Clear Sky / Call of "
                            "Pripyat layout (an original Shadow of Chernobyl archive?)";
                    return false;
                }
                const std::uint8_t* e = table.data() + p;
                Entry en;
                en.archive        = archive;
                en.sizeReal       = rd32(e);
                en.sizeCompressed = rd32(e + 4);
                en.offset         = rd32(e + n - 4);
                std::string name(reinterpret_cast<const char*>(e + 12), n - 16);
                p += n;
                if (name.empty() || name.back() == '\\' || name.back() == '/') continue;
                if (static_cast<std::uint64_t>(en.offset) + en.sizeCompressed > size) {
                    error = file.string() + ": entry '" + name + "' runs past the archive";
                    return false;
                }
                files_[normalize(name)] = std::move(en);
            }
            return true;
        }
        pos += 8 + static_cast<std::uint64_t>(len);
    }
    error = file.string() + ": no file table";
    return false;
}

bool FileSystem::mount(const fs::path& gameDir, std::string& error) {
    archives_.clear();
    files_.clear();
    std::error_code ec;
    if (!fs::is_directory(gameDir, ec)) {
        error = gameDir.string() + " is not a folder";
        return false;
    }
    // The archives, patches/ last so a patch wins over what it patches.
    std::vector<fs::path> found;
    for (fs::recursive_directory_iterator it(gameDir, fs::directory_options::skip_permission_denied, ec), end;
         it != end; it.increment(ec)) {
        if (ec) break;
        if (it.depth() > 2) { it.disable_recursion_pending(); continue; }
        if (it->is_directory(ec)) {
            std::string n = it->path().filename().string();
            for (char& c : n) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (n == "gamedata" || n == "bin" || n == "_appdata_") it.disable_recursion_pending();
            continue;
        }
        if (it->is_regular_file(ec) && isArchiveName(it->path())) found.push_back(it->path());
    }
    auto isPatch = [](const fs::path& p) {
        std::string s = p.generic_string();
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s.find("/patches/") != std::string::npos;
    };
    std::sort(found.begin(), found.end(), [&](const fs::path& a, const fs::path& b) {
        if (isPatch(a) != isPatch(b)) return !isPatch(a);
        return a < b;
    });
    std::string firstError;
    for (const fs::path& f : found) {
        std::string e;
        if (!mountArchive(f, e) && firstError.empty()) firstError = e;
    }
    // Loose files over the archives (a mod's gamedata/).
    const fs::path gd = gameDir / "gamedata";
    if (fs::is_directory(gd, ec))
        for (fs::recursive_directory_iterator it(gd, ec), end; it != end; it.increment(ec)) {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            Entry en;
            en.loose = it->path();
            files_[normalize(fs::relative(it->path(), gd, ec).generic_string())] = std::move(en);
        }
    if (files_.empty()) {
        error = firstError.empty() ? "no X-Ray archives (*.db*) and no gamedata/ in " +
                                         gameDir.string()
                                   : firstError;
        return false;
    }
    if (!firstError.empty()) error = firstError;   // mounted, but say what was skipped
    return true;
}

bool FileSystem::exists(const std::string& path) const {
    return files_.count(normalize(path)) != 0;
}

bool FileSystem::read(const std::string& path, std::vector<std::uint8_t>& out) const {
    out.clear();
    const auto it = files_.find(normalize(path));
    if (it == files_.end()) return false;
    const Entry& e = it->second;
    if (e.archive < 0) {
        std::ifstream in(e.loose, std::ios::binary);
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), {});
        return true;
    }
    std::ifstream in(archives_[e.archive], std::ios::binary);
    if (!in) return false;
    in.seekg(e.offset);
    std::vector<std::uint8_t> raw(e.sizeCompressed);
    in.read(reinterpret_cast<char*>(raw.data()), e.sizeCompressed);
    if (!in) return false;
    if (e.sizeReal == e.sizeCompressed) {
        out = std::move(raw);
        return true;
    }
    out.resize(e.sizeReal);
    if (!lzo1xDecode(raw.data(), raw.size(), out.data(), out.size())) {
        out.clear();
        return false;
    }
    return true;
}

std::vector<std::string> FileSystem::paths() const {
    std::vector<std::string> out;
    out.reserve(files_.size());
    for (const auto& [path, e] : files_) out.push_back(path);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> FileSystem::levels() const {
    std::set<std::string> names;
    for (const auto& [path, e] : files_) {
        if (path.rfind("levels/", 0) != 0) continue;
        const std::size_t slash = path.find('/', 7);
        if (slash == std::string::npos) continue;
        const std::string name = path.substr(7, slash - 7);
        if (path.compare(slash + 1, std::string::npos, "level.geom") == 0 &&
            files_.count("levels/" + name + "/level"))
            names.insert(name);
    }
    return {names.begin(), names.end()};
}

} // namespace xray
