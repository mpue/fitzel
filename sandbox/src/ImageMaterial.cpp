#include "ImageMaterial.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

#include <stb_image_write.h>

#include "ImagePixel.hpp"

namespace fs = std::filesystem;

namespace img {

namespace {

struct Word { const char* word; MapSlot slot; };
const Word kWords[] = {
    {"normal", MapSlot::Normal}, {"normals", MapSlot::Normal}, {"normalmap", MapSlot::Normal},
    {"nrm", MapSlot::Normal}, {"nor", MapSlot::Normal},
    {"height", MapSlot::Height}, {"heightmap", MapSlot::Height}, {"bump", MapSlot::Height},
    {"bumpmap", MapSlot::Height}, {"displacement", MapSlot::Height}, {"disp", MapSlot::Height},
    {"hoehe", MapSlot::Height}, {"h\xC3\xB6he", MapSlot::Height}, {"relief", MapSlot::Height},
    {"roughness", MapSlot::Roughness}, {"rough", MapSlot::Roughness}, {"rauheit", MapSlot::Roughness},
    {"rauigkeit", MapSlot::Roughness},
    {"gloss", MapSlot::Gloss}, {"glossiness", MapSlot::Gloss}, {"smoothness", MapSlot::Gloss},
    {"glanz", MapSlot::Gloss},
    {"metal", MapSlot::Metal}, {"metallic", MapSlot::Metal}, {"metalness", MapSlot::Metal},
    {"metall", MapSlot::Metal},
    {"ao", MapSlot::Occlusion}, {"occlusion", MapSlot::Occlusion}, {"ambientocclusion", MapSlot::Occlusion},
    {"emission", MapSlot::Emission}, {"emissive", MapSlot::Emission}, {"emit", MapSlot::Emission},
    {"glow", MapSlot::Emission}, {"illumination", MapSlot::Emission}, {"illum", MapSlot::Emission},
    {"leuchten", MapSlot::Emission},
    {"opacity", MapSlot::Opacity}, {"alpha", MapSlot::Opacity}, {"transparency", MapSlot::Opacity},
    {"cutout", MapSlot::Opacity}, {"deckkraft", MapSlot::Opacity}, {"transparenz", MapSlot::Opacity},
};

// Words that may follow the one that counts.
bool passedOver(const std::string& w) {
    static const char* const kFill[] = {"map", "maps", "layer", "ebene", "copy", "kopie", "tex", "texture", "textur"};
    if (std::all_of(w.begin(), w.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) return true;
    for (const char* f : kFill)
        if (w == f) return true;
    return false;
}

int lum(const std::uint8_t* p) { return (p[0] * 77 + p[1] * 150 + p[2] * 29 + 128) >> 8; }

} // namespace

MapSlot mapSlotFor(const std::string& name) {
    // Words: split at anything not a letter or digit and where camelCase turns
    // to a capital, lower-cased. Bytes past ASCII belong to the word (umlauts).
    std::vector<std::string> words;
    std::string w;
    unsigned char prev = 0;
    for (unsigned char c : name) {
        const bool part = std::isalnum(c) || c >= 0x80;
        if (!part || (std::isupper(c) && std::islower(prev))) {
            if (!w.empty()) words.push_back(w);
            w.clear();
        }
        if (part) w += char(c < 0x80 ? std::tolower(c) : c);
        prev = c;
    }
    if (!w.empty()) words.push_back(w);
    while (!words.empty() && passedOver(words.back())) words.pop_back();
    if (words.empty()) return MapSlot::Color;
    for (const Word& k : kWords)
        if (words.back() == k.word) return k.slot;
    return MapSlot::Color;
}

const char* mapSlotName(MapSlot s) {
    static const char* const kNames[] = {"Base colour", "Normal", "Height", "Roughness", "Gloss",
                                         "Metal", "Ambient occlusion", "Emission", "Opacity"};
    return kNames[std::size_t(s)];
}

MaterialImages materialImages(const Document& d, const NormalMapOptions& height) {
    MaterialImages m;
    m.w = d.width();
    m.h = d.height();
    const int layers = d.layerCount();
    const std::size_t n = std::size_t(m.w) * m.h;
    std::vector<MapSlot> slot(std::size_t(layers), MapSlot::Color);
    std::vector<bool> leaveOut(std::size_t(layers), false);
    bool has[int(MapSlot::Count)] = {};
    bool color = false;
    for (int i = 0; i < layers; ++i) {
        slot[std::size_t(i)] = mapSlotFor(d.layer(i).name);
        has[int(slot[std::size_t(i)])] = true;
        leaveOut[std::size_t(i)] = slot[std::size_t(i)] != MapSlot::Color;
        color |= slot[std::size_t(i)] == MapSlot::Color && d.layer(i).visible;
    }
    if (color) m.color = d.flattenedWithout(leaveOut);

    // One map: its layers, bottom to top, over the map's neutral value -- each
    // with its opacity, fill and mask, plain (a map layer's blend mode and
    // effects are for looking at, not for the material).
    auto build = [&](MapSlot s, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
        Pixels out(n * 4);
        for (std::size_t j = 0; j < n; ++j) {
            out[j * 4] = r; out[j * 4 + 1] = g; out[j * 4 + 2] = b; out[j * 4 + 3] = 255;
        }
        for (int i = 0; i < layers; ++i) {
            if (slot[std::size_t(i)] != s) continue;
            const Layer& l = d.layer(i);
            const Pixels& p = *l.px;
            const Pixels* mask = l.mask && l.maskOn ? l.mask.get() : nullptr;
            const float k = l.opacity * l.fill / 255.0f;
            for (std::size_t j = 0; j < n; ++j) {
                float a = p[j * 4 + 3] * k;
                if (mask) a *= px::maskValue(&(*mask)[j * 4]) / 255.0f;
                if (a <= 0.0f) continue;
                for (int c = 0; c < 3; ++c)
                    out[j * 4 + c] = std::uint8_t(out[j * 4 + c] + (p[j * 4 + c] - out[j * 4 + c]) * a + 0.5f);
            }
        }
        return out;
    };
    auto grey = [&](const Pixels& p, bool invert) {
        std::vector<std::uint8_t> g(n);
        for (std::size_t j = 0; j < n; ++j) g[j] = std::uint8_t(invert ? 255 - lum(&p[j * 4]) : lum(&p[j * 4]));
        return g;
    };

    if (has[int(MapSlot::Normal)]) {
        m.normal = build(MapSlot::Normal, 128, 128, 255);
    } else if (has[int(MapSlot::Height)]) {
        normalMap(build(MapSlot::Height, 128, 128, 128), m.w, m.h, height, m.normal);
        m.normalFromHeight = true;
    }
    m.occlusion = has[int(MapSlot::Occlusion)];
    m.roughness = has[int(MapSlot::Roughness)] || has[int(MapSlot::Gloss)];
    m.metal     = has[int(MapSlot::Metal)];
    if (m.occlusion || m.roughness || m.metal) {
        // Missing parts stay white: an average of 1 leaves the slider as it is.
        const std::vector<std::uint8_t> ao = m.occlusion ? grey(build(MapSlot::Occlusion, 255, 255, 255), false)
                                                         : std::vector<std::uint8_t>(n, 255);
        const std::vector<std::uint8_t> rough = has[int(MapSlot::Roughness)] ? grey(build(MapSlot::Roughness, 128, 128, 128), false)
                                              : has[int(MapSlot::Gloss)]      ? grey(build(MapSlot::Gloss, 128, 128, 128), true)
                                                                              : std::vector<std::uint8_t>(n, 255);
        const std::vector<std::uint8_t> metal = m.metal ? grey(build(MapSlot::Metal, 0, 0, 0), false)
                                                        : std::vector<std::uint8_t>(n, 255);
        m.orm.resize(n * 4);
        double sumR = 0.0, sumM = 0.0;
        for (std::size_t j = 0; j < n; ++j) {
            m.orm[j * 4] = ao[j]; m.orm[j * 4 + 1] = rough[j]; m.orm[j * 4 + 2] = metal[j]; m.orm[j * 4 + 3] = 255;
            sumR += rough[j];
            sumM += metal[j];
        }
        // Never 0: the shader divides by the average.
        m.roughnessMean = std::max(1.0f / 255.0f, float(sumR / (double(n) * 255.0)));
        m.metalMean     = std::max(1.0f / 255.0f, float(sumM / (double(n) * 255.0)));
    }
    if (has[int(MapSlot::Emission)]) m.emission = build(MapSlot::Emission, 0, 0, 0);
    if (has[int(MapSlot::Opacity)]) {
        const std::vector<std::uint8_t> a = grey(build(MapSlot::Opacity, 255, 255, 255), false);
        m.opacity.resize(n * 4);
        for (std::size_t j = 0; j < n; ++j) {
            m.opacity[j * 4] = m.opacity[j * 4 + 1] = m.opacity[j * 4 + 2] = a[j];
            m.opacity[j * 4 + 3] = 255;
        }
    }
    return m;
}

std::vector<std::string> materialSuffixes(const Document& d) {
    bool has[int(MapSlot::Count)] = {};
    bool color = false;
    for (int i = 0; i < d.layerCount(); ++i) {
        const MapSlot s = mapSlotFor(d.layer(i).name);
        has[int(s)] = true;
        color |= s == MapSlot::Color && d.layer(i).visible;
    }
    std::vector<std::string> out;
    if (color) out.push_back("_color");
    if (has[int(MapSlot::Normal)] || has[int(MapSlot::Height)]) out.push_back("_normal");
    if (has[int(MapSlot::Roughness)] || has[int(MapSlot::Gloss)] || has[int(MapSlot::Metal)] || has[int(MapSlot::Occlusion)])
        out.push_back("_orm");
    if (has[int(MapSlot::Emission)]) out.push_back("_emission");
    if (has[int(MapSlot::Opacity)]) out.push_back("_opacity");
    return out;
}

bool writeMaterial(const MaterialImages& m, const std::string& dir, const std::string& file,
                   const std::string& name, MaterialMaps& out, std::string& err) {
    out = MaterialMaps{};
    out.name = name;
    out.roughness = m.roughness;
    out.metal = m.metal;
    out.roughnessMean = m.roughnessMean;
    out.metalMean = m.metalMean;
    std::error_code ec;
    fs::create_directories(dir, ec);
    const std::pair<const Pixels*, std::string*> maps[] = {
        {&m.color, &out.color}, {&m.normal, &out.normal}, {&m.orm, &out.orm},
        {&m.emission, &out.emission}, {&m.opacity, &out.opacity}};
    const char* const suffix[] = {"_color", "_normal", "_orm", "_emission", "_opacity"};
    for (int i = 0; i < 5; ++i) {
        if (maps[i].first->empty()) continue;
        const fs::path p = fs::path(dir) / (file + suffix[i] + ".png");
        if (!stbi_write_png(p.string().c_str(), m.w, m.h, 4, maps[i].first->data(), m.w * 4)) {
            err = "Could not write " + p.generic_string();
            return false;
        }
        *maps[i].second = p.generic_string();
    }
    if (out.color.empty() && out.normal.empty() && out.orm.empty() && out.emission.empty() && out.opacity.empty()) {
        err = "Nothing to build: no layer is shown, and none is named for a map.";
        return false;
    }
    return true;
}

} // namespace img
