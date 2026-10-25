// The OpenGL device's resources: buffers, textures, framebuffers, programs and
// vertex arrays, and the copies between them and the CPU.
//
// OpenGL edits an object through a binding; the device's callers never bind to
// edit. So every edit here binds where it does no harm (a buffer target no VAO
// records, the active texture unit) and puts the caller's binding back after.

#include "GlDevice.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "GlEnums.hpp"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

namespace fitzel::rhi::gl {

namespace {

// Rows of any width, byte-aligned: the device's callers never pad rows, and a
// stale alignment or row length left by other code (Dear ImGui's renderer set
// GL_UNPACK_ROW_LENGTH and, before its 2026-07-15 fix, #8802/#9473, left it set)
// would read a tightly-packed buffer with the wrong stride and over-read past
// its end. Set before every transfer rather than trusted.
void unpackRows(int rowLength) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
    glPixelStorei(GL_UNPACK_IMAGE_HEIGHT, 0);
}
void packRows() {
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
}

int mipSize(int size, int level) { return std::max(1, size >> level); }

} // namespace

// --- Buffers ----------------------------------------------------------------------

GLenum GlDevice::bindBufferForTransfer(const GlBuffer& b) {
#ifdef __EMSCRIPTEN__
    // WebGL2 fixes a buffer's type at its first bind, and an index buffer has to
    // be one from the start -- through the element target, which is VAO state,
    // so with no VAO bound to record it.
    if (b.kind == BufferKind::Index) {
        glBindVertexArray(0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, b.name);
        return GL_ELEMENT_ARRAY_BUFFER;
    }
#endif
    // A target nothing else binds and no VAO records.
    glBindBuffer(GL_COPY_WRITE_BUFFER, b.name);
    return GL_COPY_WRITE_BUFFER;
}

void GlDevice::unbindBufferForTransfer(GLenum target) {
    glBindBuffer(target, 0);
    if (target == GL_ELEMENT_ARRAY_BUFFER) {
        const GlVertexArray* va = m_vertexArrays.find(m_vao.id);
        glBindVertexArray(va ? va->name : 0);
    }
}

BufferHandle GlDevice::createBuffer(BufferKind kind, BufferUsage usage, std::size_t bytes,
                                    const void* data) {
    GlBuffer b;
    b.kind  = kind;
    b.usage = usage;
    b.size  = bytes;
    glGenBuffers(1, &b.name);
    const GLenum target = bindBufferForTransfer(b);
    glBufferData(target, static_cast<GLsizeiptr>(bytes), data, usageOf(kind, usage));
    unbindBufferForTransfer(target);
    return BufferHandle{m_buffers.add(b)};
}

void GlDevice::destroy(BufferHandle h) {
    if (GlBuffer* b = m_buffers.find(h.id)) {
        glDeleteBuffers(1, &b->name);
        m_buffers.remove(h.id);
    }
}

void GlDevice::bufferData(BufferHandle h, std::size_t bytes, const void* data, BufferUsage usage) {
    GlBuffer* b = m_buffers.find(h.id);
    if (!b) return;
    b->size  = bytes;
    b->usage = usage;
    const GLenum target = bindBufferForTransfer(*b);
    glBufferData(target, static_cast<GLsizeiptr>(bytes), data, usageOf(b->kind, usage));
    unbindBufferForTransfer(target);
}

void GlDevice::bufferSubData(BufferHandle h, std::size_t offset, std::size_t bytes,
                             const void* data) {
    const GlBuffer* b = m_buffers.find(h.id);
    if (!b || bytes == 0) return;
    const GLenum target = bindBufferForTransfer(*b);
    glBufferSubData(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), data);
    unbindBufferForTransfer(target);
}

void GlDevice::readBuffer(BufferHandle h, std::size_t offset, std::size_t bytes, void* out) {
    const GlBuffer* b = m_buffers.find(h.id);
    if (!b || bytes == 0) return;
    const GLenum target = bindBufferForTransfer(*b);
    glGetBufferSubData(target, static_cast<GLintptr>(offset), static_cast<GLsizeiptr>(bytes), out);
    unbindBufferForTransfer(target);
}

std::size_t GlDevice::bufferSize(BufferHandle h) const {
    const GlBuffer* b = m_buffers.find(h.id);
    return b ? b->size : 0;
}

// --- Textures ---------------------------------------------------------------------

void GlDevice::bindForEdit(const GlTexture& t) {
    if (t.renderbuffer) glBindRenderbuffer(GL_RENDERBUFFER, t.name);
    else glBindTexture(t.target, t.name);
}

void GlDevice::restoreAfterEdit(const GlTexture& t) {
    if (t.renderbuffer) glBindRenderbuffer(GL_RENDERBUFFER, 0);
    else glBindTexture(t.target, m_unitTargets[m_activeUnit][indexOfTarget(t.target)]);
}

GLenum GlDevice::textureTarget(const GlTexture& t, int layer) const {
    if (t.target == GL_TEXTURE_CUBE_MAP && layer >= 0)
        return GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(layer);
    return t.target;
}

