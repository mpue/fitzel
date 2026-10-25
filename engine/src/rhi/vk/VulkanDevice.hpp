#pragma once

// The Vulkan backend: Vulkan 1.3 (dynamic rendering, synchronization2, the
// extended dynamic state), loaded at run time through volk.
//
// It takes the device interface at its word -- OpenGL's model, state that is set
// and stays set -- and turns it into Vulkan at draw time:
//
//  - Commands go into one command buffer per frame, recorded as the calls come.
//    A framebuffer bind opens a rendering scope lazily (the first draw or clear
//    after it); anything that cannot happen inside one (an upload, a copy, a
//    dispatch) closes it, and the next draw reopens it with LOAD. Order is
//    exactly the order of the calls.
//  - Every image lives in VK_IMAGE_LAYOUT_GENERAL from its first use on, so
//    there are no layout transitions to track; hazards are covered by a full
//    memory barrier between segments (a rendering scope, a run of transfers, a
//    dispatch). Coarse, and correct.
//  - All buffers are device-local; every write goes through a staging copy in
//    the command buffer, in order, as glBufferSubData would have it.
//  - Pipelines are baked from the remembered state (blend, colour mask, polygon
//    mode, topology, vertex layout, attachment formats) and cached; the rest of
//    the state is dynamic.
//  - Shaders are the engine's GLSL, compiled by glslang under its relaxed
//    Vulkan rules: the loose uniforms of a program become one uniform block
//    (std140) whose CPU copy the setters write and each draw uploads into a
//    ring; samplers become combined image samplers fed from the texture units.
//  - The image is drawn the way OpenGL draws it: row 0 first in memory is the
//    bottom of the picture, clip z runs -1..1 (remapped in every vertex shader),
//    and the picture is flipped once, when it is copied to the swapchain.
//
// See docs/rhi.md.

#include <array>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "VkCommon.hpp"
#include "fitzel/rhi/Device.hpp"
#include "../Pool.hpp"

namespace fitzel::rhi::vk {

bool prepareWindowHints(int attempt);
std::unique_ptr<Device> createDevice(GLFWwindow* window, bool vsync);

class ImGuiRenderer;

// --- Resources --------------------------------------------------------------------

struct VkBuf {
    VkBuffer      buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    void*         mapped = nullptr;     // Readback buffers only
    std::size_t   size = 0;             // as the caller sees it
    std::size_t   capacity = 0;         // as allocated (never 0)
    BufferKind    kind = BufferKind::Vertex;
    BufferUsage   usage = BufferUsage::Static;
    std::uint64_t lastWrite = 0;        // submission that last wrote it (for reads)
};

struct VkTex {
    VkImage       image = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    TextureDesc   desc;
    FormatInfo    fmt;
    std::uint32_t layers = 1;           // array layers of the image (a cube: 6)
    VkImageView   view = VK_NULL_HANDLE;            // for sampling
    std::unordered_map<std::uint32_t, VkImageView> attachViews;   // level << 16 | layer
    std::unordered_map<std::uint32_t, VkImageView> storageViews;  // per level
    std::uint64_t lastWrite = 0;
};

struct VkFb {
    FramebufferDesc desc;
};

// A uniform of a program as setUniform finds it: where in the block (or which
// sampler binding), and how its elements and columns are spaced.
struct UniformLoc {
    bool          sampler = false;
    std::uint32_t offset = 0;          // block: byte offset of the element
    std::uint32_t arrayStride = 0;
    std::uint32_t matrixStride = 0;
    std::uint32_t remaining = 1;        // elements from this one to the array's end
    int           samplerIndex = -1;   // into VkProg::samplers
    std::uint32_t element = 0;          // first array element (samplers)
};

struct SamplerSlot {
    std::uint32_t    binding = 0;
    std::uint32_t    count = 1;
    int              kind = 0;          // 0 2D, 1 2D array, 2 3D, 3 cube
    std::vector<int> units;            // per element; GL's default is unit 0
};

struct ResourceSlot {
    std::uint32_t    binding = 0;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
};

struct VertexInput {
    std::uint32_t location = 0;
    int           base = 0;             // 0 float, 1 int, 2 uint
};

struct VkProg {
    bool                  compute = false;
    VkShaderModule        vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE, cs = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout      layout = VK_NULL_HANDLE;
    VkPipeline            computePipeline = VK_NULL_HANDLE;

