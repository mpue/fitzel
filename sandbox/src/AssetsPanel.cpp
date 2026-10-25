#include "AssetsPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <imgui.h>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/ui/ImGuiTexture.hpp>

#include "AssetDrop.hpp"
#include "EditorContext.hpp"
#include "ModelLibrary.hpp"
#include "UiStyle.hpp"

using namespace fitzel;
namespace fs = std::filesystem;

namespace assetsui {
namespace {

using Entry = AssetDatabase::Entry;

// The types the Type filter offers, in the order it lists them.
constexpr AssetType kTypes[] = {AssetType::Texture, AssetType::Model, AssetType::Material,
                                AssetType::Sound, AssetType::Video};

// The plural for the Type filter ("Textures"); the singular is assetTypeName().
const char* pluralName(AssetType t) {
    switch (t) {
        case AssetType::Texture:  return "Textures";
        case AssetType::Model:    return "Models";
        case AssetType::Material: return "Materials";
        case AssetType::Sound:    return "Sounds";
        case AssetType::Video:    return "Videos";
        case AssetType::Unknown:  break;
    }
    return "All types";
}

// The three letters a tile shows when there is no picture to show.
const char* tagFor(AssetType t) {
    switch (t) {
        case AssetType::Texture:  return "TEX";
        case AssetType::Model:    return "MDL";
        case AssetType::Material: return "MAT";
        case AssetType::Sound:    return "SND";
        case AssetType::Video:    return "VID";
        case AssetType::Unknown:  break;
    }
    return "?";
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// One listed asset, with the keys it is sorted by worked out once.
struct Row {
    const Entry* e = nullptr;
    std::string  name;     // file name
    std::string  folder;   // its folder inside the source, "" at the root
    std::string  nameKey;  // lower-case, for sorting
    std::string  folderKey;
};

std::uintmax_t sizeOf(State& s, const Entry& e) {
    const auto it = s.sizes.find(e.id);
    if (it != s.sizes.end() && it->second.mtime == e.mtime) return it->second.bytes;
    std::error_code ec;
    std::uintmax_t n = fs::file_size(e.absPath, ec);
    if (ec) n = 0;
    s.sizes[e.id] = {e.mtime, n};
    return n;
}

std::string formatSize(std::uintmax_t n) {
    char buf[32];
    if (n < 1024)
        std::snprintf(buf, sizeof buf, "%u B", static_cast<unsigned>(n));
    else if (n < 1024ull * 1024)
        std::snprintf(buf, sizeof buf, "%.1f KB", n / 1024.0);
    else if (n < 1024ull * 1024 * 1024)
        std::snprintf(buf, sizeof buf, "%.1f MB", n / (1024.0 * 1024.0));
    else
        std::snprintf(buf, sizeof buf, "%.2f GB", n / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

// Local date and time of a file's last write, "" when it is unknown.
std::string formatTime(fs::file_time_type t) {
    if (t == fs::file_time_type{}) return {};
    // file_clock -> system_clock by the offset between their "now"s: exact
    // enough for minutes, and it needs neither clock_cast nor to_sys, which
    // the standard libraries disagree on.
    using namespace std::chrono;
    const auto sys = time_point_cast<system_clock::duration>(
        t - fs::file_time_type::clock::now() + system_clock::now());
    const std::time_t tt = system_clock::to_time_t(sys);
    std::tm tmv{};
#ifdef _WIN32
    if (localtime_s(&tmv, &tt) != 0) return {};
#else
    if (!localtime_r(&tt, &tmv)) return {};
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tmv);
    return buf;
}

void sortRows(State& s, std::vector<Row>& rows) {
    auto key = [&](const Row& a, const Row& b) -> int {
        switch (s.sortBy) {
            case SortBy::Name: break;
            case SortBy::Type:
                if (a.e->type != b.e->type)
                    return std::string(assetTypeName(a.e->type)) < assetTypeName(b.e->type) ? -1 : 1;
                break;
            case SortBy::Folder:
                if (a.folderKey != b.folderKey) return a.folderKey < b.folderKey ? -1 : 1;
                break;
            case SortBy::Source:
                if (a.e->sourceIndex != b.e->sourceIndex)
                    return a.e->sourceIndex < b.e->sourceIndex ? -1 : 1;
                break;
            case SortBy::Size: {
                const std::uintmax_t sa = sizeOf(s, *a.e), sb = sizeOf(s, *b.e);
                if (sa != sb) return sa < sb ? -1 : 1;
                break;
            }
            case SortBy::Modified:
                if (a.e->mtime != b.e->mtime) return a.e->mtime < b.e->mtime ? -1 : 1;
                break;
        }
        // Ties (and the Name column itself) go by name, then by where it lives.
        if (a.nameKey != b.nameKey) return a.nameKey < b.nameKey ? -1 : 1;
        if (a.folderKey != b.folderKey) return a.folderKey < b.folderKey ? -1 : 1;
        return 0;
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        const int k = key(a, b);
        return s.sortAscending ? k < 0 : k > 0;
    });
}

// What every view does with the widget it just drew for an asset: a click
// selects it, it can be dragged onto a slot or the viewport, right-clicked, and
// a model double-clicked into the scene.
void interact(EditorContext& ed, State& s, const Host& h, const Entry& e, bool tooltip) {
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) ||
        ImGui::IsItemClicked(ImGuiMouseButton_Right))
        s.selected = e.id;

