// The OpenGL device: state, bindings, draws, queries, the window and the UI.
// Resources (buffers, textures, framebuffers, programs, vertex arrays) are in
// GlResources.cpp.

#include "GlDevice.hpp"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "GlEnums.hpp"

namespace fitzel::rhi::gl {

// --- The window -------------------------------------------------------------------

bool prepareWindowHints(int attempt) {
#ifdef __EMSCRIPTEN__
    // The browser: a WebGL2 context on the page's canvas. Asked for by its WebGL
    // version, which is what the GLFW port reads the context version as (3 would
    // mean a WebGL 3 that does not exist).
    if (attempt > 0) return false;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    return true;
#else
    // 4.3 first, 3.3 if the driver will not give it. The engine draws in 3.3 and
    // every shader it ships is `#version 330 core`, which a 4.3 core context runs
    // unchanged -- so asking for more costs nothing and buys the two things that
    // cannot be written below it: compute shaders and shader storage buffers.
    // Nothing in the engine REQUIRES them; Caps::compute is how a feature that
    // wants them finds out whether it got them.
    static const int versions[][2] = {{4, 3}, {3, 3}};
    if (attempt < 0 || attempt >= 2) return false;
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, versions[attempt][0]);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, versions[attempt][1]);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);   // required on macOS
    return true;
#endif
}

std::unique_ptr<Device> createDevice(GLFWwindow* window, bool vsync) {
    glfwMakeContextCurrent(window);
#ifndef __EMSCRIPTEN__
    // In the browser there is no loader: the runtime provides the WebGL2 entry
    // points itself.
    if (gladLoadGL(reinterpret_cast<GLADloadfunc>(glfwGetProcAddress)) == 0)
        throw std::runtime_error("Failed to load OpenGL via GLAD");
#endif
    return std::make_unique<GlDevice>(window, vsync);
}

namespace {
std::string g_apiName, g_apiVersion, g_renderer, g_vendor;

std::string glString(GLenum e) {
    const GLubyte* s = glGetString(e);
    return s ? reinterpret_cast<const char*>(s) : "";
}
} // namespace

GlDevice::GlDevice(GLFWwindow* window, bool vsync) : m_window(window) {
    m_caps.backend = Backend::OpenGL;
#ifdef __EMSCRIPTEN__
    m_caps.web = true;
#endif
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    m_caps.apiMajor = major;
    m_caps.apiMinor = minor;
    // glad fills GLAD_GL_VERSION_* when the loader runs, one per version it
    // managed to resolve against the driver. Read rather than parsing the
    // version string on purpose: the string is the DRIVER's version, and what a
    // caller needs to know is whether the entry points are non-null.
    m_caps.compute = GLAD_GL_VERSION_4_3 != 0;
    GLint maxTex = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTex);
    m_caps.maxTextureSize = maxTex;
    if (GLAD_GL_ARB_texture_filter_anisotropic || GLAD_GL_EXT_texture_filter_anisotropic) {
        GLfloat cap = 0.0f;
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &cap);
        m_caps.maxAnisotropy = cap > 1.0f ? cap : 1.0f;
    }
    // WebGL2 answers the timestamp probe with an error and 0 bits, which is the
    // answer: no timer there.
    GLint bits = 0;
    glGetQueryiv(GL_TIMESTAMP, GL_QUERY_COUNTER_BITS, &bits);
    while (glGetError() != GL_NO_ERROR) {}
    m_caps.timestampBits = bits;
    m_caps.timerQueries  = bits > 0;
#ifndef __EMSCRIPTEN__
    if (m_caps.compute) {
        GLint64 block = 0;
        glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &block);
        m_caps.maxStorageBlockSize = block;
    }
