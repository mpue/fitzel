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

#include <fitzel/asset/AssetDatabase.hpp>
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

    {
        // One byte of noise after the index, the index size grown to take it:
        // the entries no longer account for the whole index (AST-04 -- what a
        // wrong key decrypting to parseable noise looks like).
        std::vector<char> b = bytes;
        std::uint64_t size = 0;
        for (int i = 0; i < 8; ++i) size |= static_cast<std::uint64_t>(static_cast<unsigned char>(b[32 + i])) << (8 * i);
        b.push_back('\x5a');
        put64(b, 32, size + 1);
        spit(dir / "trail.fpak", b);
        check(refused(dir / "trail.fpak", dir), "an index with bytes no entry accounts for: refused");
    }

    // Writing (AST-06): a file that cannot be read fails the pack instead of
    // being left out, and the archive already there is not touched; an
    // archive written inside the tree it packs does not pack itself.
#ifndef _WIN32
    {
        const fs::path locked = dir / "src" / "content" / "locked.txt";
        { std::ofstream(locked) << "no one may read this"; }
        fs::permissions(locked, fs::perms::none, ec);
        std::ifstream probe(locked);
        if (probe) {
            std::printf("  (skipped the unreadable-file case: running with rights to read it anyway)\n");
        } else {
            const std::uintmax_t before = fs::file_size(good, ec);
            const fitzel::pak::WriteResult r = fitzel::pak::write(dir / "src", good);
            check(!r.ok && !r.error.empty(), "a file that cannot be read fails the pack");
            check(fs::file_size(good, ec) == before && !fs::exists(dir / "good.fpak.tmp"),
                  "...and leaves the previous archive as it was, with no temp file");
        }
        fs::permissions(locked, fs::perms::owner_all, ec);
        fs::remove(locked, ec);
    }
#endif
    {
        const fs::path inside = dir / "src" / "self.fpak";
        const fitzel::pak::WriteResult r1 = fitzel::pak::write(dir / "src", inside);
        const fitzel::pak::WriteResult r2 = fitzel::pak::write(dir / "src", inside);
        check(r1.ok && r2.ok && r2.files == 2, "an archive written inside its own tree does not pack itself");
        fs::remove(inside, ec);
    }

    // Asset sidecars (AST-07/08/09): a damaged .meta keeps its GUID if one
    // can be found, is backed up, and a wrong-typed one does not throw; a
    // removed asset leaves the listing on the next poll.
    {
        const fs::path proj = dir / "proj";
        fs::create_directories(proj, ec);
        { std::ofstream(proj / "a.png") << "x"; }
        { std::ofstream(proj / "b.png") << "x"; }
        { std::ofstream(proj / "c.png") << "x"; }
        const std::string guid = "0123456789abcdef0123456789abcdef";
        { std::ofstream(proj / "a.png.meta") << "{ \"guid\": \"" << guid << "\",\n<<<<<<< HEAD\n"; }
        { std::ofstream(proj / "b.png.meta") << "{\"guid\": 12, \"importer\": {\"sRGB\": \"yes\"}}"; }
        fitzel::AssetDatabase db(dir / "no-engine-root");
        db.mountProject(proj);
        bool threw = false;
        try { db.refresh(); } catch (...) { threw = true; }
        check(!threw, "sidecars with the wrong types do not abort the scan");
        const fitzel::AssetId a = db.idForPath(proj / "a.png");
        check(a.valid() && a.toString() == guid, "a damaged sidecar's GUID is rescued");
        check(fs::exists(proj / "a.png.meta.corrupt"), "...and the damaged sidecar is kept beside it");
        check(db.idForPath(proj / "b.png").valid(), "a sidecar with a number for a GUID gets a valid one");
        const std::size_t before = db.allAssets().size();
        fs::remove(proj / "c.png", ec);
        db.pollChanges();
        check(db.allAssets().size() + 1 == before && !db.idForPath(proj / "c.png").valid(),
              "a deleted asset leaves the listing on the next poll");
    }

    fs::remove_all(dir, ec);
    std::printf(g_fails ? "\n%d check(s) failed\n" : "\nall checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
