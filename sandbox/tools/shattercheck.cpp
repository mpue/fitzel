// shattercheck -- glass that breaks (Shatter.hpp).
//
// Finding the pane is geometry, so it is measured on a window built the way the
// factory's are: panes as two-sided cards 4 mm apart, each with its own corners,
// side by side behind a frame. A shot at one takes that one -- its face, its
// back -- and not its neighbour; a shot at the frame a couple of centimetres off
// takes nothing; a thin slab goes with its edges, a thick block is not a pane.
// The cracks must cover the pane exactly (no gap, no overlap: the areas add up),
// crumbs at the hole and large pieces at the frame. The shards are as solid as
// the pane, fly with the shot, faster near the hole, and come to lie flat on the
// floor. No GL.
//
//   build/release/bin/shattercheck.exe
//   build/release/bin/shattercheck.exe --model D:/models/lost_factory/lost_factory.glb
//
// With --model, a real model's glass as well: every see-through triangle in it
// is shot in turn, and each must belong to exactly one pane of a sheet and its
// back (two or four triangles) -- one window of twenty panes is twenty panes.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <fitzel/world/Model.hpp>

#include "../src/Shatter.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}
std::string num(float v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}

// A quad as two triangles, counter-clockwise seen from where `n` points.
void quad(std::vector<glm::vec3>& t, glm::vec3 a, glm::vec3 b, glm::vec3 c, glm::vec3 d) {
    t.insert(t.end(), {a, b, c, a, c, d});
}

// A two-sided pane in the wall plane z = z0, from (x0, y0) to (x1, y1): the
// front facing +z, the back 4 mm behind facing -z (lf_lib's card()).
void card(std::vector<glm::vec3>& t, float x0, float y0, float x1, float y1, float z0) {
    quad(t, {x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0});
    const float zb = z0 - 0.004f;
    quad(t, {x0, y0, zb}, {x0, y1, zb}, {x1, y1, zb}, {x1, y0, zb});
}

// A box from lo to hi, outward faces.
void box(std::vector<glm::vec3>& t, glm::vec3 lo, glm::vec3 hi) {
    const glm::vec3 p[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z},
                            {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    quad(t, p[4], p[5], p[6], p[7]);   // +z
    quad(t, p[1], p[0], p[3], p[2]);   // -z
    quad(t, p[5], p[1], p[2], p[6]);   // +x
    quad(t, p[0], p[4], p[7], p[3]);   // -x
    quad(t, p[7], p[6], p[2], p[3]);   // +y
    quad(t, p[0], p[1], p[5], p[4]);   // -y
}

float polyArea(const std::vector<glm::vec2>& p) {
    float a = 0.0f;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const glm::vec2& u = p[i];
        const glm::vec2& v = p[(i + 1) % p.size()];
        a += u.x * v.y - u.y * v.x;
    }
    return 0.5f * a;
}

} // namespace

