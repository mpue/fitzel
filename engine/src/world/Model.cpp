#include "fitzel/world/Model.hpp"

#include <cstdio>
#include <limits>

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <stb_image.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

namespace fitzel {

namespace {

// Decode an embedded image (referenced by a base-colour texture) into RGBA.
void decodeTexture(const cgltf_texture* tex, ModelPrimitive& prim) {
    if (!tex || !tex->image || !tex->image->buffer_view) return;
    const cgltf_buffer_view* bv = tex->image->buffer_view;
    const auto* src = static_cast<const unsigned char*>(bv->buffer->data) + bv->offset;

    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(0); // glTF UVs match GL once uploaded as-is
    unsigned char* px = stbi_load_from_memory(src, static_cast<int>(bv->size),
                                              &w, &h, &ch, 4);
    if (px) {
        prim.texPixels.assign(px, px + static_cast<std::size_t>(w) * h * 4);
        prim.texWidth  = w;
        prim.texHeight = h;
        stbi_image_free(px);
    }
}

} // namespace

ModelData loadGltf(const std::string& path) {
    ModelData out;

    cgltf_options options{};
    cgltf_data*   data = nullptr;
    if (cgltf_parse_file(&options, path.c_str(), &data) != cgltf_result_success) {
        std::fprintf(stderr, "[Fitzel] failed to parse glTF '%s'\n", path.c_str());
        return out;
    }
    if (cgltf_load_buffers(&options, data, path.c_str()) != cgltf_result_success) {
        std::fprintf(stderr, "[Fitzel] failed to load glTF buffers '%s'\n", path.c_str());
        cgltf_free(data);
        return out;
    }

    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();

    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;

        float wm[16];
        cgltf_node_transform_world(&node, wm);
        const glm::mat4 model   = glm::make_mat4(wm);
        const glm::mat3 normalM = glm::mat3(glm::transpose(glm::inverse(model)));

        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nrm = nullptr;
            const cgltf_accessor* uv  = nullptr;
            for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
                switch (prim.attributes[ai].type) {
                    case cgltf_attribute_type_position: pos = prim.attributes[ai].data; break;
                    case cgltf_attribute_type_normal:   nrm = prim.attributes[ai].data; break;
                    case cgltf_attribute_type_texcoord: if (!uv) uv = prim.attributes[ai].data; break;
                    default: break;
                }
            }
            if (!pos) continue;

            ModelPrimitive mp;
            if (prim.material) {
                if (prim.material->name) mp.materialName = prim.material->name;
                mp.alphaCutout = (prim.material->alpha_mode != cgltf_alpha_mode_opaque);
                if (prim.material->has_pbr_metallic_roughness) {
                    const auto& pbr = prim.material->pbr_metallic_roughness;
                    for (int c = 0; c < 4; ++c) mp.baseColor[c] = pbr.base_color_factor[c];
                    decodeTexture(pbr.base_color_texture.texture, mp);
                }
            }

            const cgltf_size count = prim.indices ? prim.indices->count : pos->count;
            mp.vertices.reserve(count * 8);
            for (cgltf_size i = 0; i < count; ++i) {
                const cgltf_size idx =
                    prim.indices ? cgltf_accessor_read_index(prim.indices, i) : i;

                float p[3] = {0, 0, 0}, n[3] = {0, 1, 0}, t[2] = {0, 0};
                cgltf_accessor_read_float(pos, idx, p, 3);
                if (nrm) cgltf_accessor_read_float(nrm, idx, n, 3);
                if (uv)  cgltf_accessor_read_float(uv,  idx, t, 2);

                const glm::vec3 wp = glm::vec3(model * glm::vec4(p[0], p[1], p[2], 1.0f));
                const glm::vec3 wn = glm::normalize(normalM * glm::vec3(n[0], n[1], n[2]));

                mp.vertices.insert(mp.vertices.end(),
                    {wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, t[0], t[1]});
                lo = glm::min(lo, wp.y);
                hi = glm::max(hi, wp.y);
            }
            out.primitives.push_back(std::move(mp));
        }
    }

    cgltf_free(data);
    if (!out.primitives.empty()) { out.minY = lo; out.maxY = hi; }
    return out;
}

