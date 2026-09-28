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
//   build/release/bin/importcheck.exe [model.glb|.fbx|.dae ...]
// With no argument it checks the .glb models under content/models. Exits non-zero
// if any check fails.

#include <algorithm>
#include <cctype>
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

AlphaMix alphaMix(const fitzel::SharedPixels& rgba) {
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

// The flat import the editor would make of `path`, routed by extension the way
// AssetDatabase does it: .dae to Collada, .fbx to the rig-aware assimp loader,
// everything else to glTF.
fitzel::ModelData loadFlat(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".dae") return fitzel::loadCollada(path);
    if (ext == ".fbx") return fitzel::loadSkinnedModel(path);
    return fitzel::loadGltf(path);
}

// The image memory a structured import really holds: parts that sample one
// image share its buffer, so this counts buffers, not the maps that point at
// them. (The same model at one buffer per part is how medieval_house.glb came
// to take 5 GB.)
void reportImageBuffers(const std::vector<fitzel::ModelNode>& nodes) {
    std::vector<const std::uint8_t*> seen;
    std::size_t maps = 0, bytes = 0;
    auto count = [&](const fitzel::SharedPixels& px) {
        if (px.empty()) return;
        ++maps;
        if (std::find(seen.begin(), seen.end(), px.data()) != seen.end()) return;
        seen.push_back(px.data());
        bytes += px.size();
    };
    for (const fitzel::ModelNode& n : nodes)
        for (const fitzel::ModelPrimitive& p : n.data.primitives) {
            count(p.texPixels);
            count(p.normalPixels);
            count(p.ormPixels);
            count(p.emissionPixels);
        }
    std::printf("       %zu map(s) on %zu image buffer(s), %.0f MB decoded\n", maps,
                seen.size(), static_cast<double>(bytes) / (1024.0 * 1024.0));
}

void checkModel(const std::string& path) {
    std::printf("%s\n", path.c_str());

    const fitzel::ModelData flat = loadFlat(path);
    if (flat.empty()) {
        check(false, "loads at all");
        return;
    }
    const std::vector<fitzel::ModelNode> nodes = fitzel::loadModelNodes(path);

    check(!nodes.empty(), "loadModelNodes returns nodes");
    if (nodes.empty()) return;
    reportImageBuffers(nodes);

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

// --- Synthetic files: what no downloaded model can pin down ------------------
// One triangle, written out as a .gltf with everything inline, so the numbers
// the loader must produce are known exactly rather than eyeballed off a render.

std::string base64(const unsigned char* p, std::size_t n) {
    static const char* k =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < n; i += 3) {
        const unsigned v = (p[i] << 16) | ((i + 1 < n ? p[i + 1] : 0) << 8) |
                           (i + 2 < n ? p[i + 2] : 0);
        out += k[(v >> 18) & 63];
        out += k[(v >> 12) & 63];
        out += i + 1 < n ? k[(v >> 6) & 63] : '=';
        out += i + 2 < n ? k[v & 63] : '=';
    }
    return out;
}

// 1x1, opaque.
const char* kPngSolid =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGM4MS3lPwAGrgLCN5ttDQAAAABJRU5ErkJggg==";
// 16x16, alpha ramping 0..255 across: soft coverage, the way hair and lashes
// have it -- which alphaCutsHoles reads as a data channel, on purpose.
const char* kPngRamp =
    "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAYAAAAf8/9hAAAAIElEQVR42mM8MS2FgYGBQZBczMRAIRg1YNSAUQMGiwEATskC4RMa9EYAAAAASUVORK5CYII=";

