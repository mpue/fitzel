#pragma once

#include <memory>
#include <vector>

#include "ImageDoc.hpp"

// The layer effects' working parts, shared by ImageFx.cpp (which makes them) and
// ImageDoc.cpp (which composites with them). Internal to the image editor's
// core; nothing outside it includes this.
//
// What an effect draws depends on the layer's SHAPE (its alpha through its
// mask) in a neighbourhood -- a blur, a distance -- so it cannot be worked out
// pixel by pixel while compositing. Each effect that needs one keeps a map, a
// canvas-sized byte per pixel, made from the shape; the compositor reads the
// maps and does the colour per pixel (so colour, blend mode and opacity change
// for free). A map changes only where the shape changed, give or take the
// effect's reach: a brush stroke on a layer with a shadow redoes the shadow
// under the stroke, not across the canvas.
namespace img::fx {

enum Map { DropShadow, OuterGlow, InnerShadow, InnerGlow, StrokeBand, BevelHi, BevelLo, MapCount };

struct Entry {
    // What the maps were made from: the layer's buffers (weak: the cache must not
    // keep a buffer shared, or every write would clone it) and their generation.
    std::weak_ptr<Pixels> px, mask;
    std::uint32_t gen = 0;
    bool          maskOn = true;
    int           w = 0, h = 0;
    LayerStyle    style;                  // the style whose geometry the maps are for
    std::vector<std::uint8_t> alpha;      // the shape they were made from
    std::vector<std::uint8_t> map[MapCount];
    Rect          bounds;                 // where the shape has anything (gradient overlay)
    // The gradient overlay, worked out once: t = clamp(g0 + gx * x + gy * y) or, radial,
    // |(x, y) - (cx, cy)| / r.
    float gx = 0, gy = 0, g0 = 0, cx = 0, cy = 0, rInv = 0;
};

struct Cache {
    std::vector<Entry> layers;            // by index into the layer list
};

// Brings the maps of every layer with effects up to date; cheap when nothing changed.
void ensure(Cache& c, const std::vector<Layer>& layers, int w, int h);

} // namespace img::fx