namespace {

glm::mat4 aiToGlm(const aiMatrix4x4& m) {
    // aiMatrix4x4 is row-major; glm::mat4 columns are the transposed rows.
    return glm::mat4(m.a1, m.b1, m.c1, m.d1,
                     m.a2, m.b2, m.c2, m.d2,
                     m.a3, m.b3, m.c3, m.d3,
                     m.a4, m.b4, m.c4, m.d4);
}

// Decode an assimp embedded texture (compressed like PNG/JPG, or raw BGRA) to RGBA.
void decodeAiTexture(const aiTexture* tex, ModelPrimitive& mp) {
    if (!tex) return;
    if (tex->mHeight == 0) {                 // compressed blob of mWidth bytes
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* px = stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(tex->pcData),
            static_cast<int>(tex->mWidth), &w, &h, &ch, 4);
        if (px) {
            mp.texPixels.assign(px, px + static_cast<std::size_t>(w) * h * 4);
            mp.texWidth = w; mp.texHeight = h;
            stbi_image_free(px);
        }
    } else {                                 // raw aiTexel grid (B,G,R,A)
        mp.texWidth  = static_cast<int>(tex->mWidth);
        mp.texHeight = static_cast<int>(tex->mHeight);
        const std::size_t n = static_cast<std::size_t>(tex->mWidth) * tex->mHeight;
        mp.texPixels.resize(n * 4);
        for (std::size_t i = 0; i < n; ++i) {
            mp.texPixels[i * 4 + 0] = tex->pcData[i].r;
            mp.texPixels[i * 4 + 1] = tex->pcData[i].g;
            mp.texPixels[i * 4 + 2] = tex->pcData[i].b;
            mp.texPixels[i * 4 + 3] = tex->pcData[i].a;
        }
    }
}

// Walk the node tree, baking each node's world transform into its meshes so the
// output matches loadGltf (de-indexed 8-float verts, one primitive per mesh).
void collectColladaNode(const aiScene* scene, const aiNode* node,
                        const glm::mat4& parent, ModelData& out,
                        float& lo, float& hi) {
    const glm::mat4 world   = parent * aiToGlm(node->mTransformation);
    const glm::mat3 normalM = glm::mat3(glm::transpose(glm::inverse(world)));

    for (unsigned mi = 0; mi < node->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[node->mMeshes[mi]];
        if (!mesh || mesh->mNumFaces == 0) continue;

        ModelPrimitive mp;
        if (mesh->mMaterialIndex < scene->mNumMaterials) {
            const aiMaterial* mat = scene->mMaterials[mesh->mMaterialIndex];
            aiString nm;
            if (mat->Get(AI_MATKEY_NAME, nm) == AI_SUCCESS) mp.materialName = nm.C_Str();
            aiColor4D col;
            if (mat->Get(AI_MATKEY_COLOR_DIFFUSE, col) == AI_SUCCESS) {
                mp.baseColor[0] = col.r; mp.baseColor[1] = col.g;
                mp.baseColor[2] = col.b; mp.baseColor[3] = col.a;
            }
            float opacity = 1.0f;
            if (mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS && opacity < 0.999f)
                mp.alphaCutout = true;
            aiString texPath; // embedded textures only (external files: later phase)
            if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS &&
                texPath.length > 1 && texPath.data[0] == '*') {
                const int ti = std::atoi(texPath.C_Str() + 1);
                if (ti >= 0 && ti < static_cast<int>(scene->mNumTextures))
                    decodeAiTexture(scene->mTextures[ti], mp);
            }
        }

        const bool hasN  = mesh->HasNormals();
        const bool hasUV = mesh->HasTextureCoords(0);
        mp.vertices.reserve(static_cast<std::size_t>(mesh->mNumFaces) * 3 * 8);
        for (unsigned f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            if (face.mNumIndices != 3) continue; // triangulated on import
            for (int k = 0; k < 3; ++k) {
                const unsigned idx = face.mIndices[k];
                const aiVector3D& P = mesh->mVertices[idx];
                const glm::vec3 wp = glm::vec3(world * glm::vec4(P.x, P.y, P.z, 1.0f));
                glm::vec3 wn(0.0f, 1.0f, 0.0f);
                if (hasN) {
                    const aiVector3D& N = mesh->mNormals[idx];
                    wn = glm::normalize(normalM * glm::vec3(N.x, N.y, N.z));
                }
                float u = 0.0f, v = 0.0f;
                if (hasUV) {
                    const aiVector3D& T = mesh->mTextureCoords[0][idx];
                    u = T.x; v = T.y;
                }
                mp.vertices.insert(mp.vertices.end(),
                    {wp.x, wp.y, wp.z, wn.x, wn.y, wn.z, u, v});
                lo = glm::min(lo, wp.y);
                hi = glm::max(hi, wp.y);
            }
        }
        if (mp.vertexCount() > 0) out.primitives.push_back(std::move(mp));
    }

    for (unsigned c = 0; c < node->mNumChildren; ++c)
        collectColladaNode(scene, node->mChildren[c], world, out, lo, hi);
}

} // namespace

ModelData loadCollada(const std::string& path) {
    ModelData out;
    Assimp::Importer imp;
    const aiScene* scene = imp.ReadFile(
        path, aiProcess_Triangulate | aiProcess_GenSmoothNormals |
              aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality);
    if (!scene || !scene->mRootNode ||
        (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        std::fprintf(stderr, "[Fitzel] failed to load Collada '%s': %s\n",
                     path.c_str(), imp.GetErrorString());
        return out;
    }

    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    collectColladaNode(scene, scene->mRootNode, glm::mat4(1.0f), out, lo, hi);
    if (!out.primitives.empty()) { out.minY = lo; out.maxY = hi; }
    return out;
}

} // namespace fitzel