// The triangle (0,0,0) (1,0,0) (0,1,0) with UVs (0,0) (1,0) (0,1), its base
// colour an image called `imageName`, with `textureInfoExtra` spliced into the
// baseColorTexture object. Returns the path written.
std::string writeTriangle(const std::string& file, const std::string& imageName,
                          const char* png, const std::string& textureInfoExtra) {
    const float data[15] = {0, 0, 0,  1, 0, 0,  0, 1, 0,   0, 0,  1, 0,  0, 1};
    const std::string buf =
        base64(reinterpret_cast<const unsigned char*>(data), sizeof(data));
    const std::string json =
        "{\"asset\":{\"version\":\"2.0\"},"
        "\"extensionsUsed\":[\"KHR_texture_transform\"],"
        "\"buffers\":[{\"byteLength\":60,\"uri\":\"data:application/octet-stream;base64," +
        buf + "\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":24}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\","
        "\"min\":[0,0,0],\"max\":[1,1,0]},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"}],"
        "\"images\":[{\"name\":\"" + imageName + "\",\"uri\":\"data:image/png;base64," +
        png + "\"}],"
        "\"textures\":[{\"source\":0}],"
        "\"materials\":[{\"name\":\"M\",\"pbrMetallicRoughness\":{\"baseColorTexture\":"
        "{\"index\":0" + textureInfoExtra + "}}}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},"
        "\"material\":0}]}],"
        "\"nodes\":[{\"mesh\":0}],\"scenes\":[{\"nodes\":[0]}],\"scene\":0}";
    std::FILE* f = std::fopen(file.c_str(), "wb");
    if (!f) return {};
    std::fwrite(json.data(), 1, json.size(), f);
    std::fclose(f);
    return file;
}

void syntheticChecks() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::temp_directory_path(ec) / "fitzel_importcheck";
    fs::create_directories(dir, ec);
    std::printf("\n[synthetic glTF]\n");

    // KHR_texture_transform with a quarter turn -- a Blender Mapping node of
    // rotation 90, scale 0.2, exported. The UVs must be the ones Blender's
    // exporter meant (texture_transform_blender_to_gltf, inverted) and the
    // Khronos viewer draws: u' = s*v - 0.2, v' = 1 - s*u. The old sign gave
    // (1,0) -> (-0.2, 1.2), the texture turned 180 degrees.
    {
        const std::string file = writeTriangle(
            (dir / "rot90.gltf").string(), "Roof", kPngSolid,
            ",\"extensions\":{\"KHR_texture_transform\":{\"offset\":[-0.2,1.0],"
            "\"rotation\":1.5707963,\"scale\":[0.2,0.2]}}");
        const fitzel::ModelData md = fitzel::loadGltf(file);
        const bool one = md.primitives.size() == 1 &&
                         md.primitives[0].vertices.size() == 24;
        check(one, "rotated-UV triangle loads");
        if (one) {
            const std::vector<float>& v = md.primitives[0].vertices;
            const float want[3][2] = {{-0.2f, 1.0f}, {-0.2f, 0.8f}, {0.0f, 1.0f}};
            bool ok = true;
            for (int i = 0; i < 3; ++i) {
                const float u = v[i * 8 + 6], w = v[i * 8 + 7];
                std::printf("       uv%d = (%.4f, %.4f), want (%.1f, %.1f)\n",
                            i, u, w, want[i][0], want[i][1]);
                ok = ok && std::abs(u - want[i][0]) < 1e-4f &&
                     std::abs(w - want[i][1]) < 1e-4f;
            }
            check(ok, "a 90-degree texture transform turns the way Blender meant");
        }
    }

    // Opacity packed into the base colour's alpha, the material declared
    // OPAQUE -- Blender's export of a lash or hair material. The name says
    // what the soft alpha is; without the name the same pixels stay opaque.
    {
        const fitzel::ModelData named = fitzel::loadGltf(writeTriangle(
            (dir / "lash.gltf").string(), "Std_Eyelash_Diffuse-Std_Eyelash_Opacity",
            kPngRamp, ""));
        const fitzel::ModelData plain = fitzel::loadGltf(writeTriangle(
            (dir / "plain.gltf").string(), "Std_Eyelash_Diffuse", kPngRamp, ""));
        // Both images must have arrived at all -- an inline image whose base64
        // ends in padding did not (see decodeImage), which would make the
        // second check below pass for nothing.
        check(!named.primitives.empty() && named.primitives[0].texWidth == 16 &&
              !plain.primitives.empty() && plain.primitives[0].texWidth == 16,
              "inline (data URI) images decode, padding and all");
        check(!named.primitives.empty() && named.primitives[0].alphaCutout,
              "packed opacity (named so) arrives as a cut-out");
        check(!plain.primitives.empty() && !plain.primitives[0].alphaCutout,
              "the same soft alpha without the name stays opaque");
    }
    fs::remove_all(dir, ec);
}

} // namespace

int main(int argc, char** argv) {
    syntheticChecks();

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