void GlDevice::applySampler(GLenum target, const SamplerDesc& s) {
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER,
                    static_cast<GLint>(minFilterOf(s.minFilter, s.mipFilter)));
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(filterOf(s.magFilter)));
    glTexParameteri(target, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrapOf(s.wrapU)));
    glTexParameteri(target, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrapOf(s.wrapV)));
    if (target == GL_TEXTURE_3D || target == GL_TEXTURE_CUBE_MAP)
        glTexParameteri(target, GL_TEXTURE_WRAP_R, static_cast<GLint>(wrapOf(s.wrapW)));
    glTexParameteri(target, GL_TEXTURE_MAX_LEVEL, s.maxLevel);
    if (m_caps.maxAnisotropy > 1.0f)
        glTexParameterf(target, GL_TEXTURE_MAX_ANISOTROPY,
                        std::clamp(s.anisotropy, 1.0f, m_caps.maxAnisotropy));
#ifndef __EMSCRIPTEN__
    if (s.wrapU == Wrap::ClampToBorder || s.wrapV == Wrap::ClampToBorder ||
        s.wrapW == Wrap::ClampToBorder) {
        const GLfloat c[4] = {s.borderColor.r, s.borderColor.g, s.borderColor.b, s.borderColor.a};
        glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, c);
    }
#endif
}

TextureHandle GlDevice::createTexture(const TextureDesc& descIn, const void* data,
                                      Format dataFormat) {
    GlTexture t;
    t.desc = descIn;
    TextureDesc& d = t.desc;
    d.width  = std::max(1, d.width);
    d.height = std::max(1, d.height);
    d.depth  = (d.type == TextureType::Tex2DArray || d.type == TextureType::Tex3D)
                   ? std::max(1, d.depth) : 1;
    if (d.mipLevels <= 0) {
        int levels = 1;
        for (int s = std::max({d.width, d.height, d.type == TextureType::Tex3D ? d.depth : 1});
             s > 1; s >>= 1)
            ++levels;
        d.mipLevels = levels;
    }
    const GlFormat f = formatOf(d.format);
    const GlFormat df = formatOf(dataFormat == Format::Unknown ? d.format : dataFormat);

    // Drawn into, never read: a renderbuffer (the depth behind a colour target).
    if (d.type == TextureType::Tex2D && !(d.usage & (UsageSampled | UsageStorage)) && !data) {
        t.renderbuffer = true;
        t.target = GL_RENDERBUFFER;
        glGenRenderbuffers(1, &t.name);
        glBindRenderbuffer(GL_RENDERBUFFER, t.name);
        glRenderbufferStorage(GL_RENDERBUFFER, f.internal, d.width, d.height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        return TextureHandle{m_textures.add(t)};
    }

    t.target = targetOf(d.type);
    glGenTextures(1, &t.name);
    bindForEdit(t);
    unpackRows(0);
    const bool immutable = !m_caps.web && (d.usage & UsageStorage) && m_caps.compute;
    switch (d.type) {
        case TextureType::Tex2D:
            if (immutable) {
                glTexStorage2D(GL_TEXTURE_2D, d.mipLevels, f.internal, d.width, d.height);
                if (data)
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, d.width, d.height, df.format,
                                    df.type, data);
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(f.internal), d.width, d.height,
                             0, df.format, df.type, data);
            }
            break;
        case TextureType::Tex2DArray:
        case TextureType::Tex3D:
            glTexImage3D(t.target, 0, static_cast<GLint>(f.internal), d.width, d.height, d.depth,
                         0, df.format, df.type, data);
            break;
        case TextureType::Cube: {
            const std::size_t face = data ? static_cast<std::size_t>(d.width) * d.height *
                                                bytesPerPixel(dataFormat == Format::Unknown
                                                                  ? d.format : dataFormat)
                                          : 0;
            for (int i = 0; i < 6; ++i)
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(i), 0,
                             static_cast<GLint>(f.internal), d.width, d.height, 0, df.format,
                             df.type,
                             data ? static_cast<const unsigned char*>(data) + face * i : nullptr);
            break;
        }
    }
    // The other levels exist from the start, as the description promises: a
    // caller may fill them by hand (updateTexture per level) rather than
    // through generateMipmaps. Undefined contents until then.
    if (!immutable) {
        for (int level = 1; level < d.mipLevels; ++level) {
            const int w = mipSize(d.width, level), h = mipSize(d.height, level);
            switch (d.type) {
                case TextureType::Tex2D:
                    glTexImage2D(GL_TEXTURE_2D, level, static_cast<GLint>(f.internal), w, h, 0,
                                 f.format, f.type, nullptr);
                    break;
                case TextureType::Tex2DArray:
                case TextureType::Tex3D:
                    glTexImage3D(t.target, level, static_cast<GLint>(f.internal), w, h,
                                 d.type == TextureType::Tex3D ? mipSize(d.depth, level) : d.depth,
                                 0, f.format, f.type, nullptr);
                    break;
                case TextureType::Cube:
                    for (int i = 0; i < 6; ++i)
                        glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(i),
                                     level, static_cast<GLint>(f.internal), w, h, 0, f.format,
                                     f.type, nullptr);
                    break;
            }
        }
    }
    applySampler(t.target, d.sampler);
    restoreAfterEdit(t);
    return TextureHandle{m_textures.add(t)};
}

