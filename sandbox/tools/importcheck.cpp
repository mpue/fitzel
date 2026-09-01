// The model-import check: does a model that was authored as several objects
// arrive as several objects, and does each of them keep its own texture?
//
// This is the failure that has no symptom. A glTF whose structure gets dropped
// still imports, still draws, still looks right from the outside -- it is simply
// one entity where there should have been twenty, and you only find out when you
// try to select a wheel and get the whole car. That is exactly what happened
// here: the editor routes .glb through loadModelNodes, which went to assimp,
// which is built without its glTF importer. ReadFile failed, the node list came
// back empty, "more than one node?" answered no, and every GLB quietly took the
// single-entity path. Nothing logged, nothing crashed.
//
// So this asserts the two things a screenshot cannot show: that a multi-object
// file yields more than one node, and that the pixels that live inside a GLB
// still reach the node that uses them. It also checks that the parts are put
// back where they belong -- each node is recentred on its own bounding box, so
// its recorded centre plus its local vertices must reproduce the model-space
// position that the flat loadGltf import puts them at.
//
// Console program, like modelcheck and shadercheck, and for the same reason:
// the editor is /SUBSYSTEM:WINDOWS in Release and has nowhere to print to.
//   build/release/bin/importcheck.exe [model.glb ...]
// With no argument it checks the models under content/models. Exits non-zero if
// any check fails.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <fitzel/world/Model.hpp>

