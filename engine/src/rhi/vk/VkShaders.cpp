// Programs: the engine's GLSL turned into SPIR-V by glslang, and what the
// compiler made of it read back with SPIRV-Reflect.
//
// glslang compiles under GL_EXT_vulkan_glsl_relaxed: the shaders stay written
// for OpenGL -- loose `uniform float x;`, samplers given a texture unit through
// a uniform -- and the compiler gathers the loose uniforms into one block
// (fz_Default, std140, binding 64), gives every sampler a binding of its own and
// matches the stages' inputs and outputs by name. The device keeps a CPU copy of
// that block per program, which setUniform writes and each draw uploads.
//
// Two things OpenGL does differently are handled in every vertex shader, by
// wrapping its main():
//  - clip space z runs -1..1 in OpenGL and 0..1 in Vulkan: remapped at the end,
//    so the projection matrices (glm's, OpenGL's) and the depth values that come
//    out are those of the OpenGL backend;
//  - a gl_ClipDistance only clips in OpenGL while GL_CLIP_DISTANCEi is enabled,
//    in Vulkan whenever it is written: written through a uniform mask
//    (fz_ClipMask) the device fills from the raster state.

#include "VulkanDevice.hpp"

#include <algorithm>
#include <memory>
#include <cstring>
#include <regex>
#include <set>

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_reflect.h>

