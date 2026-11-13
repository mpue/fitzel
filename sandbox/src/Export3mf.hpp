#pragma once

#include <string>
#include <vector>

#include <glm/glm.hpp>

class Document;
class ModelLibrary;

// What is selected, as a 3MF file: the format the slicers -- PrusaSlicer, Bambu
// Studio, Cura -- open for a 3D printer. A zip holding one XML file of corners
// and triangles, in millimetres, with Z up.
//
// Every selected object goes in with what hangs below it, as it is drawn: an
// imported model in its pose, a modelled mesh through its modifiers, a solid as
// its shape. Each object becomes one part of a single print, standing where it
// stands in the scene, the whole set on the bed (lowest point at Z 0, centred
// over the origin). The surfaces' colours go along, so a slicer that reads
// them (Bambu Studio, Cura) can offer one filament per colour.
//
// Corners that coincide are one corner: a model's file splits a corner
// wherever its normal or UV jumps, and to a slicer an edge between two corners
// that happen to share a position is a crack in the surface. Editor only.
namespace export3mf {

// A colour the print wears: a material's name and its sRGB colour.
struct Color {
    std::string name;
    glm::vec3   rgb{0.8f};
};

// One object's surface, in world space (metres, Y up), its corners welded. In
// double: a model far from the origin keeps its tenths of a millimetre.
struct Part {
    std::string             name;
    std::vector<glm::dvec3> verts;
    std::vector<glm::ivec3> tris;       // counter-clockwise seen from outside
    std::vector<int>        triColor;   // per triangle, into Shape::colors
};

struct Shape {
    std::vector<Part>  parts;
    std::vector<Color> colors;
    glm::dvec3 lo{0.0}, hi{0.0};    // world bounds of every part
    int skipped = 0;                // objects whose surface could not be read

    bool empty() const { return parts.empty(); }
};

// The objects an export of `selected` takes: each with its subtree, once each,
// those that are on and have a surface of their own (not a light, an empty, a
// decal or a player start).
std::vector<int> objectsOf(const Document& doc, const std::vector<int>& selected);

// Their surfaces. Reading an imported model's triangles back from the GPU, and
// a textured material's colour from its map, so render thread only.
Shape gather(const Document& doc, ModelLibrary& models, const std::vector<int>& selected);

struct Options {
    float mmPerMetre = 1000.0f;   // 1000: as big as it is in the scene
};

// How big the print comes out, in millimetres: width (X), depth (Y), height (Z).
glm::vec3 printSize(const Shape& s, const Options& o);

// The model part of the file (3D/3dmodel.model), on its own for the check.
std::string modelXml(const Shape& s, const Options& o, const std::string& title);

// Write the .3mf. False with `err` set when nothing could be written.
bool write(const std::string& path, const Shape& s, const Options& o, std::string& err);

} // namespace export3mf
