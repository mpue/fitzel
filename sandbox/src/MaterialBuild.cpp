#include "MaterialBuild.hpp"

#include <fitzel/asset/AssetDatabase.hpp>

namespace matbuild {

std::string apply(const img::MaterialMaps& maps, std::vector<MaterialDef>& materials, int& sel,
                  fitzel::AssetDatabase& db) {
    int at = -1;
    for (int i = 0; i < int(materials.size()); ++i)
        if (materials[std::size_t(i)].name == maps.name && !materials[std::size_t(i)].fromModel) at = i;
    const bool fresh = at < 0;
    if (fresh) {
        MaterialDef md;
        md.assetId = fitzel::AssetId::generate();
        md.name    = maps.name;
        materials.push_back(std::move(md));
        at = int(materials.size()) - 1;
    }
    MaterialDef& md = materials[std::size_t(at)];
    // A file of the project: its id (imported on the spot when the poll has
    // not seen it yet) and the texture, which the asset watcher keeps fresh
    // when the next build writes the file again.
    int missing = 0;
    auto bind = [&](const std::string& file, std::shared_ptr<fitzel::Texture>& tex, fitzel::AssetId& id) {
        id  = file.empty() ? fitzel::AssetId{} : db.idForPath(file);
        tex = id.valid() ? db.loadTexture(id) : nullptr;
        if (!file.empty() && !tex) ++missing;
    };
    bind(maps.color, md.tex, md.texId);
    if (md.texId.valid()) {
        // One source for the base colour: the picture.
        md.videoId = {};
        md.cameraName.clear();
    }
    bind(maps.normal, md.normalTex, md.normalTexId);
    bind(maps.orm, md.ormTex, md.ormTexId);
    if (md.ormTexId.valid()) {
        // The sliders hold the map's average, as for a model's ORM map; a part
        // the layers left out is white and leaves its slider alone.
        md.ormMean = {maps.roughness ? maps.roughnessMean : 1.0f, maps.metal ? maps.metalMean : 1.0f};
        if (maps.roughness) md.roughness = maps.roughnessMean;
        if (maps.metal) md.reflectivity = maps.metalMean;
    } else {
        md.ormMean = glm::vec2(1.0f);
    }
    // Emission and opacity switch on with their map, and off again when a
    // build no longer has it: without its map the glow would cover everything.
    const bool hadGlow = md.emissionTexId.valid(), hadOpacity = md.opacityTexId.valid();
    bind(maps.emission, md.emissionTex, md.emissionTexId);
    if (md.emissionTexId.valid() && md.emission == glm::vec3(0.0f)) md.emission = glm::vec3(1.0f);
    if (hadGlow && !md.emissionTexId.valid()) md.emission = glm::vec3(0.0f);
    bind(maps.opacity, md.opacityTex, md.opacityTexId);
    if (md.opacityTexId.valid() && md.alphaMode == AlphaMode::Opaque) md.alphaMode = AlphaMode::Cutout;
    if (hadOpacity && !md.opacityTexId.valid() && md.alphaMode == AlphaMode::Cutout) md.alphaMode = AlphaMode::Opaque;
    sel = at;

    int count = 0;
    for (const std::string* f : {&maps.color, &maps.normal, &maps.orm, &maps.emission, &maps.opacity}) count += !f->empty();
    std::string msg = (fresh ? "New material \"" : "Updated material \"") + maps.name + "\" with " + std::to_string(count) +
                      (count == 1 ? " map" : " maps") + " (the project's File > Save keeps it).";
    if (missing) msg += " " + std::to_string(missing) + " could not be loaded (outside the project?).";
    return msg;
}

} // namespace matbuild
