#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json_fwd.hpp>

#include "RoadSystem.hpp"

namespace fitzel {
class Shader;
class AssetDatabase;
class TerrainStreamer;
struct TerrainEditField;
}

// Every road in the scene, the way RiverSystem holds every watercourse: a scene
// has as many roads as the author draws, each with its own control points, its
// own surface, its own side objects and its own roadside city.
//
// --- Why a list of whole RoadSystems, and not a list of paths inside one ------
// A watercourse is a path plus a rule, so RiverSystem can hold N of them in one
// object and derive N channels from one solver. A road is not that: its width,
// texture, glow, puddles, guard rails, decals, biomes, bridges, tunnels and
// loops are all per-road, and its mesh, material and collider are per-road too.
// Putting N of those inside one class would mean moving every member of
// RoadSystem into a sub-struct and rewriting every line that touches it -- for a
// result that is, exactly, N RoadSystems. So the multiplicity lives here instead
// and RoadSystem stays what it was: one road, complete.
//
// --- Why a road is never destroyed --------------------------------------------
// Road shape edits are undone by RoadShapeCmd, which borrows a RoadSystem* and
// may outlive the road being deleted -- so `remove` does not free anything, it
// marks the slot dead. The pointers every command in the history holds stay
// valid for the whole session, deleting a road is undoable by flipping one bool
// (see RoadListCmd), and the cost is a handful of kilobytes per road the author
// made and then thought better of.
//
// There is always at least one road. The editor's viewport handles, its panel
// and two hundred lines of main are written against "the road being edited", and
// a set that can be empty would make every one of them ask first. Deleting the
// last road is therefore not offered -- clearing its points is.
class RoadSet {
public:
    RoadSet(fitzel::Shader& lit, fitzel::AssetDatabase& assetDb,
            fitzel::TerrainStreamer& streamer, std::string texDir);

    // Applied to every road as it is created (including the first one and every
    // one a scene load brings in), for the wiring main can only do once it has a
    // Document -- the city's material palettes. Set it before loading a scene.
    std::function<void(RoadSystem&)> onCreate;

    // --- The list ------------------------------------------------------------
    int         count() const { return static_cast<int>(m_live.size()); }
    RoadSystem&       at(int i)       { return *m_live[clampIndex(i)]; }
    const RoadSystem& at(int i) const { return *m_live[clampIndex(i)]; }
    // The road the editor is pointed at. Never null: see the header comment.
    RoadSystem&       active()        { return at(m_sel); }
    const RoadSystem& active() const  { return at(m_sel); }
    int  selected() const { return m_sel; }
    void select(int i)    { m_sel = clampIndex(i); }

    // Add an empty road at the end of the list and select it. `name` empty picks
    // the next free "Road N".
    int  add(const std::string& name = "");
    // Drop road `i` from the scene. Refused for the last living road (there is
    // always one) -- returns false and changes nothing.
    bool remove(int i);

    // --- Undo ----------------------------------------------------------------
    // A road's identity across an add/remove undo step. Ids are handed out once
    // and never reused, so a command can name a road that is not in the list at
    // the moment it is asked to put it back.
    int  idAt(int i) const;
    int  indexOfId(int id) const;
    // Put a road back (alive = true) or take it out (false). The road's contents
    // are untouched either way -- deleting and undeleting is not a rebuild.
    void setAlive(int id, bool alive);

    // --- Iteration -----------------------------------------------------------
    // Over the LIVING roads, as pointers: `for (RoadSystem* road : roads)` reads
    // the same as the single-road code it replaces (`road->width`), which is why
    // it is pointers and not references.
    using const_iterator = std::vector<RoadSystem*>::const_iterator;
    const_iterator begin() const { return m_live.begin(); }
    const_iterator end()   const { return m_live.end(); }

    // --- Aggregates ----------------------------------------------------------
    // Grade EVERY road's corridor into `edit` and hand back the world rectangle
    // that changed. Returns false when no road had anything to build.
    //
    // All of them, not just the ones flagged: two roads that cross share cells,
    // and a corridor cut against a ground the other road had already graded is a
    // crossing that drifts every time either half is touched. Re-cutting the lot
    // from the base terrain costs a scan per road and buys a result that does not
    // depend on the order the author pressed Build in -- the same trade
    // RiverSystem::carve makes, and for the same reason. Where two carriageways
    // genuinely overlap the LAST road in the list wins the ground; nothing here
    // can grade one cell to two heights.
    bool buildAll(fitzel::TerrainEditField& edit, glm::vec2& outMin,
                  glm::vec2& outMax);
    // Re-loft every committed road on the current terrain (after a scene load or
    // a terrain change). Touches no terrain of its own.
    void rebuildMeshes();
    // Re-drape every road's guard rails, kerbs and posts -- for when the ground
    // under them moved (a river cut, a republished sculpt).
    void rebuildSideObjects();
    void refreshTextures(const std::string& projectDir);
    // True when any road's corridor is out of date, for the "Build" nag.
    bool anyNeedsBuild() const;
    // Mark every road for a rebuild (the terrain moved under all of them).
    void markNeedsBuild();

    // The highest road surface at (x,z) below `maxY`, across every enabled road.
    // False when no road covers the point. Each road is asked with its OWN
    // half-width, which is the whole point of them being separate objects.
    bool surfaceHeightAt(const glm::vec2& xz, float& outY, float maxY) const;

    // Every road's centreline in one polyline, the runs separated by a break
    // marker (see SandboxMath's isLineBreak) so the gap between two roads is not
    // mistaken for a road. What the vegetation clears itself off.
    std::vector<glm::vec2> centerlines() const;
    // The widest carriageway in the set -- the clearance vegetation keeps.
    float maxWidth() const;

    // --- Scene persistence ---------------------------------------------------
    // Writes `roads` (an array) plus, for a build that predates this class, the
    // FIRST road as the old single `road` object. Reads either.
    void save(nlohmann::json& j) const;
    void load(const nlohmann::json& j);
    // Back to one empty road, as a new scene starts.
    void clear();

private:
    // A slot outlives the road being deleted -- see the header comment.
    struct Slot {
        std::unique_ptr<RoadSystem> road;
        int  id    = 0;
        bool alive = true;
    };

    int  clampIndex(int i) const {
        if (m_live.empty()) return 0;                       // never happens
        return (i < 0) ? 0
             : (i >= static_cast<int>(m_live.size())) ? static_cast<int>(m_live.size()) - 1
             : i;
    }
    // Rebuild the living-road view after any change to the slots.
    void relist();
    // A fresh road in a new slot, wired through onCreate. Not selected, not named.
    RoadSystem& emplace();
    // Reuse slot `n` if it exists (so a scene load does not orphan the pointers
    // the history holds), else make one.
    RoadSystem& slotForLoad(int n);

    fitzel::Shader&          m_lit;
    fitzel::AssetDatabase&   m_assetDb;
    fitzel::TerrainStreamer& m_streamer;
    std::string              m_texDir;

    std::vector<Slot>        m_slots;
    std::vector<RoadSystem*> m_live;   // the alive slots, in order
    int                      m_sel = 0;
    int                      m_nextId = 1;
};
