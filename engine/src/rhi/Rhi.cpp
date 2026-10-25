// What every backend shares: the format tables, the current device, and the
// switch from a Backend value to the code that implements it.

#include "fitzel/rhi/Device.hpp"

#include <stdexcept>
#include <string>

#include "gl/GlDevice.hpp"
#ifdef FITZEL_RHI_VULKAN
#include "vk/VulkanDevice.hpp"
#endif

namespace fitzel::rhi {

bool isDepthFormat(Format f) {
    switch (f) {
        case Format::Depth16: case Format::Depth24: case Format::Depth32F:
        case Format::Depth24Stencil8: case Format::Depth32FStencil8:
            return true;
        default:
            return false;
    }
}

bool hasStencil(Format f) {
    return f == Format::Depth24Stencil8 || f == Format::Depth32FStencil8;
}

bool isFloatFormat(Format f) {
    switch (f) {
        case Format::R16F: case Format::RG16F: case Format::RGB16F: case Format::RGBA16F:
        case Format::R32F: case Format::RG32F: case Format::RGB32F: case Format::RGBA32F:
            return true;
        default:
            return false;
    }
}

int channelCount(Format f) {
    switch (f) {
        case Format::R8: case Format::R16F: case Format::R32F: case Format::R32UI:
            return 1;
        case Format::RG8: case Format::RG16F: case Format::RG32F:
            return 2;
        case Format::RGB8: case Format::RGB16F: case Format::RGB32F:
            return 3;
        case Format::RGBA8: case Format::RGBA16F: case Format::RGBA32F:
            return 4;
        case Format::Unknown:
            return 0;
        default:
            return 1;   // depth (and stencil, read as one value)
    }
}

std::size_t bytesPerPixel(Format f) {
    switch (f) {
        case Format::R8:      return 1;
        case Format::RG8:     return 2;
        case Format::RGB8:    return 3;
        case Format::RGBA8:   return 4;
        case Format::R16F:    return 2;
        case Format::RG16F:   return 4;
        case Format::RGB16F:  return 6;
        case Format::RGBA16F: return 8;
        case Format::R32F:    return 4;
        case Format::RG32F:   return 8;
        case Format::RGB32F:  return 12;
        case Format::RGBA32F: return 16;
        case Format::R32UI:   return 4;
        case Format::Depth16: return 2;
        case Format::Depth24: return 4;
        case Format::Depth32F: return 4;
        case Format::Depth24Stencil8: return 4;
        case Format::Depth32FStencil8: return 8;
        case Format::Unknown: return 0;
    }
    return 0;
}

namespace {

// The device while there is none: every call does nothing, every resource it
// is asked for is "none". What it answers is what an empty device would.
class NullDevice final : public Device {
public:
    const Caps& caps() const override { return m_caps; }
    void activate() override {}
    void present() override {}
    void setVsync(bool, bool) override {}
    void finish() override {}
    void flush() override {}
    void resyncState() override {}
    bool drainErrors() override { return false; }
    bool videoMemory(std::int64_t&, std::int64_t&) override { return false; }

    BufferHandle createBuffer(BufferKind, BufferUsage, std::size_t, const void*) override { return {}; }
    void destroy(BufferHandle) override {}
    void bufferData(BufferHandle, std::size_t, const void*, BufferUsage) override {}
    void bufferSubData(BufferHandle, std::size_t, std::size_t, const void*) override {}
    void readBuffer(BufferHandle, std::size_t, std::size_t, void*) override {}
    std::size_t bufferSize(BufferHandle) const override { return 0; }

    TextureHandle createTexture(const TextureDesc&, const void*, Format) override { return {}; }
    void destroy(TextureHandle) override {}
    void updateTexture(TextureHandle, const TextureRegion&, Format, const void*) override {}
    void generateMipmaps(TextureHandle) override {}
    void setSampler(TextureHandle, const SamplerDesc&) override {}
    const TextureDesc& textureDesc(TextureHandle) const override { return m_texture; }
    void readTexture(TextureHandle, int, int, Format, void*) override {}

    FramebufferHandle createFramebuffer(const FramebufferDesc&) override { return {}; }
    void destroy(FramebufferHandle) override {}
    void setAttachments(FramebufferHandle, const FramebufferDesc&) override {}
    const FramebufferDesc& attachments(FramebufferHandle) const override { return m_framebuffer; }
    bool framebufferComplete(FramebufferHandle) override { return false; }
    void bindFramebuffer(FramebufferHandle) override {}
    FramebufferHandle boundFramebuffer() const override { return {}; }
    void windowFramebufferSize(int& w, int& h) const override { w = h = 0; }

