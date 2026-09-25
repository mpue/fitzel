// bridgecheck -- does every bridge preset come out as a bridge?
//
// A generated bridge that is wrong still looks like geometry. What this catches
// is the kind of wrong you only see from the right angle, or only in Play:
//
//   1. Faces wound inside out (the loft works its winding out from geometry,
//      so a sign slip shows as a member that is black from outside and lit
//      from inside) -- checked as vertex normal vs triangle winding.
//   2. Piers or tower legs that stop short of the ground, or footings floating
//      over a valley: every support has to reach below the terrain.
//   3. The deck collider tipped the wrong way: on a climbing deck the box's
//      forward end has to be the higher one, or a car drives into a step.
//   4. NaNs, out-of-range indices, a preset that builds nothing.
//   5. The rule not surviving a save/load round trip.
//
// Pure geometry: no GL, no window.
//
//   build/release/bin/bridgecheck.exe [--obj <dir>]
//
// --obj writes each preset as <dir>/<n>.obj (one object per material slot), for
// looking at them outside the engine.

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../src/BridgeGen.hpp"
#include "../src/SplineGen.hpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(),
                detail.empty() ? "" : "  -- ", detail.c_str());
    if (!ok) ++failures;
}

// A valley 30 m deep across x in [-60, 60], rising on both sides; the deck
// runs along +X from rim to rim, 4 m higher at the far end so it climbs.
float ground(float x, float) {
    const float u = x / 60.0f;
    return -30.0f * std::max(0.0f, 1.0f - u * u);
}

std::vector<glm::vec3> deckLine() {
    std::vector<glm::vec3> out;
    for (int i = 0; i <= 120; ++i) {
        const float x = -60.0f + static_cast<float>(i);
        out.emplace_back(x, 4.0f * (x + 60.0f) / 120.0f, 0.0f);
    }
    return out;
}

void writeObj(const std::string& path, const splinegen::Result& r) {
    std::ofstream o(path);
    std::size_t base = 1;
    int slot = 0;
    for (const splinegen::Batch& b : r.batches) {
        o << "o slot" << slot++ << '\n';
        for (const fitzel::Vertex& v : b.data.vertices)
            o << "v " << v.position.x << ' ' << v.position.y << ' ' << v.position.z << '\n';
        for (std::size_t k = 0; k + 2 < b.data.indices.size(); k += 3)
            o << "f " << base + b.data.indices[k] << ' ' << base + b.data.indices[k + 1]
              << ' ' << base + b.data.indices[k + 2] << '\n';
        base += b.data.vertices.size();
    }
}

bool finite(const glm::vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} // namespace