    // Drag source (same GUID payload the drop targets expect).
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        const std::string g = e.id.toString();
        ImGui::SetDragDropPayload("ASSET_GUID", g.data(), 32);
        ImGui::Text("%s  %s", assetTypeName(e.type), e.relPath.c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginPopupContextItem("##asset")) {
        if (e.type == AssetType::Texture && h.editImage &&
            ImGui::MenuItem("Edit in Image editor"))
            h.editImage(e.absPath.generic_string());
        if (e.type == AssetType::Sound && h.editSound &&
            ImGui::MenuItem("Edit in Wave editor"))
            h.editSound(e.absPath.generic_string());
        if (ImGui::MenuItem("Copy path"))
            ImGui::SetClipboardText(e.absPath.generic_string().c_str());
        if (ImGui::MenuItem("Copy GUID"))
            ImGui::SetClipboardText(e.id.toString().c_str());
        ImGui::EndPopup();
    }
    if (tooltip && ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
        ImGui::SetTooltip("%s\n%s", assetTypeName(e.type), e.relPath.c_str());
    if (e.type == AssetType::Sound && h.editSound && ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        h.editSound(e.absPath.generic_string());
    if (e.type == AssetType::Model && ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const std::string mp = e.absPath.string();
        const glm::vec3 g = h.spawnAt ? h.spawnAt() : glm::vec3(0.0f);
        if (ed.isStructuredModel && ed.isStructuredModel(mp)) {
            if (ed.addModelHierarchy) ed.addModelHierarchy(g, mp);
        } else {
            const int id2 = ed.models.import(mp, ed.assetDb, ed.materials);
            if (id2 >= 0 && ed.addModelEntity) ed.addModelEntity(g, id2);
        }
    }
}

// A texture's preview, or none to draw its tag instead.
fitzel::rhi::TextureHandle previewOf(const Host& h, const Entry& e, bool onScreen) {
    return (e.type == AssetType::Texture && h.thumbnail) ? h.thumbnail(e.id, onScreen)
                                                         : fitzel::rhi::TextureHandle{};
}

// The icon a list row starts with: the picture for a texture, the tag for the
// rest, `size` square at `p`.
void drawIcon(ImDrawList* dl, ImVec2 p, float size, fitzel::rhi::TextureHandle tid, AssetType t) {
    const ImVec2 q(p.x + size, p.y + size);
    if (tid) {
        dl->AddImage(fitzel::imTexture(tid), p, q);
        return;
    }
    dl->AddRectFilled(p, q, ImGui::GetColorU32(ImGuiCol_FrameBg), 3.0f);
    const char* tag  = tagFor(t);
    ImFont*     font = ImGui::GetFont();
    const float px   = ImGui::GetFontSize() * 0.62f;
    const ImVec2 ts  = font->CalcTextSizeA(px, FLT_MAX, 0.0f, tag);
    dl->AddText(font, px, ImVec2(p.x + (size - ts.x) * 0.5f, p.y + (size - ts.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_TextDisabled), tag);
}

// `text` cut with an ellipsis until it is no wider than `width`.
std::string fitTo(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) return text;
    const char* ell = "\xE2\x80\xA6";
    std::string t = text;
    while (!t.empty() && ImGui::CalcTextSize((t + ell).c_str()).x > width) {
        t.pop_back();
        // Never leave half a UTF-8 sequence behind.
        while (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0x80) t.pop_back();
        if (!t.empty() && (static_cast<unsigned char>(t.back()) & 0xC0) == 0xC0) t.pop_back();
    }
    return t + ell;
}

// A button that reads as pressed while `on`: the view switch.
bool toggleButton(const char* label, bool on) {
    if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    const bool pressed = ImGui::Button(label);
    if (on) ImGui::PopStyleColor();
    return pressed;
}

void gridView(EditorContext& ed, State& s, const Host& h, const std::vector<Row>& rows) {
    const float pad  = ImGui::GetStyle().ItemSpacing.x;
    const auto& srcs = ed.assetDb.sources();
    for (int si = 0; si < static_cast<int>(srcs.size()); ++si) {
        if (s.source >= 0 && s.source != si) continue;
        const char* kind = srcs[si].kind == AssetSourceKind::Engine ? "Engine" : "Project";
        int count = 0;
        for (const Row& r : rows) count += (r.e->sourceIndex == si);
        const std::string hdr = srcs[si].name + " (" + kind + ")  " + std::to_string(count) +
                                "###src" + std::to_string(si);
        if (!ui::header(hdr.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) continue;
        ImGui::PushID(si);
        // A tile is the picture plus the button's frame padding on both sides.
        const ImVec2 fp    = ImGui::GetStyle().FramePadding;
        const float  tileW = s.thumbSize + fp.x * 2.0f;
        const float  avail = ImGui::GetContentRegionAvail().x;
        const int    cols  = std::max(1, static_cast<int>((avail + pad) / (tileW + pad)));
        int col = 0;
        for (const Row& r : rows) {
            const Entry& e = *r.e;
            if (e.sourceIndex != si) continue;
            if (col != 0) ImGui::SameLine();

            ImGui::PushID(e.id.toString().c_str());
            ImGui::BeginGroup();
            const ImVec2 sz(s.thumbSize, s.thumbSize);
            const fitzel::rhi::TextureHandle tid = previewOf(h, e, ImGui::IsRectVisible(sz));
            const bool sel = (e.id == s.selected);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            // The tag button is sized like an image button (picture plus frame
            // padding), so the captions in a row line up.
            if (tid) ImGui::ImageButton("##thumb", fitzel::imTexture(tid), sz);
            else     ImGui::Button(tagFor(e.type), ImVec2(sz.x + fp.x * 2.0f, sz.y + fp.y * 2.0f));
            if (sel) ImGui::PopStyleColor();
            interact(ed, s, h, e, true);

            // Caption: the file name on one line, cut to the tile's width, so
            // every tile in a row is the same height.
            ImGui::TextUnformatted(fitTo(r.name, tileW).c_str());

            ImGui::EndGroup();
            ImGui::PopID();
            col = (col + 1) % cols;
        }
        if (count == 0) ImGui::TextDisabled("  (empty)");
        ImGui::PopID();
    }
}

void listView(EditorContext& ed, State& s, const Host& h, const std::vector<Row>& rows) {
    const auto& srcs = ed.assetDb.sources();
    const float icon = std::round(ImGui::GetFontSize() * 1.5f);
    const float gap  = ImGui::GetStyle().ItemSpacing.x;
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(rows.size()), icon + ImGui::GetStyle().ItemSpacing.y);
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const Row&   r = rows[static_cast<std::size_t>(i)];
            const Entry& e = *r.e;
            ImGui::PushID(e.id.toString().c_str());
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::Selectable("##row", e.id == s.selected, ImGuiSelectableFlags_None,
                              ImVec2(0.0f, icon));
            interact(ed, s, h, e, true);
            drawIcon(dl, p, icon, previewOf(h, e, true), e.type);

            // Name, then type and where it lives, dimmed.
            const float ty = p.y + (icon - ImGui::GetTextLineHeight()) * 0.5f;
            const float x  = p.x + icon + gap;
            dl->AddText(ImVec2(x, ty), ImGui::GetColorU32(ImGuiCol_Text), r.name.c_str());
            std::string info = assetTypeName(e.type);
            if (e.sourceIndex >= 0 && e.sourceIndex < static_cast<int>(srcs.size()))
                info += "  \xC2\xB7  " + srcs[static_cast<std::size_t>(e.sourceIndex)].name;
            if (!r.folder.empty()) info += "/" + r.folder;
            const float nameW = ImGui::CalcTextSize(r.name.c_str()).x;
            dl->AddText(ImVec2(x + nameW + gap * 3.0f, ty),
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), info.c_str());
            ImGui::PopID();
        }
    }
}