namespace {

int failures = 0;
int checks   = 0;

void check(bool ok, const char* what) {
    ++checks;
    if (!ok) ++failures;
    std::printf("  %-4s %s\n", ok ? "ok" : "FAIL", what);
}

// The model-space AABB of a flat import (vertices already baked).
void flatBounds(const fitzel::ModelData& md, glm::vec3& lo, glm::vec3& hi) {
    lo = glm::vec3(1e30f);
    hi = glm::vec3(-1e30f);
    for (const fitzel::ModelPrimitive& p : md.primitives)
        for (std::size_t i = 0; i + 7 < p.vertices.size(); i += 8) {
            const glm::vec3 v(p.vertices[i], p.vertices[i + 1], p.vertices[i + 2]);
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
}

// The same AABB rebuilt from the structured import: every node's local vertices
// shifted back out by the centre it reported.
void nodeBounds(const std::vector<fitzel::ModelNode>& ns, glm::vec3& lo, glm::vec3& hi) {
    lo = glm::vec3(1e30f);
    hi = glm::vec3(-1e30f);
    for (const fitzel::ModelNode& n : ns)
        for (const fitzel::ModelPrimitive& p : n.data.primitives)
            for (std::size_t i = 0; i + 7 < p.vertices.size(); i += 8) {
                const glm::vec3 v(p.vertices[i], p.vertices[i + 1], p.vertices[i + 2]);
                lo = glm::min(lo, v + n.center);
                hi = glm::max(hi, v + n.center);
            }
}

int vertexTotal(const fitzel::ModelData& md) {
    int n = 0;
    for (const fitzel::ModelPrimitive& p : md.primitives) n += p.vertexCount();
    return n;
}

// The same three-way alpha count the loader decides cut-out by (see
// alphaCutsHoles in Model.cpp), re-measured here from the pixels that came back.
// Deliberately a second implementation: what is being checked is not the formula
// but that its verdict REACHES the primitive, on every loader path and for the
// models actually shipped.
struct AlphaMix { float holes = 0.0f, mid = 0.0f; };

AlphaMix alphaMix(const std::vector<std::uint8_t>& rgba) {
    AlphaMix m;
    const std::size_t texels = rgba.size() / 4;
    if (texels < 64) return m;
    std::size_t seen = 0, holes = 0, mid = 0;
    for (std::size_t i = 0; i < texels; i += 7) {
        const std::uint8_t a = rgba[i * 4 + 3];
        ++seen;
        if      (a < 16)  ++holes;
        else if (a < 240) ++mid;
    }
    if (!seen) return m;
    m.holes = static_cast<float>(holes) / static_cast<float>(seen);
    m.mid   = static_cast<float>(mid)   / static_cast<float>(seen);
    return m;
}

void checkModel(const std::string& path) {
    std::printf("%s\n", path.c_str());

    const fitzel::ModelData flat = fitzel::loadGltf(path);
    if (flat.empty()) {
        check(false, "loads at all (loadGltf)");
        return;
    }
    const std::vector<fitzel::ModelNode> nodes = fitzel::loadModelNodes(path);

    check(!nodes.empty(), "loadModelNodes returns nodes for a .glb");
    if (nodes.empty()) return;

    // Nothing may be lost on the way: the structured import must carry the same
    // vertices as the flat one, just grouped. (A model authored as one object
    // legitimately yields one node -- that is not the failure being hunted.)
    int structuredVerts = 0;
    int textured        = 0;
    for (const fitzel::ModelNode& n : nodes) {
        structuredVerts += vertexTotal(n.data);
        for (const fitzel::ModelPrimitive& p : n.data.primitives)
            if (!p.texPixels.empty()) { ++textured; break; }
    }
    std::printf("       %d node(s), %d of them textured, %d vertices (flat: %d)\n",
                static_cast<int>(nodes.size()), textured, structuredVerts,
                vertexTotal(flat));
    check(structuredVerts == vertexTotal(flat), "no geometry lost vs. the flat import");

    // Embedded GLB textures must survive: this is what assimp would have dropped.
    int flatTextured = 0;
    for (const fitzel::ModelPrimitive& p : flat.primitives)
        if (!p.texPixels.empty()) ++flatTextured;
    if (flatTextured > 0)
        check(textured > 0, "embedded textures reach the structured nodes");

    // Parts must go back where they came from.
    glm::vec3 flo, fhi, nlo, nhi;
    flatBounds(flat, flo, fhi);
    nodeBounds(nodes, nlo, nhi);
    const float span = std::max(1e-4f, glm::length(fhi - flo));
    const bool  placed = glm::length(nlo - flo) < span * 1e-3f &&
                         glm::length(nhi - fhi) < span * 1e-3f;
    check(placed, "node centre + local vertices reproduce the model-space bounds");

    // Every node must be usable on its own: a name to show in the outliner and
    // a non-degenerate height for the placement code.
    bool named = true;
    for (const fitzel::ModelNode& n : nodes) if (n.name.empty()) named = false;
    check(named, "every node carries a name");

    // Foliage must arrive AS foliage. A leaf card whose mask never becomes an
    // alphaCutout flag is drawn as a solid rectangle of background -- by the
    // vegetation system, by the entity renderer and in the shadow pass alike --
    // and the model file is no help: exporters ship leaf atlases under materials
    // marked OPAQUE all the time (tree2.glb here does). So: any primitive whose
    // base-colour map is mostly holes and hardly any midtones has to come back
    // flagged, whatever its material said.
    bool maskedFlagged = true;
    for (const fitzel::ModelPrimitive& p : flat.primitives) {
        if (p.texPixels.empty()) continue;
        const AlphaMix m = alphaMix(p.texPixels);
        const bool mask  = m.holes > 0.10f && m.holes < 0.99f && m.mid < 0.15f;
        if (!mask) continue;
        std::printf("       %-28s %.0f%% holes, %.0f%% midtones -> cutout %s\n",
                    p.materialName.empty() ? "(unnamed material)"
                                           : p.materialName.c_str(),
                    m.holes * 100.0f, m.mid * 100.0f, p.alphaCutout ? "yes" : "NO");
        if (!p.alphaCutout) maskedFlagged = false;
    }
    check(maskedFlagged, "a cut-out texture arrives flagged as cut-out");
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> models;
    for (int i = 1; i < argc; ++i) models.emplace_back(argv[i]);

    if (models.empty()) {
        std::error_code ec;
        for (const auto& e :
             std::filesystem::directory_iterator("content/models", ec)) {
            if (!e.is_regular_file()) continue;
            std::string ext = e.path().extension().string();
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".glb") models.push_back(e.path().generic_string());
        }
        std::sort(models.begin(), models.end());
        if (models.empty()) {
            std::printf("importcheck: no .glb found under content/models "
                        "(run from the repo root, or pass files)\n");
            return 1;
        }
    }

    for (const std::string& m : models) checkModel(m);

    std::printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