int main(int argc, char** argv) {
    using splinegen::Preset;
    std::string objDir;
    for (int a = 1; a + 1 < argc; ++a)
        if (std::strcmp(argv[a], "--obj") == 0) objDir = argv[a + 1];
    splinegen::Palette pal;
    pal.primary   = fitzel::AssetId::generate();
    pal.secondary = fitzel::AssetId::generate();
    pal.tertiary  = fitzel::AssetId::generate();
    const std::vector<glm::vec3> deck = deckLine();

    for (int pi = static_cast<int>(Preset::BeamBridge); pi < static_cast<int>(Preset::Count); ++pi) {
        const auto pr = static_cast<Preset>(pi);
        const std::string name = splinegen::presetName(pr);
        check(splinegen::presetKind(pr) == splinegen::Kind::Bridge, name + ": is a bridge preset");
        const splinegen::Style st = splinegen::preset(pr);
        const splinegen::Result r = splinegen::generateBridge(st, deck, ground, pal);
        if (!objDir.empty())
            writeObj(objDir + "/" + std::to_string(pi - static_cast<int>(Preset::BeamBridge)) +
                         ".obj", r);

        check(!r.batches.empty() && r.verts > 0, name + ": builds geometry",
              std::to_string(r.verts) + " verts");
        check(!r.budgetHit, name + ": within the piece budget");

        // Winding vs normals, finiteness, index range.
        int bad = 0, tris = 0, nonFinite = 0, badIdx = 0;
        float lowest = 1e30f;
        for (const splinegen::Batch& b : r.batches) {
            const auto& v = b.data.vertices;
            const auto& ix = b.data.indices;
            for (const fitzel::Vertex& vx : v) {
                if (!finite(vx.position) || !finite(vx.normal)) ++nonFinite;
                lowest = std::min(lowest, vx.position.y);
            }
            for (std::size_t k = 0; k + 2 < ix.size(); k += 3) {
                if (ix[k] >= v.size() || ix[k + 1] >= v.size() || ix[k + 2] >= v.size()) {
                    ++badIdx;
                    continue;
                }
                const glm::vec3 a = v[ix[k]].position, bb = v[ix[k + 1]].position,
                                c = v[ix[k + 2]].position;
                const glm::vec3 g = glm::cross(bb - a, c - a);
                if (glm::dot(g, g) < 1e-12f) continue;   // degenerate: nothing to face
                ++tris;
                const glm::vec3 n = v[ix[k]].normal + v[ix[k + 1]].normal + v[ix[k + 2]].normal;
                if (glm::dot(g, n) <= 0.0f) ++bad;
            }
        }
        check(nonFinite == 0 && badIdx == 0, name + ": finite, indices in range");
        check(bad == 0, name + ": every face wound to its normal",
              std::to_string(bad) + " of " + std::to_string(tris) + " inverted");

        // Something reaches the valley floor (-30 at the middle), unless the
        // bridge is carried only from its ends (a tied arch, a truss on
        // abutments) -- then the abutments must reach the rims.
        const bool endsOnly = st.bridge.pierEvery <= 0.0f &&
                              st.bridge.cable == bridgegen::Cable::None &&
                              st.bridge.arch != bridgegen::Arch::Below;
        if (endsOnly)
            check(lowest < -st.sink + 0.01f, name + ": abutments reach into the ground",
                  "lowest " + std::to_string(lowest));
        else
            check(lowest < -18.0f, name + ": supports reach down into the valley",
                  "lowest " + std::to_string(lowest));

        // The deck collider climbs with the deck.
        bool climbs = false;
        int deckBoxes = 0;
        for (const splinegen::Collider& c : r.colliders) {
            if (std::abs(c.pitch) < 1e-3f) continue;   // uprights
            ++deckBoxes;
            const glm::vec3 fwdEnd = c.rotation() * glm::vec3(0.0f, 0.0f, c.half.z);
            climbs = fwdEnd.y > 0.0f && fwdEnd.x > 0.0f;
            if (!climbs) break;
        }
        check(deckBoxes > 0 && climbs, name + ": deck colliders pitch up the climb",
              std::to_string(deckBoxes) + " deck boxes");
        // ...and sit under the deck line, not above it.
        bool under = true;
        for (const splinegen::Collider& c : r.colliders) {
            if (std::abs(c.pitch) < 1e-3f || c.half.x < st.bridge.width * 0.4f) continue;
            const float lineY = 4.0f * (c.center.x + 60.0f) / 120.0f;
            const glm::vec3 up = c.rotation() * glm::vec3(0.0f, c.half.y, 0.0f);
            under = under && std::abs(c.center.y + up.y - lineY) < 0.05f;
        }
        check(under, name + ": deck collider top is the running surface");
    }

    // Save / load keeps every field.
    {
        splinegen::Style st = splinegen::preset(Preset::Suspension);
        st.bridge.camber = 1.25f;
        st.bridge.pierCols = 3;
        st.bridge.trussTop = false;
        nlohmann::json j;
        bridgegen::save(j, st.bridge);
        bridgegen::Style back;
        bridgegen::load(nlohmann::json::parse(j.dump()), back);
        check(back == st.bridge, "bridge rule survives save/load");
        bridgegen::Style keep = st.bridge;
        bridgegen::load(nlohmann::json::object(), keep);
        check(keep == st.bridge, "an empty block keeps the preset's values");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
