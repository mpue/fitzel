#pragma once

// A device texture as Dear ImGui takes it (ImGui::Image, ImDrawList::AddImage).
// What that number is belongs to the backend's ImGui renderer -- an OpenGL
// texture name, a Vulkan descriptor set -- so it is asked of the device rather
// than taken from the handle.

#include <imgui.h>

#include "fitzel/rhi/Device.hpp"

namespace fitzel {

inline ImTextureID imTexture(rhi::TextureHandle texture) {
    return static_cast<ImTextureID>(rhi::device().imguiTextureId(texture));
}

} // namespace fitzel
