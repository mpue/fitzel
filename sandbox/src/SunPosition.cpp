#include "SunPosition.hpp"

#include <algorithm>
#include <cmath>

namespace sunpos {
namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int   kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

// The direction towards a sun at hour angle `H` (radians from local noon,
// negative in the morning) and declination `dec`, seen from latitude `lat`:
// east +X, up +Y, north -Z. The old sky's formula, kept as it was so a legacy
// scene is lit to the bit.
glm::vec3 direction(float lat, float dec, float H) {
    const float up    = std::sin(lat) * std::sin(dec) + std::cos(lat) * std::cos(dec) * std::cos(H);
    const float east  = -std::cos(dec) * std::sin(H);
    const float north = std::cos(lat) * std::sin(dec) - std::sin(lat) * std::cos(dec) * std::cos(H);
    return glm::normalize(glm::vec3(east, up, -north));
}

// The NOAA fractional-year terms: the equation of time (minutes) and the
// declination (radians) for day `day` at UTC hour `utcHours`.
void almanac(int day, float utcHours, float& eqTime, float& decl) {
    const float g = 2.0f * kPi / 365.0f * (static_cast<float>(day - 1) + (utcHours - 12.0f) / 24.0f);
    eqTime = 229.18f * (0.000075f + 0.001868f * std::cos(g) - 0.032077f * std::sin(g) -
                        0.014615f * std::cos(2.0f * g) - 0.040849f * std::sin(2.0f * g));
    decl = 0.006918f - 0.399912f * std::cos(g) + 0.070257f * std::sin(g) -
           0.006758f * std::cos(2.0f * g) + 0.000907f * std::sin(2.0f * g) -
           0.002697f * std::cos(3.0f * g) + 0.00148f * std::sin(3.0f * g);
}

void fill(Sun& s, float lat) {
    s.elevation = glm::degrees(std::asin(glm::clamp(s.dir.y, -1.0f, 1.0f)));
    float az = glm::degrees(std::atan2(s.dir.x, -s.dir.z));
    if (az < 0.0f) az += 360.0f;
    s.azimuth = az;
    (void)lat;
}

} // namespace

bool inSummerTime(const Place& p) {
    if (!p.summerTime) return false;
    // Last Sunday of March to last Sunday of October, near enough; mirrored south
    // of the equator, where summer time is kept across the turn of the year.
    const bool north = p.day >= 88 && p.day < 300;
    return p.latitude >= 0.0f ? north : !(p.day >= 95 && p.day < 278);
}

float zone(const Place& p) { return p.utcOffset + (inSummerTime(p) ? 1.0f : 0.0f); }

Sun at(const Place& p, float hours) {
    Sun s;
    const float lat = glm::radians(glm::clamp(p.latitude, -89.9f, 89.9f));
    if (p.legacy) {
        // The old arc: the clock is solar time, the season a declination.
        const float dec = glm::radians(p.legacyDeclination);
        s.declination = p.legacyDeclination;
        s.dir = direction(lat, dec, (hours - 12.0f) / 24.0f * 2.0f * kPi);
        s.solarNoon = 12.0f;
        const float c = -std::tan(lat) * std::tan(dec);
        if (c <= -1.0f)      { s.sunrise = -1.0f; s.sunset = 24.0f; }
        else if (c >= 1.0f)  { s.sunrise = 24.0f; s.sunset = -1.0f; }
        else {
            const float h = glm::degrees(std::acos(c)) / 15.0f;
            s.sunrise = 12.0f - h; s.sunset = 12.0f + h;
        }
        fill(s, lat);
        return s;
    }
    const int   day  = std::clamp(p.day, 1, 365);
    const float zn   = zone(p);
    const float utc  = hours - zn;
    float eq = 0.0f, dec = 0.0f;
    almanac(day, utc, eq, dec);
    s.declination = glm::degrees(dec);
    // True solar time in minutes, and the hour angle from it.
    const float tst = hours * 60.0f + eq + 4.0f * p.longitude - 60.0f * zn;
    const float ha  = tst / 4.0f - 180.0f;
    s.dir = direction(lat, dec, glm::radians(ha));
    fill(s, lat);
    // Noon, sunrise and sunset (NOAA): the sun's centre 0.833 degrees below the
    // horizon at rise and set -- the refraction and its own radius.
    float eqN = 0.0f, decN = 0.0f;
    almanac(day, 12.0f - zn, eqN, decN);
    const float noonUtcMin = 720.0f - 4.0f * p.longitude - eqN;
    s.solarNoon = noonUtcMin / 60.0f + zn;
    const float c = std::cos(glm::radians(90.833f)) / (std::cos(lat) * std::cos(decN)) -
                    std::tan(lat) * std::tan(decN);
    if (c <= -1.0f)      { s.sunrise = -1.0f; s.sunset = 24.0f; }   // the sun never sets
    else if (c >= 1.0f)  { s.sunrise = 24.0f; s.sunset = -1.0f; }   // ...or never rises
    else {
        const float h = glm::degrees(std::acos(c)) * 4.0f / 60.0f;  // hours either side of noon
        s.sunrise = s.solarNoon - h;
        s.sunset  = s.solarNoon + h;
    }
    return s;
}

