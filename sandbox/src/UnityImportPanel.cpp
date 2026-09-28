#include "UnityImportPanel.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <unordered_set>

#include <imgui.h>

#include <fitzel/asset/AssetDatabase.hpp>

#include "FolderDialog.hpp"

namespace unityimportui {

std::vector<std::pair<std::string, std::string>> scanFbx(const std::string& dir) {
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<std::filesystem::path> stack;
    if (!dir.empty()) stack.push_back(std::filesystem::path(dir));
    int scanned = 0;
    while (!stack.empty() && out.size() < 2000 && scanned < 40000) {
        const std::filesystem::path d = stack.back();
        stack.pop_back();
        std::error_code lec;
        std::filesystem::directory_iterator
            dit(d, std::filesystem::directory_options::skip_permission_denied, lec), dend;
        for (; !lec && dit != dend; dit.increment(lec)) {
            ++scanned;
            std::error_code tec;
            if (dit->is_directory(tec)) { stack.push_back(dit->path()); continue; }
            std::string ext = dit->path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".fbx") continue;
            std::error_code rec;
            std::string rel = std::filesystem::relative(dit->path(), dir, rec).generic_string();
            if (rel.empty()) rel = dit->path().filename().string();
            out.push_back({rel, dit->path().generic_string()});
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

// A model imported from OUTSIDE the project's asset tree has no persistent GUID,
// so it would vanish on reload and never show in Assets. Copy it (plus the maps
// the matcher resolved) into the project's models/ folder, register it, and
// import the copy -- now it round-trips through save/load by GUID. Returns the
// path to import.
std::string copyIntoProject(State& s, const Host& h) {
    std::error_code cec;
    const std::filesystem::path fp(s.fbx);
    std::string parent = fp.parent_path().filename().string();
    for (char& c : parent) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const bool inMeshDir = parent == "meshes" || parent == "models" ||
                           parent == "mesh"   || parent == "fbx";
    const std::string pack = (inMeshDir ? fp.parent_path().parent_path().filename()
                                        : fp.parent_path().filename()).string();
    const std::string destPack = h.modelDir + "/" + (pack.empty() ? fp.stem().string() : pack);
    const std::string destMesh = destPack + "/Meshes";
    const std::string destTex  = destPack + "/Textures";
    std::filesystem::create_directories(destMesh, cec);
    std::filesystem::create_directories(destTex, cec);
    const std::string destFbx = destMesh + "/" + fp.filename().string();
    std::filesystem::copy_file(s.fbx, destFbx,
                               std::filesystem::copy_options::overwrite_existing, cec);
    int nTex = 0;
    std::unordered_set<std::string> done;
    for (const auto& m : fitzel::previewUnityTextures(s.fbx))
        for (const std::string& t : {m.albedo, m.normal, m.emission})
            if (!t.empty() && done.insert(t).second) {
                std::error_code fc;
                std::filesystem::copy_file(
                    t, destTex + "/" + std::filesystem::path(t).filename().string(),
                    std::filesystem::copy_options::skip_existing, fc);
                if (!fc) ++nTex;
            }
    h.assetDb.refresh(); // register the copied FBX + maps (GUIDs)
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "Copied into project (%d map(s)); it now persists and appears in Assets.",
                  nTex);
    s.status = buf;
    return destFbx;
}

} // namespace

void panel(State& s, const Host& h) {
    if (!h.show) return;
    ImGui::SetNextWindowSize(ImVec2(560.0f, 470.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Import Unity asset", &h.show)) {
        if (s.dir.empty()) s.dir = h.modelDir;
        ImGui::TextWrapped(
            "Unity FBX files don't reference their textures directly, so a plain "
            "import leaves them unmapped. Point this at an asset's folder: maps "
            "kept in a Textures/ folder and named like the material or model "
            "(e.g. Rock_Albedo, Rock_Normal) are matched automatically.");
        ImGui::Separator();

        ImGui::TextWrapped("Folder: %s", s.dir.empty() ? "(none)" : s.dir.c_str());
        if (ImGui::Button("Browse...")) {
            std::string picked;
            if (ed::pickFolder(picked, s.dir)) { s.dir = picked; s.fbx.clear(); s.scanDir.clear(); }
        }
        ImGui::SameLine();
        if (ImGui::Button("Use models/ folder")) { s.dir = h.modelDir; s.fbx.clear(); s.scanDir.clear(); }
        ImGui::SameLine();
        if (ImGui::Button("Rescan")) s.scanDir.clear();

        // (Re)scan only when the folder changes, so we don't hit the disk every frame.
        if (s.dir != s.scanDir) {
            s.fbxList = scanFbx(s.dir);
            s.scanDir = s.dir;
        }

        ImGui::Spacing();
        ImGui::Text("FBX files (%d):", static_cast<int>(s.fbxList.size()));
        ImGui::BeginChild("##fbxlist", ImVec2(0.0f, 130.0f), true);
        for (const auto& f : s.fbxList)
            if (ImGui::Selectable(f.first.c_str(), s.fbx == f.second)) s.fbx = f.second;
        if (s.fbxList.empty()) ImGui::TextDisabled("(no .fbx found under this folder)");
        ImGui::EndChild();

        // Recompute the texture-match preview when the selection changes.
        if (s.fbx != s.previewFor) {
            s.preview = s.fbx.empty() ? std::vector<fitzel::UnityTexMatch>{}
                                      : fitzel::previewUnityTextures(s.fbx);
            s.nearby  = s.fbx.empty() ? std::vector<std::string>{}
                                      : fitzel::nearbyTextureFiles(s.fbx);
            s.previewFor = s.fbx;
        }

        if (!s.fbx.empty()) {
            ImGui::Text("Materials & matched maps:");
            if (ImGui::BeginTable("##unitytex", 4,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                                  ImVec2(0.0f, 150.0f))) {
                ImGui::TableSetupColumn("Material");
                ImGui::TableSetupColumn("Albedo");
                ImGui::TableSetupColumn("Normal");
                ImGui::TableSetupColumn("Emission");
                ImGui::TableHeadersRow();
                const ImVec4 ok(0.55f, 0.85f, 0.55f, 1.0f);
                const ImVec4 no(0.6f, 0.6f, 0.6f, 1.0f);
                auto cell = [&](const std::string& p) {
                    if (p.empty()) ImGui::TextColored(no, "- none");
                    else ImGui::TextColored(ok, "%s",
                                            std::filesystem::path(p).filename().string().c_str());
                };
                for (const auto& m : s.preview) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(m.material.c_str());
                    ImGui::TableSetColumnIndex(1); cell(m.albedo);
                    ImGui::TableSetColumnIndex(2); cell(m.normal);
                    ImGui::TableSetColumnIndex(3); cell(m.emission);
                }
                ImGui::EndTable();
            }
            if (s.preview.empty()) ImGui::TextDisabled("(no materials found in this FBX)");

            // Diagnostic: the actual image files the matcher looked at. If maps
            // show "- none" above but files are listed here, the naming is
            // unusual -- and these names are what to tune the matcher by.
            if (ImGui::TreeNode("Texture files found nearby (diagnostic)")) {
                if (s.nearby.empty())
                    ImGui::TextDisabled("(no image files found in the usual Textures/ folders)");
                for (const std::string& n : s.nearby) ImGui::BulletText("%s", n.c_str());
                ImGui::TreePop();
            }
        }

        ImGui::Separator();
        ImGui::BeginDisabled(s.fbx.empty());
        if (ImGui::Button("Import to scene", ImVec2(160.0f, 0.0f))) {
            std::string src = s.fbx;
            if (!h.assetDb.idForPath(s.fbx).valid()) src = copyIntoProject(s, h);
            else s.status = "Imported (already in the project).";
            if (h.importHierarchy) h.importHierarchy(h.spawnAt ? h.spawnAt() : glm::vec3(0.0f),
                                                     src, s.flipV);
        }
        ImGui::EndDisabled();
        if (!s.status.empty()) ImGui::TextDisabled("%s", s.status.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("One entity per part.");
        ImGui::Checkbox("Flip texture V", &s.flipV);
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("If the texture looks misplaced on an atlas, toggle this and "
                              "re-import.\nFBX/DAE usually need it on; some packs need it off.");
        ImGui::TextDisabled("Tip: keep the asset inside your project so it reloads with the scene.");
    }
    ImGui::End();
}

} // namespace unityimportui
