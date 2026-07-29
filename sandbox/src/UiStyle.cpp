#include "UiStyle.hpp"

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

bool header(const char* label, ImGuiTreeNodeFlags flags) {
    BoldScope bold;
    return ImGui::CollapsingHeader(label, flags);
}

void sectionText(const char* label) {
    BoldScope bold;
    ImGui::SeparatorText(label);
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

} // namespace ui
