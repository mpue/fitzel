#include "AssetsPanel.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include <glm/glm.hpp>
#include <imgui.h>

#include <fitzel/asset/AssetDatabase.hpp>

#include "AssetDrop.hpp"
#include "EditorContext.hpp"
#include "ModelLibrary.hpp"
#include "UiStyle.hpp"

using namespace fitzel;

namespace assetsui {

void panel(EditorContext& ed, State& s, const Host& h) {
    if (!h.show) return;
    if (ImGui::Begin("Assets", &h.show)) {
        // Toolbar: preview size, name filter, texture-only toggle.
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Size", &s.thumbSize, 48.0f, 160.0f, "%.0f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(150.0f);
        ImGui::InputTextWithHint("##assetFilter", "filter...", s.filter, sizeof(s.filter));
        ImGui::SameLine();
        ImGui::Checkbox("Textures only", &s.texturesOnly);
        ImGui::TextDisabled("Drag a tile onto a material slot / the viewport; "
                            "double-click a model to place it.");
        ImGui::TextDisabled("Drop files here from Explorer to copy them into the project.");

        // Take an OS file drop that landed on this window. The hit test uses the
        // cursor position captured in the drop callback, not the live one: the
        // pointer may have moved on since, and a file dropped on Assets belongs
        // in Assets either way.
        if (!h.droppedFiles.empty()) {
            const ImVec2 wp = ImGui::GetWindowPos();
            const ImVec2 ws = ImGui::GetWindowSize();
            if (h.dropX >= wp.x && h.dropX < wp.x + ws.x &&
                h.dropY >= wp.y && h.dropY < wp.y + ws.y) {
                s.dropStatus = assetdrop::importInto(h.projectDir, h.droppedFiles, ed.assetDb).message;
                h.droppedFiles.clear();
            }
        }
        if (!s.dropStatus.empty())
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", s.dropStatus.c_str());
        ImGui::Separator();

        // Case-insensitive substring match for the filter box.
        std::string flt = s.filter;
        std::transform(flt.begin(), flt.end(), flt.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        auto matches = [&](const std::string& str) {
            if (flt.empty()) return true;
            std::string l = str;
            std::transform(l.begin(), l.end(), l.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return l.find(flt) != std::string::npos;
        };

        const float pad  = ImGui::GetStyle().ItemSpacing.x;
        const auto& srcs = ed.assetDb.sources();
        for (int si = 0; si < static_cast<int>(srcs.size()); ++si) {
            const char* kind = srcs[si].kind == AssetSourceKind::Engine ? "Engine" : "Project";
            const std::string hdr = srcs[si].name + " (" + kind + ")###src" + std::to_string(si);
            if (!ui::header(hdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
            ImGui::PushID(si);
            const float avail = ImGui::GetContentRegionAvail().x;
            const int   cols  = std::max(1, static_cast<int>(avail / (s.thumbSize + pad)));
            int shown = 0, col = 0;
            for (AssetId id : ed.assetDb.allAssets()) {
                const AssetDatabase::Entry* e = ed.assetDb.entry(id);
                if (!e || e->sourceIndex != si) continue;
                const bool isTex = (e->type == AssetType::Texture);
                if (s.texturesOnly && !isTex) continue;
                if (!matches(e->relPath)) continue;
                ++shown;
                if (col != 0) ImGui::SameLine();

                ImGui::PushID(id.toString().c_str());
                ImGui::BeginGroup();

                const unsigned tid =
                    (isTex && h.thumbnail)
                        ? h.thumbnail(id, ImGui::IsRectVisible(ImVec2(s.thumbSize, s.thumbSize)))
                        : 0u;
                const ImVec2 sz(s.thumbSize, s.thumbSize);
                if (tid) {
                    ImGui::ImageButton("##thumb", (ImTextureID)(intptr_t)tid, sz);
                } else {
                    const char* tag = isTex ? "TEX"
                                    : e->type == AssetType::Model ? "MDL"
                                    : e->type == AssetType::Sound ? "SND"
                                    : e->type == AssetType::Video ? "VID" : "?";
                    ImGui::Button(tag, sz);
                }

                // Drag source (same GUID payload the drop targets expect).
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    const std::string g = id.toString();
                    ImGui::SetDragDropPayload("ASSET_GUID", g.data(), 32);
                    ImGui::Text("%s  %s", assetTypeName(e->type), e->relPath.c_str());
                    ImGui::EndDragDropSource();
                }
                if (isTex && h.editImage && ImGui::BeginPopupContextItem("##tex")) {
                    if (ImGui::MenuItem("Edit in Image editor")) h.editImage(e->absPath.generic_string());
                    ImGui::EndPopup();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s\n%s", assetTypeName(e->type), e->relPath.c_str());
                if (e->type == AssetType::Model && ImGui::IsItemHovered() &&
                    ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    const std::string mp = e->absPath.string();
                    const glm::vec3 g = h.spawnAt ? h.spawnAt() : glm::vec3(0.0f);
                    if (ed.isStructuredModel && ed.isStructuredModel(mp)) {
                        if (ed.addModelHierarchy) ed.addModelHierarchy(g, mp);
                    } else {
                        const int id2 = ed.models.import(mp, ed.assetDb, ed.materials);
                        if (id2 >= 0 && ed.addModelEntity) ed.addModelEntity(g, id2);
                    }
                }

                // Caption: file name, clipped to the tile width.
                std::string stem = std::filesystem::path(e->relPath).filename().string();
                const int maxCh = std::max(4, static_cast<int>(s.thumbSize / 7.0f));
                if (static_cast<int>(stem.size()) > maxCh)
                    stem = stem.substr(0, maxCh - 1) + "\xE2\x80\xA6"; // ellipsis
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + s.thumbSize);
                ImGui::TextUnformatted(stem.c_str());
                ImGui::PopTextWrapPos();

                ImGui::EndGroup();
                ImGui::PopID();
                col = (col + 1) % cols;
            }
            if (shown == 0) ImGui::TextDisabled("  (empty)");
            ImGui::PopID();
        }
    }
    ImGui::End();
}

} // namespace assetsui
