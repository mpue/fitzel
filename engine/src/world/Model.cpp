#include "fitzel/world/Model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <limits>
#include <unordered_map>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/matrix_decompose.hpp>

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

// Store decoded RGBA pixels into an output buffer + dimensions.
void storePixels(unsigned char* px, int w, int h,
                 std::vector<std::uint8_t>& outPix, int& outW, int& outH) {
    if (!px) return;
    outPix.assign(px, px + static_cast<std::size_t>(w) * h * 4);
    outW = w; outH = h;
    stbi_image_free(px);
}

// Name an encoded image's container from its magic bytes, so an undecodable
// embedded texture reports WHAT it is (stb can't read WebP/KTX2/etc.).
const char* imageFormat(const unsigned char* p, std::size_t n) {
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return "PNG";
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF)             return "JPEG";
    if (n >= 12 && !std::memcmp(p, "RIFF", 4) && !std::memcmp(p + 8, "WEBP", 4)) return "WebP";
    if (n >= 12 && !std::memcmp(p + 4, "ftypavif", 8))                     return "AVIF";
    if (n >= 4 && p[0] == 0xAB && p[1] == 'K' && p[2] == 'T' && p[3] == 'X') return "KTX";
    if (n >= 4 && !std::memcmp(p, "GIF8", 4))                              return "GIF";
    if (n >= 2 && p[0] == 'B' && p[1] == 'M')                             return "BMP";
    return "unknown";
}

