#include "WeatherPreset.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>

#include <nlohmann/json.hpp>

#include <fitzel/asset/Vfs.hpp>

namespace weather {
namespace {

std::string presetsPath(const std::string& projectFolder) {
    return projectFolder + "/weather.json";
}

// Case-insensitive compare, for looking a preset up by the name in a combo.
bool sameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// A tolerance rather than ==, everywhere two of these are compared. Every number
// in a preset came off a slider and went through a JSON round trip, so exact
// equality would say "edited" about a preset nobody has touched -- which is the
// one thing the modified marker must never do.
bool near(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps * std::max(1.0f, std::max(std::fabs(a), std::fabs(b)));
}
bool near(const glm::vec3& a, const glm::vec3& b) {
    return near(a.x, b.x) && near(a.y, b.y) && near(a.z, b.z);
}

bool equalSky(const Sky& a, const Sky& b) {
    return near(a.coverage, b.coverage) && near(a.density, b.density) &&
           near(a.scale, b.scale)       && near(a.wind, b.wind) &&
           near(a.base, b.base)         && near(a.top, b.top) &&
           near(a.cirrus, b.cirrus)     && near(a.cirrusHeight, b.cirrusHeight) &&
           near(a.cirrusWind, b.cirrusWind) && near(a.contrails, b.contrails) &&
           near(a.fogDensity, b.fogDensity) && near(a.fogFalloff, b.fogFalloff);
}

bool equalMedium(const FogMedium& a, const FogMedium& b) {
    return near(a.density, b.density) && near(a.color, b.color) &&
           near(a.coverage, b.coverage) && near(a.noiseScale, b.noiseScale) &&
           near(a.verticalDetail, b.verticalDetail) && near(a.detail, b.detail) &&
           near(a.warp, b.warp) && near(a.wind, b.wind) &&
           near(a.edge, b.edge) && near(a.heightFalloff, b.heightFalloff) &&
           near(a.anisotropy, b.anisotropy) &&
           near(a.sunIntensity, b.sunIntensity) &&
           near(a.ambientIntensity, b.ambientIntensity) &&
           a.shafts == b.shafts && a.selfShadow == b.selfShadow &&
           a.steps == b.steps;
}

bool equalMist(const Mist& a, const Mist& b) {
    return a.enabled == b.enabled && a.follow == b.follow &&
           near(a.span, b.span) && near(a.height, b.height) &&
           equalMedium(a.medium, b.medium);
}

bool equalAudio(const Audio& a, const Audio& b) {
    return near(a.rain, b.rain) && near(a.wind, b.wind) &&
           near(a.breeze, b.breeze) && near(a.storm, b.storm) &&
           near(a.thunder, b.thunder);
}

// Everything but the name and the built-in flag: two presets are the same
// WEATHER when they would put the sky in the same place, whatever they are
// called.
bool equalWeather(const Preset& a, const Preset& b) {
    if (!(near(a.storm, b.storm) && a.autoWeather == b.autoWeather &&
          a.lightning == b.lightning)) return false;
    if (a.setTimeOfDay != b.setTimeOfDay) return false;
    if (a.setTimeOfDay && !near(a.timeOfDay, b.timeOfDay)) return false;
    if (!equalSky(a.sky, b.sky)) return false;
    if (a.setMist != b.setMist) return false;
    if (a.setMist && !equalMist(a.mist, b.mist)) return false;
    return equalAudio(a.audio, b.audio);
}

// --- JSON -------------------------------------------------------------------
// Read-with-fallback throughout: every field defaults to what the struct already
// holds, so a file written by an older editor loads with the new fields at their
// defaults instead of at zero. A weather whose cloud top read 0 because the key
// was not there yet is a sky with no clouds in it, which looks like a bug in the
// renderer rather than a missing key.
float  jf(const nlohmann::json& j, const char* k, float d) { return j.value(k, d); }
bool   jb(const nlohmann::json& j, const char* k, bool d)  { return j.value(k, d); }
int    ji(const nlohmann::json& j, const char* k, int d)   { return j.value(k, d); }

glm::vec3 jv3(const nlohmann::json& j, const char* k, const glm::vec3& d) {
    const auto it = j.find(k);
    if (it == j.end() || !it->is_array() || it->size() != 3) return d;
    return glm::vec3((*it)[0].get<float>(), (*it)[1].get<float>(),
                     (*it)[2].get<float>());
}
nlohmann::json v3(const glm::vec3& v) {
    return nlohmann::json::array({v.x, v.y, v.z});
}

Sky readSky(const nlohmann::json& j) {
    Sky s;
    s.coverage     = jf(j, "coverage", s.coverage);
    s.density      = jf(j, "density", s.density);
    s.scale        = jf(j, "scale", s.scale);
    s.wind         = jf(j, "wind", s.wind);
    s.base         = jf(j, "base", s.base);
    s.top          = jf(j, "top", s.top);
    s.cirrus       = jf(j, "cirrus", s.cirrus);
    s.cirrusHeight = jf(j, "cirrusHeight", s.cirrusHeight);
    s.cirrusWind   = jf(j, "cirrusWind", s.cirrusWind);
    s.contrails    = jf(j, "contrails", s.contrails);
    s.fogDensity   = jf(j, "fogDensity", s.fogDensity);
    s.fogFalloff   = jf(j, "fogFalloff", s.fogFalloff);
    return s;
}
nlohmann::json writeSky(const Sky& s) {
    return {{"coverage", s.coverage}, {"density", s.density}, {"scale", s.scale},
            {"wind", s.wind}, {"base", s.base}, {"top", s.top},
            {"cirrus", s.cirrus}, {"cirrusHeight", s.cirrusHeight},
            {"cirrusWind", s.cirrusWind}, {"contrails", s.contrails},
            {"fogDensity", s.fogDensity}, {"fogFalloff", s.fogFalloff}};
}

FogMedium readMedium(const nlohmann::json& j) {
    FogMedium m;
    m.density          = jf(j, "density", m.density);
    m.color            = jv3(j, "color", m.color);
    m.coverage         = jf(j, "coverage", m.coverage);
    m.noiseScale       = jf(j, "noiseScale", m.noiseScale);
    m.verticalDetail   = jf(j, "verticalDetail", m.verticalDetail);
    m.detail           = jf(j, "detail", m.detail);
    m.warp             = jf(j, "warp", m.warp);
    m.wind             = jv3(j, "wind", m.wind);
    m.edge             = jf(j, "edge", m.edge);
    m.heightFalloff    = jf(j, "heightFalloff", m.heightFalloff);
    m.anisotropy       = jf(j, "anisotropy", m.anisotropy);
    m.sunIntensity     = jf(j, "sun", m.sunIntensity);
    m.ambientIntensity = jf(j, "ambient", m.ambientIntensity);
    m.shafts           = jb(j, "shafts", m.shafts);
    m.selfShadow       = jb(j, "selfShadow", m.selfShadow);
    m.steps            = ji(j, "steps", m.steps);
    return m;
}
nlohmann::json writeMedium(const FogMedium& m) {
    return {{"density", m.density}, {"color", v3(m.color)},
            {"coverage", m.coverage}, {"noiseScale", m.noiseScale},
            {"verticalDetail", m.verticalDetail}, {"detail", m.detail},
            {"warp", m.warp}, {"wind", v3(m.wind)}, {"edge", m.edge},
            {"heightFalloff", m.heightFalloff}, {"anisotropy", m.anisotropy},
            {"sun", m.sunIntensity}, {"ambient", m.ambientIntensity},
            {"shafts", m.shafts}, {"selfShadow", m.selfShadow},
            {"steps", m.steps}};
}

Preset readPreset(const nlohmann::json& j) {
    Preset p;
    p.name         = j.value("name", std::string{});
    p.storm        = jf(j, "storm", p.storm);
    p.autoWeather  = jb(j, "auto", p.autoWeather);
    p.lightning    = jb(j, "lightning", p.lightning);
    p.setTimeOfDay = jb(j, "setTimeOfDay", p.setTimeOfDay);
    p.timeOfDay    = jf(j, "timeOfDay", p.timeOfDay);
    if (const auto it = j.find("sky"); it != j.end() && it->is_object())
        p.sky = readSky(*it);
    p.setMist = jb(j, "setMist", p.setMist);
    if (const auto it = j.find("mist"); it != j.end() && it->is_object()) {
        p.mist.enabled = jb(*it, "enabled", p.mist.enabled);
        p.mist.follow  = jb(*it, "follow", p.mist.follow);
        p.mist.span    = jf(*it, "span", p.mist.span);
        p.mist.height  = jf(*it, "height", p.mist.height);
        if (const auto m = it->find("medium"); m != it->end() && m->is_object())
            p.mist.medium = readMedium(*m);
    }
    if (const auto it = j.find("audio"); it != j.end() && it->is_object()) {
        p.audio.rain    = jf(*it, "rain", p.audio.rain);
        p.audio.wind    = jf(*it, "wind", p.audio.wind);
        p.audio.breeze  = jf(*it, "breeze", p.audio.breeze);
        p.audio.storm   = jf(*it, "storm", p.audio.storm);
        p.audio.thunder = jf(*it, "thunder", p.audio.thunder);
    }
    return p;
}

nlohmann::json writePreset(const Preset& p) {
    nlohmann::json j;
    j["name"]         = p.name;
    j["storm"]        = p.storm;
    j["auto"]         = p.autoWeather;
    j["lightning"]    = p.lightning;
    j["setTimeOfDay"] = p.setTimeOfDay;
    j["timeOfDay"]    = p.timeOfDay;
    j["sky"]          = writeSky(p.sky);
    j["setMist"]      = p.setMist;
    j["mist"]         = {{"enabled", p.mist.enabled}, {"follow", p.mist.follow},
                         {"span", p.mist.span}, {"height", p.mist.height},
                         {"medium", writeMedium(p.mist.medium)}};
    j["audio"]        = {{"rain", p.audio.rain}, {"wind", p.audio.wind},
                         {"breeze", p.audio.breeze}, {"storm", p.audio.storm},
                         {"thunder", p.audio.thunder}};
    return j;
}

} // namespace

std::vector<Preset> builtins() {
    std::vector<Preset> out;

    {   // Fair weather at midday: what a scene should look like when nobody has
        // decided anything about the sky yet. Scattered cumulus rather than a
        // clear blue dome -- an empty sky reads as a missing feature, and the
        // shadows a few clouds cast are most of what says the ground is outside.
        Preset p;
        p.name              = "Sunny day";
        p.builtin           = true;
        p.storm             = 0.0f;
        p.lightning         = false;
        p.setTimeOfDay      = true;
        p.timeOfDay         = 13.0f;
        p.sky.coverage      = 0.22f;
        p.sky.density       = 0.9f;
        p.sky.scale         = 0.0009f;
        p.sky.wind          = 4.0f;
        p.sky.base          = 900.0f;
        p.sky.top           = 2600.0f;
        p.sky.cirrus        = 0.18f;
        p.sky.cirrusHeight  = 1600.0f;
        p.sky.contrails     = 0.15f;
        p.sky.fogDensity    = 0.0032f;
        p.sky.fogFalloff    = 0.030f;
        // Says the mist is OFF rather than saying nothing about it: picking a
        // sunny day after a dawn one has to clear the valley, or the preset is
        // only half a weather.
        p.setMist           = true;
        p.mist.enabled      = false;
        p.audio.rain        = 0.0f;
        p.audio.wind        = 0.0f;
        p.audio.breeze      = 1.0f;
        p.audio.storm       = 0.0f;
        out.push_back(p);
    }
    {   // First light with the air still full of last night's water. The whole
        // effect is the volumetric layer -- low, thick, barely moving, lit almost
        // edge-on by a sun that is only just up, which is what makes the shafts.
        // The deck above it is broken and low so the light comes through it.
        Preset p;
        p.name             = "Morning dew";
        p.builtin          = true;
        p.storm            = 0.10f;
        p.lightning        = false;
        p.setTimeOfDay     = true;
        p.timeOfDay        = 6.3f;
        p.sky.coverage     = 0.36f;
        p.sky.density      = 1.1f;
        p.sky.scale        = 0.0011f;
        p.sky.wind         = 2.0f;
        p.sky.base         = 380.0f;
        p.sky.top          = 1500.0f;
        p.sky.cirrus       = 0.30f;
        p.sky.cirrusHeight = 1500.0f;
        p.sky.cirrusWind   = 1.6f;
        p.sky.contrails    = 0.0f;
        p.sky.fogDensity   = 0.0075f;
        p.sky.fogFalloff   = 0.045f;
        p.setMist                     = true;
        p.mist.enabled                = true;
        p.mist.follow                 = true;
        p.mist.span                   = 900.0f;
        p.mist.height                 = 26.0f;
        p.mist.medium.density         = 0.055f;
        p.mist.medium.color           = {0.87f, 0.90f, 0.94f};
        p.mist.medium.coverage        = 0.42f;
        p.mist.medium.noiseScale      = 0.008f;
        // A 26 m layer is a fraction of one noise feature tall at the default
        // ratio, and a layer that thin needs its billows squashed harder still or
        // it comes out as a flat pattern pulled upward -- see FogMedium.
        p.mist.medium.verticalDetail  = 5.0f;
        p.mist.medium.detail          = 0.50f;
        p.mist.medium.warp            = 0.30f;
        p.mist.medium.wind            = {1.2f, 0.0f, 0.4f};
        p.mist.medium.edge            = 0.30f;
        p.mist.medium.heightFalloff   = 1.3f;
        p.mist.medium.anisotropy      = 0.62f;
        p.mist.medium.sunIntensity    = 1.25f;
        p.mist.medium.shafts          = true;
        p.mist.medium.selfShadow      = true;
        p.mist.medium.steps           = 40;
        p.audio.rain       = 0.0f;
        p.audio.wind       = 0.15f;
        p.audio.breeze     = 0.85f;
        p.audio.storm      = 0.0f;
        out.push_back(p);
    }
    {   // A grey lid. Not a storm -- nothing is falling and nothing is flashing --
        // but a full deck low enough to flatten every shadow in the scene, which
        // is the light most of northern Europe is actually under.
        Preset p;
        p.name             = "Overcast";
        p.builtin          = true;
        p.storm            = 0.40f;
        p.lightning        = false;
        p.sky.coverage     = 0.86f;
        p.sky.density      = 1.9f;
        p.sky.scale        = 0.0007f;
        p.sky.wind         = 9.0f;
        p.sky.base         = 520.0f;
        p.sky.top          = 2200.0f;
        p.sky.cirrus       = 0.0f;   // nothing to see through a full deck
        p.sky.contrails    = 0.0f;
        p.sky.fogDensity   = 0.0065f;
        p.sky.fogFalloff   = 0.026f;
        p.setMist          = true;
        p.mist.enabled     = false;
        p.audio.rain       = 0.0f;
        p.audio.wind       = 0.8f;
        p.audio.breeze     = 0.5f;
        p.audio.storm      = 0.0f;
        out.push_back(p);
    }
    {   // Rain, hard, and nothing else. Deliberately WITHOUT lightning: at this
        // storm value the old dial armed the flashes automatically, so a scene
        // that wanted a downpour got a thunderstorm and had no way to say no.
        // A thin ground layer comes with it -- heavy rain on warm ground is the
        // one everyday weather that really does raise mist.
        Preset p;
        p.name             = "Heavy rain";
        p.builtin          = true;
        p.storm            = 0.80f;
        p.lightning        = false;
        p.sky.coverage     = 0.95f;
        p.sky.density      = 2.4f;
        p.sky.scale        = 0.0006f;
        p.sky.wind         = 16.0f;
        p.sky.base         = 280.0f;
        p.sky.top          = 2600.0f;
        p.sky.cirrus       = 0.0f;
        p.sky.contrails    = 0.0f;
        p.sky.fogDensity   = 0.0100f;
        p.sky.fogFalloff   = 0.022f;
        p.setMist                    = true;
        p.mist.enabled               = true;
        p.mist.follow                = true;
        p.mist.span                  = 900.0f;
        p.mist.height                = 18.0f;
        p.mist.medium.density        = 0.030f;
        p.mist.medium.color          = {0.78f, 0.81f, 0.86f};
        p.mist.medium.coverage       = 0.55f;
        p.mist.medium.noiseScale     = 0.011f;
        p.mist.medium.verticalDetail = 5.0f;
        p.mist.medium.wind           = {3.0f, 0.0f, 1.4f};
        p.mist.medium.heightFalloff  = 1.4f;
        // No shafts and no self-shadow: there is no sun to make either, and both
        // are the expensive half of the march.
        p.mist.medium.shafts         = false;
        p.mist.medium.selfShadow     = false;
        p.mist.medium.steps          = 28;
        p.audio.rain       = 1.25f;
        p.audio.wind       = 0.85f;
        p.audio.breeze     = 0.0f;
        p.audio.storm      = 0.55f;
        out.push_back(p);
    }
    {   // The whole dial. Rain, gale, a ceiling almost on the ground, and the
        // flashes armed -- the one preset that wants them.
        Preset p;
        p.name             = "Storm";
        p.builtin          = true;
        p.storm            = 1.0f;
        p.lightning        = true;
        p.sky.coverage     = 1.0f;
        p.sky.density      = 2.9f;
        p.sky.scale        = 0.0006f;
        p.sky.wind         = 24.0f;
        p.sky.base         = 140.0f;
        p.sky.top          = 3000.0f;
        p.sky.cirrus       = 0.0f;
        p.sky.contrails    = 0.0f;
        p.sky.fogDensity   = 0.0130f;
        p.sky.fogFalloff   = 0.020f;
        // Off, and meant: a gale scours the air near the ground, and marching a
        // volume in the frame that is already paying for rain, spray and a
        // lightning-lit shadow pass is the wrong place to spend it.
        p.setMist          = true;
        p.mist.enabled     = false;
        p.audio.rain       = 1.20f;
        p.audio.wind       = 1.15f;
        p.audio.breeze     = 0.0f;
        p.audio.storm      = 1.20f;
        p.audio.thunder    = 1.20f;
        out.push_back(p);
    }
    return out;
}

std::vector<Preset> load(const std::string& projectFolder) {
    std::vector<Preset> out = builtins();
    if (projectFolder.empty()) return out;
    const std::string body = fitzel::vfs::readText(presetsPath(projectFolder));
    if (body.empty()) return out;
    nlohmann::json j;
    try { j = nlohmann::json::parse(body); }
    catch (const nlohmann::json::exception&) { return out; }
    const auto it = j.find("presets");
    if (it == j.end() || !it->is_array()) return out;
    for (const nlohmann::json& e : *it) {
        if (!e.is_object()) continue;
        Preset p = readPreset(e);
        if (p.name.empty()) continue;
        const int at = indexOf(out, p.name);
        if (at >= 0) {
            // A saved preset under a built-in's name is an EDITED built-in, and it
            // stays built-in: the flag says "this name is part of the editor",
            // not "these numbers are". Otherwise updating Storm once would make
            // it deletable, and there would be no way back to the shipped one.
            p.builtin = out[static_cast<std::size_t>(at)].builtin;
            out[static_cast<std::size_t>(at)] = p;
        } else {
            p.builtin = false;
            out.push_back(p);
        }
    }
    return out;
}

void save(const std::string& projectFolder, const std::vector<Preset>& presets) {
    if (projectFolder.empty()) return;
    const std::vector<Preset> stock = builtins();
    nlohmann::json arr = nlohmann::json::array();
    for (const Preset& p : presets) {
        if (p.name.empty()) continue;
        // An untouched built-in is not written. The file then holds only what the
        // author decided, and a built-in improved in a later build reaches every
        // project that never overrode it -- which is the whole reason a shipped
        // preset is not simply copied into the project the first time it is used.
        const int at = indexOf(stock, p.name);
        if (at >= 0 && equalWeather(stock[static_cast<std::size_t>(at)], p)) continue;
        arr.push_back(writePreset(p));
    }
    nlohmann::json j;
    j["presets"] = arr;
    std::ofstream f(presetsPath(projectFolder));
    if (f) f << j.dump(2) << '\n';
}

int indexOf(const std::vector<Preset>& presets, const std::string& name) {
    for (std::size_t i = 0; i < presets.size(); ++i)
        if (sameName(presets[i].name, name)) return static_cast<int>(i);
    return -1;
}

void apply(const Preset& p, const Live& live, float groundY) {
    live.storm       = p.storm;
    live.autoWeather = p.autoWeather;
    live.lightning   = p.lightning;
    live.sky         = p.sky;
    live.audio       = p.audio;
    if (p.setTimeOfDay) live.timeOfDay = p.timeOfDay;
    if (p.setMist) {
        live.mistOn     = p.mist.enabled;
        live.mistFollow = p.mist.follow;
        live.mistSize   = glm::vec3(p.mist.span, p.mist.height, p.mist.span);
        live.mistMedium = p.mist.medium;
        // Sit the layer on the ground under the eye, exactly as the panel's own
        // button does. The X/Z of the centre is left alone: with `follow` on it
        // is overwritten by the camera every frame anyway, and with it off the
        // author has placed the box somewhere on purpose.
        if (p.mist.enabled) live.mistCenter.y = groundY + p.mist.height * 0.5f;
    }
}

Preset capture(const std::string& name, const Live& live,
               bool setTimeOfDay, bool setMist) {
    Preset p;
    p.name         = name;
    p.storm        = live.storm;
    p.autoWeather  = live.autoWeather;
    p.lightning    = live.lightning;
    p.setTimeOfDay = setTimeOfDay;
    p.timeOfDay    = live.timeOfDay;
    p.sky          = live.sky;
    p.audio        = live.audio;
    p.setMist      = setMist;
    p.mist.enabled = live.mistOn;
    p.mist.follow  = live.mistFollow;
    // The box is square in plan wherever it came from -- the panel edits X and Z
    // as one number and so does a preset. Taking the larger of the two is what
    // makes a hand-stretched box round-trip to the reach it actually had.
    p.mist.span    = std::max(live.mistSize.x, live.mistSize.z);
    p.mist.height  = live.mistSize.y;
    p.mist.medium  = live.mistMedium;
    return p;
}

bool matches(const Preset& p, const Live& live) {
    return equalWeather(p, capture(p.name, live, p.setTimeOfDay, p.setMist));
}

} // namespace weather