#endif
    // Video memory: core GL cannot say, NVX_gpu_memory_info (NVIDIA) and
    // ATI_meminfo (AMD) can. Core profile: the extensions are enumerated one by
    // one, not one string.
    GLint extensions = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &extensions);
    for (GLint i = 0; i < extensions; ++i) {
        const GLubyte* e = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (!e) continue;
        const std::string ext = reinterpret_cast<const char*>(e);
        if (ext == "GL_NVX_gpu_memory_info") m_vram = VramSource::Nvx;
        else if (ext == "GL_ATI_meminfo" && m_vram == VramSource::None) m_vram = VramSource::Ati;
    }
    g_apiVersion = glString(GL_VERSION);
    g_apiName  = "OpenGL " + g_apiVersion;
    g_renderer = glString(GL_RENDERER);
    g_vendor   = glString(GL_VENDOR);
    m_caps.apiName  = g_apiName.c_str();
    m_caps.apiVersion = g_apiVersion.c_str();
    m_caps.renderer = g_renderer.c_str();
    m_caps.vendor   = g_vendor.c_str();

    setVsync(vsync, false);

    // Two things OpenGL has a switch for and every other API simply does: a
    // point takes its size from the vertex shader, and a cube map is filtered
    // across its face edges (without it the coarse mips a rough surface reads
    // show each face as its own flat patch -- a rough metal ball came out
    // faceted like a disco ball). Both on for good.
#ifndef __EMSCRIPTEN__
    glEnable(GL_PROGRAM_POINT_SIZE);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
#endif

    int w = 0, h = 0;
    glfwGetFramebufferSize(window, &w, &h);
    m_viewport = Rect{0, 0, w, h};
    m_scissor  = Rect{0, 0, w, h};
    applyAll();
}

GlDevice::~GlDevice() = default;

void GlDevice::activate() { glfwMakeContextCurrent(m_window); }

void GlDevice::present() { glfwSwapBuffers(m_window); }

void GlDevice::setVsync(bool on, bool adaptive) {
#ifndef __EMSCRIPTEN__
    // (The browser presents on its own refresh; there is no interval to set.)
    // Adaptive is a negative interval, where the WGL/GLX extension allows it.
    static const bool tear = glfwExtensionSupported("WGL_EXT_swap_control_tear") ||
                             glfwExtensionSupported("GLX_EXT_swap_control_tear");
    glfwSwapInterval(!on ? 0 : (adaptive && tear) ? -1 : 1);
#else
    (void)on;
    (void)adaptive;
#endif
}

void GlDevice::finish() { glFinish(); }
void GlDevice::flush() { glFlush(); }

bool GlDevice::drainErrors() {
    bool any = false;
    while (glGetError() != GL_NO_ERROR) any = true;
    return any;
}

bool GlDevice::videoMemory(std::int64_t& usedKB, std::int64_t& totalKB) {
    // Both report FREE memory in KB, as the driver accounts it -- good enough to
    // notice a leak or a texture flood, not to audit against. Errors the query
    // raises are cleared rather than left for the next caller to trip over.
    constexpr GLenum kNvxDedicated = 0x9047;   // TOTAL_DEDICATED_VIDMEM_NVX
    constexpr GLenum kNvxAvailable = 0x9049;   // CURRENT_AVAILABLE_VIDMEM_NVX
    constexpr GLenum kAtiFreeTex   = 0x87FC;   // TEXTURE_FREE_MEMORY_ATI
    if (m_vram == VramSource::Nvx) {
        GLint total = 0, avail = 0;
        glGetIntegerv(kNvxDedicated, &total);
        glGetIntegerv(kNvxAvailable, &avail);
        while (glGetError() != GL_NO_ERROR) {}
        if (total <= 0) return false;
        totalKB = total;
        usedKB  = total - avail;
        return true;
    }
    if (m_vram == VramSource::Ati) {
        // A 4-vector whose first element is the free pool; no total to pair it with.
        GLint v[4] = {0, 0, 0, 0};
        glGetIntegerv(kAtiFreeTex, v);
        while (glGetError() != GL_NO_ERROR) {}
        if (v[0] <= 0) return false;
        totalKB = 0;
        usedKB  = v[0];
        return true;
    }
    return false;
}

void GlDevice::windowFramebufferSize(int& width, int& height) const {
    glfwGetFramebufferSize(m_window, &width, &height);
}

// --- State ------------------------------------------------------------------------

