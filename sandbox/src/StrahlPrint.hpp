#pragma once

#include <functional>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "Export3mf.hpp"
#include "Strahl.hpp"

// A Strahl scene's solids as a 3MF for a 3D printer: the isosurfaces above all
// -- a gyroid lattice, a noise rock, metaballs, shapes that exist only as an
// equation and that no modeller could have drawn -- and with them every exact
// shape and CSG the script made, picked one by one.
//
// Each picked shape is meshed from the solid itself (TraceMesh.hpp: closed and
// manifold by construction, every corner on the real surface) and becomes one
// part of a single print, standing where it stands in the scene. The parts'
// colours are their materials' -- a pattern's average over the surface it
// covers -- so a slicer that reads them offers a filament per colour.
//
// What a scene has that is not a solid stays out: a plane has no far side, a
// mesh (a lathe, the studio's backdrop) has no inside the tracer knows of, and
// a lamp's bulb is light. No GL: the strahl CLI (--3mf), the editor's Strahl
// window and strahlcheck all go through here.
namespace strahl::print {

// One shape of the scene that could be printed.
struct Piece {
    int         root = 0;       // index into Scene::shapes->roots (and Result::roots)
    int         node = 0;       // its node in Scene::shapes->nodes
    std::string kind;           // "isosurface", "intersection", ...
    std::string file;           // where the script made it
    int         line = 0;
    bool        imported = false;   // file is not the script itself
    bool        iso = false;    // an isosurface is in it somewhere
    // World bounds of the solid itself, measured on a coarse grid (the
    // tracer's own are generous: an isosurface's are its whole box).
    glm::dvec3  lo{0.0}, hi{0.0};
    glm::dvec3  boxLo{0.0}, boxHi{0.0};   // the tracer's: the grid it is meshed on

    std::string label() const;   // "isosurface, line 18"
};

// The shapes of a built scene that are solids: bounded, and not a lamp's bulb.
std::vector<Piece> pieces(const Result& r);
// The ones an export takes when nobody picked: the isosurfaces, or every
// solid when there is none.
std::vector<int> defaultPick(const std::vector<Piece>& all);

struct Options {
    // How big: millimetres per scene unit, or (0) the longest side of the
    // picked pieces' bounds fitted to `longestMm`.
    double mmPerUnit = 0.0;
    double longestMm = 100.0;
    double cellMm    = 0.25;    // grid spacing on the print: the detail it keeps
    int    maxCells  = 1500;    // across a piece; a finer cell is coarsened to fit
    int    threads   = 0;
    // 0..1 over all pieces; false stops.
    std::function<bool(float)> progress;
};

// Millimetres per scene unit for `picked` (indices into `all`) under `o`.
double mmPerUnit(const std::vector<Piece>& all, const std::vector<int>& picked, const Options& o);
// The grid the largest of them would need, cells across its longest side.
int cellsAcross(const std::vector<Piece>& all, const std::vector<int>& picked, const Options& o);
// The cell mesh() will use: cellMm, or coarser when that would be more than
// maxCells across (a print that big still comes out, as fine as it can).
double cellUsed(const std::vector<Piece>& all, const std::vector<int>& picked, const Options& o);

// Mesh `picked` into a shape export3mf::write() takes (scene units as its
// metres, written with mmPerMetre = `mmUsed`: mmPerUnit(), or when fitting,
// what makes the meshed result's longest side exactly longestMm). Pieces that
// fail are left out and named in `notes`; false with `err` when none came out.
bool mesh(const Result& r, const std::vector<Piece>& all, const std::vector<int>& picked, const Options& o,
          export3mf::Shape& out, double& mmUsed, std::vector<std::string>& notes, std::string& err);

} // namespace strahl::print
