#include "fitzel/asset/Pak.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <system_error>
#include <vector>

#include "Chacha20.hpp"
#include "PakArchive.hpp"
#include "PakKey.hpp"

namespace fitzel::pak {

namespace fs = std::filesystem;

namespace {

// --- On-disk format ---------------------------------------------------------
//
//   [40-byte header][payload][encrypted index]
//
// The header stays plaintext, so a truncated or foreign file can be rejected
// without involving the key at all: magic, version, entry count, the archive
// salt, and where the index sits.
//
// Every entry -- and the index itself -- is encrypted with ChaCha20 under one
// key and its own nonce, derived from the salt. The salt is fresh per archive,
// which is what keeps two exports from ever encrypting different bytes under the
// same keystream (XOR two of those together and the plaintexts fall out).
//
// The index living at the END is not cosmetic: it lets the writer stream each
// file straight through without knowing any offset in advance.
constexpr char          kMagic[8]   = {'F', 'I', 'T', 'Z', 'P', 'A', 'K', '1'};
constexpr std::uint32_t kVersion    = 1;
constexpr std::size_t   kHeaderSize = 40;
constexpr std::size_t   kChunk      = 1u << 20; // 1 MiB streaming buffer

void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(x >> (i * 8)));
}
void put64(std::vector<std::uint8_t>& v, std::uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<std::uint8_t>(x >> (i * 8)));
}
std::uint32_t get32(const std::uint8_t* p) {
    std::uint32_t x = 0;
    for (int i = 0; i < 4; ++i) x |= static_cast<std::uint32_t>(p[i]) << (i * 8);
    return x;
}
std::uint64_t get64(const std::uint8_t* p) {
    std::uint64_t x = 0;
    for (int i = 0; i < 8; ++i) x |= static_cast<std::uint64_t>(p[i]) << (i * 8);
    return x;
}

} // namespace

// --- Reading ----------------------------------------------------------------

bool Archive::open(const fs::path& file) {
    m_open = false;
    m_entries.clear();
    m_byKey.clear();

    m_file.open(file, std::ios::binary);
    if (!m_file) return false;

    std::uint8_t head[kHeaderSize];
    m_file.read(reinterpret_cast<char*>(head), kHeaderSize);
    if (m_file.gcount() != static_cast<std::streamsize>(kHeaderSize)) return false;
    if (std::memcmp(head, kMagic, sizeof(kMagic)) != 0) return false;
    if (get32(head + 8) != kVersion) return false;

    const std::uint32_t count       = get32(head + 12);
    const std::uint64_t salt        = get64(head + 16);
    const std::uint64_t indexOffset = get64(head + 24);
    const std::uint64_t indexSize   = get64(head + 32);
    if (indexSize > (1ull << 30)) return false; // an index that big is nonsense

    std::vector<std::uint8_t> index(static_cast<std::size_t>(indexSize));
    m_file.seekg(static_cast<std::streamoff>(indexOffset), std::ios::beg);
    m_file.read(reinterpret_cast<char*>(index.data()),
                static_cast<std::streamsize>(indexSize));
    if (m_file.gcount() != static_cast<std::streamsize>(indexSize)) return false;

    std::uint8_t key[32];
    archiveKey(key);
    crypt::ChaCha20(key, salt).apply(index.data(), index.size());
    std::memset(key, 0, sizeof(key));

    // Every field is bounds-checked against the bytes actually left in the
    // index: a wrong key decrypts to noise, and noise has to fail cleanly
    // instead of allocating a four-gigabyte path.
    std::size_t at = 0;
    m_entries.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        if (at + 2 > index.size()) return false;
        const std::size_t len = static_cast<std::size_t>(index[at]) |
                                (static_cast<std::size_t>(index[at + 1]) << 8);
        at += 2;
        if (at + len + 24 > index.size()) return false;
        Entry e;
        e.path.assign(reinterpret_cast<const char*>(index.data() + at), len);
        at += len;
        e.offset = get64(index.data() + at); at += 8;
        e.size   = get64(index.data() + at); at += 8;
        e.nonce  = get64(index.data() + at); at += 8;
        m_byKey.emplace(lowerKey(e.path), m_entries.size());
        m_entries.push_back(std::move(e));
    }

    m_open = true;
    return true;
}

const Archive::Entry* Archive::find(const std::string& key) const {
    const auto it = m_byKey.find(key);
    return it == m_byKey.end() ? nullptr : &m_entries[it->second];
}