// Decode a glTF base-colour/diffuse image into RGBA, from wherever it lives:
// an embedded buffer view (the GLB case), an inline base64 data URI, or an
// external file relative to the model. Missing any of these left textures
// undecoded before -- so a model mixing storage/workflows lost some textures.
void decodeImage(const cgltf_image* img, const std::string& baseDir,
                 std::vector<std::uint8_t>& outPix, int& outW, int& outH,
                 const std::string& matName) {
    if (!img) return;
    stbi_set_flip_vertically_on_load(0); // glTF UVs match GL once uploaded as-is
    int w = 0, h = 0, ch = 0;

    if (img->buffer_view) {                       // embedded (typical GLB)
        const cgltf_buffer_view* bv = img->buffer_view;
        const auto* src = static_cast<const unsigned char*>(bv->buffer->data) + bv->offset;
        unsigned char* px = stbi_load_from_memory(src, static_cast<int>(bv->size),
                                                  &w, &h, &ch, 4);
        if (!px)
            std::fprintf(stderr,
                "[Fitzel] glTF material '%s': embedded texture is %s, which "
                "stb_image can't decode -> shows flat colour\n",
                matName.empty() ? "?" : matName.c_str(),
                imageFormat(src, bv->size));
        storePixels(px, w, h, outPix, outW, outH);
        return;
    }
    if (!img->uri) return;

    if (std::strncmp(img->uri, "data:", 5) == 0) { // inline base64 data URI
        const char* comma = std::strchr(img->uri, ',');
        if (!comma) return;
        const char* b64 = comma + 1;
        const cgltf_size outSize = (std::strlen(b64) / 4) * 3;
        void* decoded = nullptr;
        cgltf_options opts{};
        if (cgltf_load_buffer_base64(&opts, outSize, b64, &decoded) ==
                cgltf_result_success && decoded) {
            storePixels(stbi_load_from_memory(static_cast<unsigned char*>(decoded),
                                              static_cast<int>(outSize), &w, &h, &ch, 4),
                        w, h, outPix, outW, outH);
            std::free(decoded);
        }
        return;
    }

    // External file, relative to the model (percent-decoded).
    std::string uri = img->uri;
    uri.push_back('\0');
    cgltf_decode_uri(&uri[0]);
    const std::string file = (std::filesystem::path(baseDir) / uri.c_str())
                                 .generic_string();
    storePixels(stbi_load(file.c_str(), &w, &h, &ch, 4), w, h, outPix, outW, outH);
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
    // Directory the model lives in, for resolving external (non-embedded) images.
    const std::string baseDir =
        std::filesystem::path(path).parent_path().generic_string();

    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();

    // --- Skeleton + animation clips (skinned models; assumes a single skin) ---
    std::unordered_map<const cgltf_node*, int> jointIndex;
    const cgltf_skin* skin = data->skins_count > 0 ? &data->skins[0] : nullptr;
    if (skin) {
        out.skeleton.resize(skin->joints_count);
        for (cgltf_size j = 0; j < skin->joints_count; ++j)
            jointIndex[skin->joints[j]] = static_cast<int>(j);
        for (cgltf_size j = 0; j < skin->joints_count; ++j) {
            const cgltf_node* jn = skin->joints[j];
            SkeletonJoint& sj = out.skeleton[j];
            auto pit = jn->parent ? jointIndex.find(jn->parent) : jointIndex.end();
            if (pit != jointIndex.end()) {
                sj.parent = pit->second;
            } else if (jn->parent) { // root joint under a non-joint (e.g. armature)
                float w[16]; cgltf_node_transform_world(jn->parent, w);
                sj.baseParent = glm::make_mat4(w);
            }
            if (skin->inverse_bind_matrices) {
                float m[16];
                cgltf_accessor_read_float(skin->inverse_bind_matrices, j, m, 16);
                sj.inverseBind = glm::make_mat4(m);
            }
            if (jn->has_matrix) {
                glm::vec3 skew; glm::vec4 persp;
                glm::decompose(glm::make_mat4(jn->matrix), sj.restS, sj.restR,
                               sj.restT, skew, persp);
            } else {
                if (jn->has_translation)
                    sj.restT = glm::vec3(jn->translation[0], jn->translation[1], jn->translation[2]);
                if (jn->has_rotation)
                    sj.restR = glm::quat(jn->rotation[3], jn->rotation[0],
                                         jn->rotation[1], jn->rotation[2]);
                if (jn->has_scale)
                    sj.restS = glm::vec3(jn->scale[0], jn->scale[1], jn->scale[2]);
            }
        }
        out.animations.resize(data->animations_count);
        for (cgltf_size a = 0; a < data->animations_count; ++a) {
            const cgltf_animation& anim = data->animations[a];
            AnimationClip& clip = out.animations[a];
            clip.name = anim.name ? anim.name : ("clip " + std::to_string(a));
            clip.tracks.resize(out.skeleton.size());
            for (cgltf_size c = 0; c < anim.channels_count; ++c) {
                const cgltf_animation_channel& ch = anim.channels[c];
                auto jit = jointIndex.find(ch.target_node);
                if (jit == jointIndex.end() || !ch.sampler) continue;
                JointTrack& tr = clip.tracks[jit->second];
                const cgltf_animation_sampler* s = ch.sampler;
                const cgltf_size n = s->input->count;
                std::vector<float> times(n);
                for (cgltf_size k = 0; k < n; ++k) {
                    float tt = 0.0f; cgltf_accessor_read_float(s->input, k, &tt, 1);
                    times[k] = tt; clip.duration = std::max(clip.duration, tt);
                }
                if (ch.target_path == cgltf_animation_path_type_translation) {
                    tr.tTimes = times; tr.tVals.resize(n);
                    for (cgltf_size k = 0; k < n; ++k) {
                        float v[3]; cgltf_accessor_read_float(s->output, k, v, 3);
                        tr.tVals[k] = glm::vec3(v[0], v[1], v[2]);
                    }
                } else if (ch.target_path == cgltf_animation_path_type_rotation) {
                    tr.rTimes = times; tr.rVals.resize(n);
                    for (cgltf_size k = 0; k < n; ++k) {
                        float v[4]; cgltf_accessor_read_float(s->output, k, v, 4);
                        tr.rVals[k] = glm::quat(v[3], v[0], v[1], v[2]);
                    }
                } else if (ch.target_path == cgltf_animation_path_type_scale) {
                    tr.sTimes = times; tr.sVals.resize(n);
                    for (cgltf_size k = 0; k < n; ++k) {
                        float v[3]; cgltf_accessor_read_float(s->output, k, v, 3);
                        tr.sVals[k] = glm::vec3(v[0], v[1], v[2]);
                    }
                }
            }
        }
    }

    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& node = data->nodes[ni];
        if (!node.mesh) continue;

        const bool skinnedNode = node.skin != nullptr;
        float wm[16];
        cgltf_node_transform_world(&node, wm);
        // Skinned meshes ignore the node transform (joints place them); static
        // meshes bake the node world transform into their vertices.
        const glm::mat4 model   = skinnedNode ? glm::mat4(1.0f) : glm::make_mat4(wm);
        const glm::mat3 normalM = glm::mat3(glm::transpose(glm::inverse(model)));
        const cgltf_skin* nskin = node.skin;

        for (cgltf_size pi = 0; pi < node.mesh->primitives_count; ++pi) {
            const cgltf_primitive& prim = node.mesh->primitives[pi];
            if (prim.type != cgltf_primitive_type_triangles) continue;

            const cgltf_accessor* pos = nullptr;
            const cgltf_accessor* nrm = nullptr;
            const cgltf_accessor* uv  = nullptr;
            const cgltf_accessor* jnt = nullptr;
            const cgltf_accessor* wgt = nullptr;
            for (cgltf_size ai = 0; ai < prim.attributes_count; ++ai) {
                switch (prim.attributes[ai].type) {
                    case cgltf_attribute_type_position: pos = prim.attributes[ai].data; break;
                    case cgltf_attribute_type_normal:   nrm = prim.attributes[ai].data; break;
                    case cgltf_attribute_type_texcoord: if (!uv)  uv  = prim.attributes[ai].data; break;
                    case cgltf_attribute_type_joints:   if (!jnt) jnt = prim.attributes[ai].data; break;
                    case cgltf_attribute_type_weights:  if (!wgt) wgt = prim.attributes[ai].data; break;
                    default: break;
                }
            }
            if (!pos) continue;
            const bool primSkinned = skinnedNode && jnt && wgt && nskin && !out.skeleton.empty();

            ModelPrimitive mp;
            if (prim.material) {
                const cgltf_material* mat = prim.material;
                if (mat->name) mp.materialName = mat->name;
                mp.alphaCutout = (mat->alpha_mode != cgltf_alpha_mode_opaque);
                // Support both PBR workflows: metallic-roughness base colour and
                // the KHR spec-gloss diffuse (older exporters). Whichever provides
                // the colour texture wins -- a model mixing them keeps all its maps.
                const cgltf_texture* colorTex = nullptr;
                if (mat->has_pbr_metallic_roughness) {
                    const auto& pbr = mat->pbr_metallic_roughness;
                    for (int c = 0; c < 4; ++c) mp.baseColor[c] = pbr.base_color_factor[c];
                    colorTex = pbr.base_color_texture.texture;
                }
                if (!colorTex && mat->has_pbr_specular_glossiness) {
                    const auto& sg = mat->pbr_specular_glossiness;
                    for (int c = 0; c < 4; ++c) mp.baseColor[c] = sg.diffuse_factor[c];
                    colorTex = sg.diffuse_texture.texture;
                }
                if (colorTex)
                    decodeImage(colorTex->image, baseDir,
                                mp.texPixels, mp.texWidth, mp.texHeight, mp.materialName);
                // Tangent-space normal map (KHR standard normal_texture).
                if (mat->normal_texture.texture)
                    decodeImage(mat->normal_texture.texture->image, baseDir,
                                mp.normalPixels, mp.normalWidth, mp.normalHeight,
                                mp.materialName);
            }

            const cgltf_size count = prim.indices ? prim.indices->count : pos->count;
            mp.vertices.reserve(count * 8);
            if (primSkinned) mp.skin.reserve(count);
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

                if (primSkinned) {
                    cgltf_uint ji[4] = {0, 0, 0, 0};
                    float      jw[4] = {0, 0, 0, 0};
                    cgltf_accessor_read_uint(jnt, idx, ji, 4);
                    cgltf_accessor_read_float(wgt, idx, jw, 4);
                    VertexSkin vs;
                    for (int k = 0; k < 4; ++k) {
                        int mapped = 0;
                        const int local = static_cast<int>(ji[k]);
                        if (local >= 0 && local < static_cast<int>(nskin->joints_count)) {
                            auto mit = jointIndex.find(nskin->joints[local]);
                            if (mit != jointIndex.end()) mapped = mit->second;
                        }
                        vs.joints[k]  = mapped;
                        vs.weights[k] = jw[k];
                    }
                    mp.skin.push_back(vs);
                }
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

// Decode an assimp EMBEDDED texture (compressed like PNG/JPG, or raw BGRA) into
// an RGBA buffer + dimensions.
void decodeAiTexture(const aiTexture* tex, std::vector<std::uint8_t>& outPix,
                     int& outW, int& outH) {
    if (!tex) return;
    if (tex->mHeight == 0) {                 // compressed blob of mWidth bytes
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);
        storePixels(stbi_load_from_memory(
                        reinterpret_cast<const unsigned char*>(tex->pcData),
                        static_cast<int>(tex->mWidth), &w, &h, &ch, 4),
                    w, h, outPix, outW, outH);
    } else {                                 // raw aiTexel grid (B,G,R,A)
        outW = static_cast<int>(tex->mWidth);
        outH = static_cast<int>(tex->mHeight);
        const std::size_t n = static_cast<std::size_t>(tex->mWidth) * tex->mHeight;
        outPix.resize(n * 4);
        for (std::size_t i = 0; i < n; ++i) {
            outPix[i * 4 + 0] = tex->pcData[i].r;
            outPix[i * 4 + 1] = tex->pcData[i].g;
            outPix[i * 4 + 2] = tex->pcData[i].b;
            outPix[i * 4 + 3] = tex->pcData[i].a;
        }
    }
}

// Resolve an external texture reference (as stored in the model, often a stale
// absolute path or one with backslashes) to a real file. Tries it as given
// relative to the model, then the bare filename in the model dir, then in every
// sibling and sub directory -- so textures kept in a folder next to the models
// are still found. Empty if nothing matches.
std::string findTextureFile(const std::string& baseDir, const std::string& ref) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string r = ref;
    for (char& c : r) if (c == '\\') c = '/';
    const std::string base = fs::path(r).filename().string();
    if (base.empty()) return {};

    if (fs::path(r).is_absolute() && fs::exists(r, ec)) return r;
    fs::path p = fs::path(baseDir) / r;                 // as given, relative
    if (fs::exists(p, ec)) return p.generic_string();
    p = fs::path(baseDir) / base;                       // bare name in model dir
    if (fs::exists(p, ec)) return p.generic_string();
    // Bare name in sibling dirs of the model dir, then in sub dirs of it.
    for (const fs::path& dir : {fs::path(baseDir).parent_path(), fs::path(baseDir)})
        for (const auto& d : fs::directory_iterator(dir, ec)) {
            if (!d.is_directory(ec)) continue;
            fs::path cand = d.path() / base;
            if (fs::exists(cand, ec)) return cand.generic_string();
        }
    return {};
}