void GlDevice::applyAll() {
    applyState(m_state, true);
    glViewport(m_viewport.x, m_viewport.y, m_viewport.w, m_viewport.h);
    glScissor(m_scissor.x, m_scissor.y, m_scissor.w, m_scissor.h);
    glClearColor(m_clearColor.r, m_clearColor.g, m_clearColor.b, m_clearColor.a);
#ifdef __EMSCRIPTEN__
    glClearDepthf(m_clearDepth);
#else
    glClearDepth(m_clearDepth);
#endif
    glClearStencil(m_clearStencil);
    rebindFramebuffer();
    const GlProgram* p = m_programs.find(m_program.id);
    glUseProgram(p ? p->name : 0);
    const GlVertexArray* va = m_vertexArrays.find(m_vao.id);
    glBindVertexArray(va ? va->name : 0);
    for (int u = 0; u < kUnits; ++u)
        for (int k = 0; k < 4; ++k)
            if (m_unitTargets[u][k]) {
                glActiveTexture(GL_TEXTURE0 + u);
                glBindTexture(targetOfIndex(k), m_unitTargets[u][k]);
            }
    glActiveTexture(GL_TEXTURE0 + m_activeUnit);
}

void GlDevice::resyncState() { applyAll(); }

void GlDevice::applyState(const RenderState& s, bool force) {
    const RenderState& o = m_state;
    auto toggle = [](GLenum cap, bool on) { on ? glEnable(cap) : glDisable(cap); };

    if (force || s.blend.enabled != o.blend.enabled) toggle(GL_BLEND, s.blend.enabled);
    if (force || s.blend.srcColor != o.blend.srcColor || s.blend.dstColor != o.blend.dstColor ||
        s.blend.srcAlpha != o.blend.srcAlpha || s.blend.dstAlpha != o.blend.dstAlpha)
        glBlendFuncSeparate(blendFactor(s.blend.srcColor), blendFactor(s.blend.dstColor),
                            blendFactor(s.blend.srcAlpha), blendFactor(s.blend.dstAlpha));
    if (force || s.blend.colorOp != o.blend.colorOp || s.blend.alphaOp != o.blend.alphaOp)
        glBlendEquationSeparate(blendOp(s.blend.colorOp), blendOp(s.blend.alphaOp));

    if (force || s.depth.test != o.depth.test) toggle(GL_DEPTH_TEST, s.depth.test);
    if (force || s.depth.write != o.depth.write) glDepthMask(s.depth.write ? GL_TRUE : GL_FALSE);
    if (force || s.depth.func != o.depth.func) glDepthFunc(compareOp(s.depth.func));

    if (force || s.stencil.test != o.stencil.test) toggle(GL_STENCIL_TEST, s.stencil.test);
    if (force || s.stencil.func != o.stencil.func || s.stencil.ref != o.stencil.ref ||
        s.stencil.readMask != o.stencil.readMask)
        glStencilFunc(compareOp(s.stencil.func), s.stencil.ref, s.stencil.readMask);
    if (force || s.stencil.writeMask != o.stencil.writeMask) glStencilMask(s.stencil.writeMask);
    if (force || s.stencil.fail != o.stencil.fail || s.stencil.depthFail != o.stencil.depthFail ||
        s.stencil.pass != o.stencil.pass)
        glStencilOp(stencilOp(s.stencil.fail), stencilOp(s.stencil.depthFail),
                    stencilOp(s.stencil.pass));

    if (force || s.raster.cull != o.raster.cull) toggle(GL_CULL_FACE, s.raster.cull);
    if (force || s.raster.cullFace != o.raster.cullFace) glCullFace(cullFace(s.raster.cullFace));
#ifndef __EMSCRIPTEN__
    // Wireframe is a desktop debug view; WebGL2 cannot draw in it.
    if (force || s.raster.wireframe != o.raster.wireframe)
        glPolygonMode(GL_FRONT_AND_BACK, s.raster.wireframe ? GL_LINE : GL_FILL);
#endif
    if (force || s.raster.polygonOffset != o.raster.polygonOffset)
        toggle(GL_POLYGON_OFFSET_FILL, s.raster.polygonOffset);
    if (force || s.raster.offsetFactor != o.raster.offsetFactor ||
        s.raster.offsetUnits != o.raster.offsetUnits)
        glPolygonOffset(s.raster.offsetFactor, s.raster.offsetUnits);
    if (force || s.raster.scissor != o.raster.scissor) toggle(GL_SCISSOR_TEST, s.raster.scissor);
    if (force || s.raster.clipDistances != o.raster.clipDistances) {
        // In the browser through WEBGL_clip_cull_distance, where the browser has
        // it; where it has not, the enum is refused and the plane simply does
        // not clip (the shader translation dropped the write too).
        for (int i = 0; i < 8; ++i) {
            const bool on = (s.raster.clipDistances >> i) & 1u;
            if (force ? (on || i == 0) : on != (((o.raster.clipDistances >> i) & 1u) != 0))
                toggle(GL_CLIP_DISTANCE0 + i, on);
        }
    }

    if (force || s.colorMask != o.colorMask)
        glColorMask(s.colorMask.r, s.colorMask.g, s.colorMask.b, s.colorMask.a);

    m_state = s;
}

