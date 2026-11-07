#pragma once

// X-Ray's .ltx: an INI with inheritance -- `[dog_weak]:m_dog_e` takes every
// key of m_dog_e it does not set itself. The game's configs are ltx, and so is
// the custom data of every object in level.spawn (its [logic] and the like).
// Section and key names are case-insensitive; values come back trimmed, the
// `;` comment cut off.

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace xray {

class FileSystem;

class Ltx {
public:
    struct Section {
        std::vector<std::string> parents;
        std::vector<std::pair<std::string, std::string>> items;   // in file order
    };

    // Adds the sections of `text`; a section seen again gets the new keys.
    void parse(const std::string& text);
    // Every .ltx under `prefix` (the configs are many files that #include
    // each other; reading all of them is simpler than following the includes).
    int loadTree(const FileSystem& fs, const std::string& prefix = "config/");

    bool has(const std::string& section) const { return sections_.count(lower(section)) != 0; }
    const Section* section(const std::string& name) const;
    // A key of the section or, failing that, of its parents.
    bool find(const std::string& section, const std::string& key, std::string& out) const;
    std::string get(const std::string& section, const std::string& key,
                    const std::string& fallback = std::string()) const;
    float number(const std::string& section, const std::string& key, float fallback) const;

    static std::string lower(std::string s);
    static std::vector<std::string> list(const std::string& value);   // "a, b,c" -> {a, b, c}

private:
    bool findIn(const std::string& section, const std::string& key, std::string& out, int depth) const;
    std::map<std::string, Section> sections_;
};

} // namespace xray
