#pragma once

#include <nlohmann/json_fwd.hpp>

// Free-standing bridges: a deck laid along a spline path and carried on piers,
// arches, a truss or cables. Independent of the road -- a footbridge over a
// brook, a viaduct across a valley, a suspension bridge nobody drives on -- and
// built exactly like the fences and walls around it: the path and this rule are
// saved, the bridge itself is re-derived whenever either changes.
//
// This header is only the RULE. It is a member of splinegen::Style (a bridge is
// splinegen::Kind::Bridge), the geometry lives in BridgeGen.cpp behind
// splinegen::generateBridge, and the panel section in BridgePanel.cpp.
//
// Every bridge type is the same rule with different numbers, the way every fence
// is: a beam bridge is a deck with piers, a viaduct the same with masonry arches
// between them, a suspension bridge a deck with two towers and a cable. So the
// types are presets (splinegen::Preset::BeamBridge ...) rather than branches, and
// a truss can be put on a suspension bridge if that is what the author wants.
namespace bridgegen {

// What carries the deck besides the piers. Saved as an index: append only.
enum class Arch {
    None,
    Below,    // one arch under the deck, standing on footings, columns up to it
    Above,    // one arch over the deck, hangers down to it (a tied arch)
    Masonry,  // a stone arch in every span between the piers (a viaduct)
    Count
};

enum class Cable {
    None,
    Suspension,  // main cables draped over the towers, vertical hangers
    Stayed,      // straight stays fanning from the tower tops to the deck
    Count
};

enum class Rail {
    None,
    Parapet,  // a solid upstand along each edge
    Railing,  // posts with a hand rail and a mid rail
    Count
};

struct Style {
    // --- Deck ----------------------------------------------------------------
    float width      = 8.0f;   // edge to edge
    float thick      = 0.6f;   // slab thickness under the running surface
    float girder     = 1.0f;   // depth of the beam under the slab (0 = none)
    // Metres the middle of the deck rises above the straight line between its
    // ends -- the hump of an old stone bridge. 0 = straight.
    float camber     = 0.0f;

    // --- Edges ---------------------------------------------------------------
    Rail  rail       = Rail::Parapet;
    float railHeight = 1.1f;
    float railThick  = 0.3f;
    float postEvery  = 2.0f;   // Railing: metres between posts

    // --- Supports ------------------------------------------------------------
    // Metres between piers. Evened out over the length, so every span of a
    // bridge is the same -- nobody builds one with a stub at the end. 0 = none.
    float pierEvery  = 24.0f;
    int   pierCols   = 1;      // 1 = a wall pier, 2+ = columns with a cap beam
    float pierWidth  = 1.6f;   // along the deck (and the columns' side)
    float pierDepth  = 5.0f;   // across the deck, for a wall pier
    float abutment   = 4.0f;   // length of the block under each end (0 = none)

    // --- Arch ----------------------------------------------------------------
    Arch  arch       = Arch::None;
    // Above: crown height over the deck. Below: depth of the springing under
    // it. Masonry: the rise of each span's arch (capped at a semicircle).
    float archRise   = 12.0f;
    float archRib    = 1.2f;   // rib section (masonry: ring thickness at the crown)
    float archInset  = 0.0f;   // metres in from each end the single arch springs
    float hangerEvery= 5.0f;   // hangers / spandrel columns / suspension hangers
    float hangerThick= 0.25f;

    // --- Truss ---------------------------------------------------------------
    float trussHeight= 0.0f;   // 0 = none
    float trussPanel = 5.0f;   // metres between panel points
    float trussBar   = 0.35f;  // member section
    bool  trussTop   = true;   // cross bracing over the deck

    // --- Cables --------------------------------------------------------------
    Cable cable      = Cable::None;
    float towerHeight= 30.0f;  // above the deck
    // Where the towers stand, as a fraction of the length from each end.
    // 0.5 puts a single tower in the middle.
    float towerAt    = 0.25f;
    float towerWidth = 1.8f;
    float cableSag   = 1.5f;   // Suspension: the cable's lowest point above the deck
    float cableThick = 0.35f;
    float stayEvery  = 8.0f;   // Stayed: metres between anchors on the deck

    bool operator==(const Style& o) const;
    bool operator!=(const Style& o) const { return !(*this == o); }
};

// Scene persistence. A field the file does not carry keeps what `s` already
// holds -- the preset's value -- so older scenes load.
void save(nlohmann::json& j, const Style& s);
void load(const nlohmann::json& j, Style& s);

} // namespace bridgegen