void GlDevice::destroy(TextureHandle h) {
    GlTexture* t = m_textures.find(h.id);
    if (!t) return;
    if (t->renderbuffer) {
        glDeleteRenderbuffers(1, &t->name);
    } else {
        // Deleting a texture unbinds it from every unit; the device's picture of
        // the units has to agree, or a later texture given the same name would
        // be taken for already bound.
        glDeleteTextures(1, &t->name);
        const int k = indexOfTarget(t->target);
        for (int u = 0; u < kUnits; ++u) {
            if (m_unitTargets[u][k] == t->name) m_unitTargets[u][k] = 0;
            if (m_unit[u] == h) m_unit[u] = TextureHandle{};
        }
    }
    m_textures.remove(h.id);
}

void GlDevice::updateTexture(TextureHandle h, const TextureRegion& r, Format dataFormat,
                             const void* data) {
    const GlTexture* t = m_textures.find(h.id);
    if (!t || t->renderbuffer || !data) return;
    const TextureDesc& d = t->desc;
    const int w = r.w > 0 ? r.w : mipSize(d.width, r.level);
    const int hh = r.h > 0 ? r.h : mipSize(d.height, r.level);
    const GlFormat df = formatOf(dataFormat == Format::Unknown ? d.format : dataFormat);
    bindForEdit(*t);
    unpackRows(r.rowLength);
    switch (d.type) {
        case TextureType::Tex2D:
            glTexSubImage2D(GL_TEXTURE_2D, r.level, r.x, r.y, w, hh, df.format, df.type, data);
            break;
        case TextureType::Cube: {
            // z..z+d are faces; one upload per face, the faces one after another.
            const int faces = r.d > 0 ? r.d : 1;
            const std::size_t face = static_cast<std::size_t>(r.rowLength > 0 ? r.rowLength : w) *
                                     hh * bytesPerPixel(dataFormat == Format::Unknown ? d.format
                                                                                       : dataFormat);
            for (int i = 0; i < faces; ++i)
                glTexSubImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(r.z + i),
                                r.level, r.x, r.y, w, hh, df.format, df.type,
                                static_cast<const unsigned char*>(data) + face * i);
            break;
        }
        case TextureType::Tex2DArray:
        case TextureType::Tex3D: {
            const int dd = r.d > 0 ? r.d
                                   : (d.type == TextureType::Tex3D ? mipSize(d.depth, r.level)
                                                                   : d.depth - r.z);
            glTexSubImage3D(t->target, r.level, r.x, r.y, r.z, w, hh, dd, df.format, df.type, data);
            break;
        }
    }
    unpackRows(0);
    restoreAfterEdit(*t);
}

void GlDevice::generateMipmaps(TextureHandle h) {
    const GlTexture* t = m_textures.find(h.id);
    if (!t || t->renderbuffer) return;
    bindForEdit(*t);
    glGenerateMipmap(t->target);
    restoreAfterEdit(*t);
}

void GlDevice::setSampler(TextureHandle h, const SamplerDesc& s) {
    GlTexture* t = m_textures.find(h.id);
    if (!t || t->renderbuffer) return;
    t->desc.sampler = s;
    bindForEdit(*t);
    applySampler(t->target, s);
    restoreAfterEdit(*t);
}

const TextureDesc& GlDevice::textureDesc(TextureHandle h) const {
    static const TextureDesc none{};
    const GlTexture* t = m_textures.find(h.id);
    return t ? t->desc : none;
}

void GlDevice::readTexture(TextureHandle h, int level, int layer, Format dataFormat, void* out) {
    const GlTexture* t = m_textures.find(h.id);
    if (!t || !out) return;
    const TextureDesc& d = t->desc;
    const Format fmt = dataFormat == Format::Unknown ? d.format : dataFormat;
    const GlFormat df = formatOf(fmt);
    const int w = mipSize(d.width, level), hh = mipSize(d.height, level);
    const int layers = d.type == TextureType::Cube ? 6
                     : d.type == TextureType::Tex3D ? mipSize(d.depth, level)
                     : d.depth;
    const std::size_t layerBytes = static_cast<std::size_t>(w) * hh * bytesPerPixel(fmt);
    packRows();
#ifdef __EMSCRIPTEN__
    // WebGL2 cannot read a texture, only a framebuffer: each image goes onto a
    // scratch one and is read from there.
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    const int first = layer >= 0 ? layer : 0;
    const int count = layer >= 0 ? 1 : layers;
    for (int i = 0; i < count; ++i) {
        const int l = first + i;
        if (t->renderbuffer)
            glFramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t->name);
        else if (d.type == TextureType::Tex2D)
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->name, level);
        else if (d.type == TextureType::Cube)
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(l), t->name, level);
        else
            glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, t->name, level, l);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
            glReadPixels(0, 0, w, hh, df.format, df.type,
                         static_cast<unsigned char*>(out) + layerBytes * i);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER, glFramebufferName(m_fb));
    glDeleteFramebuffers(1, &fbo);
    (void)layers;