// Load a material's texture of `type` (embedded or an external file resolved via
// findTextureFile) into an RGBA buffer + dimensions.
void loadAiTexture(const aiScene* scene, const aiMaterial* mat, aiTextureType type,
                   const std::string& baseDir, std::vector<std::uint8_t>& outPix,
                   int& outW, int& outH) {
    aiString texPath;
    if (mat->GetTexture(type, 0, &texPath) != AI_SUCCESS || texPath.length == 0) return;
    if (texPath.data[0] == '*') { // embedded (*N -> scene->mTextures)
        const int ti = std::atoi(texPath.C_Str() + 1);
        if (ti >= 0 && ti < static_cast<int>(scene->mNumTextures))
            decodeAiTexture(scene->mTextures[ti], outPix, outW, outH);
        return;
    }
    const std::string file = findTextureFile(baseDir, texPath.C_Str());
    if (file.empty()) {
        std::fprintf(stderr, "[Fitzel] model texture not found: '%s' (near %s)\n",
                     texPath.C_Str(), baseDir.c_str());
        return;
    }
    stbi_set_flip_vertically_on_load(0);
    int w = 0, h = 0, ch = 0;
    storePixels(stbi_load(file.c_str(), &w, &h, &ch, 4), w, h, outPix, outW, outH);
}

