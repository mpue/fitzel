#pragma once
// The browser's stand-in for the desktop GLAD loader header.
//
// The engine includes <glad/gl.h> everywhere for GL's enums and entry points.
// On the desktop that is the generated GL 4.3 core loader. In the browser there
// is nothing to load -- Emscripten provides the WebGL2 functions directly -- so
// this header (on the include path only for the web build, see
// cmake/Dependencies.cmake) pulls in the GLES 3.0 headers instead, and fills
// the gap between the two for what the engine uses:
//
//  - enums desktop GL has and ES 3.0 does not. Most of them name something
//    WebGL2 gets from an extension that Emscripten enables with the context
//    (anisotropy, float render targets, clip distances); the browser rejects
//    the rest with INVALID_ENUM, which costs a console line and nothing else.
//    The code that really has to do something else says so with
//    #ifdef __EMSCRIPTEN__ at the call.
//  - entry points ES 3.0 lacks but has an exact equivalent for
//    (glDrawBuffer -> glDrawBuffers), and ones that only switch desktop-only
//    state, which become no-ops (glPolygonMode: wireframe is a debug view).
//
// GLSL is rewritten from 3.30 core to 3.00 es at compile time (Shader.cpp).
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

// --- Enums -----------------------------------------------------------------
#ifndef GL_CLAMP_TO_BORDER
#define GL_CLAMP_TO_BORDER 0x812D
#endif
#ifndef GL_TEXTURE_BORDER_COLOR
#define GL_TEXTURE_BORDER_COLOR 0x1004
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE       // EXT_texture_filter_anisotropic
#endif
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY
#define GL_MAX_TEXTURE_MAX_ANISOTROPY 0x84FF
#endif
#ifndef GL_CLIP_DISTANCE0
#define GL_CLIP_DISTANCE0 0x3000                // WEBGL_clip_cull_distance
#endif
#ifndef GL_TEXTURE_CUBE_MAP_SEAMLESS
#define GL_TEXTURE_CUBE_MAP_SEAMLESS 0x884F     // always on in ES 3.0
#endif
#ifndef GL_PROGRAM_POINT_SIZE
#define GL_PROGRAM_POINT_SIZE 0x8642            // always on in ES 3.0
#endif
#ifndef GL_LINE
#define GL_LINE 0x1B01
#endif
#ifndef GL_FILL
#define GL_FILL 0x1B02
#endif
#ifndef GL_TIMESTAMP
#define GL_TIMESTAMP 0x8E28                     // EXT_disjoint_timer_query_webgl2
#endif
#ifndef GL_TIME_ELAPSED
#define GL_TIME_ELAPSED 0x88BF
#endif
#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif
#ifndef GL_DEPTH_CLAMP
#define GL_DEPTH_CLAMP 0x864F
#endif
#ifndef GL_MULTISAMPLE
#define GL_MULTISAMPLE 0x809D
#endif

#ifndef GL_QUERY_COUNTER_BITS
#define GL_QUERY_COUNTER_BITS 0x8864
#endif

// What glad reports per version and extension (glcaps, Texture.cpp). The
// browser context is no desktop version at all: compute shaders, storage
// buffers and the GPU tracer stay off. Anisotropy is asked of the context
// regardless -- Emscripten enables the extension when the browser has it, and
// without it the query answers 0, which switches it off.
#define GLAD_GL_VERSION_4_3 0
#define GLAD_GL_ARB_texture_filter_anisotropic 0
#define GLAD_GL_EXT_texture_filter_anisotropic 1

// --- Entry points -----------------------------------------------------------
// One colour buffer named, or none: ES says the same thing with the plural.
static inline void glDrawBuffer(GLenum buf) { glDrawBuffers(1, &buf); }
// Wireframe is a desktop debug view; WebGL2 cannot draw in it.
static inline void glPolygonMode(GLenum, GLenum) {}

// GPU timestamps (GpuTimer.cpp). Its probe asks GL_QUERY_COUNTER_BITS, which
// WebGL2 answers with an error and 0 bits -- so the timer switches itself off
// and these are never reached; they only have to exist.
static inline void glQueryCounter(GLuint, GLenum) {}
static inline void glGetQueryObjectiv(GLuint, GLenum, GLint* v) { if (v) *v = 0; }
static inline void glGetQueryObjectui64v(GLuint, GLenum, GLuint64* v) { if (v) *v = 0; }

// WebGL2 has getBufferSubData (a synchronous read of a buffer's bytes) where ES
// 3.0 has only mapping; Emscripten implements it under the desktop name.
#ifdef __cplusplus
extern "C" {
#endif
void glGetBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, void* data);
#ifdef __cplusplus
}
#endif
