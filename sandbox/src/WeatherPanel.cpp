#include "WeatherPanel.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <imgui.h>

#include "UiStyle.hpp"

namespace weatherui {
namespace {

// The applied preset, or -1. Kept as a lookup rather than an index in PanelState
// because the list is rebuilt whenever the project changes and an index into the
// old one is a preset nobody picked.
int currentIndex(const PanelState& s) {
    return weather::indexOf(s.presets, s.current);
}

// A free name for "Copy": "Storm 2", "Storm 3", ... The first one that is not
// taken, so copying twice does not need the author to think of anything.
std::string freeName(const std::vector<weather::Preset>& presets,
                     const std::string& base) {
    for (int n = 2; n < 100; ++n) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%.80s %d", base.c_str(), n);
        if (weather::indexOf(presets, buf) < 0) return buf;
    }
    return base;
}

} // namespace

void drawPanel(const PanelState& s) {
    if (!ImGui::Begin("Weather & audio", &s.show)) { ImGui::End(); return; }

    const int  cur     = currentIndex(s);
    const bool edited  = cur >= 0 &&
                         !weather::matches(s.presets[static_cast<std::size_t>(cur)],
                                           s.live);
    const bool haveProj = !s.projectFolder.empty();

    // --- The shelf ----------------------------------------------------------
    // Applied on CLICK, not on a click and then an Apply button. Picking a
    // weather is the one thing this panel is for, and a second confirming press
    // buys nothing back -- every preset is one more click away from being undone
    // by picking a different one.
    ui::sectionText("Preset");
    {
        char label[128];
        if (cur < 0) std::snprintf(label, sizeof(label), "(hand-set)");
        else std::snprintf(label, sizeof(label), "%s%s",
                           s.presets[static_cast<std::size_t>(cur)].name.c_str(),
                           edited ? " *" : "");
        if (ImGui::BeginCombo("##wxpreset", label)) {
            for (std::size_t i = 0; i < s.presets.size(); ++i) {
                const weather::Preset& p = s.presets[i];
                if (ImGui::Selectable(p.name.c_str(),
                                      static_cast<int>(i) == cur)) {
                    weather::apply(p, s.live, s.groundY);
                    s.current = p.name;
                    // The two ticks below follow the preset that is showing, so
                    // Update keeps a weather's reach instead of quietly taking
                    // whatever the last one happened to leave them on.
                    s.savesTime = p.setTimeOfDay;
                    s.savesMist = p.setMist;
                    std::snprintf(s.nameBuf, s.nameCap, "%s", p.name.c_str());
                }
                if (p.builtin) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("built-in");
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        // Re-apply. The one button the combo cannot replace: getting BACK to the
        // preset you are already on after moving three sliders would otherwise
        // mean picking something else first.
        ImGui::BeginDisabled(cur < 0);
        if (ImGui::Button("Reapply"))
            weather::apply(s.presets[static_cast<std::size_t>(cur)], s.live,
                           s.groundY);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && cur >= 0)
            ImGui::SetTooltip("Put the sky back where '%s' says it should be.",
                              s.presets[static_cast<std::size_t>(cur)].name.c_str());
    }
    if (edited)
        ui::hint("Edited since it was applied -- Update writes the change into "
                 "the preset,\nReapply throws it away.");
    else if (cur < 0)
        ui::hint("Nothing applied: this sky was set by hand. Save it under a "
                 "name to get\nit back later.");

    // What a saved preset REACHES. Both default to what the applied preset said,
    // so updating one keeps its reach; both are ticks rather than a mode because
    // the answer differs per weather -- a dawn mist means six in the morning, a
    // squall means nothing about the clock at all.
    ImGui::Checkbox("Sets the time of day", &s.savesTime);
    ImGui::SameLine();
    ImGui::Checkbox("Sets the world mist", &s.savesMist);
    ui::hint("What a saved preset speaks about. Unticked, applying it leaves "
             "that\nalone -- so a scene's own dawn mist survives picking a "
             "squall.");

    // --- Keeping one --------------------------------------------------------
    ImGui::BeginDisabled(!haveProj);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("##wxname", s.nameBuf, s.nameCap);
    ImGui::SameLine();
    {
        const std::string typed = s.nameBuf ? s.nameBuf : "";
        const int at = weather::indexOf(s.presets, typed);
        // One button, two words, because "save this" is one intention: it
        // overwrites when the name is taken and adds when it is not, and the
        // label says which it is about to do before it is pressed.
        const bool over = at >= 0;
        ImGui::BeginDisabled(typed.empty());
        if (ImGui::Button(over ? "Update" : "Save as new")) {
            weather::Preset p = weather::capture(typed, s.live, s.savesTime,
                                                 s.savesMist);
            if (over) {
                p.builtin = s.presets[static_cast<std::size_t>(at)].builtin;
                s.presets[static_cast<std::size_t>(at)] = p;
            } else {
                s.presets.push_back(p);
            }
            s.current = p.name;
            weather::save(s.projectFolder, s.presets);
        }
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(cur < 0);
    if (ImGui::Button("Copy")) {
        weather::Preset p = s.presets[static_cast<std::size_t>(cur)];
        p.name    = freeName(s.presets, p.name);
        p.builtin = false;
        s.presets.push_back(p);
        s.current = p.name;
        std::snprintf(s.nameBuf, s.nameCap, "%s", p.name.c_str());
        weather::save(s.projectFolder, s.presets);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Start a new preset from this one -- the way to bend a\n"
                          "built-in without overwriting it.");
    ImGui::SameLine();
    {
        // A built-in is never deleted -- it is RESET, back to the values the
        // editor ships. Two different buttons would be two different mental
        // models for one gesture ("undo what I did to this"); one button that
        // knows which kind it is on is the same gesture either way.
        const bool isBuiltin = cur >= 0 &&
                               s.presets[static_cast<std::size_t>(cur)].builtin;
        ImGui::BeginDisabled(cur < 0);
        if (ImGui::Button(isBuiltin ? "Reset" : "Delete")) {
            const std::string name = s.presets[static_cast<std::size_t>(cur)].name;
            if (isBuiltin) {
                const std::vector<weather::Preset> stock = weather::builtins();
                const int at = weather::indexOf(stock, name);
                if (at >= 0) {
                    s.presets[static_cast<std::size_t>(cur)] =
                        stock[static_cast<std::size_t>(at)];
                    weather::apply(s.presets[static_cast<std::size_t>(cur)],
                                   s.live, s.groundY);
                }
            } else {
                s.presets.erase(s.presets.begin() + cur);
                s.current.clear();
            }
            weather::save(s.projectFolder, s.presets);
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered() && cur >= 0)
            ImGui::SetTooltip(isBuiltin
                                  ? "Built-in: put it back to the sky the editor ships."
                                  : "Remove this preset from the project.");
    }
    ImGui::EndDisabled();
    if (!haveProj)
        ui::hint("No project open, so there is nowhere to keep a preset. The "
                 "built-ins\nstill apply.");
    else
        ui::hint("Presets live in the project's weather.json, so every scene in "
                 "it shares\nthem. A preset carries the clouds, the cirrus, the "
                 "haze and the mist\nfrom Sky & atmosphere as well as everything "
                 "below.");

    // --- The dial -----------------------------------------------------------
    ImGui::Separator();
    ui::sectionText("Now");
    ImGui::Checkbox("Auto weather", &s.live.autoWeather);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drift the storm dial on its own. Overrides whatever a\n"
                          "preset set it to, so a preset with this on is a\n"
                          "CHANGING sky rather than a fixed one.");
    ImGui::SliderFloat("Storm", &s.live.storm, 0.0f, 1.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The master dial. It still overrides the cloud settings\n"
                          "towards a storm's own as it rises, so a preset's deck\n"
                          "is what you see at the bottom of this slider and the\n"
                          "storm's is what you see at the top.");
    ImGui::Checkbox("Lightning", &s.live.lightning);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Whether the sky may flash at all. Off, a downpour stays\n"
                          "a downpour however far the dial is pushed -- which is\n"
                          "the difference between heavy rain and a thunderstorm.");
    ImGui::Text("Rain %.0f%%   Wet %.0f%%   Lightning %s",
                s.rainIntensity * 100.0f, s.roadWetness * 100.0f,
                (s.live.lightning && s.live.storm > 0.5f) ? "armed" : "off");

    // --- The weather's own mixer -------------------------------------------
    ImGui::Separator();
    ui::sectionText("Sound");
    ui::hint("Gains on the four loops, on top of what the dial already derives.\n"
             "A downpour is loud rain and little wind; a squall is the other way\n"
             "round, at the same place on the slider above.");
    ImGui::SliderFloat("Rain##wx",    &s.live.audio.rain,    0.0f, 2.0f, "%.2fx");
    ImGui::SliderFloat("Wind##wx",    &s.live.audio.wind,    0.0f, 2.0f, "%.2fx");
    ImGui::SliderFloat("Breeze##wx",  &s.live.audio.breeze,  0.0f, 2.0f, "%.2fx");
    ImGui::SliderFloat("Storm bed##wx", &s.live.audio.storm, 0.0f, 2.0f, "%.2fx");
    ImGui::SliderFloat("Thunder##wx", &s.live.audio.thunder, 0.0f, 2.0f, "%.2fx");
    ui::hint("Heard while playing only -- the editor stays silent.");

    ImGui::Separator();
    ImGui::Checkbox("Mute", &s.muted);
    ImGui::SameLine();
    ImGui::SliderFloat("Volume", &s.masterVolume, 0.0f, 1.0f);
    if (!s.audioOk) ImGui::TextDisabled("(audio device unavailable)");

    ImGui::End();
}

} // namespace weatherui
