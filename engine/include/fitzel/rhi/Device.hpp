#pragma once

// The render hardware interface: every GPU call the engine, the editor and their
// tools make goes through a Device. The one implementation today is OpenGL
// (engine/src/rhi/gl), and it is the only code in the tree that includes an
// OpenGL header; a Vulkan backend sits next to it as a second subclass, chosen
// when the window is made (WindowConfig::backend).
//
// The model is OpenGL's where that costs another API nothing -- state is set and
// stays set, uniforms are set by name and kept per program, a texture carries its
// sampler -- and explicit where OpenGL's own way would not translate: resources
// are created whole from a description, nothing is "bound to edit it", and the
// device keeps a full copy of the state it was given, so a backend that has to
// bake state into pipeline objects has all of it at draw time. docs/rhi.md walks
// through what each call becomes in OpenGL and in Vulkan.
//
// Single-threaded like the context underneath: every call from the thread that
// made the window, between its creation and destruction.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "fitzel/rhi/Types.hpp"

struct GLFWwindow;
struct ImDrawData;
struct ImDrawList;
struct ImDrawCmd;

namespace fitzel::rhi {

class Device {
public:
    virtual ~Device() = default;

    virtual const Caps& caps() const = 0;

    // --- Frame ------------------------------------------------------------------
    // Called when this device becomes the current one (makeCurrent): on OpenGL,
    // its context becomes the thread's.
    virtual void activate() = 0;
    // Show what was drawn into the window's framebuffer.
    virtual void present() = 0;
    // Wait for the display's refresh to show a frame. `adaptive`: where the
    // driver can, a frame that misses the refresh is shown at once (a tear
    // line, briefly) instead of being held for the next one -- plain vsync turns
    // a GPU that needs 18 ms into 30 fps, not 55.
    virtual void setVsync(bool on, bool adaptive = false) = 0;
    // Wait until the GPU has done everything submitted so far.
    virtual void finish() = 0;
    // Hand everything recorded so far to the GPU without waiting.
    virtual void flush() = 0;
    // Put the API's real state back to what this device believes it is. For after
    // code the device does not see (a third-party renderer) touched the API.
    virtual void resyncState() = 0;
    // Video memory as the driver accounts it, in KB: in use and in all -- or,
    // with `totalKB` 0, only the free pool in `usedKB`. False when the driver
    // does not say (no API core has a way to ask; it is a vendor extension).
    virtual bool videoMemory(std::int64_t& usedKB, std::int64_t& totalKB) = 0;
    // Whether an API error was raised since the last call (debugging and tests;
    // a backend with validation layers may always answer false).
    virtual bool drainErrors() = 0;

    // --- Buffers ----------------------------------------------------------------
    virtual BufferHandle createBuffer(BufferKind kind, BufferUsage usage, std::size_t bytes,
                                      const void* data = nullptr) = 0;
    virtual void destroy(BufferHandle) = 0;
    // New storage of `bytes` (contents `data`, or undefined). The handle stays.
    virtual void bufferData(BufferHandle, std::size_t bytes, const void* data,
                            BufferUsage usage) = 0;
    virtual void bufferSubData(BufferHandle, std::size_t offset, std::size_t bytes,
                               const void* data) = 0;
    // Copy back to the CPU. Waits for the GPU unless the data is known to be
    // there (a Readback buffer after its fence).
    virtual void readBuffer(BufferHandle, std::size_t offset, std::size_t bytes,
                            void* out) = 0;
    virtual std::size_t bufferSize(BufferHandle) const = 0;

    // --- Textures ---------------------------------------------------------------
    // `data`, when given, fills level 0 (every layer/face/slice), laid out as
    // `dataFormat` (Unknown: the texture's own format), rows tightly packed.
    virtual TextureHandle createTexture(const TextureDesc& desc, const void* data = nullptr,
                                        Format dataFormat = Format::Unknown) = 0;
    virtual void destroy(TextureHandle) = 0;
    virtual void updateTexture(TextureHandle, const TextureRegion& region, Format dataFormat,
                               const void* data) = 0;
    virtual void generateMipmaps(TextureHandle) = 0;
    virtual void setSampler(TextureHandle, const SamplerDesc& sampler) = 0;
    // The description it was made with (its current sampler included).
    virtual const TextureDesc& textureDesc(TextureHandle) const = 0;
    // A whole mip level back to the CPU, as `dataFormat`, rows tightly packed and
    // in the order they were uploaded (row 0 first). `layer` picks a layer/face/slice; -1 reads all of them,
    // one after the other. Stalls: never inside a frame that has to be fast.
    virtual void readTexture(TextureHandle, int level, int layer, Format dataFormat,
                             void* out) = 0;

