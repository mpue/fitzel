#include "VkImGui.hpp"

#include <vector>

#include "VulkanDevice.hpp"

namespace fitzel::rhi::vk {

namespace {

// The OpenGL3 renderer's own shaders.
constexpr const char* kVert = R"(#version 330 core
layout (location = 0) in vec2 Position;
layout (location = 1) in vec2 UV;
layout (location = 2) in vec4 Color;
uniform mat4 ProjMtx;
out vec2 Frag_UV;
out vec4 Frag_Color;
void main() {
    Frag_UV = UV;
    Frag_Color = Color;
    gl_Position = ProjMtx * vec4(Position.xy, 0, 1);
}
)";

constexpr const char* kFrag = R"(#version 330 core
in vec2 Frag_UV;
in vec4 Frag_Color;
uniform sampler2D Texture;
layout (location = 0) out vec4 Out_Color;
void main() {
    Out_Color = Frag_Color * texture(Texture, Frag_UV.st);
}
)";

const SamplerDesc kLinear = SamplerDesc::linearClamp();
const SamplerDesc kNearest = SamplerDesc::nearestClamp();

TextureHandle handleOf(ImTextureID id) { return TextureHandle{static_cast<std::uint32_t>(id)}; }

} // namespace

ImGuiRenderer::ImGuiRenderer(VulkanDevice& device) : m_dev(device) {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = "fitzel_rhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Renderer_TextureMaxWidth = pio.Renderer_TextureMaxHeight = device.caps().maxTextureSize;
    pio.DrawCallback_ResetRenderState = resetRenderState;
    pio.DrawCallback_SetSamplerLinear = samplerLinear;
    pio.DrawCallback_SetSamplerNearest = samplerNearest;

    ProgramSource src;
    src.vertex = kVert;
    src.fragment = kFrag;
    m_program = m_dev.createProgram(src, nullptr);
    m_projLoc = m_dev.uniformLocation(m_program, "ProjMtx");
    m_texLoc = m_dev.uniformLocation(m_program, "Texture");
    const int zero = 0;
    m_dev.setUniform(m_program, m_texLoc, UniformType::Int, &zero, 1);

    m_vbo = m_dev.createBuffer(BufferKind::Vertex, BufferUsage::Stream, 0, nullptr);
    m_ibo = m_dev.createBuffer(BufferKind::Index, BufferUsage::Stream, 0, nullptr);
    const VertexAttribute attrs[3] = {
        {0, m_vbo, VertexFormat::Float2, offsetof(ImDrawVert, pos), sizeof(ImDrawVert), 0},
        {1, m_vbo, VertexFormat::Float2, offsetof(ImDrawVert, uv), sizeof(ImDrawVert), 0},
        {2, m_vbo, VertexFormat::UByte4Norm, offsetof(ImDrawVert, col), sizeof(ImDrawVert), 0},
    };
    VertexArrayDesc d;
    d.attributes = attrs;
    d.attributeCount = 3;
    d.indexBuffer = m_ibo;
    m_vao = m_dev.createVertexArray(d);
}

ImGuiRenderer::~ImGuiRenderer() {
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
        if (tex->RefCount == 1 && tex->TexID != ImTextureID_Invalid) {
            m_dev.destroy(handleOf(tex->TexID));
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
        }
    m_dev.destroy(m_vao);
    m_dev.destroy(m_vbo);
    m_dev.destroy(m_ibo);
    m_dev.destroy(m_program);
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    ImGui::GetPlatformIO().ClearRendererHandlers();
}

