// pakcheck -- does a .fpak archive refuse to be lied to?
//
// The archive header is plain text: entry count, where the index lies and how
// big it is. The index is encrypted, but a wrong key decrypts it to noise, and
// either way every number in there came out of a file. Before the bounds checks
// a forty-byte file asked for a four-billion-entry reserve and killed the
// process (std::terminate on bad_alloc), and an entry could point at bytes the
// file does not have. So here an honest archive is packed and read back, and
// then copies of it are damaged one field at a time: each must simply fail to
// mount, and the process must still be here to say so.
//
//   build/linux/bin/pakcheck
// Exits non-zero if any check fails.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <fitzel/asset/Pak.hpp>
#include <fitzel/asset/Vfs.hpp>

namespace fs = std::filesystem;

namespace {

int g_fails = 0;
void check(bool ok, const char* what) {
    std::printf(ok ? "  ok   %s\n" : "[FAIL] %s\n", what);
    if (!ok) ++g_fails;
}

std::vector<char> slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}
void spit(const fs::path& p, const std::vector<char>& b) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
}
void put32(std::vector<char>& b, std::size_t at, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}
void put64(std::vector<char>& b, std::size_t at, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b[at + i] = static_cast<char>((v >> (8 * i)) & 0xFF);
}

// Mount a damaged copy: it must fail, and nothing must be mounted after it.
bool refused(const fs::path& file, const fs::path& root) {
    fitzel::vfs::unmount();
    const bool ok = fitzel::vfs::mount(file, root);
    fitzel::vfs::unmount();
    return !ok;
}

} // namespace

int main() {
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "fitzel_pakcheck";
    fs::remove_all(dir, ec);
    fs::create_directories(dir / "src" / "content", ec);
    { std::ofstream(dir / "src" / "content" / "a.txt") << "the first file"; }
    { std::ofstream(dir / "src" / "content" / "b.txt") << std::string(5000, 'b'); }
    const fs::path good = dir / "good.fpak";

    const fitzel::pak::WriteResult w = fitzel::pak::write(dir / "src", good);
    check(w.ok && w.files == 2, "an honest archive packs");
    fitzel::vfs::unmount();
    const bool mounted = fitzel::vfs::mount(good, dir);
    const std::vector<std::uint8_t> a = fitzel::vfs::read((dir / "content" / "a.txt").generic_string());
    check(mounted && std::string(a.begin(), a.end()) == "the first file", "and reads back what went in");
    fitzel::vfs::unmount();

    const std::vector<char> bytes = slurp(good);
    // Header: magic 0..7, version 8, count 12, salt 16, index offset 24, index size 32.
    {
        std::vector<char> b = bytes;
        put32(b, 12, 0xFFFFFFFFu);
        spit(dir / "count.fpak", b);
        check(refused(dir / "count.fpak", dir), "four billion entries in a small index: refused, no terminate");
    }
    {
        std::vector<char> b = bytes;
        put64(b, 24, 0xFFFFFFFFFFFFFF00ull);
        spit(dir / "offset.fpak", b);
        check(refused(dir / "offset.fpak", dir), "an index past the end of the file: refused");
    }
    {
        std::vector<char> b = bytes;
        put64(b, 32, static_cast<std::uint64_t>(bytes.size()) + 1);
        spit(dir / "size.fpak", b);
        check(refused(dir / "size.fpak", dir), "an index bigger than the file: refused");
    }
    {
        // Payload cut out from under the index: the entries point at bytes the
        // file no longer has. The index sits at the end, so drop bytes from the
        // middle and move the index offset back by as many.
        std::vector<char> b = bytes;
        std::uint64_t off = 0;
        for (int i = 0; i < 8; ++i) off |= static_cast<std::uint64_t>(static_cast<unsigned char>(b[24 + i])) << (8 * i);
        const std::size_t cut = 3000;
        if (off > 40 + cut) {
            b.erase(b.begin() + 40, b.begin() + 40 + static_cast<std::ptrdiff_t>(cut));
            put64(b, 24, off - cut);
            spit(dir / "short.fpak", b);
            check(refused(dir / "short.fpak", dir), "entries pointing past the end of the file: refused");
        } else {
            check(false, "the test archive is big enough to cut");
        }
    }
    {
        // Forty bytes: a header and nothing else.
        std::vector<char> b(bytes.begin(), bytes.begin() + 40);
        put32(b, 12, 0x7FFFFFFFu);
        put64(b, 24, 40);
        put64(b, 32, 0);
        spit(dir / "tiny.fpak", b);
        check(refused(dir / "tiny.fpak", dir), "a forty-byte file claiming two billion entries: refused");
    }

    fs::remove_all(dir, ec);
    std::printf(g_fails ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