    // --- Framebuffers -----------------------------------------------------------
    virtual FramebufferHandle createFramebuffer(const FramebufferDesc& desc) = 0;
    virtual void destroy(FramebufferHandle) = 0;
    // Draw into other images from now on. Cheap enough per frame (a cube's faces,
    // an array's layers).
    virtual void setAttachments(FramebufferHandle, const FramebufferDesc& desc) = 0;
    virtual const FramebufferDesc& attachments(FramebufferHandle) const = 0;
    // Whether the attachments make a framebuffer that can be drawn into.
    virtual bool framebufferComplete(FramebufferHandle) = 0;

    // Draws and clears go to this framebuffer (none: the window's).
    virtual void bindFramebuffer(FramebufferHandle) = 0;
    virtual FramebufferHandle boundFramebuffer() const = 0;
    // Size of the window's framebuffer in pixels.
    virtual void windowFramebufferSize(int& width, int& height) const = 0;

    virtual void setViewport(const Rect& r) = 0;
    void setViewport(int x, int y, int w, int h) { setViewport(Rect{x, y, w, h}); }
    virtual Rect viewport() const = 0;
    virtual void setScissor(const Rect& r) = 0;
    virtual Rect scissor() const = 0;

    // Clear the bound framebuffer, within the scissor rectangle when the scissor
    // test is on and only where the write masks let it -- as a draw would.
    virtual void setClearColor(const glm::vec4& c) = 0;
    virtual glm::vec4 clearColor() const = 0;
    virtual void setClearDepth(float d) = 0;
    virtual void setClearStencil(int s) = 0;
    virtual void clear(std::uint8_t mask /* ClearMask bits */) = 0;
    // One colour attachment of the bound framebuffer to `value`, the others kept.
    virtual void clearColorAttachment(int index, const glm::vec4& value) = 0;

    // Copy a rectangle between framebuffers, scaled with `filter`. Colour comes
    // from colour attachment `srcAttachment` (or the window's back buffer) and
    // goes to every colour attachment of `dst`. Not clipped by the scissor.
    virtual void blit(FramebufferHandle src, const Rect& srcRect, FramebufferHandle dst,
                      const Rect& dstRect, std::uint8_t mask /* BlitMask */, Filter filter,
                      int srcAttachment = 0) = 0;
    // Pixels of a framebuffer image (a colour attachment, kReadDepth or, for the
    // window's, kReadFront) to the CPU, as `dataFormat`, bottom row first. Stalls.
    virtual void readPixels(FramebufferHandle, int attachment, const Rect& r,
                            Format dataFormat, void* out) = 0;
    // The same into a Readback buffer at `offset`, without waiting: put a fence
    // after it and readBuffer() once the fence is signalled.
    virtual void readPixelsToBuffer(FramebufferHandle, int attachment, const Rect& r,
                                    Format dataFormat, BufferHandle dst,
                                    std::size_t offset) = 0;
    // Copy a rectangle of a framebuffer image into a texture's level/layer.
    virtual void copyToTexture(FramebufferHandle src, int srcAttachment, const Rect& srcRect,
                               TextureHandle dst, int level, int layer, int dstX,
                               int dstY) = 0;

    // --- Programs ---------------------------------------------------------------
    // Compile and link. Invalid handle on failure, with the compiler's words in
    // `log` (and on stderr); on success `log` gets its warnings, if it had any.
    // Either way a stage's words come as "<stage>: <text>" (vertex, fragment,
    // compute).
    virtual ProgramHandle createProgram(const ProgramSource& src, std::string* log = nullptr) = 0;
    virtual void destroy(ProgramHandle) = 0;
    // Draws and dispatches use this program until another is bound.
    virtual void bindProgram(ProgramHandle) = 0;
    virtual ProgramHandle boundProgram() const = 0;
    // A uniform's location in `program`, -1 when it has none by that name (or
    // the compiler dropped it). Stable for the program's life: cache it.
    virtual int uniformLocation(ProgramHandle program, const char* name) = 0;
    // Set `count` elements of a uniform (an array from the element at `location`).
    // The program keeps the value until it is set again. Samplers are Int
    // uniforms whose value is the texture unit (bindTexture) they read.
    virtual void setUniform(ProgramHandle program, int location, UniformType type,
                            const void* data, int count = 1) = 0;
    // What the program holds at `location`: 16 floats' room, ints as ints.
    // Debugging only (it is a round trip to the driver).
    virtual void readUniform(ProgramHandle program, int location, bool asInt, void* out16) = 0;