    std::uint32_t uboBinding = UINT32_MAX;   // the default uniform block, if any
    std::vector<std::uint8_t> uniforms;      // its CPU copy
    bool          dirty = true;
    VkBuffer      uboBuffer = VK_NULL_HANDLE;   // this frame's copy in the ring
    std::uint32_t uboOffset = 0;
    std::uint64_t uboFrame = 0;

    std::unordered_map<std::string, int> names;   // uniform name -> index into locs
    std::vector<UniformLoc>   locs;
    std::vector<SamplerSlot>  samplers;
    std::vector<ResourceSlot> resources;        // storage buffers/images, explicit UBOs
    std::vector<VertexInput>  inputs;
    int clipMask = -1;                          // location of fz_ClipMask
};

struct VkVao {
    std::vector<VertexAttribute>   attrs;
    std::vector<ConstantAttribute> consts;
    BufferHandle                   index;
    BufferHandle                   constBuffer;   // the constants, a vec4 each
};

struct VkQuery {
    QueryType     type = QueryType::TimeElapsed;
    std::uint32_t first = 0;            // index in its pool (TimeElapsed: begin, end)
    std::uint64_t serial = 0;           // submission that wrote it, 0 = never
    bool          active = false;
};

struct VkFenceH {
    std::uint64_t value = 0;            // the timeline value that signals it
};

// --- Per-frame -----------------------------------------------------------------

struct Chunk {
    VkBuffer      buffer = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    unsigned char* mapped = nullptr;
    std::size_t   size = 0, used = 0;
};

struct Frame {
    VkCommandPool                pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;
    std::size_t                  cmdUsed = 0;
    std::uint64_t                lastValue = 0;     // its last submission
    VkSemaphore                  acquire = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> descPools;
    std::size_t                  descPoolUsed = 0;
    std::unordered_map<std::uint64_t, VkDescriptorSet> sets;
    std::vector<Chunk>           staging;
    std::vector<Chunk>           uniforms;
};

// --- The device -------------------------------------------------------------------

class VulkanDevice final : public Device {
public:
    VulkanDevice(GLFWwindow* window, bool vsync);
    ~VulkanDevice() override;

    const Caps& caps() const override { return m_caps; }

    void activate() override {}
    void present() override;
    void setVsync(bool on, bool adaptive) override;
    void finish() override;
    void flush() override;
    void resyncState() override;
    bool videoMemory(std::int64_t& usedKB, std::int64_t& totalKB) override;
    bool drainErrors() override { return false; }

    BufferHandle createBuffer(BufferKind kind, BufferUsage usage, std::size_t bytes,
                              const void* data) override;
    void destroy(BufferHandle) override;
    void bufferData(BufferHandle, std::size_t bytes, const void* data, BufferUsage usage) override;
    void bufferSubData(BufferHandle, std::size_t offset, std::size_t bytes, const void* data) override;
    void readBuffer(BufferHandle, std::size_t offset, std::size_t bytes, void* out) override;
    std::size_t bufferSize(BufferHandle) const override;

    TextureHandle createTexture(const TextureDesc& desc, const void* data, Format dataFormat) override;
    void destroy(TextureHandle) override;
    void updateTexture(TextureHandle, const TextureRegion& region, Format dataFormat,
                       const void* data) override;
    void generateMipmaps(TextureHandle) override;
    void setSampler(TextureHandle, const SamplerDesc& sampler) override;
    const TextureDesc& textureDesc(TextureHandle) const override;
    void readTexture(TextureHandle, int level, int layer, Format dataFormat, void* out) override;