#else
    if (t->renderbuffer) return;
    bindForEdit(*t);
    if (d.type == TextureType::Cube) {
        const int first = layer >= 0 ? layer : 0;
        const int count = layer >= 0 ? 1 : 6;
        for (int i = 0; i < count; ++i)
            glGetTexImage(GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(first + i), level,
                          df.format, df.type, static_cast<unsigned char*>(out) + layerBytes * i);
    } else if (layer >= 0 && layers > 1) {
        // OpenGL reads every layer at once; keep the one asked for.
        std::vector<unsigned char> all(layerBytes * static_cast<std::size_t>(layers));
        glGetTexImage(t->target, level, df.format, df.type, all.data());
        std::memcpy(out, all.data() + layerBytes * static_cast<std::size_t>(layer), layerBytes);
    } else {
        glGetTexImage(t->target, level, df.format, df.type, out);
    }
    restoreAfterEdit(*t);
#endif
}

// --- Framebuffers -----------------------------------------------------------------

GLuint GlDevice::glFramebufferName(FramebufferHandle h) const {
    const GlFramebuffer* f = m_framebuffers.find(h.id);
    return f ? f->name : 0;
}

void GlDevice::rebindFramebuffer() { glBindFramebuffer(GL_FRAMEBUFFER, glFramebufferName(m_fb)); }

void GlDevice::attachOne(GLenum point, const Attachment& a) {
    const GlTexture* t = m_textures.find(a.texture.id);
    if (!t) {
        // Whatever was there comes off.
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, point, GL_RENDERBUFFER, 0);
        return;
    }
    if (t->renderbuffer) {
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, point, GL_RENDERBUFFER, t->name);
        return;
    }
    switch (t->desc.type) {
        case TextureType::Tex2D:
            glFramebufferTexture2D(GL_FRAMEBUFFER, point, GL_TEXTURE_2D, t->name, a.level);
            break;
        case TextureType::Cube:
            if (a.layer >= 0) {
                glFramebufferTexture2D(GL_FRAMEBUFFER, point,
                                       GL_TEXTURE_CUBE_MAP_POSITIVE_X + static_cast<GLenum>(a.layer),
                                       t->name, a.level);
                break;
            }
            [[fallthrough]];
        case TextureType::Tex2DArray:
        case TextureType::Tex3D:
            if (a.layer >= 0) {
                glFramebufferTextureLayer(GL_FRAMEBUFFER, point, t->name, a.level, a.layer);
            } else {
#ifndef __EMSCRIPTEN__
                glFramebufferTexture(GL_FRAMEBUFFER, point, t->name, a.level);   // layered
#else
                glFramebufferTextureLayer(GL_FRAMEBUFFER, point, t->name, a.level, 0);
#endif
            }
            break;
    }
}

void GlDevice::attach(GlFramebuffer& fb, const FramebufferDesc& desc) {
    glBindFramebuffer(GL_FRAMEBUFFER, fb.name);
    for (int i = 0; i < kMaxColorAttachments; ++i)
        if (desc.color[i] != fb.desc.color[i])
            attachOne(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i), desc.color[i]);

    const GlTexture* dt = m_textures.find(desc.depth.texture.id);
    const GLenum point = !dt ? 0
                       : hasStencil(dt->desc.format) ? GL_DEPTH_STENCIL_ATTACHMENT
                                                     : GL_DEPTH_ATTACHMENT;
    if (fb.depthPoint && fb.depthPoint != point) attachOne(fb.depthPoint, Attachment{});
    if (point && (desc.depth != fb.desc.depth || point != fb.depthPoint))
        attachOne(point, desc.depth);
    fb.depthPoint = point;

    // Output i goes to attachment i; none at all draws depth only (and an
    // older driver will not call a depth-only framebuffer complete unless it
    // is told there is no colour to draw or read).
    GLenum bufs[kMaxColorAttachments];
    int n = 0;
    for (int i = 0; i < kMaxColorAttachments; ++i) {
        bufs[i] = desc.color[i].texture ? GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(i) : GL_NONE;
        if (desc.color[i].texture) n = i + 1;
    }
    if (n == 0) {
        const GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
        glReadBuffer(GL_NONE);
        fb.readBuffer = GL_NONE;
    } else {
        glDrawBuffers(n, bufs);
        fb.readBuffer = GL_NONE;
        for (int i = 0; i < n; ++i)
            if (bufs[i] != GL_NONE) { fb.readBuffer = bufs[i]; break; }
        glReadBuffer(fb.readBuffer);
    }
    fb.desc = desc;
    rebindFramebuffer();
}

FramebufferHandle GlDevice::createFramebuffer(const FramebufferDesc& desc) {
    GlFramebuffer fb;
    glGenFramebuffers(1, &fb.name);
    attach(fb, desc);
    return FramebufferHandle{m_framebuffers.add(fb)};
}

void GlDevice::destroy(FramebufferHandle h) {
    GlFramebuffer* fb = m_framebuffers.find(h.id);
    if (!fb) return;
    glDeleteFramebuffers(1, &fb->name);   // falls back to the window's if bound
    if (m_fb == h) m_fb = FramebufferHandle{};
    m_framebuffers.remove(h.id);
}

