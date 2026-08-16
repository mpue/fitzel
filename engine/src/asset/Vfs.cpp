#include "fitzel/asset/Vfs.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <system_error>

#include "PakArchive.hpp"

namespace fitzel::vfs {

namespace fs = std::filesystem;

namespace {

std::unique_ptr<pak::Archive> g_archive;
std::string                   g_root;     // mount root, '/'-separated, lowercased
// The same root as the caller spelled it. Lookups fold case; everything handed
// BACK must not. std::filesystem::relative and the AssetDatabase's root-prefix
// match are lexical and case-sensitive, so a lowercased path returned from here
// would stop matching the source root it came from.
std::string                   g_rootAsGiven;

// The archive lookup key for `path`, or nullopt when the path cannot be inside
// the archive at all (absolute and outside the mount root). Relative paths are
// taken as-is: the runtime chdir's to the executable's directory at startup,
// which is exactly what the archive is mounted at. The mount root itself maps to
// the empty key -- valid, and meaning "everything".
std::optional<std::string> keyFor(const std::string& path) {
    std::string k = pak::lowerKey(path);
    while (k.rfind("./", 0) == 0) k.erase(0, 2);
    while (k.size() > 1 && k.back() == '/') k.pop_back();
    if (k.empty()) return std::nullopt;

    const bool absolute = (k.size() > 1 && k[1] == ':') || k[0] == '/';
    if (absolute) {
        if (g_root.empty() || k.compare(0, g_root.size(), g_root) != 0)
            return std::nullopt;
        if (k.size() == g_root.size()) return std::string{}; // the root itself
        if (k[g_root.size()] != '/') return std::nullopt;    // a sibling directory
        k.erase(0, g_root.size() + 1);
    }
    // "a/b/../c" would miss the index; the codebase builds paths by joining, so
    // this only shows up if someone hand-writes one. Not worth normalising here.
    return k;
}

// Turn an archive-relative entry path back into the shape the caller used: they
// asked with an absolute directory, they get absolute paths back -- in the
// root's original spelling, and with the entry's own, so what comes out of here
// can be compared with, and made relative to, the paths the caller already holds.
std::string unkey(const std::string& rel, bool absolute) {
    return absolute ? g_rootAsGiven + "/" + rel : rel;
}

bool looksAbsolute(const std::string& p) {
    return (p.size() > 1 && p[1] == ':') || (!p.empty() && p[0] == '/');
}

// "content/models/" -- the prefix every entry under `dir` starts with. An empty
// prefix is the mount root, i.e. every entry; nullopt is a directory the archive
// cannot hold, and the caller should not look inside it at all.
std::optional<std::string> dirPrefix(const std::string& dir) {
    std::optional<std::string> k = keyFor(dir);
    if (k && !k->empty() && k->back() != '/') k->push_back('/');
    return k;
}

} // namespace

bool mount(const fs::path& pakFile, const fs::path& root) {
    unmount();
    std::error_code ec;
    if (!fs::exists(pakFile, ec)) return false;

    auto archive = std::make_unique<pak::Archive>();
    if (!archive->open(pakFile)) {
        std::fprintf(stderr, "[Fitzel] '%s' is not a readable archive\n",
                     pakFile.string().c_str());
        return false;
    }

    fs::path canon = fs::weakly_canonical(root, ec);
    if (ec) canon = root;
    g_rootAsGiven = canon.generic_string();
    while (!g_rootAsGiven.empty() && g_rootAsGiven.back() == '/')
        g_rootAsGiven.pop_back();
    g_root = pak::lowerKey(g_rootAsGiven);
    g_archive = std::move(archive);
    std::fprintf(stderr, "[Fitzel] mounted archive '%s' (%zu files)\n",
                 pakFile.filename().string().c_str(), g_archive->entries().size());
    return true;
}

void unmount() {
    g_archive.reset();
    g_root.clear();
    g_rootAsGiven.clear();
}

bool packed() { return g_archive && g_archive->isOpen(); }

bool inPak(const std::string& path) {
    if (!packed()) return false;
    const std::optional<std::string> k = keyFor(path);
    return k && !k->empty() && g_archive->find(*k) != nullptr;
}

std::vector<std::uint8_t> read(const std::string& path) {
    if (packed()) {
        const std::optional<std::string> k = keyFor(path);
        if (k && !k->empty())
            if (const pak::Archive::Entry* e = g_archive->find(*k))
                return g_archive->read(*e);
    }
    std::ifstream in(fs::path(path), std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff size = in.tellg();
    if (size <= 0) return {};
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(out.data()), size);
    if (in.gcount() != size) return {};
    return out;
}

std::string readText(const std::string& path) {
    const std::vector<std::uint8_t> bytes = read(path);
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool exists(const std::string& path) {
    if (inPak(path)) return true;
    std::error_code ec;
    return fs::exists(fs::path(path), ec);
}

bool isDirectory(const std::string& path) {
    if (packed()) {
        if (const std::optional<std::string> prefix = dirPrefix(path))
            for (const auto& e : g_archive->entries())
                if (pak::lowerKey(e.path).rfind(*prefix, 0) == 0) return true;
    }
    std::error_code ec;
    return fs::is_directory(fs::path(path), ec);
}

std::vector<std::string> listFiles(const std::string& dir, bool recursive) {
    std::set<std::string> out; // sorted + deduplicated across both backends
    const bool            absolute = looksAbsolute(dir);

    if (const std::optional<std::string> prefix = packed() ? dirPrefix(dir)
                                                           : std::nullopt) {
        for (const auto& e : g_archive->entries()) {
            if (pak::lowerKey(e.path).rfind(*prefix, 0) != 0) continue;
            const std::string tail = e.path.substr(prefix->size());
            if (!recursive && tail.find('/') != std::string::npos) continue;
            out.insert(unkey(e.path, absolute));
        }
    }

    std::error_code ec;
    if (fs::is_directory(fs::path(dir), ec)) {
        auto add = [&](const fs::directory_entry& de) {
            if (de.is_regular_file(ec)) out.insert(de.path().generic_string());
        };
        if (recursive) {
            for (fs::recursive_directory_iterator it(dir, ec), end; it != end;
                 it.increment(ec)) {
                if (ec) break;
                add(*it);
            }
        } else {
            for (fs::directory_iterator it(dir, ec), end; it != end;
                 it.increment(ec)) {
                if (ec) break;
                add(*it);
            }
        }
    }
    return {out.begin(), out.end()};
}

std::vector<std::string> listDirs(const std::string& dir) {
    std::set<std::string> out;

    if (const std::optional<std::string> prefix = packed() ? dirPrefix(dir)
                                                           : std::nullopt) {
        for (const auto& e : g_archive->entries()) {
            if (pak::lowerKey(e.path).rfind(*prefix, 0) != 0) continue;
            const std::string tail  = e.path.substr(prefix->size());
            const std::size_t slash = tail.find('/');
            if (slash != std::string::npos) out.insert(tail.substr(0, slash));
        }
    }

    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) out.insert(it->path().filename().generic_string());
    }
    return {out.begin(), out.end()};
}

} // namespace fitzel::vfs
