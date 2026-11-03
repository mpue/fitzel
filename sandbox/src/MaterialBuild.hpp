#pragma once

#include <string>
#include <vector>

#include "ImageMaterial.hpp"
#include "SceneTypes.hpp"

namespace fitzel { class AssetDatabase; }

// The image editor's "Build material from layers", the library's half: the
// maps it wrote (ImageMaterial.hpp) become a library material -- the one of
// that name, updated, or a new one. A rebuild therefore changes the material
// every object already wears, and only its maps: colour, normal, ORM,
// emission and opacity are what the layers say (a slot without a layer is
// emptied); glass, tint, alpha cut-off and the rest stay as they were set.
// Saved with the project, as every library material is. Editor only.
namespace matbuild {

// The status line. `sel` is pointed at the material.
std::string apply(const img::MaterialMaps& maps, std::vector<MaterialDef>& materials, int& sel,
                  fitzel::AssetDatabase& db);

} // namespace matbuild
