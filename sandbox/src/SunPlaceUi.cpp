#include "SunPlaceUi.hpp"

#include <cmath>
#include <cstdio>

#include <imgui.h>

#include "UiStyle.hpp"

namespace sunui {
namespace {

const char* compass(float azimuth) {
    static const char* d[16] = {"N", "NNO", "NO", "ONO", "O", "OSO", "SO", "SSO",
                                "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    const int i = static_cast<int>(std::lround(azimuth / 22.5f)) & 15;
    return d[i];
}

} // namespace

const char* clock(float hours) {
    static char buf[4][8];
    static int slot = 0;
    slot = (slot + 1) & 3;
    float h = std::fmod(hours, 24.0f);
    if (h < 0.0f) h += 24.0f;
    int m = static_cast<int>(std::lround(h * 60.0f)) % (24 * 60);
    std::snprintf(buf[slot], sizeof(buf[slot]), "%02d:%02d", m / 60, m % 60);
    return buf[slot];
}

bool placeEditor(sunpos::Place& p, float hours) {
    bool changed = false;
    const auto& cities = sunpos::cities();
    const int city = sunpos::cityAt(p);
    if (ImGui::BeginCombo("Place", city >= 0 ? cities[static_cast<std::size_t>(city)].name : "Custom")) {
        for (int i = 0; i < static_cast<int>(cities.size()); ++i) {
            const sunpos::City& c = cities[static_cast<std::size_t>(i)];
            if (ImGui::Selectable(c.name, i == city)) {
                p.latitude = c.latitude;
                p.longitude = c.longitude;
                p.utcOffset = c.utcOffset;
                p.summerTime = c.summerTime;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    changed |= ImGui::SliderFloat("Latitude", &p.latitude, -80.0f, 80.0f,
                                  p.latitude >= 0.0f ? "%.2f\xC2\xB0 N" : "%.2f\xC2\xB0 (S)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Degrees north of the equator; south is negative.");
    changed |= ImGui::SliderFloat("Longitude", &p.longitude, -180.0f, 180.0f,
                                  p.longitude >= 0.0f ? "%.2f\xC2\xB0 E" : "%.2f\xC2\xB0 (W)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Degrees east of Greenwich; west is negative. With the time zone it\n"
                          "decides WHEN the sun is highest on the clock: at 8.7\xC2\xB0 E in\n"
                          "Central European Summer Time that is about half past one.");
    {
        float off = p.utcOffset;
        if (ImGui::SliderFloat("Time zone", &off, -12.0f, 14.0f, "UTC%+.1f")) {
            p.utcOffset = std::round(off * 2.0f) * 0.5f;
            changed = true;
        }
    }
    changed |= ImGui::Checkbox("Summer time", &p.summerTime);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The place puts its clocks forward an hour in its summer (end of March\n"
                          "to end of October north of the equator).");
    {
        int month = 1, dom = 1;
        sunpos::monthDay(p.day, month, dom);
        ImGui::SetNextItemWidth(0.35f * ImGui::CalcItemWidth());
        if (ImGui::BeginCombo("##month", sunpos::monthName(month))) {
            for (int m = 1; m <= 12; ++m)
                if (ImGui::Selectable(sunpos::monthName(m), m == month)) {
                    p.day = sunpos::dayOfYear(m, std::min(dom, sunpos::daysIn(m)));
                    changed = true;
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(0.65f * ImGui::CalcItemWidth() - ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::SliderInt("Date", &dom, 1, sunpos::daysIn(month))) {
            p.day = sunpos::dayOfYear(month, dom);
            changed = true;
        }
    }
    if (changed) p.legacy = false;
    const sunpos::Sun s = sunpos::at(p, hours);
    if (p.legacy)
        ui::hint("This scene still has the old sun: the clock is sun time, the season a\n"
                 "slider. Set a place or a date and it runs on real ones.");
    if (s.elevation > 0.0f)
        ui::hint("At %s: %.0f\xC2\xB0 high in the %s (%.0f\xC2\xB0)", clock(hours), s.elevation,
                 compass(s.azimuth), s.azimuth);
    else
        ui::hint("At %s the sun is down (%.0f\xC2\xB0)", clock(hours), s.elevation);
    if (s.sunrise < 0.0f)       ui::hint("It does not set today. Highest at %s.", clock(s.solarNoon));
    else if (s.sunset < 0.0f)   ui::hint("It does not rise today.");
    else if (p.legacy)          ui::hint("Rises %s, highest %s, sets %s (sun time)", clock(s.sunrise),
                                         clock(s.solarNoon), clock(s.sunset));
    else ui::hint("Rises %s, highest %s, sets %s (UTC%+.1f)", clock(s.sunrise), clock(s.solarNoon),
                  clock(s.sunset), sunpos::zone(p));
    return changed;
}

} // namespace sunui