// Every see-through triangle of the model's parts, shot at its middle.
void checkModel(const std::string& path) {
    const std::vector<fitzel::ModelNode> nodes = fitzel::loadModelNodes(path, true);
    check(!nodes.empty(), "the model loads", path);
    int panes = 0, tris = 0, odd = 0, parts = 0;
    float thinnest = 1e9f, thickest = 0.0f, largest = 0.0f;
    for (const fitzel::ModelNode& node : nodes)
        for (const fitzel::ModelPrimitive& p : node.data.primitives) {
            // As the import judges it: cut out only with a map to cut by.
            if (p.baseColor[3] >= 0.99f || (p.alphaCutout && !p.texPixels.empty())) continue;
            std::vector<glm::vec3> t;
            std::vector<glm::vec2> uv;
            for (std::size_t i = 0; i + 7 < p.vertices.size(); i += 8) {
                t.push_back({p.vertices[i], p.vertices[i + 1], p.vertices[i + 2]});
                uv.push_back({p.vertices[i + 6], p.vertices[i + 7]});
            }
            const std::size_t nt = t.size() / 3;
            if (nt == 0) continue;
            ++parts;
            tris += static_cast<int>(nt);
            std::vector<bool> gone(nt, false);
            for (std::size_t k = 0; k < nt; ++k) {
                if (gone[k]) continue;
                const glm::vec3 hit = (t[k * 3] + t[k * 3 + 1] + t[k * 3 + 2]) / 3.0f;
                shatter::Pane pane;
                std::vector<std::uint32_t> taken;
                if (!shatter::paneAt(t, uv, gone, hit, 0.006f, pane, taken)) { ++odd; gone[k] = true; continue; }
                for (std::uint32_t x : taken) gone[x] = true;
                ++panes;
                if (taken.size() != 2 && taken.size() != 4) ++odd;
                thinnest = std::min(thinnest, pane.thickness);
                thickest = std::max(thickest, pane.thickness);
                largest = std::max(largest, pane.area());
            }
        }
    std::printf("      %d glass parts, %d triangles, %d panes, %.1f..%.1f mm thick, largest %.3f m2\n", parts, tris,
                panes, thinnest * 1000.0f, thickest * 1000.0f, largest);
    check(panes > 0, "it has glass");
    check(odd == 0, "every glass triangle is in one pane of two or four", std::to_string(odd) + " odd");
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 2 && std::string(argv[1]) == "--model") {
        checkModel(argv[2]);
        std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all ok", failures, failures == 1 ? "" : "s");
        return failures ? 1 : 0;
    }

    // --- What is glass --------------------------------------------------------------
    {
        MaterialDef glass;   glass.glass = true;
        MaterialDef faded;   faded.opacity = 0.42f;                     // the factory's
        MaterialDef leaves;  leaves.opacity = 0.5f; leaves.alphaMode = AlphaMode::Cutout;
        MaterialDef brick;
        check(shatter::breakable(glass), "a Glass material breaks");
        check(shatter::breakable(faded), "a see-through one (opacity 0.42) breaks");
        check(!shatter::breakable(leaves), "a cut-out one does not");
        check(!shatter::breakable(brick), "an opaque one does not");
    }

    // --- The window -----------------------------------------------------------------
    // Two panes 0.30 x 0.45 with a 4.5 cm bar between them, and a third one
    // above the first: the bars themselves are another material, not in here.
    std::vector<glm::vec3> win;
    card(win, 0.0f, 0.0f, 0.30f, 0.45f, 0.0f);         // pane A, triangles 0..3
    card(win, 0.345f, 0.0f, 0.645f, 0.45f, 0.0f);      // pane B, 4..7
    card(win, 0.0f, 0.495f, 0.30f, 0.945f, 0.0f);      // pane C, 8..11
    std::vector<glm::vec2> uvs;
    for (const glm::vec3& p : win) uvs.push_back({p.x * 2.0f, p.y * 2.0f});   // 2 per metre
    shatter::Pane pane;
    std::vector<std::uint32_t> taken;
    const glm::vec3 hitA(0.10f, 0.20f, 0.0f);
    {
        const bool found = shatter::paneAt(win, uvs, {}, hitA, 0.006f, pane, taken);
        check(found, "a shot at pane A finds a pane");
        std::vector<std::uint32_t> sorted = taken;
        std::sort(sorted.begin(), sorted.end());
        check(sorted == std::vector<std::uint32_t>{0, 1, 2, 3}, "it is A: its face and its back, nothing else",
              std::to_string(taken.size()) + " triangles");
        check(std::abs(pane.thickness - 0.004f) < 1e-4f, "as thick as front to back", num(pane.thickness));
        check(std::abs(glm::dot(pane.origin, pane.n) - glm::dot(glm::vec3(0, 0, -0.002f), pane.n)) < 1e-4f,
              "its middle between the two sheets");
        check(std::abs(pane.area() - 0.30f * 0.45f) < 1e-4f, "its outline is the pane", num(pane.area()));
        check(pane.contains(hitA, 0.001f) && pane.contains(glm::vec3(0.29f, 0.44f, -0.004f), 0.001f),
              "the hit and the back's far corner are in it");
        check(!pane.contains(glm::vec3(0.40f, 0.20f, 0.0f), 0.01f), "pane B is not");
        check(!pane.contains(glm::vec3(0.10f, 0.20f, 0.05f), 0.01f), "nor a point 5 cm in front of it");
        const glm::vec2 uv = pane.uvAt(pane.flat(glm::vec3(0.30f, 0.45f, 0.0f)));
        check(glm::length(uv - glm::vec2(0.60f, 0.90f)) < 1e-3f, "the texture runs on across it",
              num(uv.x) + ", " + num(uv.y));
    }
    {
        std::vector<std::uint32_t> t2;
        shatter::Pane p2;
        check(!shatter::paneAt(win, uvs, {}, glm::vec3(0.3225f, 0.20f, 0.0f), 0.006f, p2, t2),
              "a shot at the bar between two panes breaks nothing");
        check(!shatter::paneAt(win, uvs, {}, glm::vec3(0.10f, 0.20f, 0.03f), 0.006f, p2, t2),
              "nor one at the frame 3 cm in front of the glass");
        // A gone pane is skipped: the same shot again meets nothing.
        std::vector<bool> skip(win.size() / 3, false);
        for (std::uint32_t t : taken) skip[t] = true;
        check(!shatter::paneAt(win, uvs, skip, hitA, 0.006f, p2, t2), "a broken pane is not found again");
        check(shatter::paneAt(win, uvs, skip, glm::vec3(0.50f, 0.20f, 0.0f), 0.006f, p2, t2) && t2.size() == 4 &&
                  t2[0] == 4,
              "pane B still breaks on its own");
    }
    {
        // A slab 1 cm thick: face, back and the four thin edges.
        std::vector<glm::vec3> slab;
        box(slab, glm::vec3(0.0f, 0.0f, -0.01f), glm::vec3(1.0f, 1.2f, 0.0f));
        shatter::Pane p2;
        std::vector<std::uint32_t> t2;
        bool found = shatter::paneAt(slab, {}, {}, glm::vec3(0.5f, 0.5f, 0.0f), 0.006f, p2, t2);
        check(found && t2.size() == slab.size() / 3, "a glass slab goes with its edges",
              std::to_string(t2.size()) + " of " + std::to_string(slab.size() / 3));
        check(std::abs(p2.thickness - 0.01f) < 1e-4f, "as thick as the slab", num(p2.thickness));
        std::vector<glm::vec3> block;
        box(block, glm::vec3(0.0f, 0.0f, -0.10f), glm::vec3(1.0f, 1.2f, 0.0f));
        found = shatter::paneAt(block, {}, {}, glm::vec3(0.5f, 0.5f, 0.0f), 0.006f, p2, t2);
        check(found && t2.size() < block.size() / 3, "a 10 cm block is not a pane (its back stays)",
              std::to_string(t2.size()) + " of " + std::to_string(block.size() / 3));
    }

    // --- The cracks -----------------------------------------------------------------
    const glm::vec2 at = pane.flat(hitA);
    const std::vector<shatter::Piece> pieces = shatter::crack(pane, at, 1234u);
    {
        float sum = 0.0f, smallestNear = 1e9f, largestFar = 0.0f;
        bool ccw = true, inside = true;
        for (const auto& piece : pieces) {
            float a = 0.0f;
            glm::vec2 c(0.0f);
            for (const auto& poly : piece) {
                const float pa = polyArea(poly);
                ccw = ccw && pa > 0.0f;
                a += pa;
                for (const glm::vec2& p : poly) {
                    c += p / static_cast<float>(poly.size() * piece.size());
                    inside = inside && pane.contains(pane.world(p), 1e-4f);
                }
            }
            sum += a;
            const float d = glm::length(c - at);
            if (d < 0.05f) smallestNear = std::min(smallestNear, a);
            if (d > 0.15f) largestFar = std::max(largestFar, a);
        }
        check(pieces.size() >= 15 && pieces.size() <= 150, "the pane breaks into many pieces",
              std::to_string(pieces.size()));
        check(std::abs(sum - pane.area()) < pane.area() * 0.005f, "together they are the pane, no gap, no overlap",
              num(sum) + " of " + num(pane.area()));
        check(ccw, "every piece runs counter-clockwise");
        check(inside, "no piece reaches past the outline");
        check(smallestNear * 10.0f < largestFar, "crumbs at the hole, large pieces at the frame",
              num(smallestNear * 1e4f) + " cm2 vs " + num(largestFar * 1e4f) + " cm2");
        const std::vector<shatter::Piece> again = shatter::crack(pane, at, 1234u);
        const std::vector<shatter::Piece> other = shatter::crack(pane, at, 99u);
        check(again.size() == pieces.size() && again[0][0][1] == pieces[0][0][1], "the same seed, the same cracks");
        check(other.size() != pieces.size() || other[0][0][1] != pieces[0][0][1], "another seed, other cracks");
    }

    // --- The shards -----------------------------------------------------------------
    const glm::vec3 dir(0.0f, 0.0f, -1.0f);   // shot from the front, into the room
    std::vector<shatter::Shard> shards = shatter::makeShards(pane, pieces, at, dir, 1.0f, 77u);
    {
        float front = 0.0f, nearSpeed = 0.0f, farSpeed = 0.0f;
        int nNear = 0, nFar = 0, along = 0;
        bool closed = true;
        for (const shatter::Shard& s : shards) {
            for (std::size_t i = 0; i + 2 < s.shape.size(); i += 3) {
                const glm::vec3 c = glm::cross(s.shape[i + 1].position - s.shape[i].position,
                                               s.shape[i + 2].position - s.shape[i].position);
                // Every triangle faces the way its normal says (outward).
                if (glm::length(c) > 1e-9f && glm::dot(c, s.shape[i].normal) <= 0.0f) closed = false;
                if (glm::dot(s.shape[i].normal, pane.n) > 0.99f) front += 0.5f * glm::length(c);
            }
            const float d = glm::length(pane.flat(s.pos) - at);
            const float sp = glm::length(s.vel);
            if (d < 0.05f) { nearSpeed += sp; ++nNear; }
            if (d > 0.20f) { farSpeed += sp; ++nFar; }
            if (glm::dot(s.vel, dir) > 0.0f) ++along;
        }
        check(shards.size() == pieces.size(), "a shard for every piece");
        check(std::abs(front - pane.area()) < pane.area() * 0.005f, "their faces add up to the pane",
              num(front) + " of " + num(pane.area()));
        check(closed, "every face of every shard turned outward");
        check(nNear > 0 && nFar > 0 && nearSpeed / nNear > 2.0f * farSpeed / nFar,
              "near the hole they fly faster than at the frame",
              num(nNear ? nearSpeed / nNear : 0) + " vs " + num(nFar ? farSpeed / nFar : 0) + " m/s");
        check(along * 10 >= static_cast<int>(shards.size()) * 9, "they go on with the shot",
              std::to_string(along) + " of " + std::to_string(shards.size()));
    }
    {
        // Lifted 2 m and let fall onto a floor at y = 0.
        const shatter::FloorFn floor = [](const glm::vec3&, float& y) { y = 0.0f; return true; };
        for (shatter::Shard& s : shards) s.pos.y += 2.0f;
        int asked = 0;
        const shatter::FloorFn counted = [&](const glm::vec3& p, float& y) { ++asked; return floor(p, y); };
        for (int k = 0; k < 120 * 6; ++k)
            for (shatter::Shard& s : shards) shatter::step(s, 1.0f / 120.0f, counted);
        int resting = 0;
        float worstLow = 0.0f, worstFlat = 1.0f;
        for (const shatter::Shard& s : shards) {
            resting += s.resting ? 1 : 0;
            worstLow = std::max(worstLow, std::abs(shatter::lowest(s)));
            worstFlat = std::min(worstFlat, std::abs((s.q * s.face).y));
        }
        check(resting == static_cast<int>(shards.size()), "after six seconds they all lie still",
              std::to_string(resting) + " of " + std::to_string(shards.size()));
        check(worstLow < 0.002f, "on the floor, not in it or above it", num(worstLow));
        check(worstFlat > 0.999f, "lying flat", num(worstFlat));
        check(asked < static_cast<int>(shards.size()) * 40, "the floor asked now and then, not every step",
              std::to_string(asked) + " times for " + std::to_string(shards.size()));
        shatter::Shard s = shards[0];
        s.resting = false; s.settling = false; s.vel = glm::vec3(0.0f); s.floorAt = glm::vec3(1e30f);
        const shatter::FloorFn none = [](const glm::vec3&, float&) { return false; };
        for (int k = 0; k < 120 * 10 && !s.lost; ++k) shatter::step(s, 1.0f / 120.0f, none);
        check(s.lost, "with nothing under it a shard is let go");
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "all ok", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