void GlDevice::setAttachments(FramebufferHandle h, const FramebufferDesc& desc) {
    if (GlFramebuffer* fb = m_framebuffers.find(h.id)) attach(*fb, desc);
}

const FramebufferDesc& GlDevice::attachments(FramebufferHandle h) const {
    static const FramebufferDesc none{};
    const GlFramebuffer* fb = m_framebuffers.find(h.id);
    return fb ? fb->desc : none;
}

bool GlDevice::framebufferComplete(FramebufferHandle h) {
    glBindFramebuffer(GL_FRAMEBUFFER, glFramebufferName(h));
    const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    rebindFramebuffer();
    return ok;
}

void GlDevice::bindFramebuffer(FramebufferHandle h) {
    const GlFramebuffer* fb = m_framebuffers.find(h.id);
    if (!fb) h = FramebufferHandle{};
    if (h != m_fb) {
        m_fb = h;
        glBindFramebuffer(GL_FRAMEBUFFER, fb ? fb->name : 0);
    }
#ifdef __EMSCRIPTEN__
    // WebGL drops a draw -- "feedback loop formed between framebuffer and
    // active texture" -- when a texture it renders into is still bound to a unit
    // that ANY sampler of the program names, read or not. The post chain left a
    // target's depth on unit 1, and every material without a normal map left
    // uNormalMap pointing there: the whole next scene pass was being thrown
    // away. Desktop GL only minds what a shader really reads.
    if (fb) {
        auto unbind = [this](const Attachment& a) {
            const GlTexture* t = m_textures.find(a.texture.id);
            if (!t || t->renderbuffer) return;
            const int k = indexOfTarget(t->target);
            for (int u = 0; u < kUnits; ++u)
                if (m_unitTargets[u][k] == t->name) {
                    activeUnit(u);
                    glBindTexture(t->target, 0);
                    m_unitTargets[u][k] = 0;
                    if (m_unit[u] == a.texture) m_unit[u] = TextureHandle{};
                }
        };
        for (const Attachment& a : fb->desc.color) unbind(a);
        unbind(fb->desc.depth);
    }
#endif
}

bool GlDevice::bindForRead(FramebufferHandle h, int attachment) {
    const GlFramebuffer* fb = m_framebuffers.find(h.id);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fb ? fb->name : 0);
    if (!fb) {
#ifndef __EMSCRIPTEN__
        glReadBuffer(attachment == kReadFront ? GL_FRONT : GL_BACK);
#else
        glReadBuffer(GL_BACK);
#endif
        return true;
    }
    if (attachment >= 0) {
        if (attachment >= kMaxColorAttachments || !fb->desc.color[attachment].texture) return false;
        glReadBuffer(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(attachment));
    }
    return true;
}

void GlDevice::restoreRead(FramebufferHandle h) {
    const GlFramebuffer* fb = m_framebuffers.find(h.id);
    if (fb) glReadBuffer(fb->readBuffer);
    else glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, glFramebufferName(m_fb));
}

void GlDevice::blit(FramebufferHandle src, const Rect& s, FramebufferHandle dst, const Rect& d,
                    std::uint8_t mask, Filter filter, int srcAttachment) {
    GLbitfield bits = 0;
    if (mask & BlitColor) bits |= GL_COLOR_BUFFER_BIT;
    if (mask & BlitDepth) bits |= GL_DEPTH_BUFFER_BIT;
    if (!bits) return;
    // OpenGL clips a blit by the scissor; the device's blit is a copy, not a draw.
    if (m_state.raster.scissor) glDisable(GL_SCISSOR_TEST);
    if (bindForRead(src, (mask & BlitColor) ? srcAttachment : kReadDepth)) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, glFramebufferName(dst));
        glBlitFramebuffer(s.x, s.y, s.x + s.w, s.y + s.h, d.x, d.y, d.x + d.w, d.y + d.h, bits,
                          filterOf(filter));
    }
    restoreRead(src);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, glFramebufferName(m_fb));
    if (m_state.raster.scissor) glEnable(GL_SCISSOR_TEST);
}

void GlDevice::readPixels(FramebufferHandle h, int attachment, const Rect& r, Format dataFormat,
                          void* out) {
    if (!out || r.w <= 0 || r.h <= 0) return;
    const GlFormat df = formatOf(dataFormat);
    packRows();
    if (bindForRead(h, attachment))
        glReadPixels(r.x, r.y, r.w, r.h, df.format, df.type, out);
    restoreRead(h);
}

void GlDevice::readPixelsToBuffer(FramebufferHandle h, int attachment, const Rect& r,
                                  Format dataFormat, BufferHandle dst, std::size_t offset) {
    const GlBuffer* b = m_buffers.find(dst.id);
    if (!b || r.w <= 0 || r.h <= 0) return;
    const GlFormat df = formatOf(dataFormat);
    packRows();
    // Bound only for this read: a pixel-pack buffer left bound would turn every
    // later glReadPixels into a write into it.
    glBindBuffer(GL_PIXEL_PACK_BUFFER, b->name);
    if (bindForRead(h, attachment))
        glReadPixels(r.x, r.y, r.w, r.h, df.format, df.type, reinterpret_cast<void*>(offset));
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    restoreRead(h);
}