    FramebufferHandle createFramebuffer(const FramebufferDesc& desc) override;
    void destroy(FramebufferHandle) override;
    void setAttachments(FramebufferHandle, const FramebufferDesc& desc) override;
    const FramebufferDesc& attachments(FramebufferHandle) const override;
    bool framebufferComplete(FramebufferHandle) override;
    void bindFramebuffer(FramebufferHandle) override;
    FramebufferHandle boundFramebuffer() const override { return m_fb; }
    void windowFramebufferSize(int& width, int& height) const override;

    using Device::setViewport;
    void setViewport(const Rect& r) override { m_viewport = r; }
    Rect viewport() const override { return m_viewport; }
    void setScissor(const Rect& r) override { m_scissor = r; }
    Rect scissor() const override { return m_scissor; }

    void setClearColor(const glm::vec4& c) override { m_clearColor = c; }
    glm::vec4 clearColor() const override { return m_clearColor; }
    void setClearDepth(float d) override { m_clearDepth = d; }
    void setClearStencil(int s) override { m_clearStencil = s; }
    void clear(std::uint8_t mask) override;
    void clearColorAttachment(int index, const glm::vec4& value) override;

    void blit(FramebufferHandle src, const Rect& srcRect, FramebufferHandle dst,
              const Rect& dstRect, std::uint8_t mask, Filter filter, int srcAttachment) override;
    void readPixels(FramebufferHandle, int attachment, const Rect& r, Format dataFormat,
                    void* out) override;
    void readPixelsToBuffer(FramebufferHandle, int attachment, const Rect& r, Format dataFormat,
                            BufferHandle dst, std::size_t offset) override;
    void copyToTexture(FramebufferHandle src, int srcAttachment, const Rect& srcRect,
                       TextureHandle dst, int level, int layer, int dstX, int dstY) override;

    ProgramHandle createProgram(const ProgramSource& src, std::string* log) override;
    void destroy(ProgramHandle) override;
    void bindProgram(ProgramHandle h) override { m_program = h; }
    ProgramHandle boundProgram() const override { return m_program; }
    int uniformLocation(ProgramHandle program, const char* name) override;
    void setUniform(ProgramHandle program, int location, UniformType type, const void* data,
                    int count) override;
    void readUniform(ProgramHandle program, int location, bool asInt, void* out16) override;

    void bindTexture(int unit, TextureHandle) override;
    TextureHandle boundTexture(int unit) const override;
    void bindImage(int unit, TextureHandle, int level, Access access, Format format) override;
    void bindBufferBase(BufferKind kind, int index, BufferHandle) override;

    VertexArrayHandle createVertexArray(const VertexArrayDesc& desc) override;
    void destroy(VertexArrayHandle) override;
    void setVertexArray(VertexArrayHandle, const VertexArrayDesc& desc) override;
    void bindVertexArray(VertexArrayHandle h) override { m_vao = h; }
    VertexArrayHandle boundVertexArray() const override { return m_vao; }

    void setRenderState(const RenderState& s) override { m_state = s; }
    const RenderState& renderState() const override { return m_state; }

    void draw(Primitive prim, int first, int count, int instances) override;
    void drawIndexed(Primitive prim, int count, IndexType type, std::size_t byteOffset,
                     int instances, int baseVertex) override;
    void dispatch(int groupsX, int groupsY, int groupsZ) override;
    void memoryBarrier(std::uint32_t barriers) override;

    QueryHandle createQuery(QueryType type) override;
    void destroy(QueryHandle) override;
    void beginQuery(QueryHandle) override;
    void endQuery(QueryHandle) override;
    void timestamp(QueryHandle) override;
    bool queryReady(QueryHandle) override;
    std::uint64_t queryResult(QueryHandle) override;

    FenceHandle createFence() override;
    void destroy(FenceHandle) override;
    bool fenceSignaled(FenceHandle, std::uint64_t timeoutNs) override;

    void imguiInit(GLFWwindow* window) override;
    void imguiShutdown() override;
    void imguiNewFrame() override;
    void imguiRender(ImDrawData* data) override;
    std::uint64_t imguiTextureId(TextureHandle h) const override { return h.id; }
    ImGuiCallback imguiTextureSamplerCallback() const override;

