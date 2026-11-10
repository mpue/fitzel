#pragma once

#include <vector>

#include <glm/glm.hpp>

// Where the sun stands, from where on Earth the scene is, the date and the clock.
//
// The sun used to be an hour angle on a fixed arc: a latitude, a declination
// slider standing in for the season, and a clock that was the sun's own time
// (noon = highest). Good enough to light a scene; useless for putting one
// somewhere. An alpine valley at 47 degrees north on the 21st of June stands in
// the sun from mid-morning; the same valley on a sun arc for the equator in
// February, at the scene's default of seven in the morning, is in the shadow of
// its own mountains all day -- and nothing in the editor said why.
//
// So the place is a place: latitude, longitude, the time zone its clocks keep
// (summer time included), and a day of the year. The clock in the Sky panel is
// then the clock on the wall there, and the sun follows the NOAA solar position
// equations (the equation of time and the declination from the fractional year):
// within a minute of an almanac for sunrise and noon, which is all a scene needs.
//
// Pure arithmetic: no ImGui, no GL, no scene. The editor widgets for it live in
// SunPlaceUi.hpp; the harness (worldcheck) checks it against known places.
namespace sunpos {

struct Place {
    float latitude   = 50.11f;  // degrees north (south negative) -- Frankfurt
    float longitude  = 8.68f;   // degrees east (west negative)
    float utcOffset  = 1.0f;    // the zone's standard time, hours east of UTC
    bool  summerTime = true;    // the place keeps summer time (+1 h) in its season
    int   day        = 172;     // day of the year, 1 = 1 January (172 = 21 June)

    // Scenes from before places: the clock is the sun's own time and the season
    // a declination, read so they light exactly as they did -- the old arc
    // through `latitude` alone. Editing the place clears it.
    bool  legacy            = false;
    float legacyDeclination = -10.4f;

    bool operator==(const Place&) const = default;
};

struct Sun {
    glm::vec3 dir{0.0f, 1.0f, 0.0f};  // towards the sun; east +X, north -Z (the scene's axes)
    float elevation   = 90.0f;        // degrees above the horizon
    float azimuth     = 180.0f;       // degrees from north, clockwise (east 90, south 180)
    float declination = 0.0f;         // degrees
    // Clock hours, in the place's time (summer time included). With the sun up
    // all day `sunrise` < 0, with it down all day `sunset` < 0.
    float solarNoon = 12.0f, sunrise = 6.0f, sunset = 18.0f;
};

// The sun at clock time `hours` (0..24) on the place's day.
[[nodiscard]] Sun at(const Place& p, float hours);

// The clock's offset from UTC on the place's day: standard time, plus the hour
// of summer time while it is kept (the EU's season north of the equator, the
// mirrored one south of it -- close enough to a week for any scene).
[[nodiscard]] float zone(const Place& p);
[[nodiscard]] bool  inSummerTime(const Place& p);

// Calendar helpers (a non-leap year: a scene has no year).
[[nodiscard]] int  dayOfYear(int month, int dayOfMonth);         // month 1..12
void               monthDay(int dayOfYear, int& month, int& dayOfMonth);
[[nodiscard]] int  daysIn(int month);
[[nodiscard]] const char* monthName(int month);                  // "Jan" .. "Dez"

// A few places to start from; the latitude and longitude stay editable.
struct City {
    const char* name;
    float latitude, longitude, utcOffset;
    bool  summerTime;
};
[[nodiscard]] const std::vector<City>& cities();
// The city `p` stands at (within a few kilometres), or -1.
[[nodiscard]] int cityAt(const Place& p);

} // namespace sunpos