void GlDevice::copyToTexture(FramebufferHandle src, int srcAttachment, const Rect& s,
                             TextureHandle dst, int level, int layer, int dstX, int dstY) {
    const GlTexture* t = m_textures.find(dst.id);
    if (!t || t->renderbuffer) return;
    if (bindForRead(src, srcAttachment)) {
        bindForEdit(*t);
        if (t->desc.type == TextureType::Tex2DArray || t->desc.type == TextureType::Tex3D)
            glCopyTexSubImage3D(t->target, level, dstX, dstY, std::max(layer, 0), s.x, s.y, s.w, s.h);
        else
            glCopyTexSubImage2D(textureTarget(*t, layer), level, dstX, dstY, s.x, s.y, s.w, s.h);
        restoreAfterEdit(*t);
    }
    restoreRead(src);
}

// --- Programs ---------------------------------------------------------------------

namespace {

#ifdef __EMSCRIPTEN__
// Whether the browser lets a vertex shader write gl_ClipDistance. WebGL2 has no
// user clip planes of its own; WEBGL_clip_cull_distance adds them back (Chrome
// has it, not every browser does). Asked once, on the first shader compiled --
// by then the context exists and is current.
bool clipDistanceAvailable() {
    static const bool have = [] {
        const EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_get_current_context();
        return ctx && emscripten_webgl_enable_extension(ctx, "WEBGL_clip_cull_distance");
    }();
    return have;
}

// WebGL2 speaks GLSL ES 3.00, the shaders are written in desktop GLSL 3.30
// core. The two are close enough that a rewrite at compile time covers every
// shader shipped, so the files stay written for one target:
//  - the version line, and the default precisions ES requires (a fragment
//    shader has no default float precision there, and no sampler type but
//    sampler2D/samplerCube has one in either stage);
//  - FITZEL_WEB defined, for the few places a shader has to choose (the sampler
//    budget of lit.frag: WebGL2 allows 16 per stage);
//  - gl_ClipDistance through the extension when there is one, and dropped when
//    there is not -- the water's reflection then simply is not clipped.
// web/shadercheck.py does the same rewrite offline; keep the two in step.
std::string translateForES(GLenum type, std::string_view source) {
    std::string s(source);
    const std::size_t v = s.find("#version");
    if (v == std::string::npos) return s;
    std::size_t nl = s.find('\n', v);
    if (nl == std::string::npos) nl = s.size() - 1;
    std::string head = "#version 300 es\n";
    const bool clips = type == GL_VERTEX_SHADER &&
                       s.find("gl_ClipDistance") != std::string::npos;
    if (clips && clipDistanceAvailable()) {
        head += "#extension GL_ANGLE_clip_cull_distance : require\n";
    } else if (clips) {
        for (std::size_t p = s.find("gl_ClipDistance"); p != std::string::npos;
             p = s.find("gl_ClipDistance", p)) {
            const std::size_t ls = s.rfind('\n', p);
            const std::size_t le = s.find('\n', p);
            const std::size_t from = (ls == std::string::npos) ? 0 : ls + 1;
            s.erase(from, (le == std::string::npos ? s.size() : le) - from);
            p = from;
        }
    }
    head +=
        "precision highp float;\n"
        "precision highp int;\n"
        "precision highp sampler2D;\n"
        "precision highp sampler3D;\n"
        "precision highp samplerCube;\n"
        "precision highp sampler2DArray;\n"
        "precision highp sampler2DShadow;\n"
        "precision highp sampler2DArrayShadow;\n"
        "precision highp samplerCubeShadow;\n"
        "precision highp isampler2D;\n"
        "precision highp usampler2D;\n"
        "precision highp isampler3D;\n"
        "precision highp usampler3D;\n"
        "precision highp isampler2DArray;\n"
        "precision highp usampler2DArray;\n"
        "#define FITZEL_WEB 1\n"
        "#line 2\n";   // the driver's error lines stay those of the file
    s.replace(v, nl + 1 - v, head);
    return s;
}

// WebGL2 refuses a draw -- INVALID_OPERATION, nothing drawn -- when two active
// samplers of different types name the same texture unit, whether or not the
// shader reads them on that draw. Every sampler starts at unit 0, so a program
// with a sampler2D and a samplerCube that one path never sets breaks every draw
// of that path; desktop drivers shrug the same thing off. So each sampler TYPE
// starts on a unit of its own (samplers of one type may share), picked from the
// units the browser build leaves unused: the desktop's point-shadow cubes past
// the first (13-15; the browser's array sits on 12), the terrain normal maps
// past the first (19-23), the light grid's G and B (25, 26) and 31. A sampler
// the engine sets moves off its default as on the desktop.
void assignDefaultSamplerUnits(GLuint program) {
    GLint count = 0;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
    for (GLint i = 0; i < count; ++i) {
        char name[256];
        GLsizei len = 0;
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform(program, static_cast<GLuint>(i), sizeof(name), &len, &size,
                           &type, name);
        int unit = -1;
        switch (type) {
            case GL_SAMPLER_2D:              unit = 31; break;
            case GL_SAMPLER_2D_ARRAY:        unit = 25; break;
            case GL_SAMPLER_3D:              unit = 13; break;
            case GL_SAMPLER_CUBE:            unit = 14; break;
            case GL_SAMPLER_2D_SHADOW:       unit = 15; break;
            case GL_SAMPLER_2D_ARRAY_SHADOW: unit = 19; break;
            case GL_SAMPLER_CUBE_SHADOW:     unit = 20; break;
            case GL_INT_SAMPLER_2D:
            case GL_UNSIGNED_INT_SAMPLER_2D: unit = 21; break;
            case GL_INT_SAMPLER_3D:
            case GL_UNSIGNED_INT_SAMPLER_3D: unit = 22; break;
            case GL_INT_SAMPLER_2D_ARRAY:
            case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY: unit = 23; break;
            default: break;
        }
        if (unit < 0) continue;
        const GLint loc = glGetUniformLocation(program, name);
        if (loc < 0) continue;
        // An array ("uX[0]") takes every element to the same unit.
        std::vector<GLint> units(static_cast<std::size_t>(std::max(size, 1)), unit);
        glUniform1iv(loc, static_cast<GLsizei>(units.size()), units.data());
    }
}
#endif

const char* stageName(GLenum type) {
    switch (type) {
        case GL_VERTEX_SHADER:   return "vertex";
        case GL_FRAGMENT_SHADER: return "fragment";
        default:                 return "compute";
    }
}

GLuint compileStage(GLenum type, std::string_view source, std::string* log) {
    const GLuint shader = glCreateShader(type);
#ifdef __EMSCRIPTEN__
    const std::string es = translateForES(type, source);
    source = es;
#endif
    const char* src = source.data();
    const auto  len = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &src, &len);
    glCompileShader(shader);

