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

} // namespace fitzel
