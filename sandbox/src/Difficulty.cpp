#include "Difficulty.hpp"

#include <algorithm>
#include <fstream>

#include <nlohmann/json.hpp>

#include "Component.hpp"
#include "RaceSim.hpp"
#include "SceneTypes.hpp"

namespace difficulty {
namespace {

// The ladder, written down once. Read a COLUMN to see what one knob does across
// the ladder; read a ROW to see what a step means. Both readings have to make
// sense, which is the whole reason it is a table and not four functions.
//
//                    name      pace  catchup  slip  craft  damage  heal  boost
const Level kLevels[kCount] = {
    {"ROOKIE", 0.86f, 0.70f, 0.55f, 0.80f, 0.60f, 1.60f, 1.35f},
    {"PRO",    1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f, 1.00f},
    {"ACE",    1.12f, 0.80f, 1.60f, 1.10f, 1.30f, 0.80f, 0.90f},
    {"ELITE",  1.24f, 0.55f, 2.60f, 1.20f, 1.70f, 0.60f, 0.80f},
};

} // namespace

const Level& level(int i) {
    return kLevels[std::clamp(i, 0, kCount - 1)];
}

const char* name(int i) { return level(i).name; }

Profile load(const std::string& file) {
    Profile p;
    std::ifstream f(file);
    if (!f) return p;                        // never played: the neutral step
    nlohmann::json j;
    try { f >> j; } catch (...) { return p; }
    p.level = std::clamp(j.value("difficulty", p.level), 0, kCount - 1);
    return p;
}

void save(const std::string& file, const Profile& p) {
    const nlohmann::json j = {{"difficulty", std::clamp(p.level, 0, kCount - 1)}};
    std::ofstream f(file);
    if (f) f << j.dump(2) << '\n';
}

void applyToField(std::vector<Entity>& entities, int lvl, int skipA, int skipB) {
    const Level& L = level(lvl);
    for (Entity& e : entities) {
        auto* op = e.components.get<OpponentComponent>();
        if (!op) continue;
        if (e.id == skipA || e.id == skipB) continue;  // a seat, not a rival
        if (!op->entered) continue;
        // Grip goes as the SQUARE of the pace step, because a corner is taken at
        // sqrt(grip / curvature): scaled linearly, a step would mean less in the
        // bends than on the straights -- and the bends are where a field is
        // actually decided.
        op->speed *= L.pace;
        op->accel *= L.pace;
        op->brake *= L.pace;
        op->grip  *= L.pace * L.pace;
        op->catchup = std::clamp(op->catchup * L.catchup, 0.0f, 1.0f);
        // Both of these are already fractions of "as well as it can be done", so
        // they clamp at 1: a step above PRO cannot make a racer drive a better
        // line than the perfect one, it can only stop a lower step's roughness.
        op->racingLine = std::clamp(op->racingLine * L.craft, 0.0f, 1.0f);
        op->awareness  = std::clamp(op->awareness  * L.craft, 0.0f, 1.0f);
        op->slipScale  = L.slip;
    }
}

void applyToPlayer(racesim::RaceState& st, int lvl) {
    const Level& L = level(lvl);
    st.damageScale = L.damage;
    st.healScale   = L.heal;
    st.boostScale  = L.boost;
}

} // namespace difficulty