// Walk the node tree, baking each node's world transform into its meshes so the
// output matches loadGltf (de-indexed 8-float verts, one primitive per mesh).
// Convert one assimp mesh into a de-indexed ModelPrimitive, baking `world` into
// the positions/normals (matching loadGltf's static output).
ModelPrimitive aiMeshToPrimitive(const aiScene* scene, const aiMesh* mesh,
                                 const glm::mat4& world, const std::string& baseDir) {
    const glm::mat3 normalM = glm::mat3(glm::transpose(glm::inverse(world)));
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
        // Base colour (embedded or external file next to the model).
        loadAiTexture(scene, mat, aiTextureType_DIFFUSE, baseDir,
                      mp.texPixels, mp.texWidth, mp.texHeight);
        if (mp.texPixels.empty()) // some FBX put the colour map under BASE_COLOR
            loadAiTexture(scene, mat, aiTextureType_BASE_COLOR, baseDir,
                          mp.texPixels, mp.texWidth, mp.texHeight);
        // Normal map (FBX often uses HEIGHT/bump for it).
        loadAiTexture(scene, mat, aiTextureType_NORMALS, baseDir,
                      mp.normalPixels, mp.normalWidth, mp.normalHeight);
        if (mp.normalPixels.empty())
            loadAiTexture(scene, mat, aiTextureType_HEIGHT, baseDir,
                          mp.normalPixels, mp.normalWidth, mp.normalHeight);
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
        }
    }
    return mp;
}

