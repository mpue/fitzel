#include "XrLtx.hpp"

#include <cctype>
#include <cstdlib>

#include "XrArchive.hpp"

namespace xray {

namespace {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

} // namespace

std::string Ltx::lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> Ltx::list(const std::string& value) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : value + ",") {
        if (c == ',') {
            const std::string t = trim(cur);
            if (!t.empty()) out.push_back(t);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    return out;
}

void Ltx::parse(const std::string& text) {
    Section* cur = nullptr;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (const std::size_t c = line.find(';'); c != std::string::npos) line.erase(c);
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;   // #include: loadTree reads them all
        if (line[0] == '[') {
            const std::size_t close = line.find(']');
            if (close == std::string::npos) { cur = nullptr; continue; }
            cur = &sections_[lower(trim(line.substr(1, close - 1)))];
            const std::size_t colon = line.find(':', close);
            if (colon != std::string::npos)
                for (const std::string& p : list(line.substr(colon + 1))) cur->parents.push_back(lower(p));
            continue;
        }
        if (!cur) continue;
        const std::size_t eq = line.find('=');
        std::string key = eq == std::string::npos ? line : trim(line.substr(0, eq));
        std::string value = eq == std::string::npos ? std::string() : trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        cur->items.emplace_back(lower(key), std::move(value));
    }
}

int Ltx::loadTree(const FileSystem& fs, const std::string& prefix) {
    int n = 0;
    for (const std::string& path : fs.paths()) {
        if (path.rfind(prefix, 0) != 0 || path.size() < 4 || path.compare(path.size() - 4, 4, ".ltx") != 0)
            continue;
        std::vector<std::uint8_t> bytes;
        if (!fs.read(path, bytes)) continue;
        parse(std::string(bytes.begin(), bytes.end()));
        ++n;
    }
    return n;
}

const Ltx::Section* Ltx::section(const std::string& name) const {
    const auto it = sections_.find(lower(name));
    return it == sections_.end() ? nullptr : &it->second;
}

bool Ltx::findIn(const std::string& section, const std::string& key, std::string& out, int depth) const {
    if (depth > 16) return false;
    const auto it = sections_.find(section);
    if (it == sections_.end()) return false;
    // The last setting of a key wins, as when a section is reopened.
    for (auto i = it->second.items.rbegin(); i != it->second.items.rend(); ++i)
        if (i->first == key) { out = i->second; return true; }
    // A later parent overrides an earlier one.
    for (auto p = it->second.parents.rbegin(); p != it->second.parents.rend(); ++p)
        if (findIn(*p, key, out, depth + 1)) return true;
    return false;
}

bool Ltx::find(const std::string& section, const std::string& key, std::string& out) const {
    return findIn(lower(section), lower(key), out, 0);
}

std::string Ltx::get(const std::string& section, const std::string& key, const std::string& fallback) const {
    std::string v;
    return find(section, key, v) ? v : fallback;
}

float Ltx::number(const std::string& section, const std::string& key, float fallback) const {
    std::string v;
    if (!find(section, key, v) || v.empty()) return fallback;
    char* end = nullptr;
    const float f = std::strtof(v.c_str(), &end);
    return end == v.c_str() ? fallback : f;
}

} // namespace xray