    // For the ImGui renderer: sample unit 0 with this instead of the texture's
    // own sampler (null: the texture's own).
    void setUnitZeroSampler(const SamplerDesc* s) { m_unit0Sampler = s; }

private:
    friend class ImGuiRenderer;

    // --- set-up (VulkanDevice.cpp)
    void createInstance();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapchain();
    void destroySwapchain();
    void createBackbuffers(int width, int height);
    void createFrames();
    void createDummies();

    // --- submission
    VkCommandBuffer cmd();                // the command buffer being recorded
    void beginCommands();
    std::uint64_t submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE);
    void waitValue(std::uint64_t value);
    std::uint64_t completedValue() const;
    void nextFrame();
    void deferDelete(std::function<void()> f);
    void runDeferred();
    // The value the commands being recorded now will signal.
    std::uint64_t pendingValue() const { return m_submitted + 1; }
    // Make sure `value` is submitted (flushing if it is the pending one) and done.
    void waitFor(std::uint64_t value);

    // --- segments and barriers
    enum class Segment { None, Render, Transfer, Compute };
    void enterSegment(Segment s);
    void fullBarrier();
    void endRendering();
    void ensureRendering();
    void transferTouch(VkBuffer b);       // a barrier if `b` was already written in this run
    void transferTouch(VkImage i);

    // --- memory
    struct Staging { VkBuffer buffer; std::size_t offset; unsigned char* ptr; };
    Staging stage(std::size_t bytes, std::size_t align = 16);
    Staging uniformRing(std::size_t bytes);
    Chunk makeChunk(std::size_t bytes, VkBufferUsageFlags usage);
    void allocBuffer(VkBuf& b, std::size_t bytes);
    void freeBuffer(VkBuf& b);
    void uploadBuffer(VkBuf& b, std::size_t offset, std::size_t bytes, const void* data);

    // --- images (VkResources.cpp)
    void createImage(VkTex& t);
    void clearNewImage(VkTex& t);
    VkImageView attachView(VkTex& t, int level, int layer);
    VkImageView storageView(VkTex& t, int level);
    void freeTexture(VkTex& t);
    VkSampler sampler(const SamplerDesc& s);
    // The image and subresource behind a framebuffer attachment (a colour index,
    // kReadDepth, kReadFront; `fb` none: the window's).
    struct Source { VkTex* tex; int level; int layer; VkImageAspectFlags aspect; };
    bool source(FramebufferHandle fb, int attachment, Source& out);
    void copyImageToBuffer(const Source& s, const Rect& r, VkBuffer dst, std::size_t offset);
    void readImage(const Source& s, const Rect& r, Format dataFormat, void* out);

    // --- drawing (VulkanDevice.cpp)
    struct PipelineKey;
    VkPipeline graphicsPipeline(VkProg& p, Primitive prim);
    void prepareDraw(VkProg& p, Primitive prim, VkPipelineBindPoint point);
    void bindDescriptors(VkProg& p, VkPipelineBindPoint point);
    void uploadUniforms(VkProg& p);
    void bindVertexInput(VkProg& p);
    void setDynamicState();
    const FramebufferDesc& currentDesc() const;
    VkTex* textureForUnit(int unit, int kind);
    VkDescriptorPool descriptorPool();

    // --- shaders (VkShaders.cpp)
    bool buildProgram(VkProg& p, const ProgramSource& src, std::string* log);
    void freeProgram(VkProg& p);

    GLFWwindow*      m_window = nullptr;
    Caps             m_caps;
    std::string      m_apiName, m_apiVersion, m_renderer, m_vendor;
    FormatTable      m_formats;

    VkInstance       m_instance = VK_NULL_HANDLE;
    VkSurfaceKHR     m_surface = VK_NULL_HANDLE;
    VkPhysicalDevice m_gpu = VK_NULL_HANDLE;
    VkDevice         m_device = VK_NULL_HANDLE;
    VkQueue          m_queue = VK_NULL_HANDLE;
    std::uint32_t    m_queueFamily = 0;
    VmaAllocator     m_vma = nullptr;
    VkPhysicalDeviceProperties m_props{};
    bool             m_memoryBudget = false;
    bool             m_wireframe = false;
    float            m_maxAniso = 1.0f;
    VkPipelineCache  m_pipelineCache = VK_NULL_HANDLE;

