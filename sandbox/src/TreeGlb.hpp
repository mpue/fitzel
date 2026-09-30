#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "TreeGen.hpp"

// A generated tree written out as a .glb -- the form every tree path in the
// engine already reads: vegetation species (with their generated LODs and
// impostors), scene models, the path tracer, the exported game (.fpak). A
// second tree format would have needed a second copy of each of those.
namespace treeglb {

// An image as it goes into the file: the encoded bytes (PNG or JPEG, embedded
// as they are -- no re-encode, no quality lost) and its size. `rgba` holds the
// decoded pixels too, for the built-ins, which exist nowhere else to load from.
struct Image {
    std::vector<std::uint8_t> bytes;   // encoded
    std::string mime;                  // "image/png" | "image/jpeg"
    int w = 0, h = 0;
    std::vector<std::uint8_t> rgba;    // decoded (built-ins only)
    bool valid() const { return !bytes.empty() && w > 0 && h > 0; }
};

// A PNG or JPEG file's bytes, through the VFS. Invalid if it is neither.
Image readImage(const std::string& path);
// Stand-ins for a tree that has no textures of its own (procedural PNGs), so
// every preset works on a machine without any bark or leaf images.
Image builtinBark();
Image builtinLeaf();     // one broad leaf, its stalk at the bottom of the image
Image builtinNeedles();  // a needle spray along a twig, the twig from the bottom
// A DirectX-convention normal map turned into glTF's (green up): decoded,
// green inverted, re-encoded as PNG; `rgba` holds the pixels. Invalid in,
// invalid out.
Image flipGreen(const Image& in);

// The tree as binary glTF: one node, one mesh with a bark part (opaque) and a
// leaf part (alpha MASK, double-sided), both textures embedded, and the
// generator's parameters under asset.extras.fitzelTree -- so the file alone
// reopens in the generator. `barkNormal`, when valid, becomes the bark's
// normalTexture at params.barkNormalStrength; it must already be in glTF's
// convention (see flipGreen). False (with `err`) if it could not be written.
bool writeGlb(const std::string& path, const treegen::Mesh& mesh, const treegen::Params& params,
              const Image& bark, const Image& leaf, const Image& barkNormal,
              std::string* err = nullptr);

// The parameters a .glb was generated from, if it was.
bool readParams(const std::string& path, treegen::Params& out);

} // namespace treeglb