void collectColladaNode(const aiScene* scene, const aiNode* node,
                        const glm::mat4& parent, const std::string& baseDir,
                        ModelData& out, float& lo, float& hi) {
    const glm::mat4 world = parent * aiToGlm(node->mTransformation);
    for (unsigned mi = 0; mi < node->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[node->mMeshes[mi]];
        if (!mesh || mesh->mNumFaces == 0) continue;
        ModelPrimitive mp = aiMeshToPrimitive(scene, mesh, world, baseDir);
        for (int i = 0; i + 7 < static_cast<int>(mp.vertices.size()); i += 8) {
            lo = glm::min(lo, mp.vertices[i + 1]);
            hi = glm::max(hi, mp.vertices[i + 1]);
        }
        if (mp.vertexCount() > 0) out.primitives.push_back(std::move(mp));
    }
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        collectColladaNode(scene, node->mChildren[c], world, baseDir, out, lo, hi);
}

// Structure-preserving walk: one ModelNode per mesh-bearing node, its meshes
// world-baked then recentred on their combined AABB (so each sits at the origin;
// `center` is where that origin belongs in model space).
void collectStructuredNode(const aiScene* scene, const aiNode* node,
                           const glm::mat4& parent, const std::string& baseDir,
                           std::vector<ModelNode>& out) {
    const glm::mat4 world = parent * aiToGlm(node->mTransformation);
    if (node->mNumMeshes > 0) {
        ModelNode mn;
        mn.name = node->mName.length ? node->mName.C_Str() : "part";
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (unsigned mi = 0; mi < node->mNumMeshes; ++mi) {
            const aiMesh* mesh = scene->mMeshes[node->mMeshes[mi]];
            if (!mesh || mesh->mNumFaces == 0) continue;
            ModelPrimitive mp = aiMeshToPrimitive(scene, mesh, world, baseDir);
            if (mp.vertexCount() == 0) continue;
            for (int i = 0; i + 7 < static_cast<int>(mp.vertices.size()); i += 8) {
                lo = glm::min(lo, glm::vec3(mp.vertices[i], mp.vertices[i+1], mp.vertices[i+2]));
                hi = glm::max(hi, glm::vec3(mp.vertices[i], mp.vertices[i+1], mp.vertices[i+2]));
            }
            mn.data.primitives.push_back(std::move(mp));
        }
        if (!mn.data.primitives.empty()) {
            const glm::vec3 c = 0.5f * (lo + hi);
            for (ModelPrimitive& p : mn.data.primitives)
                for (int i = 0; i + 7 < static_cast<int>(p.vertices.size()); i += 8) {
                    p.vertices[i]     -= c.x;
                    p.vertices[i + 1] -= c.y;
                    p.vertices[i + 2] -= c.z;
                }
            mn.data.minY = lo.y - c.y;
            mn.data.maxY = hi.y - c.y;
            mn.center = c;
            out.push_back(std::move(mn));
        }
    }
    for (unsigned c = 0; c < node->mNumChildren; ++c)
        collectStructuredNode(scene, node->mChildren[c], world, baseDir, out);
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

    const std::string baseDir =
        std::filesystem::path(path).parent_path().generic_string();
    float lo = std::numeric_limits<float>::max();
    float hi = std::numeric_limits<float>::lowest();
    collectColladaNode(scene, scene->mRootNode, glm::mat4(1.0f), baseDir, out, lo, hi);
    if (!out.primitives.empty()) { out.minY = lo; out.maxY = hi; }
    return out;
}