    // --- Bindings ---------------------------------------------------------------
    // A texture to a unit the program's samplers can name. None unbinds.
    virtual void bindTexture(int unit, TextureHandle) = 0;
    virtual TextureHandle boundTexture(int unit) const = 0;
    // A texture level for image load/store at `unit` (compute).
    virtual void bindImage(int unit, TextureHandle, int level, Access access, Format format) = 0;
    // A buffer to an indexed uniform or storage block binding (`kind` says which).
    virtual void bindBufferBase(BufferKind kind, int index, BufferHandle) = 0;

    // --- Vertex input -----------------------------------------------------------
    virtual VertexArrayHandle createVertexArray(const VertexArrayDesc& desc) = 0;
    virtual void destroy(VertexArrayHandle) = 0;
    // Re-point the attributes (a buffer was replaced, a layout changed).
    virtual void setVertexArray(VertexArrayHandle, const VertexArrayDesc& desc) = 0;
    virtual void bindVertexArray(VertexArrayHandle) = 0;
    virtual VertexArrayHandle boundVertexArray() const = 0;

    // --- State ------------------------------------------------------------------
    virtual void setRenderState(const RenderState& s) = 0;
    virtual const RenderState& renderState() const = 0;

    // Piecewise, for code that changes one thing and leaves the rest.
    void setBlend(const BlendState& b)     { RenderState s = renderState(); s.blend = b; setRenderState(s); }
    void setDepth(const DepthState& d)     { RenderState s = renderState(); s.depth = d; setRenderState(s); }
    void setStencil(const StencilState& t) { RenderState s = renderState(); s.stencil = t; setRenderState(s); }
    void setRaster(const RasterState& r)   { RenderState s = renderState(); s.raster = r; setRenderState(s); }
    void setColorMask(const ColorMask& m)  { RenderState s = renderState(); s.colorMask = m; setRenderState(s); }

    void setBlendEnabled(bool on)  { RenderState s = renderState(); s.blend.enabled = on; setRenderState(s); }
    // Same factors for colour and alpha.
    void setBlendFunc(BlendFactor src, BlendFactor dst) {
        RenderState s = renderState();
        s.blend.srcColor = s.blend.srcAlpha = src;
        s.blend.dstColor = s.blend.dstAlpha = dst;
        setRenderState(s);
    }
    void setBlendFuncSeparate(BlendFactor srcColor, BlendFactor dstColor, BlendFactor srcAlpha,
                              BlendFactor dstAlpha) {
        RenderState s = renderState();
        s.blend.srcColor = srcColor; s.blend.dstColor = dstColor;
        s.blend.srcAlpha = srcAlpha; s.blend.dstAlpha = dstAlpha;
        setRenderState(s);
    }
    void setBlendOp(BlendOp op) {
        RenderState s = renderState(); s.blend.colorOp = s.blend.alphaOp = op; setRenderState(s);
    }
    void setDepthTest(bool on)        { RenderState s = renderState(); s.depth.test = on; setRenderState(s); }
    void setDepthWrite(bool on)       { RenderState s = renderState(); s.depth.write = on; setRenderState(s); }
    void setDepthFunc(CompareOp f)    { RenderState s = renderState(); s.depth.func = f; setRenderState(s); }
    void setStencilTest(bool on)      { RenderState s = renderState(); s.stencil.test = on; setRenderState(s); }
    void setStencilFunc(CompareOp f, int ref, std::uint32_t readMask) {
        RenderState s = renderState();
        s.stencil.func = f; s.stencil.ref = ref; s.stencil.readMask = readMask;
        setRenderState(s);
    }
    void setStencilOp(StencilOp fail, StencilOp depthFail, StencilOp pass) {
        RenderState s = renderState();
        s.stencil.fail = fail; s.stencil.depthFail = depthFail; s.stencil.pass = pass;
        setRenderState(s);
    }
    void setStencilWriteMask(std::uint32_t m) { RenderState s = renderState(); s.stencil.writeMask = m; setRenderState(s); }
    void setCullEnabled(bool on)  { RenderState s = renderState(); s.raster.cull = on; setRenderState(s); }
    // Which faces culling removes, when it is on.
    void setCullFace(CullMode f)  { RenderState s = renderState(); s.raster.cullFace = f; setRenderState(s); }
    bool cullEnabled() const      { return renderState().raster.cull; }
    void setWireframe(bool on)    { RenderState s = renderState(); s.raster.wireframe = on; setRenderState(s); }
    void setScissorTest(bool on)  { RenderState s = renderState(); s.raster.scissor = on; setRenderState(s); }
    void setPolygonOffset(bool on, float factor, float units) {
        RenderState s = renderState();
        s.raster.polygonOffset = on; s.raster.offsetFactor = factor; s.raster.offsetUnits = units;
        setRenderState(s);
    }
    void setPolygonOffsetEnabled(bool on) { RenderState s = renderState(); s.raster.polygonOffset = on; setRenderState(s); }
    void setClipDistance(int i, bool on) {
        RenderState s = renderState();
        const auto bit = static_cast<std::uint8_t>(1u << i);
        s.raster.clipDistances = on ? (s.raster.clipDistances | bit)
                                    : (s.raster.clipDistances & ~bit);
        setRenderState(s);
    }
    void setColorWrite(bool on) { setColorMask(ColorMask{on, on, on, on}); }