std::vector<std::uint8_t> Archive::read(const Entry& e) const {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(e.size));
    if (e.size == 0) return out;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_file.clear(); // an earlier short read may have left the eof bit set
        m_file.seekg(static_cast<std::streamoff>(e.offset), std::ios::beg);
        m_file.read(reinterpret_cast<char*>(out.data()),
                    static_cast<std::streamsize>(e.size));
        if (m_file.gcount() != static_cast<std::streamsize>(e.size)) return {};
    }
    std::uint8_t key[32];
    archiveKey(key);
    crypt::ChaCha20(key, e.nonce).apply(out.data(), out.size());
    std::memset(key, 0, sizeof(key));
    return out;
}

// --- Writing ----------------------------------------------------------------

WriteResult write(const fs::path& root, const fs::path& outFile,
                  const std::function<bool(const std::string&)>& skip) {
    WriteResult res;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        res.error = "pack root is not a directory: " + root.string();
        return res;
    }

    // Collect first, sorted: a stable entry order makes two packs of the same
    // tree comparable, which is worth a lot when an export looks wrong.
    std::vector<std::pair<std::string, fs::path>> files; // relative -> absolute
    for (fs::recursive_directory_iterator it(root, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string rel = fs::relative(it->path(), root, ec).generic_string();
        if (ec || rel.empty() || rel.rfind("..", 0) == 0) continue;
        if (rel.size() > 0xFFFF) continue; // the index stores a 16-bit length
        if (skip && skip(rel)) continue;
        files.emplace_back(rel, it->path());
    }
    std::sort(files.begin(), files.end());

    fs::create_directories(outFile.parent_path(), ec);
    std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
    if (!out) {
        res.error = "cannot write " + outFile.string();
        return res;
    }

    std::mt19937_64     rng{std::random_device{}()};
    const std::uint64_t salt = rng();

    std::vector<std::uint8_t> header(kHeaderSize, 0); // rewritten at the end
    out.write(reinterpret_cast<const char*>(header.data()), kHeaderSize);

    std::uint8_t key[32];
    archiveKey(key);

    struct Written {
        std::string   rel;
        std::uint64_t offset = 0, size = 0, nonce = 0;
    };
    std::vector<Written> written;
    written.reserve(files.size());

    std::vector<std::uint8_t> buf(kChunk);
    std::uint64_t             at = kHeaderSize;
    for (const auto& [rel, abs] : files) {
        std::ifstream in(abs, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "[Fitzel] pack: cannot read '%s' -- skipped\n",
                         abs.string().c_str());
            continue;
        }
        // Nonce 0 belongs to the index, so entries start at 1. Unique inside the
        // archive (the counter) and across archives (the salt), which is all a
        // stream cipher asks of a nonce.
        const std::uint64_t nonce = salt + written.size() + 1;
        crypt::ChaCha20     cipher(key, nonce);
        std::uint64_t       size = 0;
        while (in) {
            in.read(reinterpret_cast<char*>(buf.data()),
                    static_cast<std::streamsize>(buf.size()));
            const std::streamsize got = in.gcount();
            if (got <= 0) break;
            cipher.apply(buf.data(), static_cast<std::size_t>(got));
            out.write(reinterpret_cast<const char*>(buf.data()), got);
            size += static_cast<std::uint64_t>(got);
        }
        written.push_back({rel, at, size, nonce});
        at += size;
        res.bytes += size;
    }

    std::vector<std::uint8_t> index;
    for (const Written& w : written) {
        index.push_back(static_cast<std::uint8_t>(w.rel.size() & 0xFF));
        index.push_back(static_cast<std::uint8_t>((w.rel.size() >> 8) & 0xFF));
        index.insert(index.end(), w.rel.begin(), w.rel.end());
        put64(index, w.offset);
        put64(index, w.size);
        put64(index, w.nonce);
    }
    // The index is encrypted too -- otherwise the file names alone would tell
    // anyone what is in there, and where each asset starts.
    crypt::ChaCha20(key, salt).apply(index.data(), index.size());
    std::memset(key, 0, sizeof(key));
    out.write(reinterpret_cast<const char*>(index.data()),
              static_cast<std::streamsize>(index.size()));

    header.clear();
    header.insert(header.end(), kMagic, kMagic + sizeof(kMagic));
    put32(header, kVersion);
    put32(header, static_cast<std::uint32_t>(written.size()));
    put64(header, salt);
    put64(header, at); // index offset
    put64(header, static_cast<std::uint64_t>(index.size()));
    out.seekp(0, std::ios::beg);
    out.write(reinterpret_cast<const char*>(header.data()),
              static_cast<std::streamsize>(header.size()));
    out.flush();

    res.ok    = out.good();
    res.files = written.size();
    if (!res.ok) res.error = "write failed (disk full?)";
    return res;
}

} // namespace fitzel::pak