    // The swapchain, and the window's framebuffer drawn before it: two colour
    // images taking turns (the one shown last is what kReadFront reads) and a
    // depth-stencil buffer.
    VkSwapchainKHR             m_swapchain = VK_NULL_HANDLE;
    VkFormat                   m_swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D                 m_swapExtent{};
    std::vector<VkImage>       m_swapImages;
    std::vector<VkSemaphore>   m_renderDone;     // per swapchain image
    bool                       m_vsync = true;
    bool                       m_adaptive = false;
    bool                       m_swapDirty = false;
    TextureHandle              m_back[2];
    TextureHandle              m_backDepth;
    int                        m_backIndex = 0;
    FramebufferDesc            m_windowDesc;
    int                        m_backW = 0, m_backH = 0;

    // Submission: a timeline semaphore counts every submit.
    VkSemaphore   m_timeline = VK_NULL_HANDLE;
    std::uint64_t m_submitted = 0;
    std::array<Frame, 2> m_frames;
    int           m_frameIndex = 0;
    std::uint64_t m_frameCounter = 1;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
    std::deque<std::pair<std::uint64_t, std::function<void()>>> m_deferred;

    Pool<VkBuf>    m_buffers;
    Pool<VkTex>    m_textures;
    Pool<VkFb>     m_framebuffers;
    Pool<VkProg>   m_programs;
    Pool<VkVao>    m_vertexArrays;
    Pool<VkQuery>  m_queries;
    Pool<VkFenceH> m_fences;

    VkQueryPool m_timestampPool = VK_NULL_HANDLE, m_occlusionPool = VK_NULL_HANDLE;
    std::vector<std::uint32_t> m_freeTimestamps, m_freeOcclusion;
    QueryHandle m_activeOcclusion;

    std::unordered_map<std::uint64_t, VkSampler>  m_samplers;
    std::unordered_map<std::uint64_t, VkPipeline> m_pipelines;
    std::unordered_map<std::uint64_t, std::uint32_t> m_pipelineOwner;   // -> program id

    // What the caller set, as OpenGL would remember it.
    RenderState       m_state;
    Rect              m_viewport, m_scissor;
    glm::vec4         m_clearColor{0.0f};
    float             m_clearDepth = 1.0f;
    int               m_clearStencil = 0;
    FramebufferHandle m_fb;
    ProgramHandle     m_program;
    VertexArrayHandle m_vao;
    struct Unit { TextureHandle last; std::array<TextureHandle, 4> byKind; };
    std::array<Unit, 32> m_units{};
    struct ImageUnit { TextureHandle tex; int level = 0; };
    std::array<ImageUnit, 8> m_imageUnits{};
    std::array<BufferHandle, 16> m_storageBuffers{}, m_uniformBuffers{};
    const SamplerDesc* m_unit0Sampler = nullptr;

    // What is recorded.
    Segment       m_segment = Segment::None;
    bool          m_rendering = false;
    VkExtent2D    m_renderExtent{};
    std::vector<VkBuffer> m_transferBuffers;   // written in the current transfer run
    std::vector<VkImage>  m_transferImages;
    VkPipeline    m_boundPipeline = VK_NULL_HANDLE;
    struct Dynamic {
        bool      valid = false;
        VkViewport viewport{};
        VkRect2D  scissor{};
        RenderState state;
    } m_dyn;

    // Stand-ins: a 1x1 (0,0,0,1) texture of each kind for a sampler with nothing
    // bound (what an incomplete texture samples as in OpenGL), and the vec4
    // (0,0,0,1) an attribute nothing feeds reads.
    std::array<TextureHandle, 4> m_dummy{};
    BufferHandle m_defaultAttrib;

    std::unique_ptr<ImGuiRenderer> m_imgui;
};

} // namespace fitzel::rhi::vk