void detailsView(EditorContext& ed, State& s, const Host& h, std::vector<Row>& rows) {
    const ImGuiTableFlags flags =
        ImGuiTableFlags_Sortable | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
        ImGuiTableFlags_Hideable | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable("##assetDetails", 6, flags)) return;
    const float em = ImGui::GetFontSize();
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch | ImGuiTableColumnFlags_NoHide |
                                        ImGuiTableColumnFlags_DefaultSort,
                            3.0f, static_cast<ImGuiID>(SortBy::Name));
    ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, em * 5.0f,
                            static_cast<ImGuiID>(SortBy::Type));
    ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_WidthStretch, 2.0f,
                            static_cast<ImGuiID>(SortBy::Folder));
    ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, em * 5.0f,
                            static_cast<ImGuiID>(SortBy::Source));
    ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed |
                                        ImGuiTableColumnFlags_PreferSortDescending,
                            em * 5.0f, static_cast<ImGuiID>(SortBy::Size));
    ImGui::TableSetupColumn("Modified", ImGuiTableColumnFlags_WidthFixed |
                                            ImGuiTableColumnFlags_PreferSortDescending,
                            em * 8.0f, static_cast<ImGuiID>(SortBy::Modified));
    ImGui::TableHeadersRow();

    // A click on a header sorts by it -- here and in the other two views.
    if (ImGuiTableSortSpecs* spec = ImGui::TableGetSortSpecs()) {
        if (spec->SpecsDirty && spec->SpecsCount > 0) {
            s.sortBy         = static_cast<SortBy>(spec->Specs[0].ColumnUserID);
            s.sortAscending  = spec->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
            spec->SpecsDirty = false;
        }
    }
    sortRows(s, rows);

    const auto& srcs = ed.assetDb.sources();
    const float icon = ImGui::GetTextLineHeight();
    ImDrawList* dl   = ImGui::GetWindowDrawList();
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(rows.size()));
    while (clip.Step()) {
        for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
            const Row&   r = rows[static_cast<std::size_t>(i)];
            const Entry& e = *r.e;
            ImGui::PushID(e.id.toString().c_str());
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            // The icon sits in the indent the label leaves for it.
            ImGui::Selectable(("      " + r.name).c_str(), e.id == s.selected,
                              ImGuiSelectableFlags_SpanAllColumns);
            interact(ed, s, h, e, false);
            drawIcon(dl, p, icon, previewOf(h, e, true), e.type);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted(assetTypeName(e.type));
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", r.folder.empty() ? "/" : r.folder.c_str());
            ImGui::TableNextColumn();
            if (e.sourceIndex >= 0 && e.sourceIndex < static_cast<int>(srcs.size()))
                ImGui::TextUnformatted(srcs[static_cast<std::size_t>(e.sourceIndex)].kind ==
                                               AssetSourceKind::Engine ? "Engine" : "Project");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(formatSize(sizeOf(s, e)).c_str());
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", formatTime(e.mtime).c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

} // namespace