namespace fitzel::rhi::vk {

namespace {

constexpr std::uint32_t kDefaultBlockBinding = 64;

bool g_glslangReady = false;

void initGlslang() {
    if (g_glslangReady) return;
    glslang::InitializeProcess();
    g_glslangReady = true;
}

// Wrap a vertex shader's main() (see the top of this file).
std::string wrapVertex(const std::string& src) {
    std::size_t at = src.find("#version");
    if (at == std::string::npos) return src;
    at = src.find('\n', at);
    if (at == std::string::npos) return src;
    ++at;
    // After the #extension lines: a declaration may not come before them.
    for (std::size_t e = src.find("#extension", at); e != std::string::npos;
         e = src.find("#extension", e + 1)) {
        const std::size_t nl = src.find('\n', e);
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    const int line = 1 + static_cast<int>(std::count(src.begin(), src.begin() + static_cast<std::ptrdiff_t>(at), '\n'));

    std::set<int> clips;
    static const std::regex use(R"(gl_ClipDistance\s*\[\s*(\d+)\s*\])");
    for (std::sregex_iterator it(src.begin(), src.end(), use), end; it != end; ++it)
        clips.insert(std::stoi((*it)[1].str()));

    std::string head = "#define main fz_main\n";
    if (!clips.empty())
        head += "#define gl_ClipDistance fz_ClipDistance\nfloat fz_ClipDistance[8];\n";
    head += "#line " + std::to_string(line) + "\n";

    std::string tail = "\n#undef main\n";
    if (!clips.empty()) tail += "#undef gl_ClipDistance\nuniform int fz_ClipMask;\n";
    tail += "void main() {\n    fz_main();\n";
    for (int i : clips)
        tail += "    gl_ClipDistance[" + std::to_string(i) + "] = ((fz_ClipMask >> " + std::to_string(i) +
                ") & 1) != 0 ? fz_ClipDistance[" + std::to_string(i) + "] : 1.0;\n";
    tail += "    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n}\n";
    return src.substr(0, at) + head + src.substr(at) + tail;
}

const char* stageName(EShLanguage s) {
    switch (s) {
        case EShLangVertex:   return "vertex";
        case EShLangFragment: return "fragment";
        default:              return "compute";
    }
}

struct Reflected {
    std::vector<SpvReflectDescriptorBinding*> bindings;
    std::vector<SpvReflectInterfaceVariable*> inputs;
    SpvReflectShaderModule module{};
};

int kindOfImage(const SpvReflectImageTraits& im) {
    if (im.dim == SpvDim3D) return 2;
    if (im.dim == SpvDimCube) return 3;
    return im.arrayed ? 1 : 0;
}

int baseOf(SpvReflectFormat f) {
    switch (f) {
        case SPV_REFLECT_FORMAT_R32_SINT: case SPV_REFLECT_FORMAT_R32G32_SINT:
        case SPV_REFLECT_FORMAT_R32G32B32_SINT: case SPV_REFLECT_FORMAT_R32G32B32A32_SINT:
            return 1;
        case SPV_REFLECT_FORMAT_R32_UINT: case SPV_REFLECT_FORMAT_R32G32_UINT:
        case SPV_REFLECT_FORMAT_R32G32B32_UINT: case SPV_REFLECT_FORMAT_R32G32B32A32_UINT:
            return 2;
        default:
            return 0;
    }
}

// Every floating-point operation decorated NoContraction: the driver may not
// fuse a multiply and an add into one FMA (or distribute a product over a sum
// to get one). The engine's value noise hashes integer lattice points through
// fract(p * 123.34) and the like, and a neighbour cell's corner -- hash(i +
// vec2(1, 0)) here, hash(i') there -- must come out the same bit for bit or the
// noise tears along every cell edge. The OpenGL driver happens not to contract
// there; this makes Vulkan keep the same arithmetic, at the price of the FMAs.
void forbidContraction(std::vector<unsigned int>& spv) {
    if (spv.size() < 5) return;
    constexpr unsigned kDecorate = 71, kNoContraction = 42;
    auto isFloatOp = [](unsigned op) {
        switch (op) {
            case 127:   // OpFNegate
            case 129:   // OpFAdd
            case 131:   // OpFSub
            case 133:   // OpFMul
            case 136:   // OpFDiv
            case 140:   // OpFRem
            case 141:   // OpFMod
            case 142:   // OpVectorTimesScalar
            case 143:   // OpMatrixTimesScalar
            case 144:   // OpVectorTimesMatrix
            case 145:   // OpMatrixTimesVector
            case 146:   // OpMatrixTimesMatrix
            case 148:   // OpDot
                return true;
            default:
                return false;
        }
    };
    std::vector<unsigned int> ids;
    std::size_t insertAt = 0;
    for (std::size_t i = 5; i < spv.size();) {
        const unsigned op = spv[i] & 0xFFFFu, count = spv[i] >> 16;
        if (count == 0) return;
        if (op == 71 || op == 72 || op == 73 || op == 74 || op == 332 || op == 5632 || op == 5633)
            insertAt = i + count;
        if (isFloatOp(op) && count >= 3) ids.push_back(spv[i + 2]);
        i += count;
    }
    if (!insertAt || ids.empty()) return;
    std::vector<unsigned int> deco;
    deco.reserve(ids.size() * 3);
    for (unsigned id : ids) deco.insert(deco.end(), {(3u << 16) | kDecorate, id, kNoContraction});
    spv.insert(spv.begin() + static_cast<std::ptrdiff_t>(insertAt), deco.begin(), deco.end());
}

} // namespace

bool VulkanDevice::buildProgram(VkProg& p, const ProgramSource& src, std::string* log) {
    initGlslang();
    struct Stage { EShLanguage lang; std::string text; std::unique_ptr<glslang::TShader> shader; };
    std::vector<Stage> stages;
    if (src.compute) {
        stages.push_back({EShLangCompute, src.compute, nullptr});
        p.compute = true;
    } else {
        if (!src.vertex || !src.fragment) return false;
        stages.push_back({EShLangVertex, wrapVertex(src.vertex), nullptr});
        stages.push_back({EShLangFragment, src.fragment, nullptr});
    }
    const auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
    glslang::TProgram program;
    bool ok = true;
    for (Stage& s : stages) {
        s.shader = std::make_unique<glslang::TShader>(s.lang);
        const char* text = s.text.c_str();
        s.shader->setStrings(&text, 1);
        s.shader->setEnvInput(glslang::EShSourceGlsl, s.lang, glslang::EShClientVulkan, 100);
        s.shader->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
        s.shader->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
        s.shader->setEnvInputVulkanRulesRelaxed();
        s.shader->setAutoMapBindings(true);
        s.shader->setAutoMapLocations(true);
        s.shader->setGlobalUniformBlockName("fz_Default");
        s.shader->setGlobalUniformSet(0);
        s.shader->setGlobalUniformBinding(kDefaultBlockBinding);
        const bool parsed = s.shader->parse(GetDefaultResources(), 100, false, messages);
        const char* info = s.shader->getInfoLog();
        if (!parsed) {
            std::fprintf(stderr, "[Fitzel] %s shader compile error:\n%s\n", stageName(s.lang), info);
            if (log) *log += std::string(stageName(s.lang)) + ": " + info;
            ok = false;
        } else if (log && info && *info) {
            *log += std::string(stageName(s.lang)) + ": " + info;
        }
        program.addShader(s.shader.get());
    }
    if (!ok) return false;
    // Bindings and locations for the program as a whole: left to each stage,
    // glslang numbers a vertex shader's samplers and a fragment shader's from
    // zero alike, and one sampler would land on another's binding.
    bool mapped = false;
    if (program.link(messages)) {
        std::unique_ptr<glslang::TIoMapper> mapper(glslang::GetGlslIoMapper());
        std::unique_ptr<glslang::TIoMapResolver> resolver(program.getGlslIoResolver(stages[0].lang));
        mapped = resolver && program.mapIO(resolver.get(), mapper.get());
    }
    if (!mapped) {
        std::fprintf(stderr, "[Fitzel] shader link error:\n%s\n", program.getInfoLog());
        if (log) *log += program.getInfoLog();
        return false;
    }

    // SPIR-V per stage, and what it holds.
    std::vector<Reflected> refl(stages.size());
    for (std::size_t i = 0; i < stages.size(); ++i) {
        std::vector<unsigned int> spv;
        glslang::SpvOptions opt;
        glslang::GlslangToSpv(*program.getIntermediate(stages[i].lang), spv, &opt);
        forbidContraction(spv);
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = spv.size() * sizeof(unsigned int);
        mi.pCode = spv.data();
        VkShaderModule mod = VK_NULL_HANDLE;
        FZ_VK(vkCreateShaderModule(m_device, &mi, nullptr, &mod));
        if (stages[i].lang == EShLangVertex) p.vs = mod;
        else if (stages[i].lang == EShLangFragment) p.fs = mod;
        else p.cs = mod;

        Reflected& r = refl[i];
        if (spvReflectCreateShaderModule(mi.codeSize, spv.data(), &r.module) != SPV_REFLECT_RESULT_SUCCESS)
            return false;
        std::uint32_t n = 0;
        spvReflectEnumerateDescriptorBindings(&r.module, &n, nullptr);
        r.bindings.resize(n);
        spvReflectEnumerateDescriptorBindings(&r.module, &n, r.bindings.data());
        if (stages[i].lang == EShLangVertex) {
            spvReflectEnumerateInputVariables(&r.module, &n, nullptr);
            r.inputs.resize(n);
            spvReflectEnumerateInputVariables(&r.module, &n, r.inputs.data());
        }
    }

    // The union of every stage's bindings: one set, visible to all stages.
    std::vector<VkDescriptorSetLayoutBinding> layout;
    auto addName = [&p](const std::string& name, const UniformLoc& loc, std::uint32_t count) {
        const int index = static_cast<int>(p.locs.size());
        p.locs.push_back(loc);
        if (!p.names.count(name)) p.names[name] = index;
        if (count > 1 || name.find('[') == std::string::npos) p.names.emplace(name + "[0]", index);
    };
    for (Reflected& r : refl) {
        for (SpvReflectDescriptorBinding* b : r.bindings) {
            const bool known = std::any_of(layout.begin(), layout.end(),
                                           [b](const auto& l) { return l.binding == b->binding; });
            if (known) continue;
            VkDescriptorSetLayoutBinding lb{};
            lb.binding = b->binding;
            lb.descriptorCount = std::max(1u, b->count);
            lb.stageFlags = p.compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS;
            switch (b->descriptor_type) {
                case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: {
                    lb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    SamplerSlot s;
                    s.binding = b->binding;
                    s.count = lb.descriptorCount;
                    s.kind = kindOfImage(b->image);
                    s.units.assign(s.count, 0);
                    const int slot = static_cast<int>(p.samplers.size());
                    p.samplers.push_back(s);
                    for (std::uint32_t e = 0; e < s.count; ++e) {
                        UniformLoc loc;
                        loc.sampler = true;
                        loc.samplerIndex = slot;
                        loc.element = e;
                        loc.remaining = s.count - e;
                        const std::string base = b->name ? b->name : "";
                        if (e == 0) addName(base, loc, s.count);
                        if (s.count > 1) addName(base + "[" + std::to_string(e) + "]", loc, 1);
                    }
                    break;
                }
                case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    if (b->binding == kDefaultBlockBinding) {
                        lb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                        p.uboBinding = b->binding;
                        p.uniforms.assign(b->block.size, 0);
                        for (std::uint32_t m = 0; m < b->block.member_count; ++m) {
                            const SpvReflectBlockVariable& v = b->block.members[m];
                            const std::uint32_t count = v.array.dims_count ? v.array.dims[0] : 1;
                            for (std::uint32_t e = 0; e < count; ++e) {
                                UniformLoc loc;
                                loc.offset = v.offset + e * v.array.stride;
                                loc.arrayStride = v.array.stride;
                                loc.matrixStride = v.numeric.matrix.stride;
                                loc.remaining = count - e;
                                const std::string base = v.name ? v.name : "";
                                if (e == 0) addName(base, loc, count);
                                if (count > 1) addName(base + "[" + std::to_string(e) + "]", loc, 1);
                            }
                        }
                    } else {
                        lb.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                        p.resources.push_back({b->binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER});
                    }
                    break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                    lb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    p.resources.push_back({b->binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER});
                    break;
                case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    lb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    p.resources.push_back({b->binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE});
                    break;
                default:
                    continue;   // nothing the engine's shaders use
            }
            layout.push_back(lb);
        }
    }
    if (auto it = p.names.find("fz_ClipMask"); it != p.names.end()) p.clipMask = it->second;
    if (!refl.empty() && !p.compute)
        for (SpvReflectInterfaceVariable* v : refl[0].inputs) {
            if (v->decoration_flags & SPV_REFLECT_DECORATION_BUILT_IN) continue;
            if (v->location == UINT32_MAX) continue;
            p.inputs.push_back({v->location, baseOf(v->format)});
        }
    std::sort(p.inputs.begin(), p.inputs.end(),
              [](const VertexInput& a, const VertexInput& b) { return a.location < b.location; });
    for (Reflected& r : refl) spvReflectDestroyShaderModule(&r.module);

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = static_cast<std::uint32_t>(layout.size());
    li.pBindings = layout.data();
    FZ_VK(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &p.setLayout));
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p.setLayout;
    FZ_VK(vkCreatePipelineLayout(m_device, &pl, nullptr, &p.layout));
    return true;
}

void VulkanDevice::freeProgram(VkProg& p) {
    if (p.computePipeline) vkDestroyPipeline(m_device, p.computePipeline, nullptr);
    if (p.layout) vkDestroyPipelineLayout(m_device, p.layout, nullptr);
    if (p.setLayout) vkDestroyDescriptorSetLayout(m_device, p.setLayout, nullptr);
    for (VkShaderModule m : {p.vs, p.fs, p.cs})
        if (m) vkDestroyShaderModule(m_device, m, nullptr);
    p = VkProg{};
}

ProgramHandle VulkanDevice::createProgram(const ProgramSource& src, std::string* log) {
    VkProg p;
    if (!buildProgram(p, src, log)) {
        freeProgram(p);
        return {};
    }
    return ProgramHandle{m_programs.add(std::move(p))};
}

void VulkanDevice::destroy(ProgramHandle h) {
    VkProg* p = m_programs.find(h.id);
    if (!p) return;
    if (m_program == h) m_program = ProgramHandle{};
    // Its pipelines go with it: the slot is handed out again, and a pipeline
    // cached under the old program would be taken for the new one's.
    std::vector<VkPipeline> pipes;
    for (auto it = m_pipelineOwner.begin(); it != m_pipelineOwner.end();) {
        if (it->second == h.id) {
            pipes.push_back(m_pipelines[it->first]);
            if (m_boundPipeline == m_pipelines[it->first]) m_boundPipeline = VK_NULL_HANDLE;
            m_pipelines.erase(it->first);
            it = m_pipelineOwner.erase(it);
        } else {
            ++it;
        }
    }
    auto doomed = std::make_shared<VkProg>(std::move(*p));
    m_programs.remove(h.id);
    deferDelete([this, doomed, pipes] {
        for (VkPipeline pipe : pipes) vkDestroyPipeline(m_device, pipe, nullptr);
        freeProgram(*doomed);
    });
}

int VulkanDevice::uniformLocation(ProgramHandle h, const char* name) {
    const VkProg* p = m_programs.find(h.id);
    if (!p || !name) return -1;
    const auto it = p->names.find(name);
    return it == p->names.end() ? -1 : it->second;
}

void VulkanDevice::setUniform(ProgramHandle h, int location, UniformType type, const void* data, int count) {
    VkProg* p = m_programs.find(h.id);
    if (!p || location < 0 || location >= static_cast<int>(p->locs.size()) || !data || count <= 0) return;
    const UniformLoc& L = p->locs[static_cast<std::size_t>(location)];
    const auto n = std::min<std::uint32_t>(static_cast<std::uint32_t>(count), L.remaining);
    if (L.sampler) {
        // A sampler's value is the texture unit it reads.
        SamplerSlot& s = p->samplers[static_cast<std::size_t>(L.samplerIndex)];
        const auto* units = static_cast<const std::int32_t*>(data);
        for (std::uint32_t k = 0; k < n; ++k) s.units[L.element + k] = units[k];
        return;
    }
    const auto* src = static_cast<const unsigned char*>(data);
    std::uint32_t comps = 1, cols = 0;
    switch (type) {
        case UniformType::Int: case UniformType::Float: comps = 1; break;
        case UniformType::IVec2: case UniformType::Vec2: comps = 2; break;
        case UniformType::IVec3: case UniformType::Vec3: comps = 3; break;
        case UniformType::IVec4: case UniformType::Vec4: comps = 4; break;
        case UniformType::Mat3: comps = 3; cols = 3; break;
        case UniformType::Mat4: comps = 4; cols = 4; break;
    }
    for (std::uint32_t k = 0; k < n; ++k) {
        std::uint8_t* dst = p->uniforms.data() + L.offset + k * L.arrayStride;
        if (dst + 4 > p->uniforms.data() + p->uniforms.size()) break;
        if (cols == 0) {
            std::memcpy(dst, src, comps * 4);
            src += comps * 4;
        } else {
            const std::uint32_t stride = L.matrixStride ? L.matrixStride : 16;
            for (std::uint32_t c = 0; c < cols; ++c) {
                std::memcpy(dst + c * stride, src, comps * 4);
                src += comps * 4;
            }
        }
    }
    p->dirty = true;
}

void VulkanDevice::readUniform(ProgramHandle h, int location, bool, void* out16) {
    const VkProg* p = m_programs.find(h.id);
    if (!p || location < 0 || location >= static_cast<int>(p->locs.size()) || !out16) return;
    const UniformLoc& L = p->locs[static_cast<std::size_t>(location)];
    if (L.sampler) {
        const std::int32_t unit = p->samplers[static_cast<std::size_t>(L.samplerIndex)].units[L.element];
        std::memcpy(out16, &unit, 4);
        return;
    }
    const std::size_t bytes = std::min<std::size_t>(64, p->uniforms.size() - L.offset);
    std::memcpy(out16, p->uniforms.data() + L.offset, bytes);
}

} // namespace fitzel::rhi::vk