    void setViewport(const Rect& r) override { m_viewport = r; }
    Rect viewport() const override { return m_viewport; }
    void setScissor(const Rect& r) override { m_scissor = r; }
    Rect scissor() const override { return m_scissor; }
    void setClearColor(const glm::vec4& c) override { m_clear = c; }
    glm::vec4 clearColor() const override { return m_clear; }
    void setClearDepth(float) override {}
    void setClearStencil(int) override {}
    void clear(std::uint8_t) override {}
    void clearColorAttachment(int, const glm::vec4&) override {}
    void blit(FramebufferHandle, const Rect&, FramebufferHandle, const Rect&, std::uint8_t, Filter,
              int) override {}
    void readPixels(FramebufferHandle, int, const Rect&, Format, void*) override {}
    void readPixelsToBuffer(FramebufferHandle, int, const Rect&, Format, BufferHandle,
                            std::size_t) override {}
    void copyToTexture(FramebufferHandle, int, const Rect&, TextureHandle, int, int, int,
                       int) override {}

    ProgramHandle createProgram(const ProgramSource&, std::string* log) override {
        if (log) *log = "no device";
        return {};
    }
    void destroy(ProgramHandle) override {}
    void bindProgram(ProgramHandle) override {}
    ProgramHandle boundProgram() const override { return {}; }
    int uniformLocation(ProgramHandle, const char*) override { return -1; }
    void setUniform(ProgramHandle, int, UniformType, const void*, int) override {}
    void readUniform(ProgramHandle, int, bool, void*) override {}

    void bindTexture(int, TextureHandle) override {}
    TextureHandle boundTexture(int) const override { return {}; }
    void bindImage(int, TextureHandle, int, Access, Format) override {}
    void bindBufferBase(BufferKind, int, BufferHandle) override {}

    VertexArrayHandle createVertexArray(const VertexArrayDesc&) override { return {}; }
    void destroy(VertexArrayHandle) override {}
    void setVertexArray(VertexArrayHandle, const VertexArrayDesc&) override {}
    void bindVertexArray(VertexArrayHandle) override {}
    VertexArrayHandle boundVertexArray() const override { return {}; }

    void setRenderState(const RenderState& s) override { m_state = s; }
    const RenderState& renderState() const override { return m_state; }

    void draw(Primitive, int, int, int) override {}
    void drawIndexed(Primitive, int, IndexType, std::size_t, int, int) override {}
    void dispatch(int, int, int) override {}
    void memoryBarrier(std::uint32_t) override {}

    QueryHandle createQuery(QueryType) override { return {}; }
    void destroy(QueryHandle) override {}
    void beginQuery(QueryHandle) override {}
    void endQuery(QueryHandle) override {}
    void timestamp(QueryHandle) override {}
    bool queryReady(QueryHandle) override { return true; }
    std::uint64_t queryResult(QueryHandle) override { return 0; }
    FenceHandle createFence() override { return {}; }
    void destroy(FenceHandle) override {}
    bool fenceSignaled(FenceHandle, std::uint64_t) override { return true; }

    void imguiInit(GLFWwindow*) override {}
    void imguiShutdown() override {}
    void imguiNewFrame() override {}
    void imguiRender(ImDrawData*) override {}
    std::uint64_t imguiTextureId(TextureHandle) const override { return 0; }
    ImGuiCallback imguiTextureSamplerCallback() const override { return nullptr; }

private:
    Caps            m_caps;
    TextureDesc     m_texture;
    FramebufferDesc m_framebuffer;
    RenderState     m_state;
    Rect            m_viewport, m_scissor;
    glm::vec4       m_clear{0.0f};
};

NullDevice g_null;
Device*    g_current = nullptr;

} // namespace

Device& device() { return g_current ? *g_current : g_null; }

bool hasDevice() { return g_current != nullptr; }

void makeCurrent(Device* d) {
    g_current = d;
    if (d) d->activate();
}

bool prepareWindowHints(Backend backend, int attempt) {
    switch (backend) {
        case Backend::OpenGL: return gl::prepareWindowHints(attempt);
        case Backend::Vulkan:
#ifdef FITZEL_RHI_VULKAN
            return vk::prepareWindowHints(attempt);
#else
            return false;
#endif
    }
    return false;
}

std::unique_ptr<Device> createDevice(Backend backend, GLFWwindow* window, bool vsync) {
    std::unique_ptr<Device> d;
    switch (backend) {
        case Backend::OpenGL: d = gl::createDevice(window, vsync); break;
        case Backend::Vulkan:
#ifdef FITZEL_RHI_VULKAN
            d = vk::createDevice(window, vsync);
#endif
            break;
    }
    if (!d) throw std::runtime_error("fitzel::rhi: no such backend");
    makeCurrent(d.get());
    return d;
}

} // namespace fitzel::rhi