    GLint success = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char info[4096];
        glGetShaderInfoLog(shader, sizeof(info), nullptr, info);
        std::fprintf(stderr, "[Fitzel] %s shader compile error:\n%s\n", stageName(type), info);
        if (log) *log += std::string(stageName(type)) + ": " + info;
        glDeleteShader(shader);
        return 0;
    }
    if (log) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        if (len > 1) {
            std::string info(static_cast<std::size_t>(len), '\0');
            glGetShaderInfoLog(shader, len, nullptr, info.data());
            info.resize(std::strlen(info.c_str()));
            *log += std::string(stageName(type)) + ": " + info;
        }
    }
    return shader;
}

} // namespace

ProgramHandle GlDevice::createProgram(const ProgramSource& src, std::string* log) {
    std::vector<GLuint> stages;
    bool compute = false;
    if (src.compute) {
#ifndef __EMSCRIPTEN__
        if (!m_caps.compute) {
            if (log) *log = "compute shaders need OpenGL 4.3";
            return {};
        }
        compute = true;
        stages.push_back(compileStage(GL_COMPUTE_SHADER, src.compute, log));
#else
        if (log) *log = "no compute shaders in WebGL2";
        return {};
#endif
    } else {
        if (!src.vertex || !src.fragment) return {};
        stages.push_back(compileStage(GL_VERTEX_SHADER, src.vertex, log));
        stages.push_back(compileStage(GL_FRAGMENT_SHADER, src.fragment, log));
    }
    if (std::find(stages.begin(), stages.end(), 0u) != stages.end()) {
        for (GLuint s : stages) if (s) glDeleteShader(s);
        return {};
    }

    const GLuint program = glCreateProgram();
    for (GLuint s : stages) glAttachShader(program, s);
    glLinkProgram(program);
    for (GLuint s : stages) glDeleteShader(s);

    GLint success = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char info[4096];
        glGetProgramInfoLog(program, sizeof(info), nullptr, info);
        std::fprintf(stderr, "[Fitzel] shader link error:\n%s\n", info);
        if (log) *log += info;
        glDeleteProgram(program);
        return {};
    }
    GlProgram p;
    p.name = program;
    p.compute = compute;
    const ProgramHandle h{m_programs.add(p)};
#ifdef __EMSCRIPTEN__
    glUseProgram(program);
    assignDefaultSamplerUnits(program);
    const GlProgram* cur = m_programs.find(m_program.id);
    glUseProgram(cur ? cur->name : 0);
#endif
    return h;
}

void GlDevice::destroy(ProgramHandle h) {
    GlProgram* p = m_programs.find(h.id);
    if (!p) return;
    // Current: off first, or a program given the same slot later would be taken
    // for already bound.
    if (m_program == h) {
        glUseProgram(0);
        m_program = ProgramHandle{};
    }
    glDeleteProgram(p->name);
    m_programs.remove(h.id);
}

int GlDevice::uniformLocation(ProgramHandle h, const char* name) {
    const GlProgram* p = m_programs.find(h.id);
    return p ? glGetUniformLocation(p->name, name) : -1;
}

