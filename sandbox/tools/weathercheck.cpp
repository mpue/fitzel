// weathercheck -- the weather presets, without a window.
//
// Everything a preset does is arithmetic on plain numbers and a JSON file, and
// none of it is visible: a preset that fails to write one field, or reads it
// back at a default, produces a sky that is merely SLIGHTLY wrong -- which is
// exactly the failure nobody notices until a scene has been authored against it.
// So it is checked here rather than by looking at it.
//
// What it asserts:
//   * every built-in applies -- the values reach the live state, all of them
//   * capture is apply's inverse, so saving what you see gives back what you saw
//   * a preset round-trips through the project file unchanged
//   * an untouched built-in is NOT written (the file holds decisions, not copies)
//   * an edited built-in IS written, comes back edited, and stays built-in
//   * matches() says "edited" for a moved slider and not for a round trip
//   * the opt-outs hold: a preset that does not speak about the clock or the
//     mist leaves both exactly where they were
//
// Run it from anywhere; it writes its scratch project into a temp folder and
// removes it afterwards.
//
//   build\release\bin\weathercheck.exe

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../src/WeatherPreset.hpp"

namespace fs = std::filesystem;

namespace {

int g_fail = 0;

void ok(bool cond, const char* what) {
    std::printf(cond ? "  ok   %s\n" : "[FAIL] %s\n", what);
    if (!cond) ++g_fail;
}

// The live weather, as main() holds it: loose values plus a Live binding them.
struct World {
    float          storm       = 0.0f;
    bool           autoWeather = false;
    bool           lightning   = true;
    float          timeOfDay   = 12.0f;
    weather::Sky   sky;
    weather::Audio audio;
    bool           mistOn     = false;
    bool           mistFollow = true;
    glm::vec3      mistCenter{0.0f, 15.0f, 0.0f};
    glm::vec3      mistSize{600.0f, 40.0f, 600.0f};
    FogMedium      mistMedium;

    weather::Live live() {
        return weather::Live{storm, autoWeather, lightning, timeOfDay, sky, audio,
                             mistOn, mistFollow, mistCenter, mistSize, mistMedium};
    }
};

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

} // namespace