void ImGuiRenderer::updateTexture(ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantCreate) {
        TextureDesc d;
        d.format = Format::RGBA8;
        d.width = tex->Width;
        d.height = tex->Height;
        d.sampler = kLinear;
        const TextureHandle h = m_dev.createTexture(d, tex->GetPixels(), Format::RGBA8);
        tex->SetTexID(static_cast<ImTextureID>(h.id));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        const TextureHandle h = handleOf(tex->TexID);
        for (const ImTextureRect& r : tex->Updates) {
            TextureRegion reg;
            reg.x = r.x;
            reg.y = r.y;
            reg.w = r.w;
            reg.h = r.h;
            reg.rowLength = tex->Width;
            m_dev.updateTexture(h, reg, Format::RGBA8, tex->GetPixelsAt(r.x, r.y));
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
        m_dev.destroy(handleOf(tex->TexID));
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
}

void ImGuiRenderer::setupState(int fbWidth, int fbHeight, const ImDrawData* data) {
    RenderState s = m_dev.renderState();
    s.blend.enabled = true;
    s.blend.srcColor = BlendFactor::SrcAlpha;
    s.blend.dstColor = BlendFactor::OneMinusSrcAlpha;
    s.blend.srcAlpha = BlendFactor::One;
    s.blend.dstAlpha = BlendFactor::OneMinusSrcAlpha;
    s.blend.colorOp = s.blend.alphaOp = BlendOp::Add;
    s.raster.cull = false;
    s.raster.wireframe = false;
    s.raster.scissor = true;
    s.raster.clipDistances = 0;
    s.depth.test = false;
    s.stencil.test = false;
    m_dev.setRenderState(s);
    m_dev.setViewport(0, 0, fbWidth, fbHeight);
    // OpenGL's lower-left origin, as the OpenGL3 renderer has it.
    const float L = data->DisplayPos.x, R = data->DisplayPos.x + data->DisplaySize.x;
    const float T = data->DisplayPos.y, B = data->DisplayPos.y + data->DisplaySize.y;
    const float proj[16] = {
        2.0f / (R - L), 0.0f, 0.0f, 0.0f,
        0.0f, 2.0f / (T - B), 0.0f, 0.0f,
        0.0f, 0.0f, -1.0f, 0.0f,
        (R + L) / (L - R), (T + B) / (B - T), 0.0f, 1.0f,
    };
    m_dev.bindProgram(m_program);
    m_dev.setUniform(m_program, m_projLoc, UniformType::Mat4, proj, 1);
    m_dev.bindVertexArray(m_vao);
    m_mode = Mode::Linear;
}

void ImGuiRenderer::render(ImDrawData* data) {
    if (!data) return;
    const int fbW = static_cast<int>(data->DisplaySize.x * data->FramebufferScale.x);
    const int fbH = static_cast<int>(data->DisplaySize.y * data->FramebufferScale.y);
    if (data->Textures)
        for (ImTextureData* tex : *data->Textures)
            if (tex->Status != ImTextureStatus_OK) updateTexture(tex);
    if (fbW <= 0 || fbH <= 0 || data->TotalVtxCount == 0) return;

    // Every list's vertices and indices in one go.
    std::vector<ImDrawVert> verts;
    std::vector<ImDrawIdx> idx;
    verts.reserve(static_cast<std::size_t>(data->TotalVtxCount));
    idx.reserve(static_cast<std::size_t>(data->TotalIdxCount));
    for (const ImDrawList* l : data->CmdLists) {
        verts.insert(verts.end(), l->VtxBuffer.begin(), l->VtxBuffer.end());
        idx.insert(idx.end(), l->IdxBuffer.begin(), l->IdxBuffer.end());
    }
    m_dev.bufferData(m_vbo, verts.size() * sizeof(ImDrawVert), verts.data(), BufferUsage::Stream);
    m_dev.bufferData(m_ibo, idx.size() * sizeof(ImDrawIdx), idx.data(), BufferUsage::Stream);

    // Everything touched is put back, as the OpenGL3 renderer does.
    const RenderState prevState = m_dev.renderState();
    const ProgramHandle prevProgram = m_dev.boundProgram();
    const VertexArrayHandle prevVao = m_dev.boundVertexArray();
    const Rect prevViewport = m_dev.viewport(), prevScissor = m_dev.scissor();
    const TextureHandle prevTex = m_dev.boundTexture(0);

    setupState(fbW, fbH, data);
    const ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    const ImVec2 off = data->DisplayPos, scale = data->FramebufferScale;
    int vtxBase = 0, idxBase = 0;
    for (const ImDrawList* l : data->CmdLists) {
        for (const ImDrawCmd& cmd : l->CmdBuffer) {
            if (cmd.UserCallback) {
                if (cmd.UserCallback == pio.DrawCallback_ResetRenderState) setupState(fbW, fbH, data);
                else if (cmd.UserCallback == samplerLinear) m_mode = Mode::Linear;
                else if (cmd.UserCallback == samplerNearest) m_mode = Mode::Nearest;
                else if (cmd.UserCallback == useTextureSampling) m_mode = Mode::Own;
                else cmd.UserCallback(l, &cmd);
                continue;
            }
            const ImVec2 lo((cmd.ClipRect.x - off.x) * scale.x, (cmd.ClipRect.y - off.y) * scale.y);
            const ImVec2 hi((cmd.ClipRect.z - off.x) * scale.x, (cmd.ClipRect.w - off.y) * scale.y);
            if (hi.x <= lo.x || hi.y <= lo.y) continue;
            m_dev.setScissor(Rect{static_cast<int>(lo.x), static_cast<int>(static_cast<float>(fbH) - hi.y),
                                  static_cast<int>(hi.x - lo.x), static_cast<int>(hi.y - lo.y)});
            m_dev.bindTexture(0, handleOf(cmd.GetTexID()));
            m_dev.setUnitZeroSampler(m_mode == Mode::Linear ? &kLinear
                                     : m_mode == Mode::Nearest ? &kNearest : nullptr);
            m_dev.drawIndexed(Primitive::Triangles, static_cast<int>(cmd.ElemCount),
                              sizeof(ImDrawIdx) == 2 ? IndexType::U16 : IndexType::U32,
                              (static_cast<std::size_t>(idxBase) + cmd.IdxOffset) * sizeof(ImDrawIdx), 1,
                              vtxBase + static_cast<int>(cmd.VtxOffset));
        }
        vtxBase += l->VtxBuffer.Size;
        idxBase += l->IdxBuffer.Size;
    }
    m_dev.setUnitZeroSampler(nullptr);
    m_dev.setRenderState(prevState);
    m_dev.bindProgram(prevProgram);
    m_dev.bindVertexArray(prevVao);
    m_dev.setViewport(prevViewport);
    m_dev.setScissor(prevScissor);
    m_dev.bindTexture(0, TextureHandle{});
    m_dev.bindTexture(0, prevTex);
}

} // namespace fitzel::rhi::vk
