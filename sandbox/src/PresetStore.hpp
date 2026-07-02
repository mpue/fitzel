#pragma once

#include <functional>
#include <string>
#include <vector>

// A named registry of tunable parameters, saved/loaded as simple "key value"
// text files (one per scene look) under presets/. Each tunable is bound by name
// to a getter/setter, so unknown or missing keys are ignored -- old presets keep
// working as fields come and go.
class PresetStore {
public:
    void addFloat(const char* key, float& ref);
    void addBool(const char* key, bool& ref);
    void addInt(const char* key, int& ref);

    // Preset files (stem names, without extension), sorted.
    std::vector<std::string> list() const;
    void save(const std::string& name) const;
    bool load(const std::string& name);   // false if the file is missing
    void remove(const std::string& name) const;

private:
    struct Field {
        std::string                key;
        std::function<float()>     get;
        std::function<void(float)> set;
    };
    std::vector<Field> m_fields;
};