void panel(EditorContext& ed, State& s, const Host& h) {
    if (!h.show) return;
    if (ImGui::Begin("Assets", &h.show)) {
        const float em      = ImGui::GetFontSize();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const auto& srcs    = ed.assetDb.sources();
        if (s.source >= static_cast<int>(srcs.size())) s.source = -1;

        // Toolbar, first row: the view, the tile size (grid only), Import.
        if (toggleButton("Grid", s.view == View::Grid)) s.view = View::Grid;
        ImGui::SameLine(0.0f, 2.0f);
        if (toggleButton("List", s.view == View::List)) s.view = View::List;
        ImGui::SameLine(0.0f, 2.0f);
        if (toggleButton("Details", s.view == View::Details)) s.view = View::Details;
        if (s.view == View::Grid) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(em * 7.0f);
            ImGui::SliderFloat("##size", &s.thumbSize, 48.0f, 160.0f, "Size %.0f");
        }
        ImGui::SameLine();
        if (ImGui::Button("Import...")) {
            const assetdrop::Result r = assetdrop::importWithDialog(h.projectDir, ed.assetDb);
            if (!r.message.empty()) s.dropStatus = r.message;
        }
        ImGui::SetItemTooltip("Pick files to copy into the project.\n"
                              "Or drop them here from the file manager.");
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        ImGui::SetItemTooltip("Drag an asset onto a material slot or the viewport.\n"
                              "Double-click a model to place it.\n"
                              "Right-click for more. Click a Details header to sort.");

        // What passes the name and source filters, counted per type so the Type
        // list can say how many each holds; then what passes the type as well.
        int perType[8] = {};
        int total = 0;
        std::vector<Row> rows;
        for (AssetId id : ed.assetDb.allAssets()) {
            const Entry* e = ed.assetDb.entry(id);
            if (!e) continue;
            ++total;
            if (s.source >= 0 && e->sourceIndex != s.source) continue;
            const fs::path rel(e->relPath);
            std::string name = rel.filename().string();
            if (!ui::icontains(name.c_str(), s.filter)) continue;
            ++perType[static_cast<int>(e->type)];
            if (s.type != AssetType::Unknown && e->type != s.type) continue;
            Row r;
            r.e         = e;
            r.name      = std::move(name);
            r.folder    = rel.parent_path().generic_string();
            r.nameKey   = lower(r.name);
            r.folderKey = lower(r.folder);
            rows.push_back(std::move(r));
        }
        int matchAll = 0;
        for (int n : perType) matchAll += n;

        // Second row: name, type, source.
        const float typeW = em * 9.0f, srcW = em * 9.0f;
        const float nameW = std::max(em * 6.0f, ImGui::GetContentRegionAvail().x - typeW -
                                                    srcW - spacing * 2.0f);
        ImGui::SetNextItemWidth(nameW);
        ImGui::InputTextWithHint("##assetFilter", "Filter by name...", s.filter, sizeof(s.filter),
                                 ImGuiInputTextFlags_EscapeClearsAll);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(typeW);
        const int shownOfType =
            s.type == AssetType::Unknown ? matchAll : perType[static_cast<int>(s.type)];
        const std::string typeLbl =
            std::string(pluralName(s.type)) + " (" + std::to_string(shownOfType) + ")";
        if (ImGui::BeginCombo("##type", typeLbl.c_str())) {
            const std::string all = "All types (" + std::to_string(matchAll) + ")";
            if (ImGui::Selectable(all.c_str(), s.type == AssetType::Unknown))
                s.type = AssetType::Unknown;
            for (AssetType t : kTypes) {
                const std::string l = std::string(pluralName(t)) + " (" +
                                      std::to_string(perType[static_cast<int>(t)]) + ")";
                if (ImGui::Selectable(l.c_str(), s.type == t)) s.type = t;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(srcW);
        auto srcName = [&](int si) {
            if (si < 0) return std::string("All sources");
            const auto& src = srcs[static_cast<std::size_t>(si)];
            return src.name + (src.kind == AssetSourceKind::Engine ? " (Engine)" : " (Project)");
        };
        if (ImGui::BeginCombo("##source", srcName(s.source).c_str())) {
            for (int si = -1; si < static_cast<int>(srcs.size()); ++si)
                if (ImGui::Selectable(srcName(si).c_str(), s.source == si)) s.source = si;
            ImGui::EndCombo();
        }

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
        ui::hint("%d of %d assets", static_cast<int>(rows.size()), total);
        if (s.filter[0] || s.type != AssetType::Unknown || s.source >= 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear filters")) {
                s.filter[0] = '\0';
                s.type      = AssetType::Unknown;
                s.source    = -1;
            }
        }
        if (!s.dropStatus.empty())
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", s.dropStatus.c_str());
        ImGui::Separator();

        if (rows.empty()) {
            ImGui::TextDisabled(total == 0 ? "No assets yet. Drop files here or use Import..."
                                           : "Nothing matches the filter.");
        } else if (s.view == View::Details) {
            detailsView(ed, s, h, rows);   // sorts, once the table has said by what
        } else {
            sortRows(s, rows);
            if (s.view == View::Grid) gridView(ed, s, h, rows);
            else                      listView(ed, s, h, rows);
        }
    }
    ImGui::End();
}

} // namespace assetsui
