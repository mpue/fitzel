#pragma once

// The OpenGL backend: OpenGL 4.3 core where the driver has it, 3.3 core where it
// does not, and WebGL2 (GLSL ES 3.00) in the browser. The only code in the tree
// that includes <glad/gl.h>.
//
// Private to the engine: nothing outside src/rhi/gl includes this header.

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <glad/gl.h>

#include "fitzel/rhi/Device.hpp"

namespace fitzel::rhi::gl {

bool prepareWindowHints(int attempt);
std::unique_ptr<Device> createDevice(GLFWwindow* window, bool vsync);

// Handles are slots in one of these: index + 1, so that 0 stays "none". Freed
// slots are reused, as OpenGL reuses names.
template <class T>
class Pool {
public:
    std::uint32_t add(T item) {
        if (!m_free.empty()) {
            const std::uint32_t i = m_free.back();
            m_free.pop_back();
            m_items[i] = std::move(item);
            m_live[i] = true;
            return i + 1;
        }
        m_items.push_back(std::move(item));
        m_live.push_back(true);
        return static_cast<std::uint32_t>(m_items.size());
    }
    T* find(std::uint32_t id) {
        if (id == 0 || id > m_items.size() || !m_live[id - 1]) return nullptr;
        return &m_items[id - 1];
    }
    const T* find(std::uint32_t id) const {
        if (id == 0 || id > m_items.size() || !m_live[id - 1]) return nullptr;
        return &m_items[id - 1];
    }
    void remove(std::uint32_t id) {
        if (!find(id)) return;
        m_items[id - 1] = T{};
        m_live[id - 1] = false;
        m_free.push_back(id - 1);
    }

private:
    std::vector<T>             m_items;
    std::vector<bool>          m_live;
    std::vector<std::uint32_t> m_free;
};

struct GlBuffer {
    GLuint      name = 0;
    BufferKind  kind = BufferKind::Vertex;
    BufferUsage usage = BufferUsage::Static;
    std::size_t size = 0;
};

struct GlTexture {
    GLuint      name = 0;
    GLenum      target = 0;        // GL_TEXTURE_2D, ..., or GL_RENDERBUFFER
    bool        renderbuffer = false;
    TextureDesc desc;
};

struct GlFramebuffer {
    GLuint          name = 0;
    FramebufferDesc desc;
    GLenum          depthPoint = 0;   // where desc.depth is attached, 0 = nowhere
    GLenum          readBuffer = GL_NONE;   // the one reads default to
};

struct GlProgram {
    GLuint name = 0;
    bool   compute = false;
};

struct GlVertexArray {
    GLuint                         name = 0;
    std::uint64_t                  enabled = 0; // bit i: attribute location i is on
    std::vector<ConstantAttribute> constants;
    BufferHandle                   indexBuffer;
};

struct GlQuery {
    GLuint    name = 0;
    QueryType type = QueryType::TimeElapsed;
};

struct GlFence {
    GLsync sync = nullptr;
};

constexpr int kUnits = 32;

class GlDevice final : public Device {
public:
    GlDevice(GLFWwindow* window, bool vsync);
    ~GlDevice() override;

    const Caps& caps() const override { return m_caps; }

    void activate() override;
    void present() override;
    void setVsync(bool on) override;
    void finish() override;
    void flush() override;
    void resyncState() override;
    bool drainErrors() override;
    bool videoMemory(std::int64_t& usedKB, std::int64_t& totalKB) override;

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

    void setViewport(const Rect& r) override;
    Rect viewport() const override { return m_viewport; }
    void setScissor(const Rect& r) override;
    Rect scissor() const override { return m_scissor; }

    void setClearColor(const glm::vec4& c) override;
    glm::vec4 clearColor() const override { return m_clearColor; }
    void setClearDepth(float d) override;
    void setClearStencil(int s) override;
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
    void bindProgram(ProgramHandle) override;
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
    void bindVertexArray(VertexArrayHandle) override;
    VertexArrayHandle boundVertexArray() const override { return m_vao; }

    void setRenderState(const RenderState& s) override;
    const RenderState& renderState() const override { return m_state; }

    void draw(Primitive prim, int first, int count, int instances) override;
    void drawIndexed(Primitive prim, int count, IndexType type, std::size_t byteOffset,
                     int instances) override;
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
    std::uint64_t imguiTextureId(TextureHandle) const override;
    ImGuiCallback imguiTextureSamplerCallback() const override;

private:
    // The real OpenGL state, issued unconditionally (resyncState, start-up).
    void applyAll();
    void applyState(const RenderState& s, bool force);

    // Edit a texture or buffer without disturbing what is bound for drawing:
    // these bind on a scratch target or the active unit, then put back what the
    // device remembers was there.
    void bindForEdit(const GlTexture& t);
    void restoreAfterEdit(const GlTexture& t);
    void activeUnit(int unit);
    // Bind `b` where an upload or read can reach it without touching VAO state.
    GLenum bindBufferForTransfer(const GlBuffer& b);
    void unbindBufferForTransfer(GLenum target);

    // The GL objects behind the device's own bindings, as they should be now.
    void rebindFramebuffer();
    GLuint glFramebufferName(FramebufferHandle) const;
    void attach(GlFramebuffer& fb, const FramebufferDesc& desc);
    void attachOne(GLenum point, const Attachment& a);
    // Bind `fb` for reading attachment `attachment` (colour index, kReadDepth,
    // kReadFront); returns false when there is nothing to read there.
    bool bindForRead(FramebufferHandle fb, int attachment);
    void restoreRead(FramebufferHandle fb);
    void applySampler(GLenum target, const SamplerDesc& s);
    void specifyVertexArray(GlVertexArray& va, const VertexArrayDesc& desc);
    GLenum textureTarget(const GlTexture& t, int layer) const;

    GLFWwindow* m_window = nullptr;
    Caps        m_caps;
    enum class VramSource { None, Nvx, Ati } m_vram = VramSource::None;

    Pool<GlBuffer>      m_buffers;
    Pool<GlTexture>     m_textures;
    Pool<GlFramebuffer> m_framebuffers;
    Pool<GlProgram>     m_programs;
    Pool<GlVertexArray> m_vertexArrays;
    Pool<GlQuery>       m_queries;
    Pool<GlFence>       m_fences;

    // What is bound, as the device's caller set it.
    RenderState       m_state;
    Rect              m_viewport;
    Rect              m_scissor;
    glm::vec4         m_clearColor{0.0f};
    float             m_clearDepth = 1.0f;
    int               m_clearStencil = 0;
    FramebufferHandle m_fb;
    ProgramHandle     m_program;
    VertexArrayHandle m_vao;
    // Per unit: the texture the caller bound last, and what is on each GL target
    // there (a unit holds one texture per target in OpenGL).
    std::array<TextureHandle, kUnits> m_unit{};
    std::array<std::array<GLuint, 4>, kUnits> m_unitTargets{};
    int               m_activeUnit = 0;
};

} // namespace fitzel::rhi::gl