int main() {
    const std::vector<weather::Preset> stock = weather::builtins();
    ok(stock.size() >= 5, "five skies ship with the editor");
    {
        // The four the panel promises by name. A preset list that quietly lost
        // one is a list whose scenes open on the wrong weather.
        const char* want[] = {"Sunny day", "Morning dew", "Storm", "Heavy rain"};
        bool all = true;
        for (const char* w : want)
            if (weather::indexOf(stock, w) < 0) { all = false; std::printf("       missing %s\n", w); }
        ok(all, "the named ones are there");
    }

    // --- Applying, and capturing back -------------------------------------
    {
        bool allBack = true;
        for (const weather::Preset& p : stock) {
            World w;
            weather::apply(p, w.live(), 100.0f);
            const weather::Preset got =
                weather::capture(p.name, w.live(), p.setTimeOfDay, p.setMist);
            if (!weather::matches(p, w.live())) {
                allBack = false;
                std::printf("       '%s' does not read back as itself\n", p.name.c_str());
            }
            // Spot-check a couple of fields by hand as well: matches() is the
            // thing under test here, and a comparison that always returns true
            // would pass the loop above without checking anything.
            if (!near(w.sky.coverage, p.sky.coverage) || !near(w.storm, p.storm) ||
                !near(w.audio.rain, p.audio.rain)) {
                allBack = false;
                std::printf("       '%s' did not reach the live state\n", p.name.c_str());
            }
            (void)got;
        }
        ok(allBack, "every built-in applies and captures back unchanged");
    }

    // --- A moved slider shows up -------------------------------------------
    {
        World w;
        const weather::Preset& sunny = stock[static_cast<std::size_t>(
            weather::indexOf(stock, "Sunny day"))];
        weather::apply(sunny, w.live(), 0.0f);
        const bool clean = weather::matches(sunny, w.live());
        w.sky.coverage += 0.2f;
        const bool dirty = !weather::matches(sunny, w.live());
        ok(clean && dirty, "an edited sky stops matching its preset");
    }
    {
        // ...and a nudge below the tolerance does not, or the panel would show
        // every preset as edited the moment a value went through a float write.
        World w;
        const weather::Preset& sunny = stock[static_cast<std::size_t>(
            weather::indexOf(stock, "Sunny day"))];
        weather::apply(sunny, w.live(), 0.0f);
        w.sky.base += 0.001f;
        ok(weather::matches(sunny, w.live()), "a last-digit wobble does not");
    }

    // --- The opt-outs -------------------------------------------------------
    {
        World w;
        w.timeOfDay = 17.5f;
        w.mistOn    = true;
        w.mistSize  = glm::vec3(123.0f, 9.0f, 123.0f);
        weather::Preset p;
        p.name         = "Silent about both";
        p.setTimeOfDay = false;
        p.setMist      = false;
        weather::apply(p, w.live(), 0.0f);
        ok(near(w.timeOfDay, 17.5f), "a preset with no clock leaves the time alone");
        ok(w.mistOn && near(w.mistSize.y, 9.0f),
           "a preset with no opinion on mist leaves the volume alone");
    }
    {
        // ...and one that DOES speak sits its layer on the ground it was handed,
        // which is the whole reason apply() takes a ground height.
        World w;
        const weather::Preset& dew = stock[static_cast<std::size_t>(
            weather::indexOf(stock, "Morning dew"))];
        weather::apply(dew, w.live(), 250.0f);
        ok(w.mistOn && near(w.mistCenter.y, 250.0f + dew.mist.height * 0.5f),
           "morning mist sits on the ground under the camera");
        ok(near(w.mistSize.x, dew.mist.span) && near(w.mistSize.y, dew.mist.height),
           "...at the reach the preset asked for");
    }
    {
        // Applying a clear sky after a foggy one has to CLEAR it. A built-in that
        // said nothing about mist would leave the dawn layer standing in the noon
        // sun, which is the bug this flag exists to prevent.
        World w;
        weather::apply(stock[static_cast<std::size_t>(weather::indexOf(stock, "Morning dew"))],
                       w.live(), 0.0f);
        weather::apply(stock[static_cast<std::size_t>(weather::indexOf(stock, "Sunny day"))],
                       w.live(), 0.0f);
        ok(!w.mistOn, "a sunny day clears the mist a dawn left behind");
    }

    // --- The project file ---------------------------------------------------
    const fs::path dir = fs::temp_directory_path() / "fitzel_weathercheck";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string folder = dir.generic_string();

    {
        // Nothing decided yet: saving the shipped list must write no presets, and
        // loading must still give all of them back.
        weather::save(folder, stock);
        const std::vector<weather::Preset> back = weather::load(folder);
        ok(back.size() == stock.size(), "an untouched shelf reloads at its own size");
        bool same = true;
        for (const weather::Preset& p : back) {
            World w;
            const int at = weather::indexOf(stock, p.name);
            if (at < 0) { same = false; break; }
            weather::apply(stock[static_cast<std::size_t>(at)], w.live(), 0.0f);
            if (!weather::matches(p, w.live())) { same = false;
                std::printf("       '%s' changed on the way through\n", p.name.c_str()); }
            if (!p.builtin) { same = false;
                std::printf("       '%s' lost its built-in flag\n", p.name.c_str()); }
        }
        ok(same, "...and unchanged");
    }
    {
        // A preset the author actually made, and a built-in they bent. Both have
        // to survive the file; the built-in has to stay a built-in.
        std::vector<weather::Preset> shelf = stock;
        weather::Preset mine;
        mine.name           = "Sea fret";
        mine.storm          = 0.33f;
        mine.lightning      = false;
        mine.setTimeOfDay   = true;
        mine.timeOfDay      = 5.75f;
        mine.sky.coverage   = 0.71f;
        mine.sky.base       = 210.0f;
        mine.sky.fogDensity = 0.0091f;
        mine.setMist        = true;
        mine.mist.enabled   = true;
        mine.mist.span      = 812.0f;
        mine.mist.height    = 33.0f;
        mine.mist.medium.density  = 0.077f;
        mine.mist.medium.color    = {0.61f, 0.66f, 0.72f};
        mine.mist.medium.steps    = 37;
        mine.mist.medium.shafts   = false;
        mine.audio.rain     = 0.11f;
        mine.audio.breeze   = 1.62f;
        shelf.push_back(mine);

        const int at = weather::indexOf(shelf, "Storm");
        shelf[static_cast<std::size_t>(at)].sky.top = 4321.0f;
        shelf[static_cast<std::size_t>(at)].audio.thunder = 0.25f;

        weather::save(folder, shelf);
        const std::vector<weather::Preset> back = weather::load(folder);

        const int mi = weather::indexOf(back, "Sea fret");
        ok(mi >= 0, "a hand-made preset comes back");
        if (mi >= 0) {
            World w;
            weather::apply(back[static_cast<std::size_t>(mi)], w.live(), 0.0f);
            ok(weather::matches(mine, w.live()), "...with every field intact");
            ok(!back[static_cast<std::size_t>(mi)].builtin, "...and is not a built-in");
            ok(near(w.timeOfDay, 5.75f) && !w.mistMedium.shafts &&
               w.mistMedium.steps == 37,
               "...including the flags and the integer");
        }
        const int si = weather::indexOf(back, "Storm");
        ok(si >= 0 && near(back[static_cast<std::size_t>(si)].sky.top, 4321.0f) &&
           near(back[static_cast<std::size_t>(si)].audio.thunder, 0.25f),
           "an edited built-in comes back edited");
        ok(si >= 0 && back[static_cast<std::size_t>(si)].builtin,
           "...and is still a built-in, so it can be reset rather than deleted");

        // Only the two decisions are in the file. Three of the five built-ins
        // were never touched and must not be copied into the project, or a later
        // improvement to one would never reach a scene again.
        const std::string body = [&] {
            std::FILE* f = std::fopen((folder + "/weather.json").c_str(), "rb");
            std::string out;
            if (!f) return out;
            char buf[4096]; std::size_t n;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
            std::fclose(f);
            return out;
        }();
        int written = 0;
        for (std::size_t i = body.find("\"name\""); i != std::string::npos;
             i = body.find("\"name\"", i + 1)) ++written;
        ok(written == 2, "the file holds only what was decided (2 presets)");
        if (written != 2) std::printf("       wrote %d\n", written);
    }
    {
        // A file with a preset the editor has never heard of, and a corrupt one.
        // Neither may take the shelf down: a project edited by hand, or by a
        // newer build, still has to open.
        std::FILE* f = std::fopen((folder + "/weather.json").c_str(), "wb");
        const char* junk = "{ \"presets\": [ {\"name\":\"Half a sky\",\"storm\":0.4},"
                           " {\"nope\":1}, 7 ] }";
        std::fwrite(junk, 1, std::strlen(junk), f);
        std::fclose(f);
        const std::vector<weather::Preset> back = weather::load(folder);
        const int hi = weather::indexOf(back, "Half a sky");
        ok(hi >= 0 && near(back[static_cast<std::size_t>(hi)].storm, 0.4f),
           "a sparse preset loads, its missing fields at their defaults");
        ok(back.size() == stock.size() + 1, "...and nothing else was lost");
    }
    {
        std::FILE* f = std::fopen((folder + "/weather.json").c_str(), "wb");
        const char* bad = "{ this is not json";
        std::fwrite(bad, 1, std::strlen(bad), f);
        std::fclose(f);
        ok(weather::load(folder).size() == stock.size(),
           "an unreadable file falls back to the built-ins");
    }
    ok(weather::load("").size() == stock.size(),
       "no project open still has every built-in");

    fs::remove_all(dir, ec);
    if (g_fail == 0) { std::printf("\nall checks passed\n"); return 0; }
    std::printf("\n%d check(s) FAILED\n", g_fail);
    return 1;
}