void GlDevice::setUniform(ProgramHandle h, int loc, UniformType type, const void* data, int count) {
    if (loc < 0 || !data) return;
    // OpenGL 3.3 sets uniforms on the current program only.
    bindProgram(h);
    if (m_program != h) return;
    const auto* i = static_cast<const GLint*>(data);
    const auto* f = static_cast<const GLfloat*>(data);
    switch (type) {
        case UniformType::Int:   glUniform1iv(loc, count, i); break;
        case UniformType::IVec2: glUniform2iv(loc, count, i); break;
        case UniformType::IVec3: glUniform3iv(loc, count, i); break;
        case UniformType::IVec4: glUniform4iv(loc, count, i); break;
        case UniformType::Float: glUniform1fv(loc, count, f); break;
        case UniformType::Vec2:  glUniform2fv(loc, count, f); break;
        case UniformType::Vec3:  glUniform3fv(loc, count, f); break;
        case UniformType::Vec4:  glUniform4fv(loc, count, f); break;
        case UniformType::Mat3:  glUniformMatrix3fv(loc, count, GL_FALSE, f); break;
        case UniformType::Mat4:  glUniformMatrix4fv(loc, count, GL_FALSE, f); break;
    }
}

void GlDevice::readUniform(ProgramHandle h, int loc, bool asInt, void* out16) {
    const GlProgram* p = m_programs.find(h.id);
    if (!p || loc < 0) return;
    if (asInt) glGetUniformiv(p->name, loc, static_cast<GLint*>(out16));
    else glGetUniformfv(p->name, loc, static_cast<GLfloat*>(out16));
}

// --- Vertex arrays ----------------------------------------------------------------

namespace {
struct AttribFormat {
    GLint     size;
    GLenum    type;
    GLboolean normalized;
    bool      integer;
    GLsizei   bytes;
};
AttribFormat attribFormat(VertexFormat f) {
    switch (f) {
        case VertexFormat::Float:      return {1, GL_FLOAT, GL_FALSE, false, 4};
        case VertexFormat::Float2:     return {2, GL_FLOAT, GL_FALSE, false, 8};
        case VertexFormat::Float3:     return {3, GL_FLOAT, GL_FALSE, false, 12};
        case VertexFormat::Float4:     return {4, GL_FLOAT, GL_FALSE, false, 16};
        case VertexFormat::UByte4Norm: return {4, GL_UNSIGNED_BYTE, GL_TRUE, false, 4};
        case VertexFormat::Int:        return {1, GL_INT, GL_FALSE, true, 4};
        case VertexFormat::UInt:       return {1, GL_UNSIGNED_INT, GL_FALSE, true, 4};
    }
    return {3, GL_FLOAT, GL_FALSE, false, 12};
}
} // namespace

// Cheap enough to do per draw (the grass re-points its instances per tile):
// no allocation, and only what changed is switched off.
void GlDevice::specifyVertexArray(GlVertexArray& va, const VertexArrayDesc& desc) {
    glBindVertexArray(va.name);
    std::uint64_t enabled = 0;
    for (std::size_t i = 0; i < desc.attributeCount; ++i) {
        const VertexAttribute& a = desc.attributes[i];
        const GlBuffer* b = m_buffers.find(a.buffer.id);
        if (!b) continue;
        const AttribFormat f = attribFormat(a.format);
        glBindBuffer(GL_ARRAY_BUFFER, b->name);
        glEnableVertexAttribArray(a.location);
        const void* offset = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a.offset));
        if (f.integer)
            glVertexAttribIPointer(a.location, f.size, f.type, static_cast<GLsizei>(a.stride), offset);
        else
            glVertexAttribPointer(a.location, f.size, f.type, f.normalized,
                                  static_cast<GLsizei>(a.stride), offset);
        glVertexAttribDivisor(a.location, a.divisor);
        if (a.location < 64) enabled |= std::uint64_t{1} << a.location;
    }
    for (std::uint32_t loc = 0; loc < 64; ++loc)
        if (((va.enabled & ~enabled) >> loc) & 1u) {
            glDisableVertexAttribArray(loc);
            glVertexAttribDivisor(loc, 0);
        }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    const GlBuffer* ib = m_buffers.find(desc.indexBuffer.id);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ib ? ib->name : 0);
    va.enabled = enabled;
    va.constants.assign(desc.constants, desc.constants + desc.constantCount);
    va.indexBuffer = desc.indexBuffer;
    const GlVertexArray* cur = m_vertexArrays.find(m_vao.id);
    glBindVertexArray(cur ? cur->name : 0);
}

VertexArrayHandle GlDevice::createVertexArray(const VertexArrayDesc& desc) {
    GlVertexArray va;
    glGenVertexArrays(1, &va.name);
    specifyVertexArray(va, desc);
    return VertexArrayHandle{m_vertexArrays.add(std::move(va))};
}

void GlDevice::destroy(VertexArrayHandle h) {
    GlVertexArray* va = m_vertexArrays.find(h.id);
    if (!va) return;
    glDeleteVertexArrays(1, &va->name);   // reverts to none if bound
    if (m_vao == h) m_vao = VertexArrayHandle{};
    m_vertexArrays.remove(h.id);
}

void GlDevice::setVertexArray(VertexArrayHandle h, const VertexArrayDesc& desc) {
    if (GlVertexArray* va = m_vertexArrays.find(h.id)) specifyVertexArray(*va, desc);
}

} // namespace fitzel::rhi::gl