    // --- Draws ------------------------------------------------------------------
    // With the bound program, vertex array, textures and state, into the bound
    // framebuffer.
    virtual void draw(Primitive prim, int first, int count, int instances = 1) = 0;
    // From the vertex array's index buffer, `byteOffset` into it; `baseVertex`
    // is added to every index (not in WebGL2, which has no such thing).
    virtual void drawIndexed(Primitive prim, int count, IndexType type,
                             std::size_t byteOffset = 0, int instances = 1,
                             int baseVertex = 0) = 0;
    virtual void dispatch(int groupsX, int groupsY, int groupsZ) = 0;
    virtual void memoryBarrier(std::uint32_t barriers /* Barrier bits */) = 0;

    // --- Queries and fences -----------------------------------------------------
    virtual QueryHandle createQuery(QueryType type) = 0;
    virtual void destroy(QueryHandle) = 0;
    // TimeElapsed and AnySamplesPassed: around the work they measure. One of a
    // type at a time.
    virtual void beginQuery(QueryHandle) = 0;
    virtual void endQuery(QueryHandle) = 0;
    // Timestamp: record the GPU clock when the work before it is done.
    virtual void timestamp(QueryHandle) = 0;
    virtual bool queryReady(QueryHandle) = 0;
    // The result (waits if it is not ready): ns, or 0/1 for AnySamplesPassed.
    virtual std::uint64_t queryResult(QueryHandle) = 0;

    // A marker after everything submitted so far.
    virtual FenceHandle createFence() = 0;
    virtual void destroy(FenceHandle) = 0;
    // Whether the GPU has passed it, waiting at most `timeoutNs`.
    virtual bool fenceSignaled(FenceHandle, std::uint64_t timeoutNs = 0) = 0;

    // --- Dear ImGui -------------------------------------------------------------
    // The UI is drawn by the backend's own ImGui renderer, on the GLFW platform
    // backend set up for this API (ImGui_ImplGlfw_NewFrame stays the caller's).
    virtual void imguiInit(GLFWwindow* window) = 0;
    virtual void imguiShutdown() = 0;
    virtual void imguiNewFrame() = 0;
    virtual void imguiRender(ImDrawData* data) = 0;
    // What ImGui::Image takes for a texture (an ImTextureID's value).
    virtual std::uint64_t imguiTextureId(TextureHandle) const = 0;
    // An ImDrawList callback after which images sample with their texture's own
    // sampler (Nearest for pixel art) instead of the UI's linear one.
    using ImGuiCallback = void (*)(const ImDrawList*, const ImDrawCmd*);
    virtual ImGuiCallback imguiTextureSamplerCallback() const = 0;
};

// The device of the window that was made last (or made current). Every engine
// class that holds GPU resources talks to this one. With no window (yet, or any
// more) it is a device that draws nothing and makes nothing -- so a texture
// that outlives its window at exit is released into the void, not into a crash.
Device& device();
bool    hasDevice();   // false while device() is that null device

// --- Making one -------------------------------------------------------------------
// Window does this; nothing else needs to.

// GLFW window hints for the next glfwCreateWindow. `attempt` counts tries:
// a backend can ask for less on a second try (OpenGL 4.3, then 3.3). False when
// it has nothing left to try.
bool prepareWindowHints(Backend backend, int attempt);
// A device on a freshly made window (on OpenGL: its context made current and
// the entry points loaded). Throws std::runtime_error when that fails. The new
// device becomes current.
std::unique_ptr<Device> createDevice(Backend backend, GLFWwindow* window, bool vsync);
// Make `d` the one device() answers (null: none).
void makeCurrent(Device* d);

} // namespace fitzel::rhi
