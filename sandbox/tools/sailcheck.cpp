// sailcheck -- canvas the wind moves (SailCloth.hpp), measured on a real ship.
//
// A model is loaded the way the editor imports it, part by part; the part
// named on the command line is the canvas and every other part holds it. Then:
// the canvas splits into its pieces, every piece is tied somewhere (most by
// touching a yard, a stay or a rope, the rest at their top), what is tied does
// not move, the free edges move furthest, a gale moves it more than a breeze
// and calm hardly at all, nothing comes out NaN, and a frame's posing is cheap
// enough to do every frame. No GL.
//
//   build/release/bin/sailcheck.exe [model.glb] [canvas part name]
//   (default: D:/fitzel_projects/the_ship/models/sailboat.glb CinematicSails.001)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/world/Model.hpp>

#include "../src/SailCloth.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}

std::string num(float v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

// A node's corners in the model's space, as triangles.
void cornersOf(const fitzel::ModelNode& n, std::vector<fitzel::Vertex>& out) {
    for (const fitzel::ModelPrimitive& p : n.data.primitives)
        for (std::size_t i = 0; i + 7 < p.vertices.size(); i += 8) {
            fitzel::Vertex v{};
            v.position = glm::vec3(p.vertices[i], p.vertices[i + 1], p.vertices[i + 2]) + n.center;
            v.normal   = glm::vec3(p.vertices[i + 3], p.vertices[i + 4], p.vertices[i + 5]);
            v.uv       = glm::vec2(p.vertices[i + 6], p.vertices[i + 7]);
            out.push_back(v);
        }
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string path = argc > 1 ? argv[1] : "D:/fitzel_projects/the_ship/models/sailboat.glb";
    const std::string canvasName = argc > 2 ? argv[2] : "CinematicSails.001";

    const std::vector<fitzel::ModelNode> nodes = fitzel::loadModelNodes(path);
    if (nodes.empty()) {
        std::printf("cannot load %s\n", path.c_str());
        return 2;
    }
    std::vector<fitzel::Vertex> canvas;
    std::vector<glm::vec3> holders;
    for (const fitzel::ModelNode& n : nodes) {
        if (n.name == canvasName) {
            cornersOf(n, canvas);
            continue;
        }
        std::vector<fitzel::Vertex> c;
        cornersOf(n, c);
        for (const fitzel::Vertex& v : c) holders.push_back(v.position);
    }
    std::printf("%s: %zu canvas corners, %zu holding corners\n", canvasName.c_str(), canvas.size(),
                holders.size());
    check(!canvas.empty(), "the canvas part is in the model");
    if (canvas.empty()) return 1;

    sail::Cloth cloth;
    const auto t0 = std::chrono::steady_clock::now();
    const bool built = cloth.build(canvas, holders, 0.6f, 1.0f);
    const float buildMs =
        std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    check(built, "the canvas builds", num(buildMs) + " ms");
    if (!built) return 1;
    std::size_t touching = 0;
    for (const auto& is : cloth.islands) touching += is.seeds > 0 ? 1 : 0;
    std::printf("     %zu corners drawn (from %zu), %zu nodes, %zu triangles, %zu pieces (%zu tied by touching)\n",
                cloth.rest.size(), canvas.size(), cloth.nodePos.size(), cloth.nodeTris.size() / 3,
                cloth.islands.size(), touching);
    check(cloth.rest.size() < canvas.size() / 2, "the corners are shared once indexed");
    check(touching * 2 > cloth.islands.size(), "most pieces are tied where they touch the rig",
          std::to_string(touching) + " of " + std::to_string(cloth.islands.size()));
    bool everyTied = true;
    for (std::size_t n = 0; n < cloth.nodePos.size(); ++n)
        if (cloth.weight[n] < 0.0f || cloth.weight[n] > 1.0f) everyTied = false;
    std::vector<int> tiedPer(cloth.islands.size(), 0);
    for (std::size_t n = 0; n < cloth.nodePos.size(); ++n)
        if (cloth.held[n]) ++tiedPer[cloth.island[n]];
    for (int k : tiedPer) everyTied = everyTied && k > 0;
    check(everyTied, "every piece is tied somewhere, every weight in 0..1");

    // Pose it: a gale from astern (the model's bow is +Z), a fresh breeze, calm.
    const auto spread = [&](float speed, float& heldMove, float& freeMove, float& maxMove, bool& finite) {
        sail::Cloth c = cloth;
        std::vector<fitzel::Vertex> out;
        sail::Cloth::Air air;
        air.dir = glm::vec3(0.0f, 0.0f, 1.0f);
        air.speed = speed;
        heldMove = freeMove = maxMove = 0.0f;
        finite = true;
        int freeCount = 0;
        float freeSum = 0.0f;
        for (int f = 0; f < 120; ++f) {
            c.pose(air, 0.5f, 0.3f, 1.0f / 60.0f, out);
            for (std::size_t i = 0; i < out.size(); ++i) {
                const float d = glm::length(out[i].position - c.rest[i].position);
                const std::uint32_t n = c.node[i];
                finite = finite && std::isfinite(d) && std::isfinite(out[i].normal.x);
                maxMove = std::max(maxMove, d);
                if (c.held[n]) heldMove = std::max(heldMove, d);
                if (c.weight[n] > 0.85f) { freeSum += d; ++freeCount; }
            }
        }
        freeMove = freeCount ? freeSum / static_cast<float>(freeCount) : 0.0f;
    };
    float hG, fG, mG, hB, fB, mB, hC, fC, mC;
    bool okG, okB, okC;
    spread(21.0f, hG, fG, mG, okG);
    spread(8.0f, hB, fB, mB, okB);
    spread(0.0f, hC, fC, mC, okC);
    check(okG && okB && okC, "nothing comes out NaN");
    check(hG < 1e-4f, "where it is tied it does not move", num(hG) + " m");
    check(fG > 0.05f && mG < 2.0f, "the free edges move in a gale, within reason",
          "mean " + num(fG) + " m, most " + num(mG) + " m");
    check(fB < fG && fB > 0.01f, "a breeze moves them less", "mean " + num(fB) + " m");
    check(mC < 1e-4f, "calm moves nothing", num(mC) + " m");

    // What a frame costs.
    {
        sail::Cloth c = cloth;
        std::vector<fitzel::Vertex> out;
        sail::Cloth::Air air;
        air.dir = glm::vec3(0.6f, 0.0f, 0.8f);
        air.speed = 21.0f;
        c.pose(air, 0.5f, 0.3f, 1.0f / 60.0f, out);   // warm
        const auto a = std::chrono::steady_clock::now();
        const int frames = 60;
        for (int f = 0; f < frames; ++f) c.pose(air, 0.5f, 0.3f, 1.0f / 60.0f, out);
        const float ms = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - a)
                             .count() / frames;
        check(ms < 4.0f, "posing it costs little per frame", num(ms) + " ms");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all good", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
