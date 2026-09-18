#include "UiStyle.hpp"

#include <algorithm>
#include <cstdio>

#include <cctype>
#include <cfloat>
#include <cstdarg>

namespace ui {

namespace {
ImFont* g_bold = nullptr;

// Push the semibold font at the current size. ImGui 1.92 takes a size with the
// font; 0.0f means "keep the size in effect", which is what we want -- the
// heading differs from body text by weight only.
struct BoldScope {
    bool pushed = false;
    BoldScope() {
        if (g_bold) { ImGui::PushFont(g_bold, 0.0f); pushed = true; }
    }
    ~BoldScope() { if (pushed) ImGui::PopFont(); }
};
} // namespace

void setBoldFont(ImFont* bold) { g_bold = bold; }

ImFont* boldFont() { return g_bold; }

bool header(const char* label, ImGuiTreeNodeFlags flags) {
    BoldScope bold;
    // The theme's Header colours are the accent-washed SELECTION colour (rows in
    // the hierarchy, lists). A section bar is not a selection, and a panel of
    // orange bars would leave the real selection nothing to stand out with --
    // so section bars borrow the neutral frame shades instead.
    ImGui::PushStyleColor(ImGuiCol_Header,        ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));
    const bool open = ImGui::CollapsingHeader(label, flags);
    ImGui::PopStyleColor(3);
    return open;
}

void sectionText(const char* label) {
    BoldScope bold;
    ImGui::SeparatorText(label);
}

void title(const char* fmt, ...) {
    BoldScope bold;
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

void hint(const char* fmt, ...) {
    // 0.92x of the current size: enough to read as secondary, not so small that
    // it turns into noise at 100% display scaling.
    ImGui::PushFont(nullptr, ImGui::GetFontSize() * 0.92f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    va_list args;
    va_start(args, fmt);
    ImGui::TextWrappedV(fmt, args);
    va_end(args);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

bool icontains(const char* hay, const char* needle) {
    if (!needle || !*needle) return true;
    if (!hay) return false;
    for (const char* h = hay; *h; ++h) {
        const char* a = h;
        const char* b = needle;
        while (*a && *b &&
               std::tolower(static_cast<unsigned char>(*a)) ==
               std::tolower(static_cast<unsigned char>(*b))) { ++a; ++b; }
        if (!*b) return true;
    }
    return false;
}

bool searchBox(const char* id, char* buf, std::size_t cap, const char* placeholder) {
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputTextWithHint(id, placeholder, buf, cap,
                             ImGuiInputTextFlags_EscapeClearsAll);
    return buf[0] != 0;
}

float stepperWidth(const char* fmt) {
    char wide[64];
    std::snprintf(wide, sizeof wide, fmt, -8.88f);
    const float em = ImGui::GetFontSize();
    return 2.0f * std::max(em * 1.6f, 26.0f) + 6.0f +
           ImGui::CalcTextSize(wide).x + ImGui::GetStyle().FramePadding.x * 2.0f +
           em * 0.4f;
}

bool stepper(const char* id, float& v, float step, float lo, float hi,
             const char* fmt, float width) {
    const float em = ImGui::GetFontSize();
    if (width <= 0.0f) width = stepperWidth(fmt);
    bool changed = false;
    ImGui::PushID(id);
    ImGui::BeginGroup();
    const ImVec2 bs(std::max(em * 1.6f, 26.0f), ImGui::GetFrameHeight() + em * 0.35f);
    if (ImGui::Button("-", bs)) { v = std::max(lo, v - step); changed = true; }
    ImGui::SetItemTooltip("%.3g less", step);
    ImGui::SameLine(0.0f, 3.0f);
    char buf[64];
    std::snprintf(buf, sizeof buf, fmt, v);
    // The value between them reads as a field, not as a third target: drawn in
    // the frame colour, and pressing it does nothing.
    const ImVec4 frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    ImGui::PushStyleColor(ImGuiCol_Button, frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, frame);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, frame);
    ImGui::Button(buf, ImVec2(std::max(width - 2.0f * bs.x - 6.0f, em * 2.5f), bs.y));
    ImGui::PopStyleColor(3);
    ImGui::SameLine(0.0f, 3.0f);
    if (ImGui::Button("+", bs)) { v = std::min(hi, v + step); changed = true; }
    ImGui::SetItemTooltip("%.3g more", step);
    ImGui::EndGroup();
    ImGui::PopID();
    return changed;
}

} // namespace ui
