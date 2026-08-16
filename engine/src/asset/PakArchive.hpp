#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace fitzel::pak {

// Lookup key for an archive path: '/'-separated and lowercased. Windows file
// names are case-insensitive and the same texture is spelled three ways across
// a project's scene files, so the index is keyed on the folded form.
inline std::string lowerKey(std::string s) {
    for (char& c : s) {
        if (c == '\\') c = '/';
        else if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

// A mounted .fpak, opened for reading. Holds the decrypted index in memory and
// the archive file open; entry payloads are decrypted on demand, one whole file
// per read (which is how every loader in the engine wants its bytes anyway).
//
// Reads are mutex-guarded, so an Archive can be shared by whatever thread is
// loading -- the seek and the read have to stay together.
class Archive {
public:
    struct Entry {
        std::string   path;    // as stored: relative, '/'-separated
        std::uint64_t offset = 0;
        std::uint64_t size   = 0;
        std::uint64_t nonce  = 0;
    };

    bool open(const std::filesystem::path& file); // false: missing/corrupt/wrong key
    bool isOpen() const { return m_open; }

    // `key` is the entry path lowercased (see vfs::normalize) -- Windows file
    // names reach us in whatever case the scene file happened to store.
    const Entry* find(const std::string& key) const;
    const std::vector<Entry>& entries() const { return m_entries; }

    std::vector<std::uint8_t> read(const Entry& e) const; // empty on a short read

private:
    mutable std::mutex    m_mutex;
    mutable std::ifstream m_file;
    std::vector<Entry>    m_entries;
    std::unordered_map<std::string, std::size_t> m_byKey;
    bool                  m_open = false;
};

} // namespace fitzel::pak