std::vector<ModelNode> loadModelNodes(const std::string& path) {
    std::vector<ModelNode> out;
    Assimp::Importer imp;
    const aiScene* scene = imp.ReadFile(
        path, aiProcess_Triangulate | aiProcess_GenSmoothNormals |
              aiProcess_JoinIdenticalVertices | aiProcess_ImproveCacheLocality);
    if (!scene || !scene->mRootNode ||
        (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE)) {
        std::fprintf(stderr, "[Fitzel] failed to load model '%s': %s\n",
                     path.c_str(), imp.GetErrorString());
        return out;
    }
    const std::string baseDir =
        std::filesystem::path(path).parent_path().generic_string();
    collectStructuredNode(scene, scene->mRootNode, glm::mat4(1.0f), baseDir, out);
    return out;
}

namespace {

glm::vec3 sampleVec3(const std::vector<float>& times,
                     const std::vector<glm::vec3>& vals, float t) {
    if (t <= times.front()) return vals.front();
    if (t >= times.back())  return vals.back();
    std::size_t i = 0;
    while (i + 1 < times.size() && times[i + 1] < t) ++i;
    const float span = times[i + 1] - times[i];
    const float f = span > 1e-8f ? (t - times[i]) / span : 0.0f;
    return glm::mix(vals[i], vals[i + 1], f);
}

glm::quat sampleQuat(const std::vector<float>& times,
                     const std::vector<glm::quat>& vals, float t) {
    if (t <= times.front()) return vals.front();
    if (t >= times.back())  return vals.back();
    std::size_t i = 0;
    while (i + 1 < times.size() && times[i + 1] < t) ++i;
    const float span = times[i + 1] - times[i];
    const float f = span > 1e-8f ? (t - times[i]) / span : 0.0f;
    glm::quat a = vals[i], b = vals[i + 1];
    if (glm::dot(a, b) < 0.0f) b = -b;                // shortest arc
    return glm::normalize(a * (1.0f - f) + b * f);    // nlerp
}

} // namespace

