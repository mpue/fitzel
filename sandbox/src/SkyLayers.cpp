#include "SkyLayers.hpp"

#include <algorithm>
#include <cmath>

namespace skylayers {
namespace {

// Which weather::Sky field is which cloud kind. The ordering below walks this
// instead of repeating six near-identical blocks, which is the only reason
// adding a seventh cloud type is one line here rather than an edit in a loop.
//
// The member pointer is what makes that possible: the slots are named fields on
// weather::Sky (so a preset file stays readable and a diff stays meaningful)
// but a loop needs them as a sequence, and a pointer-to-member is both.
//
// The panel has its own copy of this table, with the labels and the height
// ranges on it -- see SkyLayersPanel.cpp. Two short tables rather than one
// shared one because the shared one would have to live in the header and drag
// the panel's strings into the shipped player, which never draws a slider.
struct Slot {
    weather::Sheet weather::Sky::*field;
    Kind kind;
};

constexpr Slot kSlots[] = {
    {&weather::Sky::stratus,       kStratus},
    {&weather::Sky::stratocumulus, kStratocumulus},
    {&weather::Sky::altocumulus,   kAltocumulus},
    {&weather::Sky::cirrus,        kCirrus},
    {&weather::Sky::cirrocumulus,  kCirrocumulus},
    {&weather::Sky::contrails,     kContrails},
};

} // namespace

// --- Ordering ---------------------------------------------------------------
std::vector<Packed> order(const weather::Sky& sky, float cumulusBase,
                          float cumulusTop) {
    std::vector<Packed> out;
    out.reserve(kMaxLayers);

    for (const Slot& s : kSlots) {
        const weather::Sheet& sh = sky.*(s.field);
        // A layer that is off, or has nothing in it, is not sorted and not
        // uploaded: it would cost a plane intersection and a mask evaluation
        // per pixel to produce zero.
        if (!sh.on || sh.amount <= 0.0f) continue;
        Packed p;
        p.kind   = s.kind;
        p.amount = sh.amount;
        p.height = sh.height;
        p.scale  = sh.scale;
        p.wind   = sh.wind;
        p.dirX   = std::cos(sh.dir);
        p.dirZ   = std::sin(sh.dir);
        out.push_back(p);
    }

    // The cumulus, at its top. Sorting the deck on its TOP rather than its base
    // is what puts a sheet that sits inside the deck's own altitude range in
    // front of it, which is the right way round: you are under both, and the
    // one whose cloud you meet first is the lower one.
    Packed cu;
    cu.kind   = kCumulus;
    cu.height = cumulusTop;
    out.push_back(cu);
    (void)cumulusBase;

    // Highest first: painter's order for something you are standing under.
    std::stable_sort(out.begin(), out.end(),
                     [](const Packed& a, const Packed& b) {
                         return a.height > b.height;
                     });
    if (out.size() > static_cast<std::size_t>(kMaxLayers))
        out.resize(static_cast<std::size_t>(kMaxLayers));
    return out;
}

} // namespace skylayers
