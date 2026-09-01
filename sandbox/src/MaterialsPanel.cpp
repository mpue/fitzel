#include "MaterialsPanel.hpp"

#include <cstdio>
#include <string>

#include <glm/glm.hpp>
#include <imgui.h>

#include <fitzel/asset/AssetDatabase.hpp>
#include <fitzel/graphics/Texture.hpp>
#include <fitzel/graphics/VideoTexture.hpp>

#include "Component.hpp"   // MaterialComponent
#include "UiStyle.hpp"
#include "VideoLibrary.hpp"

namespace materialsui {

using fitzel::AssetDatabase;
using fitzel::AssetId;
using fitzel::AssetType;
using fitzel::Texture;

void drawPanel(const PanelState& s) {
    if (!s.show) return;
    if (ImGui::Begin("Materials", &s.show)) {
        if (ImGui::Button("New")) {
            s.sel = static_cast<int>(s.materials.size());
            s.document.addMaterial("Material " + std::to_string(s.materials.size()),
                                 glm::vec3(0.7f), 0.0f, 0.2f);
        }
        ImGui::SameLine();
        const bool selFromModel = s.sel >= 0 &&
            s.sel < static_cast<int>(s.materials.size()) &&
            s.materials[s.sel].fromModel;
        // Model materials are owned by their model -> not deletable here.
        ImGui::BeginDisabled(s.materials.size() <= 1 || selFromModel);
        if (ImGui::Button("Delete") && s.materials.size() > 1 && !selFromModel) {
            const AssetId removedId = s.materials[s.sel].assetId;
            s.materials.erase(s.materials.begin() + s.sel);
            // Meshes that used it fall back to the first material.
            for (Entity& e : s.entities)
                if (auto* mc = e.components.get<MaterialComponent>();
                    mc && mc->material == removedId)
                    mc->material = s.materials[0].assetId;
            s.sel = glm::clamp(s.sel, 0,
                                static_cast<int>(s.materials.size()) - 1);
        }
        ImGui::EndDisabled();

        ImGui::Separator();
        const bool matFiltering =
            ui::searchBox("##matFilter", s.filter, s.filterCap);
        int matShown = 0;
        for (int i = 0; i < static_cast<int>(s.materials.size()); ++i) {
            if (!ui::icontains(s.materials[i].name.c_str(), s.filter)) continue;
            ++matShown;
            const std::string lbl = s.materials[i].name + "##m" + std::to_string(i);
            if (ImGui::Selectable(lbl.c_str(), i == s.sel)) s.sel = i;
            // Draggable, with the same GUID payload the Assets browser sends: the
            // viewport drops it onto whatever is under the cursor -- one face of a
            // modelled mesh, or the whole object. A material is a thing you point
            // at here and want over there, and this is the shortest way to say so.
            if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                const std::string g = s.materials[i].assetId.toString();
                ImGui::SetDragDropPayload("ASSET_GUID", g.data(), g.size());
                ImGui::TextUnformatted(s.materials[i].name.c_str());
                ImGui::EndDragDropSource();
            }
        }
        // Say what the filter is holding back rather than leaving an
        // empty list to be read as an empty library. The editor below
        // keeps showing the SELECTED material even when the filter
        // hides its row -- typing in the box must not throw away the
        // thing you were editing.
        if (matFiltering)
            ui::hint("%d of %d materials match \"%s\".", matShown,
                     static_cast<int>(s.materials.size()), s.filter);
        ImGui::Separator();

        if (s.sel >= 0 && s.sel < static_cast<int>(s.materials.size())) {
            MaterialDef& md = s.materials[s.sel];
            char mbuf[64];
            std::snprintf(mbuf, sizeof(mbuf), "%s", md.name.c_str());
            if (ImGui::InputText("Name", mbuf, sizeof(mbuf))) md.name = mbuf;
            if (md.fromModel)
                ImGui::TextDisabled("From model (edits are saved with "
                                    "the scene, not as a .fmat)");
            // A textured material samples its base-colour map, so the
            // flat albedo would do nothing; it gets a tint multiplied
            // over the map instead (white = the map untouched).
            if (md.tex)
                ImGui::ColorEdit3("Tint", &md.tint.x);
            else
                ImGui::ColorEdit3("Albedo", &md.albedo.x);
            ImGui::SliderFloat("Reflectivity", &md.reflectivity, 0.0f, 1.0f);
            ImGui::SliderFloat("Roughness", &md.roughness, 0.0f, 1.0f);
            ImGui::SliderFloat("Opacity", &md.opacity, 0.0f, 1.0f);
            // Texture-alpha handling ("transparency map"). Cutout
            // masks (hard edges); Blend alpha-blends (soft/glassy).
            const char* alphaModes[] = { "Opaque", "Cutout", "Blend" };
            int am = static_cast<int>(md.alphaMode);
            if (ImGui::Combo("Alpha mode", &am, alphaModes, 3))
                md.alphaMode = static_cast<AlphaMode>(am);
            if (md.alphaMode == AlphaMode::Cutout)
                ImGui::SliderFloat("Cutoff", &md.alphaCutoff, 0.0f, 1.0f);
            if (md.alphaMode != AlphaMode::Opaque && !md.tex)
                ImGui::TextDisabled("(needs a base texture with an alpha channel)");
            ImGui::Checkbox("Glass", &md.glass);
            if (md.glass) {
                ImGui::SameLine();
                ImGui::TextDisabled("(clear centre, reflective rim)");
            }
            // Emission: self-illumination (glow). Colour + strength apply
            // to all materials; the optional emission-map slot (below,
            // file-backed materials only) restricts the glow to its texels.
            ImGui::ColorEdit3("Emission", &md.emission.x);
            ImGui::SliderFloat("Emission strength", &md.emissionStrength,
                               0.0f, 8.0f);
            if (md.emission != glm::vec3(0.0f))
                ImGui::TextDisabled("Strength >~1.5 makes the glow bloom "
                                    "into the surroundings.");
            // Texture slots (base colour / normal / emission). Drop a
            // Texture asset from the Assets browser on one to bind it;
            // hover the swatch for a big preview. A model material
            // starts out on the maps the model shipped -- "Model" puts
            // one back, "Clear" empties the slot. Bound textures persist
            // by GUID: into the .fmat for library materials, into the
            // scene's model-material overrides for model-owned ones.
            auto mapSlot = [&](const char* label, const char* tag,
                               std::shared_ptr<Texture>& tex,
                               AssetId& texId,
                               const std::shared_ptr<Texture>& shipped) {
                std::string slot = "(none)";
                if (texId.valid()) {
                    const AssetDatabase::Entry* te = s.assetDb.entry(texId);
                    slot = te ? te->relPath : texId.toString();
                } else if (tex) {
                    slot = "(from model)";
                }
                ImGui::Text("%s", label);
                ImGui::SameLine(140.0f);
                s.texSwatch(tex, texId);
                if (tex && tex->isValid() && ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip();
                    ImGui::Image((ImTextureID)(intptr_t)tex->id(),
                                 ImVec2(256.0f, 256.0f));
                    ImGui::Text("%d x %d", tex->width(), tex->height());
                    ImGui::EndTooltip();
                }
                ImGui::Button((slot + "##" + tag).c_str());
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl =
                            ImGui::AcceptDragDropPayload("ASSET_GUID")) {
                        const AssetId gid = AssetId::fromString(std::string(
                            static_cast<const char*>(pl->Data), pl->DataSize));
                        if (s.assetDb.typeForId(gid) == AssetType::Texture) {
                            texId = gid;
                            tex   = s.assetDb.loadTexture(gid);
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (texId.valid() || tex) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(
                            (std::string("Clear##") + tag).c_str())) {
                        texId = {};
                        tex.reset();
                    }
                }
                if (shipped && tex != shipped) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(
                            (std::string("Model##") + tag).c_str())) {
                        texId = {};
                        tex   = shipped;
                    }
                }
            };
            mapSlot("Base texture:", "texslot", md.tex, md.texId,
                    md.modelTex);
            // One source per slot: a texture dropped on the base slot
            // wins over a video that was bound there.
            if (md.texId.valid() && md.videoId.valid()) md.videoId = {};
            // Video slot: drop a .fvid asset (an mp4 dropped on the
            // Assets panel is transcoded into one). It feeds the same
            // base-colour slot as the texture above, so binding one
            // clears the other -- the frame loop then points md.tex
            // at the playing texture and keeps it there.
            {
                std::string vslot = "(none)";
                if (md.videoId.valid()) {
                    const AssetDatabase::Entry* ve =
                        s.assetDb.entry(md.videoId);
                    vslot = ve ? ve->relPath : md.videoId.toString();
                }
                ImGui::Text("Video:");
                ImGui::SameLine(140.0f);
                ImGui::Button((vslot + "##vidslot").c_str());
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* pl =
                            ImGui::AcceptDragDropPayload("ASSET_GUID")) {
                        const AssetId gid = AssetId::fromString(std::string(
                            static_cast<const char*>(pl->Data), pl->DataSize));
                        if (s.assetDb.typeForId(gid) == AssetType::Video) {
                            md.videoId = gid;
                            md.texId   = {};   // same slot, one source
                            md.tex.reset();    // rebound next frame
                        }
                    }
                    ImGui::EndDragDropTarget();
                }
                if (md.videoId.valid()) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear##vidslot")) {
                        md.videoId = {};
                        md.tex.reset();
                    }
                    if (auto v = s.videos.get(s.assetDb, md.videoId)) {
                        ui::hint("%d x %d, %d frames at %.0f fps "
                                 "(%.1f s, loops)",
                                 v->width(), v->height(),
                                 v->frameCount(), v->fps(),
                                 v->duration());
                    } else {
                        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.3f, 1.0f),
                                           "This video won't open.");
                    }
                }
            }
            mapSlot("Normal map:", "nrmslot", md.normalTex, md.normalTexId,
                    md.modelNormalTex);
            mapSlot("Emission map:", "emslot", md.emissionTex,
                    md.emissionTexId, md.modelEmissionTex);
            ImGui::TextDisabled("Reflectivity mirrors the scene (env probe).");
        }
    }
    ImGui::End();
}

} // namespace materialsui
