#pragma once

// Dear ImGui drawn through the device itself rather than imgui_impl_vulkan: the
// same projection, scissor arithmetic and blend as the OpenGL3 renderer the
// OpenGL backend uses, so the UI lands on the picture the same way round as
// everything else drawn into the window's framebuffer (row 0 at the bottom),
// and ImGui::Image takes a texture handle like any draw.

#include <imgui.h>

#include "fitzel/rhi/Types.hpp"

namespace fitzel::rhi::vk {

class VulkanDevice;

class ImGuiRenderer {
public:
    explicit ImGuiRenderer(VulkanDevice& device);
    ~ImGuiRenderer();
    ImGuiRenderer(const ImGuiRenderer&) = delete;
    ImGuiRenderer& operator=(const ImGuiRenderer&) = delete;

    void render(ImDrawData* data);

    // Draw callbacks: they do nothing themselves; render() recognises them.
    static void useTextureSampling(const ImDrawList*, const ImDrawCmd*) {}
    static void resetRenderState(const ImDrawList*, const ImDrawCmd*) {}
    static void samplerLinear(const ImDrawList*, const ImDrawCmd*) {}
    static void samplerNearest(const ImDrawList*, const ImDrawCmd*) {}

private:
    void updateTexture(ImTextureData* tex);
    void setupState(int fbWidth, int fbHeight, const ImDrawData* data);

    VulkanDevice&     m_dev;
    ProgramHandle     m_program;
    int               m_projLoc = -1, m_texLoc = -1;
    BufferHandle      m_vbo, m_ibo;
    VertexArrayHandle m_vao;
    // How images sample: ImGui's linear or nearest (both clamped), or with the
    // texture's own sampler (useTextureSampling).
    enum class Mode { Linear, Nearest, Own } m_mode = Mode::Linear;
};

} // namespace fitzel::rhi::vk