void GlDevice::setRenderState(const RenderState& s) {
    if (s == m_state) return;
    applyState(s, false);
}

void GlDevice::setViewport(const Rect& r) {
    if (r == m_viewport) return;
    m_viewport = r;
    glViewport(r.x, r.y, r.w, r.h);
}

void GlDevice::setScissor(const Rect& r) {
    if (r == m_scissor) return;
    m_scissor = r;
    glScissor(r.x, r.y, r.w, r.h);
}

void GlDevice::setClearColor(const glm::vec4& c) {
    if (c == m_clearColor) return;
    m_clearColor = c;
    glClearColor(c.r, c.g, c.b, c.a);
}

void GlDevice::setClearDepth(float d) {
    if (d == m_clearDepth) return;
    m_clearDepth = d;
#ifdef __EMSCRIPTEN__
    glClearDepthf(d);
#else
    glClearDepth(d);
#endif
}

void GlDevice::setClearStencil(int s) {
    if (s == m_clearStencil) return;
    m_clearStencil = s;
    glClearStencil(s);
}

void GlDevice::clear(std::uint8_t mask) {
    GLbitfield bits = 0;
    if (mask & ClearColor)   bits |= GL_COLOR_BUFFER_BIT;
    if (mask & ClearDepth)   bits |= GL_DEPTH_BUFFER_BIT;
    if (mask & ClearStencil) bits |= GL_STENCIL_BUFFER_BIT;
    if (bits) glClear(bits);
}

void GlDevice::clearColorAttachment(int index, const glm::vec4& value) {
    const GLfloat v[4] = {value.r, value.g, value.b, value.a};
    glClearBufferfv(GL_COLOR, index, v);
}

// --- Bindings ---------------------------------------------------------------------

void GlDevice::activeUnit(int unit) {
    if (unit == m_activeUnit) return;
    glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
    m_activeUnit = unit;
}

void GlDevice::bindTexture(int unit, TextureHandle h) {
    if (unit < 0 || unit >= kUnits) return;
    const GlTexture* t = m_textures.find(h.id);
    if (!t || t->renderbuffer) {
        // None: whatever is on the unit comes off, on every target.
        for (int k = 0; k < 4; ++k)
            if (m_unitTargets[unit][k]) {
                activeUnit(unit);
                glBindTexture(targetOfIndex(k), 0);
                m_unitTargets[unit][k] = 0;
            }
        m_unit[unit] = TextureHandle{};
        return;
    }
    m_unit[unit] = h;
    const int k = indexOfTarget(t->target);
    if (m_unitTargets[unit][k] == t->name) return;
    activeUnit(unit);
    glBindTexture(t->target, t->name);
    m_unitTargets[unit][k] = t->name;
}

TextureHandle GlDevice::boundTexture(int unit) const {
    if (unit < 0 || unit >= kUnits) return {};
    return m_unit[unit];
}

void GlDevice::bindImage(int unit, TextureHandle h, int level, Access access, Format format) {
#ifndef __EMSCRIPTEN__
    const GlTexture* t = m_textures.find(h.id);
    if (!m_caps.compute) return;
    glBindImageTexture(static_cast<GLuint>(unit), t ? t->name : 0, level, GL_FALSE, 0,
                       accessOf(access), formatOf(format).internal);
#else
    (void)unit; (void)h; (void)level; (void)access; (void)format;
#endif
}

