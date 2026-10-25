// The Vulkan device's resources: buffers (device-local, written through
// staging copies in the command stream), textures (VK_IMAGE_LAYOUT_GENERAL
// from birth), samplers, framebuffers, vertex arrays, and the copies between
// them and the CPU.

#include "VulkanDevice.hpp"

#include <algorithm>
#include <cstring>

namespace fitzel::rhi::vk {

namespace {
constexpr std::size_t kStagingChunk = 16u << 20;
constexpr std::size_t kUniformChunk = 4u << 20;

std::size_t alignUp(std::size_t v, std::size_t a) { return (v + a - 1) / a * a; }

int mipSize(int size, int level) { return std::max(1, size >> level); }
} // namespace

// --- Memory -----------------------------------------------------------------------

Chunk VulkanDevice::makeChunk(std::size_t bytes, VkBufferUsageFlags usage) {
    Chunk c;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VmaAllocationInfo info{};
    FZ_VK(vmaCreateBuffer(m_vma, &bi, &ai, &c.buffer, &c.alloc, &info));
    c.mapped = static_cast<unsigned char*>(info.pMappedData);
    c.size = bytes;
    return c;
}

VulkanDevice::Staging VulkanDevice::stage(std::size_t bytes, std::size_t align) {
    Frame& f = m_frames[static_cast<std::size_t>(m_frameIndex)];
    for (Chunk& c : f.staging) {
        const std::size_t at = alignUp(c.used, align);
        if (at + bytes <= c.size) {
            c.used = at + bytes;
            return {c.buffer, at, c.mapped + at};
        }
    }
    f.staging.push_back(makeChunk(std::max(bytes, kStagingChunk), VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    Chunk& c = f.staging.back();
    c.used = bytes;
    return {c.buffer, 0, c.mapped};
}

VulkanDevice::Staging VulkanDevice::uniformRing(std::size_t bytes) {
    Frame& f = m_frames[static_cast<std::size_t>(m_frameIndex)];
    const std::size_t align = std::max<std::size_t>(m_props.limits.minUniformBufferOffsetAlignment, 16);
    for (Chunk& c : f.uniforms) {
        const std::size_t at = alignUp(c.used, align);
        if (at + bytes <= c.size) {
            c.used = at + bytes;
            return {c.buffer, at, c.mapped + at};
        }
    }
    f.uniforms.push_back(makeChunk(std::max(bytes, kUniformChunk), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT));
    Chunk& c = f.uniforms.back();
    c.used = bytes;
    return {c.buffer, 0, c.mapped};
}

void VulkanDevice::allocBuffer(VkBuf& b, std::size_t bytes) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<std::size_t>(bytes, 16);
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (b.kind == BufferKind::Readback) {
        bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    } else {
        bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    }
    VmaAllocationInfo info{};
    FZ_VK(vmaCreateBuffer(m_vma, &bi, &ai, &b.buffer, &b.alloc, &info));
    b.mapped = info.pMappedData;
    b.capacity = bi.size;
}

void VulkanDevice::freeBuffer(VkBuf& b) {
    if (b.buffer) vmaDestroyBuffer(m_vma, b.buffer, b.alloc);
    b.buffer = VK_NULL_HANDLE;
    b.alloc = nullptr;
}

void VulkanDevice::uploadBuffer(VkBuf& b, std::size_t offset, std::size_t bytes, const void* data) {
    if (!bytes) return;
    if (b.kind == BufferKind::Readback) {
        waitFor(b.lastWrite);
        std::memcpy(static_cast<unsigned char*>(b.mapped) + offset, data, bytes);
        return;
    }
    enterSegment(Segment::Transfer);
    transferTouch(b.buffer);
    const Staging s = stage(bytes);
    std::memcpy(s.ptr, data, bytes);
    VkBufferCopy c{s.offset, offset, bytes};
    vkCmdCopyBuffer(m_cmd, s.buffer, b.buffer, 1, &c);
}

// --- Buffers ----------------------------------------------------------------------

BufferHandle VulkanDevice::createBuffer(BufferKind kind, BufferUsage usage, std::size_t bytes,
                                        const void* data) {
    VkBuf b;
    b.kind = kind;
    b.usage = usage;
    b.size = bytes;
    allocBuffer(b, bytes);
    if (data) {
        uploadBuffer(b, 0, bytes, data);
    } else if (kind != BufferKind::Readback) {
        // Zero, rather than whatever the memory held: OpenGL leaves it
        // undefined too, and zero is the undefined nobody notices.
        enterSegment(Segment::Transfer);
        transferTouch(b.buffer);
        vkCmdFillBuffer(m_cmd, b.buffer, 0, VK_WHOLE_SIZE, 0);
    }
    return BufferHandle{m_buffers.add(b)};
}

void VulkanDevice::destroy(BufferHandle h) {
    VkBuf* b = m_buffers.find(h.id);
    if (!b) return;
    const VkBuffer buf = b->buffer;
    const VmaAllocation alloc = b->alloc;
    m_buffers.remove(h.id);
    deferDelete([this, buf, alloc] { vmaDestroyBuffer(m_vma, buf, alloc); });
}

void VulkanDevice::bufferData(BufferHandle h, std::size_t bytes, const void* data, BufferUsage usage) {
    VkBuf* b = m_buffers.find(h.id);
    if (!b) return;
    b->usage = usage;
    // Commands run in the order they are recorded, so storage big enough can
    // simply be written again: draws recorded before this read what was there
    // then. Only growing needs a new buffer (the old one lives on until the
    // GPU is done with it).
    if (bytes > b->capacity) {
        const VkBuffer buf = b->buffer;
        const VmaAllocation alloc = b->alloc;
        deferDelete([this, buf, alloc] { vmaDestroyBuffer(m_vma, buf, alloc); });
        allocBuffer(*b, bytes + bytes / 2);
    }
    b->size = bytes;
    if (data) uploadBuffer(*b, 0, bytes, data);
}

void VulkanDevice::bufferSubData(BufferHandle h, std::size_t offset, std::size_t bytes,
                                 const void* data) {
    VkBuf* b = m_buffers.find(h.id);
    if (!b || !data || offset + bytes > b->capacity) return;
    uploadBuffer(*b, offset, bytes, data);
}

void VulkanDevice::readBuffer(BufferHandle h, std::size_t offset, std::size_t bytes, void* out) {
    VkBuf* b = m_buffers.find(h.id);
    if (!b || !bytes || offset + bytes > b->capacity) return;
    if (b->kind == BufferKind::Readback) {
        waitFor(b->lastWrite);
        vmaInvalidateAllocation(m_vma, b->alloc, offset, bytes);
        std::memcpy(out, static_cast<const unsigned char*>(b->mapped) + offset, bytes);
        return;
    }
    VkBuf tmp;
    tmp.kind = BufferKind::Readback;
    allocBuffer(tmp, bytes);
    enterSegment(Segment::Transfer);
    transferTouch(tmp.buffer);
    VkBufferCopy c{offset, 0, bytes};
    vkCmdCopyBuffer(m_cmd, b->buffer, tmp.buffer, 1, &c);
    waitFor(pendingValue());
    vmaInvalidateAllocation(m_vma, tmp.alloc, 0, bytes);
    std::memcpy(out, tmp.mapped, bytes);
    freeBuffer(tmp);
}

std::size_t VulkanDevice::bufferSize(BufferHandle h) const {
    const VkBuf* b = m_buffers.find(h.id);
    return b ? b->size : 0;
}

// --- Textures ---------------------------------------------------------------------

void VulkanDevice::createImage(VkTex& t) {
    const TextureDesc& d = t.desc;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(m_gpu, t.fmt.format, &fp);
    const VkFormatFeatureFlags feat = fp.optimalTilingFeatures;
    const bool depth = (t.fmt.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = d.type == TextureType::Tex3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    ci.format = t.fmt.format;
    ci.extent = {static_cast<std::uint32_t>(d.width), static_cast<std::uint32_t>(d.height),
                 d.type == TextureType::Tex3D ? static_cast<std::uint32_t>(d.depth) : 1u};
    ci.mipLevels = static_cast<std::uint32_t>(d.mipLevels);
    t.layers = d.type == TextureType::Cube ? 6u
             : d.type == TextureType::Tex2DArray ? static_cast<std::uint32_t>(d.depth) : 1u;
    ci.arrayLayers = t.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    // Any texture can be drawn into in OpenGL, and the engine does attach ones
    // made "only" to be sampled.
    if (!depth && (feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
        ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (depth && (feat & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((d.usage & UsageStorage) && (feat & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (d.type == TextureType::Cube) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (d.type == TextureType::Tex3D) ci.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    FZ_VK(vmaCreateImage(m_vma, &ci, &ai, &t.image, &t.alloc, nullptr));

    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = d.type == TextureType::Tex3D ? VK_IMAGE_VIEW_TYPE_3D
                : d.type == TextureType::Cube ? VK_IMAGE_VIEW_TYPE_CUBE
                : d.type == TextureType::Tex2DArray ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                    : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.fmt.format;
    // Sampled, a depth-stencil image is its depth.
    vi.subresourceRange = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : t.fmt.aspect, 0,
                           ci.mipLevels, 0, t.layers};
    FZ_VK(vkCreateImageView(m_device, &vi, nullptr, &t.view));
}

void VulkanDevice::clearNewImage(VkTex& t) {
    // From nothing to GENERAL, the one layout it will ever have, and cleared:
    // colour to zero, depth to the far plane.
    enterSegment(Segment::Transfer);
    transferTouch(t.image);
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.image = t.image;
    ib.subresourceRange = {t.fmt.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(m_cmd, &di);
    const VkImageSubresourceRange all = ib.subresourceRange;
    if (t.fmt.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) {
        const VkClearDepthStencilValue v{1.0f, 0};
        vkCmdClearDepthStencilImage(m_cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &all);
    } else {
        const VkClearColorValue v{};
        vkCmdClearColorImage(m_cmd, t.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &all);
    }
}

TextureHandle VulkanDevice::createTexture(const TextureDesc& descIn, const void* data, Format dataFormat) {
    VkTex t;
    t.desc = descIn;
    TextureDesc& d = t.desc;
    d.width  = std::max(1, d.width);
    d.height = std::max(1, d.height);
    d.depth  = (d.type == TextureType::Tex2DArray || d.type == TextureType::Tex3D) ? std::max(1, d.depth) : 1;
    if (d.mipLevels <= 0) {
        int levels = 1;
        for (int s = std::max({d.width, d.height, d.type == TextureType::Tex3D ? d.depth : 1}); s > 1; s >>= 1)
            ++levels;
        d.mipLevels = levels;
    }
    t.fmt = m_formats.info(d.format);
    createImage(t);
    clearNewImage(t);
    const TextureHandle h{m_textures.add(std::move(t))};
    if (data) {
        TextureRegion r;
        r.d = d.type == TextureType::Cube ? 6 : d.depth;
        updateTexture(h, r, dataFormat == Format::Unknown ? d.format : dataFormat, data);
    }
    return h;
}

void VulkanDevice::freeTexture(VkTex& t) {
    for (auto& [k, v] : t.attachViews) vkDestroyImageView(m_device, v, nullptr);
    for (auto& [k, v] : t.storageViews) vkDestroyImageView(m_device, v, nullptr);
    if (t.view) vkDestroyImageView(m_device, t.view, nullptr);
    if (t.image) vmaDestroyImage(m_vma, t.image, t.alloc);
    t = VkTex{};
}

void VulkanDevice::destroy(TextureHandle h) {
    VkTex* t = m_textures.find(h.id);
    if (!t) return;
    for (Unit& u : m_units) {
        if (u.last == h) u.last = TextureHandle{};
        for (TextureHandle& k : u.byKind) if (k == h) k = TextureHandle{};
    }
    for (ImageUnit& u : m_imageUnits) if (u.tex == h) u = ImageUnit{};
    // A framebuffer being drawn into that names it: the rendering scope ends
    // here, before the image goes.
    if (m_rendering) {
        const FramebufferDesc& d = currentDesc();
        bool attached = d.depth.texture == h;
        for (const Attachment& a : d.color) attached = attached || a.texture == h;
        if (attached) endRendering();
    }
    auto doomed = std::make_shared<VkTex>(std::move(*t));
    m_textures.remove(h.id);
    deferDelete([this, doomed] { freeTexture(*doomed); });
}

void VulkanDevice::updateTexture(TextureHandle h, const TextureRegion& r, Format dataFormat,
                                 const void* data) {
    VkTex* t = m_textures.find(h.id);
    if (!t || !data) return;
    const TextureDesc& d = t->desc;
    const Format from = dataFormat == Format::Unknown ? d.format : dataFormat;
    const Format to = t->fmt.stored;
    const int w = r.w > 0 ? r.w : mipSize(d.width, r.level);
    const int hh = r.h > 0 ? r.h : mipSize(d.height, r.level);
    int depthCount = 1;
    if (d.type == TextureType::Tex3D) depthCount = r.d > 0 ? r.d : mipSize(d.depth, r.level);
    else if (d.type == TextureType::Tex2DArray) depthCount = r.d > 0 ? r.d : d.depth - r.z;
    else if (d.type == TextureType::Cube) depthCount = r.d > 0 ? r.d : 1;
    const std::size_t srcRow = static_cast<std::size_t>(r.rowLength > 0 ? r.rowLength : w);
    const std::size_t srcPx = bytesPerPixel(from), dstPx = bytesPerPixel(to);
    const std::size_t dstSlice = static_cast<std::size_t>(w) * hh * dstPx;
    const std::size_t total = dstSlice * static_cast<std::size_t>(depthCount);

    enterSegment(Segment::Transfer);
    transferTouch(t->image);
    const Staging s = stage(total, 16);
    const auto* src = static_cast<const unsigned char*>(data);
    for (int z = 0; z < depthCount; ++z)
        for (int y = 0; y < hh; ++y)
            convertPixels(from, src + ((static_cast<std::size_t>(z) * hh + y) * srcRow) * srcPx, to,
                          s.ptr + static_cast<std::size_t>(z) * dstSlice +
                              static_cast<std::size_t>(y) * w * dstPx,
                          static_cast<std::size_t>(w));
    VkBufferImageCopy c{};
    c.bufferOffset = s.offset;
    const bool depth = (t->fmt.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    c.imageSubresource.aspectMask = depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : t->fmt.aspect;
    c.imageSubresource.mipLevel = static_cast<std::uint32_t>(r.level);
    if (d.type == TextureType::Tex3D) {
        c.imageSubresource.baseArrayLayer = 0;
        c.imageSubresource.layerCount = 1;
        c.imageOffset = {r.x, r.y, r.z};
        c.imageExtent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(hh),
                         static_cast<std::uint32_t>(depthCount)};
    } else {
        c.imageSubresource.baseArrayLayer = static_cast<std::uint32_t>(r.z);
        c.imageSubresource.layerCount = static_cast<std::uint32_t>(depthCount);
        c.imageOffset = {r.x, r.y, 0};
        c.imageExtent = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(hh), 1};
    }
    vkCmdCopyBufferToImage(m_cmd, s.buffer, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

void VulkanDevice::generateMipmaps(TextureHandle h) {
    VkTex* t = m_textures.find(h.id);
    if (!t || t->desc.mipLevels < 2 || (t->fmt.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(m_gpu, t->fmt.format, &fp);
    const VkFilter filter = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                                ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    enterSegment(Segment::Transfer);
    const TextureDesc& d = t->desc;
    for (int level = 1; level < d.mipLevels; ++level) {
        fullBarrier();   // the level above is written before it is read
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<std::uint32_t>(level - 1), 0, t->layers};
        b.srcOffsets[1] = {mipSize(d.width, level - 1), mipSize(d.height, level - 1),
                           d.type == TextureType::Tex3D ? mipSize(d.depth, level - 1) : 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, static_cast<std::uint32_t>(level), 0, t->layers};
        b.dstOffsets[1] = {mipSize(d.width, level), mipSize(d.height, level),
                           d.type == TextureType::Tex3D ? mipSize(d.depth, level) : 1};
        vkCmdBlitImage(m_cmd, t->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       filter);
    }
    m_transferImages.clear();
    m_transferBuffers.clear();
    m_transferImages.push_back(t->image);
}

void VulkanDevice::setSampler(TextureHandle h, const SamplerDesc& s) {
    if (VkTex* t = m_textures.find(h.id)) t->desc.sampler = s;
}

const TextureDesc& VulkanDevice::textureDesc(TextureHandle h) const {
    static const TextureDesc none{};
    const VkTex* t = m_textures.find(h.id);
    return t ? t->desc : none;
}

VkSampler VulkanDevice::sampler(const SamplerDesc& s) {
    struct Key {
        std::uint32_t minF, magF, mip, u, v, w, maxLevel, border;
        float aniso;
    } k{};
    k.minF = static_cast<std::uint32_t>(s.minFilter);
    k.magF = static_cast<std::uint32_t>(s.magFilter);
    k.mip = static_cast<std::uint32_t>(s.mipFilter);
    k.u = static_cast<std::uint32_t>(s.wrapU);
    k.v = static_cast<std::uint32_t>(s.wrapV);
    k.w = static_cast<std::uint32_t>(s.wrapW);
    k.maxLevel = static_cast<std::uint32_t>(std::max(s.maxLevel, 0));
    // Vulkan's border is one of three colours: the nearest of them.
    const float lum = s.borderColor.r + s.borderColor.g + s.borderColor.b;
    k.border = s.borderColor.a < 0.5f ? 0u : (lum > 1.5f ? 2u : 1u);
    k.aniso = std::clamp(s.anisotropy, 1.0f, m_maxAniso);
    std::uint64_t hash = 1469598103934665603ull;
    const auto* p = reinterpret_cast<const unsigned char*>(&k);
    for (std::size_t i = 0; i < sizeof(k); ++i) { hash ^= p[i]; hash *= 1099511628211ull; }
    if (auto it = m_samplers.find(hash); it != m_samplers.end()) return it->second;

    auto wrap = [](Wrap w) {
        switch (w) {
            case Wrap::Repeat:         return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case Wrap::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
            case Wrap::ClampToEdge:    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case Wrap::ClampToBorder:  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    };
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = s.magFilter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = s.minFilter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = s.mipFilter == MipFilter::Linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                     : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap(s.wrapU);
    ci.addressModeV = wrap(s.wrapV);
    ci.addressModeW = wrap(s.wrapW);
    ci.anisotropyEnable = k.aniso > 1.0f;
    ci.maxAnisotropy = k.aniso;
    ci.minLod = 0.0f;
    // No mip filter: level 0 only, as OpenGL samples a texture whose minifying
    // filter names no mip level.
    ci.maxLod = s.mipFilter == MipFilter::None ? 0.0f : static_cast<float>(k.maxLevel);
    ci.borderColor = k.border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                   : k.border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                   : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler smp = VK_NULL_HANDLE;
    FZ_VK(vkCreateSampler(m_device, &ci, nullptr, &smp));
    m_samplers[hash] = smp;
    return smp;
}

VkImageView VulkanDevice::attachView(VkTex& t, int level, int layer) {
    const std::uint32_t key = (static_cast<std::uint32_t>(level) << 16) |
                              static_cast<std::uint32_t>(std::max(layer, 0));
    if (auto it = t.attachViews.find(key); it != t.attachViews.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.fmt.format;
    vi.subresourceRange = {t.fmt.aspect, static_cast<std::uint32_t>(level), 1,
                           static_cast<std::uint32_t>(std::max(layer, 0)), 1};
    VkImageView v = VK_NULL_HANDLE;
    FZ_VK(vkCreateImageView(m_device, &vi, nullptr, &v));
    t.attachViews[key] = v;
    return v;
}

VkImageView VulkanDevice::storageView(VkTex& t, int level) {
    const auto key = static_cast<std::uint32_t>(level);
    if (auto it = t.storageViews.find(key); it != t.storageViews.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = t.image;
    vi.viewType = t.desc.type == TextureType::Tex3D ? VK_IMAGE_VIEW_TYPE_3D
                : t.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = t.fmt.format;
    vi.subresourceRange = {t.fmt.aspect, key, 1, 0, t.layers};
    VkImageView v = VK_NULL_HANDLE;
    FZ_VK(vkCreateImageView(m_device, &vi, nullptr, &v));
    t.storageViews[key] = v;
    return v;
}

// --- Framebuffers -----------------------------------------------------------------

FramebufferHandle VulkanDevice::createFramebuffer(const FramebufferDesc& desc) {
    return FramebufferHandle{m_framebuffers.add(VkFb{desc})};
}

void VulkanDevice::destroy(FramebufferHandle h) {
    if (!m_framebuffers.find(h.id)) return;
    if (m_fb == h) {
        endRendering();
        m_fb = FramebufferHandle{};
    }
    m_framebuffers.remove(h.id);
}

void VulkanDevice::setAttachments(FramebufferHandle h, const FramebufferDesc& desc) {
    VkFb* f = m_framebuffers.find(h.id);
    if (!f) return;
    if (m_fb == h) endRendering();
    f->desc = desc;
}

const FramebufferDesc& VulkanDevice::attachments(FramebufferHandle h) const {
    static const FramebufferDesc none{};
    const VkFb* f = m_framebuffers.find(h.id);
    return f ? f->desc : none;
}

bool VulkanDevice::framebufferComplete(FramebufferHandle h) {
    const VkFb* f = m_framebuffers.find(h.id);
    if (!f) return false;
    bool any = false;
    for (const Attachment& a : f->desc.color) {
        if (!a.texture) continue;
        const VkTex* t = m_textures.find(a.texture.id);
        if (!t || a.level >= t->desc.mipLevels) return false;
        any = true;
    }
    if (f->desc.depth.texture) {
        const VkTex* t = m_textures.find(f->desc.depth.texture.id);
        if (!t || !(t->fmt.aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
        any = true;
    }
    return any;
}

bool VulkanDevice::source(FramebufferHandle fb, int attachment, Source& out) {
    const VkFb* f = m_framebuffers.find(fb.id);
    const FramebufferDesc& d = f ? f->desc : m_windowDesc;
    Attachment a;
    if (!f && attachment == kReadFront) {
        a.texture = m_back[m_backIndex ^ 1];
    } else if (attachment == kReadDepth) {
        a = d.depth;
    } else if (attachment >= 0 && attachment < kMaxColorAttachments) {
        a = d.color[attachment];
    } else if (!f) {
        a = d.color[0];
    }
    VkTex* t = m_textures.find(a.texture.id);
    if (!t) return false;
    const bool depth = (t->fmt.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    out = {t, a.level, std::max(a.layer, 0),
           depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT)};
    return true;
}

namespace {
VkImageSubresourceLayers layersOf(const VkTex& t, VkImageAspectFlags aspect, int level, int layer) {
    if (t.desc.type == TextureType::Tex3D) return {aspect, static_cast<std::uint32_t>(level), 0, 1};
    return {aspect, static_cast<std::uint32_t>(level), static_cast<std::uint32_t>(layer), 1};
}
std::int32_t sliceOf(const VkTex& t, int layer) {
    return t.desc.type == TextureType::Tex3D ? layer : 0;
}
} // namespace

void VulkanDevice::copyImageToBuffer(const Source& s, const Rect& r, VkBuffer dst, std::size_t offset) {
    enterSegment(Segment::Transfer);
    transferTouch(dst);
    VkBufferImageCopy c{};
    c.bufferOffset = offset;
    c.imageSubresource = layersOf(*s.tex, s.aspect, s.level, s.layer);
    c.imageOffset = {r.x, r.y, sliceOf(*s.tex, s.layer)};
    c.imageExtent = {static_cast<std::uint32_t>(r.w), static_cast<std::uint32_t>(r.h), 1};
    vkCmdCopyImageToBuffer(m_cmd, s.tex->image, VK_IMAGE_LAYOUT_GENERAL, dst, 1, &c);
}

void VulkanDevice::readImage(const Source& s, const Rect& r, Format dataFormat, void* out) {
    const Format stored = s.aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? Format::Depth32F : s.tex->fmt.stored;
    const std::size_t count = static_cast<std::size_t>(r.w) * r.h;
    VkBuf tmp;
    tmp.kind = BufferKind::Readback;
    allocBuffer(tmp, count * bytesPerPixel(stored));
    copyImageToBuffer(s, r, tmp.buffer, 0);
    waitFor(pendingValue());
    vmaInvalidateAllocation(m_vma, tmp.alloc, 0, VK_WHOLE_SIZE);
    convertPixels(stored, tmp.mapped, dataFormat == Format::Unknown ? stored : dataFormat, out, count);
    freeBuffer(tmp);
}

void VulkanDevice::readTexture(TextureHandle h, int level, int layer, Format dataFormat, void* out) {
    VkTex* t = m_textures.find(h.id);
    if (!t || !out) return;
    const Format fmt = dataFormat == Format::Unknown ? t->desc.format : dataFormat;
    const int w = mipSize(t->desc.width, level), hh = mipSize(t->desc.height, level);
    const int layers = t->desc.type == TextureType::Tex3D ? mipSize(t->desc.depth, level)
                                                           : static_cast<int>(t->layers);
    const std::size_t slice = static_cast<std::size_t>(w) * hh * bytesPerPixel(fmt);
    const int first = layer >= 0 ? layer : 0, count = layer >= 0 ? 1 : layers;
    const bool depth = (t->fmt.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
    for (int i = 0; i < count; ++i) {
        const Source s{t, level, first + i,
                       depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT) : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT)};
        readImage(s, Rect{0, 0, w, hh}, fmt, static_cast<unsigned char*>(out) + slice * i);
    }
}

void VulkanDevice::readPixels(FramebufferHandle fb, int attachment, const Rect& r, Format dataFormat,
                              void* out) {
    if (!out || r.w <= 0 || r.h <= 0) return;
    Source s;
    if (!source(fb, attachment, s)) return;
    readImage(s, r, dataFormat, out);
}

void VulkanDevice::readPixelsToBuffer(FramebufferHandle fb, int attachment, const Rect& r,
                                      Format dataFormat, BufferHandle dst, std::size_t offset) {
    VkBuf* b = m_buffers.find(dst.id);
    if (!b || r.w <= 0 || r.h <= 0) return;
    Source s;
    if (!source(fb, attachment, s)) return;
    const Format stored = s.aspect == VK_IMAGE_ASPECT_DEPTH_BIT ? Format::Depth32F : s.tex->fmt.stored;
    if (stored != dataFormat) {
        // A copy moves bytes: the data comes as the image holds it. Said once.
        static bool told = false;
        if (!told) std::fprintf(stderr, "[Fitzel] Vulkan: readPixelsToBuffer cannot convert formats\n");
        told = true;
    }
    copyImageToBuffer(s, r, b->buffer, offset);
    b->lastWrite = pendingValue();
}

void VulkanDevice::blit(FramebufferHandle src, const Rect& sr, FramebufferHandle dst, const Rect& dr,
                        std::uint8_t mask, Filter filter, int srcAttachment) {
    enterSegment(Segment::Transfer);
    auto one = [&](const Source& a, const Source& b, VkFilter f) {
        transferTouch(b.tex->image);
        if (sr.w == dr.w && sr.h == dr.h && a.tex->fmt.format == b.tex->fmt.format) {
            VkImageCopy c{};
            c.srcSubresource = layersOf(*a.tex, a.aspect, a.level, a.layer);
            c.srcOffset = {sr.x, sr.y, sliceOf(*a.tex, a.layer)};
            c.dstSubresource = layersOf(*b.tex, b.aspect, b.level, b.layer);
            c.dstOffset = {dr.x, dr.y, sliceOf(*b.tex, b.layer)};
            c.extent = {static_cast<std::uint32_t>(sr.w), static_cast<std::uint32_t>(sr.h), 1};
            vkCmdCopyImage(m_cmd, a.tex->image, VK_IMAGE_LAYOUT_GENERAL, b.tex->image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            return;
        }
        VkImageBlit bl{};
        bl.srcSubresource = layersOf(*a.tex, a.aspect, a.level, a.layer);
        bl.srcOffsets[0] = {sr.x, sr.y, sliceOf(*a.tex, a.layer)};
        bl.srcOffsets[1] = {sr.x + sr.w, sr.y + sr.h, sliceOf(*a.tex, a.layer) + 1};
        bl.dstSubresource = layersOf(*b.tex, b.aspect, b.level, b.layer);
        bl.dstOffsets[0] = {dr.x, dr.y, sliceOf(*b.tex, b.layer)};
        bl.dstOffsets[1] = {dr.x + dr.w, dr.y + dr.h, sliceOf(*b.tex, b.layer) + 1};
        vkCmdBlitImage(m_cmd, a.tex->image, VK_IMAGE_LAYOUT_GENERAL, b.tex->image, VK_IMAGE_LAYOUT_GENERAL,
                       1, &bl, f);
    };
    if (mask & BlitColor) {
        Source a;
        if (source(src, srcAttachment, a)) {
            const VkFb* f = m_framebuffers.find(dst.id);
            const FramebufferDesc& d = f ? f->desc : m_windowDesc;
            for (int i = 0; i < kMaxColorAttachments; ++i) {
                if (!d.color[i].texture) continue;
                Source b;
                if (source(dst, i, b))
                    one(a, b, filter == Filter::Linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            }
        }
    }
    if (mask & BlitDepth) {
        Source a, b;
        if (source(src, kReadDepth, a) && source(dst, kReadDepth, b)) one(a, b, VK_FILTER_NEAREST);
    }
}

void VulkanDevice::copyToTexture(FramebufferHandle src, int srcAttachment, const Rect& sr,
                                 TextureHandle dstH, int level, int layer, int dstX, int dstY) {
    VkTex* t = m_textures.find(dstH.id);
    Source a;
    if (!t || !source(src, srcAttachment, a)) return;
    enterSegment(Segment::Transfer);
    transferTouch(t->image);
    const int l = std::max(layer, 0);
    if (a.tex->fmt.format == t->fmt.format) {
        VkImageCopy c{};
        c.srcSubresource = layersOf(*a.tex, a.aspect, a.level, a.layer);
        c.srcOffset = {sr.x, sr.y, sliceOf(*a.tex, a.layer)};
        c.dstSubresource = layersOf(*t, a.aspect, level, l);
        c.dstOffset = {dstX, dstY, sliceOf(*t, l)};
        c.extent = {static_cast<std::uint32_t>(sr.w), static_cast<std::uint32_t>(sr.h), 1};
        vkCmdCopyImage(m_cmd, a.tex->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else {
        VkImageBlit bl{};
        bl.srcSubresource = layersOf(*a.tex, a.aspect, a.level, a.layer);
        bl.srcOffsets[0] = {sr.x, sr.y, sliceOf(*a.tex, a.layer)};
        bl.srcOffsets[1] = {sr.x + sr.w, sr.y + sr.h, sliceOf(*a.tex, a.layer) + 1};
        bl.dstSubresource = layersOf(*t, a.aspect, level, l);
        bl.dstOffsets[0] = {dstX, dstY, sliceOf(*t, l)};
        bl.dstOffsets[1] = {dstX + sr.w, dstY + sr.h, sliceOf(*t, l) + 1};
        vkCmdBlitImage(m_cmd, a.tex->image, VK_IMAGE_LAYOUT_GENERAL, t->image, VK_IMAGE_LAYOUT_GENERAL, 1,
                       &bl, VK_FILTER_NEAREST);
    }
}

// --- Vertex arrays ----------------------------------------------------------------

namespace {
std::vector<float> constantData(const VertexArrayDesc& d) {
    std::vector<float> v;
    for (std::size_t i = 0; i < d.constantCount; ++i) {
        const glm::vec4& c = d.constants[i].value;
        v.insert(v.end(), {c.x, c.y, c.z, c.w});
    }
    return v;
}
} // namespace

VertexArrayHandle VulkanDevice::createVertexArray(const VertexArrayDesc& desc) {
    VkVao va;
    va.attrs.assign(desc.attributes, desc.attributes + desc.attributeCount);
    va.consts.assign(desc.constants, desc.constants + desc.constantCount);
    va.index = desc.indexBuffer;
    if (!va.consts.empty()) {
        const std::vector<float> data = constantData(desc);
        va.constBuffer = createBuffer(BufferKind::Vertex, BufferUsage::Static, data.size() * 4, data.data());
    }
    return VertexArrayHandle{m_vertexArrays.add(std::move(va))};
}

void VulkanDevice::destroy(VertexArrayHandle h) {
    VkVao* va = m_vertexArrays.find(h.id);
    if (!va) return;
    if (va->constBuffer) destroy(va->constBuffer);
    m_vertexArrays.remove(h.id);
    if (m_vao == h) m_vao = VertexArrayHandle{};
}

void VulkanDevice::setVertexArray(VertexArrayHandle h, const VertexArrayDesc& desc) {
    VkVao* va = m_vertexArrays.find(h.id);
    if (!va) return;
    va->attrs.assign(desc.attributes, desc.attributes + desc.attributeCount);
    va->index = desc.indexBuffer;
    // The constants are uploaded only when they change: re-pointing an array
    // per draw (the trees, the grass tiles) must not cost a copy each time.
    bool same = va->consts.size() == desc.constantCount;
    for (std::size_t i = 0; same && i < desc.constantCount; ++i)
        same = va->consts[i].location == desc.constants[i].location &&
               va->consts[i].value == desc.constants[i].value;
    if (same) return;
    va->consts.assign(desc.constants, desc.constants + desc.constantCount);
    if (va->constBuffer) destroy(va->constBuffer);
    va->constBuffer = BufferHandle{};
    if (!va->consts.empty()) {
        const std::vector<float> data = constantData(desc);
        va->constBuffer = createBuffer(BufferKind::Vertex, BufferUsage::Static, data.size() * 4, data.data());
    }
}

} // namespace fitzel::rhi::vk
