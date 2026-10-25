#pragma once

// The device's vocabulary in OpenGL's words.

#include <glad/gl.h>

#include "fitzel/rhi/Types.hpp"

namespace fitzel::rhi::gl {

// How a Format is stored (internal) and how data in it is laid out (format/type).
struct GlFormat {
    GLenum internal = 0;
    GLenum format   = 0;
    GLenum type     = 0;
};

inline GlFormat formatOf(Format f) {
    switch (f) {
        case Format::R8:      return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
        case Format::RG8:     return {GL_RG8, GL_RG, GL_UNSIGNED_BYTE};
        case Format::RGB8:    return {GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE};
        case Format::RGBA8:   return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
        case Format::R16F:    return {GL_R16F, GL_RED, GL_HALF_FLOAT};
        case Format::RG16F:   return {GL_RG16F, GL_RG, GL_HALF_FLOAT};
        case Format::RGB16F:  return {GL_RGB16F, GL_RGB, GL_HALF_FLOAT};
        case Format::RGBA16F: return {GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT};
        case Format::R32F:    return {GL_R32F, GL_RED, GL_FLOAT};
        case Format::RG32F:   return {GL_RG32F, GL_RG, GL_FLOAT};
        case Format::RGB32F:  return {GL_RGB32F, GL_RGB, GL_FLOAT};
        case Format::RGBA32F: return {GL_RGBA32F, GL_RGBA, GL_FLOAT};
        case Format::R32UI:   return {GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT};
        case Format::Depth16: return {GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT};
        case Format::Depth24: return {GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT};
        case Format::Depth32F: return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
        case Format::Depth24Stencil8:
            return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
        case Format::Depth32FStencil8:
            return {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV};
        case Format::Unknown: break;
    }
    return {};
}

// A unit holds one texture per target; these index the four the device uses.
inline GLenum targetOfIndex(int k) {
    switch (k) {
        case 1:  return GL_TEXTURE_2D_ARRAY;
        case 2:  return GL_TEXTURE_3D;
        case 3:  return GL_TEXTURE_CUBE_MAP;
        default: return GL_TEXTURE_2D;
    }
}
inline int indexOfTarget(GLenum target) {
    switch (target) {
        case GL_TEXTURE_2D_ARRAY: return 1;
        case GL_TEXTURE_3D:       return 2;
        case GL_TEXTURE_CUBE_MAP: return 3;
        default:                  return 0;
    }
}
inline GLenum targetOf(TextureType t) {
    switch (t) {
        case TextureType::Tex2DArray: return GL_TEXTURE_2D_ARRAY;
        case TextureType::Tex3D:      return GL_TEXTURE_3D;
        case TextureType::Cube:       return GL_TEXTURE_CUBE_MAP;
        case TextureType::Tex2D:      break;
    }
    return GL_TEXTURE_2D;
}

inline GLenum wrapOf(Wrap w) {
    switch (w) {
        case Wrap::Repeat:         return GL_REPEAT;
        case Wrap::MirroredRepeat: return GL_MIRRORED_REPEAT;
        case Wrap::ClampToEdge:    return GL_CLAMP_TO_EDGE;
        case Wrap::ClampToBorder:
#ifdef __EMSCRIPTEN__
            // WebGL2 has no border: the edge texel is the nearest thing.
            return GL_CLAMP_TO_EDGE;
#else
            return GL_CLAMP_TO_BORDER;
#endif
    }
    return GL_CLAMP_TO_EDGE;
}

inline GLenum minFilterOf(Filter f, MipFilter m) {
    const bool lin = f == Filter::Linear;
    switch (m) {
        case MipFilter::None:    return lin ? GL_LINEAR : GL_NEAREST;
        case MipFilter::Nearest: return lin ? GL_LINEAR_MIPMAP_NEAREST : GL_NEAREST_MIPMAP_NEAREST;
        case MipFilter::Linear:  return lin ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
    }
    return GL_LINEAR;
}

inline GLenum filterOf(Filter f) { return f == Filter::Linear ? GL_LINEAR : GL_NEAREST; }

inline GLenum blendFactor(BlendFactor f) {
    switch (f) {
        case BlendFactor::Zero:             return GL_ZERO;
        case BlendFactor::One:              return GL_ONE;
        case BlendFactor::SrcColor:         return GL_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return GL_ONE_MINUS_SRC_COLOR;
        case BlendFactor::DstColor:         return GL_DST_COLOR;
        case BlendFactor::OneMinusDstColor: return GL_ONE_MINUS_DST_COLOR;
        case BlendFactor::SrcAlpha:         return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha:         return GL_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
        case BlendFactor::ConstantColor:    return GL_CONSTANT_COLOR;
        case BlendFactor::OneMinusConstantColor: return GL_ONE_MINUS_CONSTANT_COLOR;
        case BlendFactor::SrcAlphaSaturate: return GL_SRC_ALPHA_SATURATE;
    }
    return GL_ONE;
}

inline GLenum blendOp(BlendOp o) {
    switch (o) {
        case BlendOp::Add:             return GL_FUNC_ADD;
        case BlendOp::Subtract:        return GL_FUNC_SUBTRACT;
        case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case BlendOp::Min:             return GL_MIN;
        case BlendOp::Max:             return GL_MAX;
    }
    return GL_FUNC_ADD;
}

inline GLenum compareOp(CompareOp c) {
    switch (c) {
        case CompareOp::Never:        return GL_NEVER;
        case CompareOp::Less:         return GL_LESS;
        case CompareOp::Equal:        return GL_EQUAL;
        case CompareOp::LessEqual:    return GL_LEQUAL;
        case CompareOp::Greater:      return GL_GREATER;
        case CompareOp::NotEqual:     return GL_NOTEQUAL;
        case CompareOp::GreaterEqual: return GL_GEQUAL;
        case CompareOp::Always:       return GL_ALWAYS;
    }
    return GL_LESS;
}

inline GLenum stencilOp(StencilOp o) {
    switch (o) {
        case StencilOp::Keep:          return GL_KEEP;
        case StencilOp::Zero:          return GL_ZERO;
        case StencilOp::Replace:       return GL_REPLACE;
        case StencilOp::Increment:     return GL_INCR;
        case StencilOp::IncrementWrap: return GL_INCR_WRAP;
        case StencilOp::Decrement:     return GL_DECR;
        case StencilOp::DecrementWrap: return GL_DECR_WRAP;
        case StencilOp::Invert:        return GL_INVERT;
    }
    return GL_KEEP;
}

inline GLenum cullFace(CullMode m) {
    switch (m) {
        case CullMode::Back:         return GL_BACK;
        case CullMode::Front:        return GL_FRONT;
        case CullMode::FrontAndBack: return GL_FRONT_AND_BACK;
    }
    return GL_BACK;
}

inline GLenum primitiveOf(Primitive p) {
    switch (p) {
        case Primitive::Points:        return GL_POINTS;
        case Primitive::Lines:         return GL_LINES;
        case Primitive::LineStrip:     return GL_LINE_STRIP;
        case Primitive::Triangles:     return GL_TRIANGLES;
        case Primitive::TriangleStrip: return GL_TRIANGLE_STRIP;
    }
    return GL_TRIANGLES;
}

inline GLenum usageOf(BufferKind kind, BufferUsage u) {
    if (kind == BufferKind::Readback) return GL_STREAM_READ;
    switch (u) {
        case BufferUsage::Static:  return GL_STATIC_DRAW;
        case BufferUsage::Dynamic: return GL_DYNAMIC_DRAW;
        case BufferUsage::Stream:  return GL_STREAM_DRAW;
    }
    return GL_STATIC_DRAW;
}

#ifndef __EMSCRIPTEN__
// Image load/store: desktop 4.3 only.
inline GLenum accessOf(Access a) {
    switch (a) {
        case Access::ReadOnly:  return GL_READ_ONLY;
        case Access::WriteOnly: return GL_WRITE_ONLY;
        case Access::ReadWrite: return GL_READ_WRITE;
    }
    return GL_READ_WRITE;
}
#endif

} // namespace fitzel::rhi::gl