void GlDevice::bindBufferBase(BufferKind kind, int index, BufferHandle h) {
    const GlBuffer* b = m_buffers.find(h.id);
    const GLuint name = b ? b->name : 0;
    if (kind == BufferKind::Uniform) {
        glBindBufferBase(GL_UNIFORM_BUFFER, static_cast<GLuint>(index), name);
    } else if (kind == BufferKind::Storage) {
#ifndef __EMSCRIPTEN__
        if (m_caps.compute)
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(index), name);
#endif
    }
}

void GlDevice::bindProgram(ProgramHandle h) {
    if (h == m_program) return;
    const GlProgram* p = m_programs.find(h.id);
    glUseProgram(p ? p->name : 0);
    m_program = p ? h : ProgramHandle{};
}

void GlDevice::bindVertexArray(VertexArrayHandle h) {
    const GlVertexArray* va = m_vertexArrays.find(h.id);
    if (h != m_vao) {
        glBindVertexArray(va ? va->name : 0);
        m_vao = va ? h : VertexArrayHandle{};
    }
    // An attribute no buffer feeds reads the context's current value, which is
    // not part of the VAO: set it whenever the array that wants it is bound.
    if (va)
        for (const ConstantAttribute& c : va->constants)
            glVertexAttrib4f(c.location, c.value.x, c.value.y, c.value.z, c.value.w);
}

// --- Draws ------------------------------------------------------------------------

void GlDevice::draw(Primitive prim, int first, int count, int instances) {
    if (count <= 0 || instances <= 0) return;
    if (instances == 1) glDrawArrays(primitiveOf(prim), first, count);
    else glDrawArraysInstanced(primitiveOf(prim), first, count, instances);
}

void GlDevice::drawIndexed(Primitive prim, int count, IndexType type, std::size_t byteOffset,
                           int instances, int baseVertex) {
    if (count <= 0 || instances <= 0) return;
    const GLenum t = type == IndexType::U16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
    const void* offset = reinterpret_cast<const void*>(byteOffset);
#ifndef __EMSCRIPTEN__
    if (baseVertex != 0) {
        if (instances == 1) glDrawElementsBaseVertex(primitiveOf(prim), count, t, offset, baseVertex);
        else glDrawElementsInstancedBaseVertex(primitiveOf(prim), count, t, offset, instances,
                                               baseVertex);
        return;
    }
#endif
    if (instances == 1) glDrawElements(primitiveOf(prim), count, t, offset);
    else glDrawElementsInstanced(primitiveOf(prim), count, t, offset, instances);
}

void GlDevice::dispatch(int groupsX, int groupsY, int groupsZ) {
#ifndef __EMSCRIPTEN__
    if (m_caps.compute)
        glDispatchCompute(static_cast<GLuint>(groupsX), static_cast<GLuint>(groupsY),
                          static_cast<GLuint>(groupsZ));
#else
    (void)groupsX; (void)groupsY; (void)groupsZ;
#endif
}

void GlDevice::memoryBarrier(std::uint32_t barriers) {
#ifndef __EMSCRIPTEN__
    if (!m_caps.compute) return;
    if (barriers == BarrierAll) {
        glMemoryBarrier(GL_ALL_BARRIER_BITS);
        return;
    }
    GLbitfield bits = 0;
    if (barriers & BarrierImageAccess)   bits |= GL_SHADER_IMAGE_ACCESS_BARRIER_BIT;
    if (barriers & BarrierTextureFetch)  bits |= GL_TEXTURE_FETCH_BARRIER_BIT;
    if (barriers & BarrierTextureUpdate) bits |= GL_TEXTURE_UPDATE_BARRIER_BIT;
    if (barriers & BarrierStorage)       bits |= GL_SHADER_STORAGE_BARRIER_BIT;
    if (barriers & BarrierBufferUpdate)  bits |= GL_BUFFER_UPDATE_BARRIER_BIT;
    if (bits) glMemoryBarrier(bits);
#else
    (void)barriers;
#endif
}

// --- Queries and fences -----------------------------------------------------------

namespace {
GLenum queryTarget(QueryType t) {
    switch (t) {
        case QueryType::TimeElapsed:      return GL_TIME_ELAPSED;
        case QueryType::Timestamp:        return GL_TIMESTAMP;
        case QueryType::AnySamplesPassed: return GL_ANY_SAMPLES_PASSED;
    }
    return GL_TIME_ELAPSED;
}
} // namespace

