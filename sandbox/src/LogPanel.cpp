#include "LogPanel.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <imgui.h>

#include <fitzel/core/LogCapture.hpp>

#include "UiStyle.hpp"

namespace logui {
namespace {

using fitzel::logcap::Line;
using fitzel::logcap::Stream;

constexpr std::size_t kMaxLines = 10000;   // same as the capture's ring

enum class Level : std::uint8_t { Plain, Warn, Error };

struct Entry {
    Line  line;
    Level level;
};

// The panel's state lives here rather than in main: nothing outside the window
// reads it, and it survives the window being closed and reopened.
struct View {
    std::vector<Entry>  entries;
    std::uint64_t       lastSeq    = 0;
    std::vector<int>    visible;          // indices into entries, after filtering
    bool                dirty      = true;
    bool                showOut    = true;
    bool                showErr    = true;
    bool                autoScroll = true;
    char                filter[128] = {};
    std::string         lastFilter;
};

View& view() {
    static View v;
    return v;
}

// Severity is read off the words in the line, not the stream it came on: plenty
// of libraries put plain progress on stderr, and plenty of errors are printf'd.
Level classify(const std::string& t) {
    const char* s = t.c_str();
    if (ui::icontains(s, "error") || ui::icontains(s, "fatal") ||
        ui::icontains(s, "failed") || ui::icontains(s, "exception") ||
        ui::icontains(s, "abort"))
        return Level::Error;
    if (ui::icontains(s, "warn")) return Level::Warn;
    return Level::Plain;
}

bool passes(const View& v, const Entry& e) {
    if (e.line.stream == Stream::Out && !v.showOut) return false;
    if (e.line.stream == Stream::Err && !v.showErr) return false;
    return ui::icontains(e.line.text.c_str(), v.filter);
}

void pull(View& v) {
    std::vector<Line> fresh;
    v.lastSeq = fitzel::logcap::linesSince(v.lastSeq, fresh);
    if (fresh.empty()) return;
    for (Line& l : fresh) {
        const Level lv = classify(l.text);
        v.entries.push_back({std::move(l), lv});
    }
    if (v.entries.size() > kMaxLines) {
        v.entries.erase(v.entries.begin(),
                        v.entries.begin() + static_cast<std::ptrdiff_t>(v.entries.size() - kMaxLines));
        v.dirty = true;   // indices shifted
    }
    if (v.dirty) return;
    // Only the new tail needs filtering.
    for (int i = static_cast<int>(v.entries.size() - fresh.size());
         i < static_cast<int>(v.entries.size()); ++i)
        if (passes(v, v.entries[i])) v.visible.push_back(i);
}

void refilter(View& v) {
    v.visible.clear();
    for (int i = 0; i < static_cast<int>(v.entries.size()); ++i)
        if (passes(v, v.entries[i])) v.visible.push_back(i);
    v.dirty = false;
}

ImVec4 colourFor(const Entry& e) {
    switch (e.level) {
    case Level::Error: return ImVec4(1.00f, 0.42f, 0.38f, 1.0f);
    case Level::Warn:  return ImVec4(0.98f, 0.78f, 0.36f, 1.0f);
    case Level::Plain: break;
    }
    // stderr that reads as neither is still set apart, a shade warmer.
    return e.line.stream == Stream::Err ? ImVec4(0.90f, 0.80f, 0.74f, 1.0f)
                                        : ImGui::GetStyleColorVec4(ImGuiCol_Text);
}

} // namespace

void drawPanel(const PanelState& s) {
    View& v = view();
    // Pulled even while the window is closed would be wasted work; the ring
    // holds the lines until it is opened.
    ImGui::SetNextWindowSize(ImVec2(720.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Log", &s.show)) {
        ImGui::End();
        return;
    }

    if (!fitzel::logcap::active()) {
        ui::hint("Output capture is not running in this build -- "
                 "stdout and stderr go to the terminal only.");
        ImGui::End();
        return;
    }

    pull(v);

    // --- Toolbar ---------------------------------------------------------
    if (ImGui::Checkbox("stdout", &v.showOut)) v.dirty = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("stderr", &v.showErr)) v.dirty = true;
    ImGui::SameLine();
    ImGui::Checkbox("Auto-scroll", &v.autoScroll);
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        v.entries.clear();
        v.visible.clear();
        fitzel::logcap::clear();
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        // What is shown, not everything kept: filter first, then copy.
        std::string all;
        for (int i : v.visible) {
            all += v.entries[i].line.text;
            all += '\n';
        }
        ImGui::SetClipboardText(all.c_str());
    }
    ImGui::SameLine();
    int errors = 0, warnings = 0;
    for (const Entry& e : v.entries) {
        if (e.level == Level::Error) ++errors;
        else if (e.level == Level::Warn) ++warnings;
    }
    ImGui::TextDisabled("%d lines", static_cast<int>(v.entries.size()));
    if (errors) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.00f, 0.42f, 0.38f, 1.0f), "%d errors", errors);
    }
    if (warnings) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.98f, 0.78f, 0.36f, 1.0f), "%d warnings", warnings);
    }

    const bool filtering = ui::searchBox("##logfilter", v.filter, sizeof(v.filter), "Filter...");
    if (v.lastFilter != v.filter) {
        v.lastFilter = v.filter;
        v.dirty      = true;
    }
    if (v.dirty) refilter(v);
    if (filtering)
        ui::hint("%d of %d lines match", static_cast<int>(v.visible.size()),
                 static_cast<int>(v.entries.size()));

    ImGui::Separator();

    // --- Lines -----------------------------------------------------------
    if (ImGui::BeginChild("##logLines", ImVec2(0, 0), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        // Following the tail stops the moment the user scrolls up to read,
        // and picks up again once they are back at the bottom.
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(v.visible.size()));
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const Entry& e = v.entries[v.visible[r]];
                ImGui::TextDisabled("%8.2f", e.line.time);
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, colourFor(e));
                ImGui::TextUnformatted(e.line.text.c_str(),
                                       e.line.text.c_str() + e.line.text.size());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                    ImGui::SetClipboardText(e.line.text.c_str());
            }
        }
        clip.End();
        ImGui::PopStyleVar();

        if (v.autoScroll && atBottom) ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace logui