int daysIn(int month) { return kDays[std::clamp(month, 1, 12) - 1]; }

int dayOfYear(int month, int dayOfMonth) {
    month = std::clamp(month, 1, 12);
    int d = std::clamp(dayOfMonth, 1, daysIn(month));
    for (int m = 1; m < month; ++m) d += kDays[m - 1];
    return d;
}

void monthDay(int dayOfYear, int& month, int& dayOfMonth) {
    int d = std::clamp(dayOfYear, 1, 365);
    month = 1;
    while (d > kDays[month - 1]) { d -= kDays[month - 1]; ++month; }
    dayOfMonth = d;
}

const char* monthName(int month) {
    static const char* n[12] = {"Jan", "Feb", "Mär", "Apr", "Mai", "Jun",
                                "Jul", "Aug", "Sep", "Okt", "Nov", "Dez"};
    return n[std::clamp(month, 1, 12) - 1];
}

const std::vector<City>& cities() {
    static const std::vector<City> list = {
        {"Frankfurt",     50.11f,    8.68f,  1.0f, true},
        {"Berlin",        52.52f,   13.40f,  1.0f, true},
        {"Hamburg",       53.55f,    9.99f,  1.0f, true},
        {"München",       48.14f,   11.58f,  1.0f, true},
        {"Kassel",        51.31f,    9.48f,  1.0f, true},
        {"Münster",       51.96f,    7.63f,  1.0f, true},
        {"Kiel",          54.32f,   10.14f,  1.0f, true},
        {"Wien",          48.21f,   16.37f,  1.0f, true},
        {"Zürich",        47.38f,    8.54f,  1.0f, true},
        {"Innsbruck",     47.27f,   11.39f,  1.0f, true},
        {"Bergen",        60.39f,    5.32f,  1.0f, true},
        {"Tromsø",        69.65f,   18.96f,  1.0f, true},
        {"Reykjavík",     64.15f,  -21.94f,  0.0f, false},
        {"Edinburgh",     55.95f,   -3.19f,  0.0f, true},
        {"Lissabon",      38.72f,   -9.14f,  0.0f, true},
        {"Nizza",         43.70f,    7.27f,  1.0f, true},
        {"Palma",         39.57f,    2.65f,  1.0f, true},
        {"New York",      40.71f,  -74.01f, -5.0f, true},
        {"Grand Canyon",  36.06f, -112.14f, -7.0f, false},
        {"Kapstadt",     -33.92f,   18.42f,  2.0f, false},
        {"Quito",         -0.18f,  -78.47f, -5.0f, false},
    };
    return list;
}

int cityAt(const Place& p) {
    const auto& l = cities();
    for (int i = 0; i < static_cast<int>(l.size()); ++i)
        if (std::fabs(l[static_cast<std::size_t>(i)].latitude - p.latitude) < 0.05f &&
            std::fabs(l[static_cast<std::size_t>(i)].longitude - p.longitude) < 0.05f)
            return i;
    return -1;
}

} // namespace sunpos