QueryHandle GlDevice::createQuery(QueryType type) {
    GlQuery q;
    q.type = type;
    glGenQueries(1, &q.name);
    return QueryHandle{m_queries.add(q)};
}

void GlDevice::destroy(QueryHandle h) {
    if (GlQuery* q = m_queries.find(h.id)) {
        glDeleteQueries(1, &q->name);
        m_queries.remove(h.id);
    }
}

void GlDevice::beginQuery(QueryHandle h) {
    if (const GlQuery* q = m_queries.find(h.id)) glBeginQuery(queryTarget(q->type), q->name);
}

void GlDevice::endQuery(QueryHandle h) {
    if (const GlQuery* q = m_queries.find(h.id)) glEndQuery(queryTarget(q->type));
}

void GlDevice::timestamp(QueryHandle h) {
    if (const GlQuery* q = m_queries.find(h.id)) glQueryCounter(q->name, GL_TIMESTAMP);
}

bool GlDevice::queryReady(QueryHandle h) {
    const GlQuery* q = m_queries.find(h.id);
    if (!q) return false;
    GLuint ready = 0;
    glGetQueryObjectuiv(q->name, GL_QUERY_RESULT_AVAILABLE, &ready);
    return ready != 0;
}

std::uint64_t GlDevice::queryResult(QueryHandle h) {
    const GlQuery* q = m_queries.find(h.id);
    if (!q) return 0;
    if (q->type == QueryType::AnySamplesPassed) {
        GLuint any = 0;
        glGetQueryObjectuiv(q->name, GL_QUERY_RESULT, &any);
        return any;
    }
    GLuint64 ns = 0;
    glGetQueryObjectui64v(q->name, GL_QUERY_RESULT, &ns);
    return ns;
}

FenceHandle GlDevice::createFence() {
    GlFence f;
    f.sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    return FenceHandle{m_fences.add(f)};
}

void GlDevice::destroy(FenceHandle h) {
    if (GlFence* f = m_fences.find(h.id)) {
        if (f->sync) glDeleteSync(f->sync);
        m_fences.remove(h.id);
    }
}

bool GlDevice::fenceSignaled(FenceHandle h, std::uint64_t timeoutNs) {
    const GlFence* f = m_fences.find(h.id);
    if (!f || !f->sync) return true;
    // Flushed when actually waiting: a wait on a fence that was never sent to
    // the GPU would wait out its whole timeout.
    const GLenum r = glClientWaitSync(f->sync, timeoutNs ? GL_SYNC_FLUSH_COMMANDS_BIT : 0,
                                      timeoutNs);
    return r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED;
}

// --- Dear ImGui -------------------------------------------------------------------

void GlDevice::imguiInit(GLFWwindow* window) {
    // install_callbacks = true: ImGui chains the callbacks Input already set.
    ImGui_ImplGlfw_InitForOpenGL(window, true);
#ifdef __EMSCRIPTEN__
    ImGui_ImplOpenGL3_Init("#version 300 es");   // WebGL2
#else
    ImGui_ImplOpenGL3_Init("#version 330");
#endif
}

void GlDevice::imguiShutdown() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
}

void GlDevice::imguiNewFrame() { ImGui_ImplOpenGL3_NewFrame(); }

void GlDevice::imguiRender(ImDrawData* data) {
    ImGui_ImplOpenGL3_RenderDrawData(data);
    // The ImGui renderer saves and restores what it touches; this is the safety
    // net for whatever it does not, once a frame.
    applyAll();
}

std::uint64_t GlDevice::imguiTextureId(TextureHandle h) const {
    const GlTexture* t = m_textures.find(h.id);
    return (t && !t->renderbuffer) ? t->name : 0;
}

namespace {
// The OpenGL3 ImGui renderer binds a sampler object of its own (linear) for every
// image, and a sampler object beats the texture's own filter and wrap: unbinding
// it lets a Nearest/Repeat texture (the image editor's pixels, its checkerboard)
// sample as it was made to.
void useTextureSampling(const ImDrawList*, const ImDrawCmd*) { glBindSampler(0, 0); }
} // namespace

Device::ImGuiCallback GlDevice::imguiTextureSamplerCallback() const { return useTextureSampling; }

} // namespace fitzel::rhi::gl
