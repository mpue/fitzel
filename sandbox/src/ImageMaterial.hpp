#pragma once

#include <string>
#include <vector>

#include "ImageDoc.hpp"

// Image > Build material from layers: one layered picture as a whole material.
//
// The last word of a layer's name says what the layer is -- "Normal",
// "Wall roughness", "brick_metal", "AO", "Glow" -- with "map", "layer",
// "copy" and numbers at the end passed over, so "Normal map" and
// "Roughness copy 2" still count. Only the last word: "Metal plate" is a
// picture of a plate, part of the colour. Every layer not named for a map
// makes the base colour, as it is seen (hidden ones left out); a map layer
// counts whether shown or not, so it can be hidden while the colour is
// painted. Where a map layer is see-through, the map has its neutral value
// (flat, mid grey, not metal, not occluded, dark, solid).
//
// Roughness (or Gloss, its inverse), Metal and AO are packed the way the
// engine's materials read them (glTF: R occlusion, G roughness, B metal); a
// Height layer becomes the normal map when there is no Normal layer.
//
// No GPU, no scene: this side writes PNGs. The editor's material library
// takes them from there (MaterialBuild.hpp).
namespace img {

enum class MapSlot { Color, Normal, Height, Roughness, Gloss, Metal, Occlusion, Emission, Opacity, Count };

MapSlot     mapSlotFor(const std::string& layerName);
const char* mapSlotName(MapSlot s);   // "Base colour", "Normal" ...

// The maps as pictures (RGBA, top row first), ready to be written.
struct MaterialImages {
    int    w = 0, h = 0;
    Pixels color, normal, orm, emission, opacity;   // empty: no such map
    bool   normalFromHeight = false;
    // What the ORM map carries, and its averages (0..1): the material's
    // roughness and metal sliders are set to them, as for a model's ORM map.
    bool   roughness = false, metal = false, occlusion = false;
    float  roughnessMean = 1.0f, metalMean = 1.0f;
};
// `height` turns a Height layer into the normal map.
MaterialImages materialImages(const Document& d, const NormalMapOptions& height);

// The files written for a material -- what the editor's library takes.
struct MaterialMaps {
    std::string name;                                   // the material's
    std::string color, normal, orm, emission, opacity;  // absolute paths; "" = no such map
    bool  roughness = false, metal = false;
    float roughnessMean = 1.0f, metalMean = 1.0f;
};
// <dir>/<file>_color.png, _normal.png, _orm.png, _emission.png, _opacity.png --
// the ones there are. False and why.
bool writeMaterial(const MaterialImages& m, const std::string& dir, const std::string& file,
                   const std::string& name, MaterialMaps& out, std::string& err);
// The suffixes a document's layers would give ("_color", "_normal" ...), for
// saying beforehand which files a build writes.
std::vector<std::string> materialSuffixes(const Document& d);

} // namespace img
