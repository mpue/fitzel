#include "Export3mfDialog.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string_view>

#include <imgui.h>

#include "EditorContext.hpp"
#include "FolderDialog.hpp"

namespace export3mf {
namespace {

constexpr const char* kTitle = "Export as 3MF";

// A name that is a file name on every system.
std::string fileStem(std::string s) {
    for (char& c : s)
        if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos ||
            static_cast<unsigned char>(c) < 0x20)
            c = '_';
    return s.empty() ? std::string("selection") : s;
}

std::size_t triangles(const Shape& s) {
    std::size_t n = 0;
    for (const Part& p : s.parts) n += p.tris.size();
    return n;
}

} // namespace

bool canExport(const EditorContext& ed) {
    return ed.sel.valid() && !objectsOf(ed.document, ed.sel.ids()).empty();
}

Options options(const Dialog& d) {
    Options o;
    if (d.fit) {
        const glm::vec3 real = printSize(d.shape, Options{});
        const float longest = std::max({real.x, real.y, real.z});
        if (longest > 1e-6f) o.mmPerMetre = 1000.0f * d.fitMm / longest;
    }
    return o;
}

void begin(Dialog& d, EditorContext& ed) {
    if (!ed.sel.valid()) return;
    d.shape = gather(ed.document, ed.models, ed.sel.ids());
    if (d.shape.empty()) {
        ed.status = "3MF: nothing selected has a surface to print";
        return;
    }
    d.objects = static_cast<int>(d.shape.parts.size());
    // One object: its name. Several: the active one's, which is what a
    // selection is usually named after (the root of what was picked).
    d.name = fileStem(ed.entities[ed.sel.index()].name) + ".3mf";
    d.opening = true;
}

void draw(Dialog& d, EditorContext& ed, const std::string& projectDir) {
    if (d.opening) {
        ImGui::OpenPopup(kTitle);
        d.opening = false;
    }
    const float em = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(em * 26.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kTitle, nullptr, ImGuiWindowFlags_NoSavedSettings)) return;

    bool done = false;
    ImGui::Text("%d object%s, %zu triangles, %zu colour%s", d.objects, d.objects == 1 ? "" : "s",
                triangles(d.shape), d.shape.colors.size(), d.shape.colors.size() == 1 ? "" : "s");
    if (d.shape.skipped > 0)
        ImGui::TextDisabled("%d left out: no surface to read (a model not loaded)", d.shape.skipped);
    ImGui::Spacing();

    ImGui::TextUnformatted("Size");
    if (ImGui::RadioButton("As in the scene (1 m = 1000 mm)", !d.fit)) d.fit = false;
    if (ImGui::RadioButton("Longest side", d.fit)) d.fit = true;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(em * 7.0f);
    ImGui::BeginDisabled(!d.fit);
    ImGui::DragFloat("mm##fit", &d.fitMm, 0.5f, 1.0f, 2000.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
    ImGui::EndDisabled();

    const glm::vec3 mm = printSize(d.shape, options(d));
    ImGui::Spacing();
    ImGui::Text("Prints at %.1f x %.1f x %.1f mm (W x D x H)", mm.x, mm.y, mm.z);
    // A common bed is 180 to 350 mm across. Bigger is not wrong -- the slicer
    // can scale it -- but whoever exports a 30 m tower at real size wants to know.
    if (std::max({mm.x, mm.y, mm.z}) > 350.0f)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Larger than most print beds.");
    else if (std::max({mm.x, mm.y, mm.z}) < 5.0f)
        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Smaller than most printers can make out.");
    ImGui::Spacing();

    if (ImGui::Button("Export...", ImVec2(em * 8.0f, 0.0f))) {
        std::string out;
        const std::string start = !d.dir.empty() ? d.dir : projectDir;
        if (ed::saveFile(out, start, d.name, "3MF (3D printing)", "*.3mf", "3mf")) {
            std::string err;
            if (write(out, d.shape, options(d), err)) {
                char size[96];
                std::snprintf(size, sizeof size, "%.1f x %.1f x %.1f mm", mm.x, mm.y, mm.z);
                ed.status = "Exported " + std::filesystem::path(out).filename().string() + " (" +
                            std::to_string(d.objects) + (d.objects == 1 ? " object, " : " objects, ") + size + ")";
                d.dir = std::filesystem::path(out).parent_path().generic_string();
            } else {
                ed.status = "3MF: " + err;
            }
            done = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(em * 8.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) done = true;
    if (done) {
        d.shape = Shape{};   // a model's worth of triangles: not kept for nothing
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace export3mf