std::vector<glm::mat4> sampleSkeleton(const ModelData& model, int clip, float timeSec) {
    std::vector<glm::mat4> palette;
    if (clip < 0 || clip >= static_cast<int>(model.animations.size()) ||
        model.skeleton.empty())
        return palette;
    const AnimationClip& c = model.animations[clip];
    const std::size_t J = model.skeleton.size();

    // Animated local transform per joint (rest where a track is absent).
    std::vector<glm::mat4> local(J);
    for (std::size_t j = 0; j < J; ++j) {
        const SkeletonJoint& sj = model.skeleton[j];
        glm::vec3 T = sj.restT; glm::quat R = sj.restR; glm::vec3 S = sj.restS;
        if (j < c.tracks.size()) {
            const JointTrack& tr = c.tracks[j];
            if (!tr.tVals.empty()) T = sampleVec3(tr.tTimes, tr.tVals, timeSec);
            if (!tr.rVals.empty()) R = sampleQuat(tr.rTimes, tr.rVals, timeSec);
            if (!tr.sVals.empty()) S = sampleVec3(tr.sTimes, tr.sVals, timeSec);
        }
        local[j] = glm::translate(glm::mat4(1.0f), T) * glm::mat4_cast(R) *
                   glm::scale(glm::mat4(1.0f), S);
    }

    // Global transform, resolving parents first (works for any joint order).
    std::vector<glm::mat4> global(J);
    std::vector<char> done(J, 0);
    std::function<void(int)> resolve = [&](int j) {
        if (done[j]) return;
        const int p = model.skeleton[j].parent;
        glm::mat4 base = model.skeleton[j].baseParent;
        if (p >= 0) { resolve(p); base = global[p]; }
        global[j] = base * local[j];
        done[j] = 1;
    };
    for (std::size_t j = 0; j < J; ++j) resolve(static_cast<int>(j));

    palette.resize(J);
    for (std::size_t j = 0; j < J; ++j)
        palette[j] = global[j] * model.skeleton[j].inverseBind;
    return palette;
}

void skinPrimitive(const ModelPrimitive& prim, const std::vector<glm::mat4>& palette,
                   std::vector<Vertex>& out) {
    const int vc = prim.vertexCount();
    out.resize(static_cast<std::size_t>(vc));
    const bool skinned = !palette.empty() &&
                         prim.skin.size() == static_cast<std::size_t>(vc);
    for (int i = 0; i < vc; ++i) {
        const float* v = &prim.vertices[static_cast<std::size_t>(i) * 8];
        glm::vec3 P(v[0], v[1], v[2]);
        glm::vec3 N(v[3], v[4], v[5]);
        if (skinned) {
            const VertexSkin& s = prim.skin[static_cast<std::size_t>(i)];
            glm::mat4 m(0.0f);
            float wsum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                const float w = s.weights[k];
                if (w <= 0.0f) continue;
                const int j = s.joints[k];
                if (j < 0 || j >= static_cast<int>(palette.size())) continue;
                m += palette[j] * w;
                wsum += w;
            }
            if (wsum > 1e-5f) {
                if (wsum < 0.999f || wsum > 1.001f) m /= wsum; // normalize stray weights
                P = glm::vec3(m * glm::vec4(P, 1.0f));
                N = glm::mat3(m) * N;
            }
        }
        out[static_cast<std::size_t>(i)].position = P;
        out[static_cast<std::size_t>(i)].normal   = glm::normalize(N);
        out[static_cast<std::size_t>(i)].uv        = glm::vec2(v[6], v[7]);
    }
}

} // namespace fitzel
