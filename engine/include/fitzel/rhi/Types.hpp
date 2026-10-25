#pragma once

// The vocabulary of the render hardware interface: handles, formats and the
// state a draw is made in. Nothing here names a graphics API -- see Device.hpp
// for the calls, and docs/rhi.md for how each concept maps onto OpenGL (the
// backend there is) and Vulkan (the one this is shaped to admit).

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <glm/glm.hpp>

namespace fitzel::rhi {

// --- Handles ----------------------------------------------------------------------
//
// A resource is a number the backend hands out and alone knows the meaning of:
// a slot in its own tables, never an OpenGL name or a Vulkan pointer. Zero is
// "none" -- and, for a framebuffer, the window's own (see FramebufferHandle).
// Typed, so a texture cannot be passed where a buffer is wanted, and opaque, so
// nothing outside the backend can hand one to an API call behind its back.

template <class Tag>
struct Handle {
    std::uint32_t id = 0;

    constexpr Handle() = default;
    constexpr explicit Handle(std::uint32_t v) : id(v) {}

    constexpr bool valid() const { return id != 0; }
    constexpr explicit operator bool() const { return id != 0; }
    friend constexpr bool operator==(Handle a, Handle b) { return a.id == b.id; }
    friend constexpr bool operator!=(Handle a, Handle b) { return a.id != b.id; }
    friend constexpr bool operator<(Handle a, Handle b) { return a.id < b.id; }
};

using BufferHandle      = Handle<struct BufferTag>;
using TextureHandle     = Handle<struct TextureTag>;
// Zero is the window's swapchain image (OpenGL's default framebuffer).
using FramebufferHandle = Handle<struct FramebufferTag>;
using ProgramHandle     = Handle<struct ProgramTag>;
// The vertex input of a draw: which buffers feed which attributes, plus the
// index buffer. OpenGL keeps that in a VAO; Vulkan splits it into the vertex
// input state of a pipeline and the buffers bound at draw time.
using VertexArrayHandle = Handle<struct VertexArrayTag>;
using QueryHandle       = Handle<struct QueryTag>;
using FenceHandle       = Handle<struct FenceTag>;

// --- Formats ----------------------------------------------------------------------
//
// One list for both what a texture stores and how pixels handed to or taken from
// it are laid out. An upload whose data format differs from the texture's is
// converted (OpenGL does it in the driver; a backend without that does it itself);
// a read-back converts the other way.
enum class Format : std::uint8_t {
    Unknown,
    R8, RG8, RGB8, RGBA8,
    R16F, RG16F, RGB16F, RGBA16F,
    R32F, RG32F, RGB32F, RGBA32F,
    R32UI,
    Depth16, Depth24, Depth32F,
    Depth24Stencil8, Depth32FStencil8,
};

bool isDepthFormat(Format f);
bool hasStencil(Format f);
bool isFloatFormat(Format f);   // a float or half-float colour format
int  channelCount(Format f);    // components per pixel (depth counts as 1)
// Bytes per pixel when laid out as data (RGB8 = 3, Depth24Stencil8 = 4, ...).
std::size_t bytesPerPixel(Format f);

enum class TextureType : std::uint8_t { Tex2D, Tex2DArray, Tex3D, Cube };

// What a texture is for. Sampled is the norm; a texture that is only ever drawn
// into and never read (a depth buffer behind a colour target) leaves it out, and
// a backend may then keep it somewhere cheaper (an OpenGL renderbuffer, a Vulkan
// transient attachment). Storage: written by compute through image load/store.
enum TextureUsage : std::uint8_t {
    UsageSampled      = 1u << 0,
    UsageRenderTarget = 1u << 1,
    UsageStorage      = 1u << 2,
};

// --- Sampling ---------------------------------------------------------------------

enum class Filter : std::uint8_t { Nearest, Linear };
// Between mip levels. None samples level 0 only, whatever else the texture holds.
enum class MipFilter : std::uint8_t { None, Nearest, Linear };
enum class Wrap : std::uint8_t { Repeat, MirroredRepeat, ClampToEdge, ClampToBorder };

// How a texture is read. OpenGL keeps it on the texture, Vulkan in a sampler
// object next to it; here it belongs to the texture as in OpenGL, and a backend
// with samplers keeps one per distinct description.
struct SamplerDesc {
    Filter    minFilter = Filter::Linear;
    Filter    magFilter = Filter::Linear;
    MipFilter mipFilter = MipFilter::None;
    Wrap      wrapU = Wrap::ClampToEdge;
    Wrap      wrapV = Wrap::ClampToEdge;
    Wrap      wrapW = Wrap::ClampToEdge;
    // > 1: anisotropic filtering, clamped to what the device has (Caps).
    float     anisotropy = 1.0f;
    // The highest mip level sampled; mips past it are ignored.
    int       maxLevel = 1000;
    glm::vec4 borderColor{0.0f};   // for Wrap::ClampToBorder

    static SamplerDesc nearestClamp() {
        SamplerDesc s;
        s.minFilter = s.magFilter = Filter::Nearest;
        return s;
    }
    static SamplerDesc linearClamp() { return {}; }
    static SamplerDesc linearRepeat() {
        SamplerDesc s;
        s.wrapU = s.wrapV = s.wrapW = Wrap::Repeat;
        return s;
    }
    // Trilinear and repeating: the sampler of a material's map.
    static SamplerDesc mipmappedRepeat(float anisotropy = 1.0f) {
        SamplerDesc s = linearRepeat();
        s.mipFilter  = MipFilter::Linear;
        s.anisotropy = anisotropy;
        return s;
    }
};

struct TextureDesc {
    TextureType type   = TextureType::Tex2D;
    Format      format = Format::RGBA8;
    int width  = 1;
    int height = 1;
    // Layers of a 2D array, slices of a 3D texture; 1 otherwise (a cube has six
    // faces and says so itself).
    int depth  = 1;
    // Mip levels storage is made for. 0: the full chain down to 1x1.
    int mipLevels = 1;
    std::uint8_t usage = UsageSampled | UsageRenderTarget;
    SamplerDesc  sampler;
};

// Part of one mip level: a rectangle of layers (array), slices (3D) or faces
// (cube, z = face index in +X -X +Y -Y +Z -Z order). w/h/d of 0 mean "the whole
// level". `rowLength` is the source's row pitch in pixels when it is wider than
// w (a sub-rectangle of a bigger picture); 0 means tightly packed. Rows are
// always byte-aligned: no row padding is assumed on either side.
struct TextureRegion {
    int level = 0;
    int x = 0, y = 0, z = 0;
    int w = 0, h = 0, d = 0;
    int rowLength = 0;
};

// --- Buffers ----------------------------------------------------------------------

// What a buffer is bound as. Fixed for its life (WebGL insists, Vulkan wants
// the usage at creation anyway).
enum class BufferKind : std::uint8_t {
    Vertex,
    Index,
    Uniform,
    Storage,
    // The destination of an asynchronous pixel read-back (readPixelsToBuffer):
    // read on the CPU once a fence says the copy has landed.
    Readback,
};

// How often the contents change: a hint for where the memory lives.
enum class BufferUsage : std::uint8_t { Static, Dynamic, Stream };

// --- Vertex input -----------------------------------------------------------------

enum class VertexFormat : std::uint8_t {
    Float, Float2, Float3, Float4,
    UByte4Norm,   // four bytes, read as 0..1
    Int, UInt,    // read as an integer attribute (ivec/uvec in the shader)
};

struct VertexAttribute {
    std::uint32_t location = 0;
    BufferHandle  buffer;
    VertexFormat  format   = VertexFormat::Float3;
    std::uint32_t offset   = 0;    // bytes into the buffer
    std::uint32_t stride   = 0;    // bytes between elements; 0 = tightly packed
    // 0: advances per vertex. 1: per instance.
    std::uint32_t divisor  = 0;
};

// An attribute the shader reads that no buffer feeds: every vertex sees `value`.
struct ConstantAttribute {
    std::uint32_t location = 0;
    glm::vec4     value{0.0f, 0.0f, 0.0f, 1.0f};
};

struct VertexArrayDesc {
    const VertexAttribute*   attributes = nullptr;
    std::size_t              attributeCount = 0;
    const ConstantAttribute* constants = nullptr;
    std::size_t              constantCount = 0;
    BufferHandle             indexBuffer;   // none: draw() only, no drawIndexed()
};

enum class Primitive : std::uint8_t { Points, Lines, LineStrip, Triangles, TriangleStrip };
enum class IndexType : std::uint8_t { U16, U32 };

// --- Render state -----------------------------------------------------------------
//
// Everything a draw depends on besides what it reads. Set piece by piece or as a
// whole; the device remembers it, so it can also be asked what it is (to put it
// back after a pass). A Vulkan backend folds it, with the program, the vertex
// layout and the framebuffer's formats, into the pipeline it draws with.

enum class CompareOp : std::uint8_t {
    Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always,
};

enum class BlendFactor : std::uint8_t {
    Zero, One,
    SrcColor, OneMinusSrcColor, DstColor, OneMinusDstColor,
    SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha,
    ConstantColor, OneMinusConstantColor, SrcAlphaSaturate,
};
enum class BlendOp : std::uint8_t { Add, Subtract, ReverseSubtract, Min, Max };

enum class CullMode : std::uint8_t { Back, Front, FrontAndBack };

enum class StencilOp : std::uint8_t {
    Keep, Zero, Replace, Increment, IncrementWrap, Decrement, DecrementWrap, Invert,
};

struct BlendState {
    bool        enabled  = false;
    BlendFactor srcColor = BlendFactor::One;
    BlendFactor dstColor = BlendFactor::Zero;
    BlendFactor srcAlpha = BlendFactor::One;
    BlendFactor dstAlpha = BlendFactor::Zero;
    BlendOp     colorOp  = BlendOp::Add;
    BlendOp     alphaOp  = BlendOp::Add;
    friend bool operator==(const BlendState&, const BlendState&) = default;
};

struct DepthState {
    bool      test  = false;
    bool      write = true;
    CompareOp func  = CompareOp::Less;
    friend bool operator==(const DepthState&, const DepthState&) = default;
};

// One set of stencil rules for both faces.
struct StencilState {
    bool          test      = false;
    CompareOp     func      = CompareOp::Always;
    int           ref       = 0;
    std::uint32_t readMask  = 0xFF;
    std::uint32_t writeMask = 0xFF;
    StencilOp     fail      = StencilOp::Keep;
    StencilOp     depthFail = StencilOp::Keep;
    StencilOp     pass      = StencilOp::Keep;
    friend bool operator==(const StencilState&, const StencilState&) = default;
};

struct RasterState {
    bool     cull       = false;
    CullMode cullFace   = CullMode::Back;   // which faces cull removes
    bool     wireframe  = false;
    bool     polygonOffset = false;
    float    offsetFactor  = 0.0f;
    float    offsetUnits   = 0.0f;
    bool     scissor    = false;
    // Bit i: the vertex stage's gl_ClipDistance[i] clips.
    std::uint8_t clipDistances = 0;
    friend bool operator==(const RasterState&, const RasterState&) = default;
};

struct ColorMask {
    bool r = true, g = true, b = true, a = true;
    friend bool operator==(const ColorMask&, const ColorMask&) = default;
};

struct RenderState {
    BlendState   blend;
    DepthState   depth;
    StencilState stencil;
    RasterState  raster;
    ColorMask    colorMask;
    friend bool operator==(const RenderState&, const RenderState&) = default;
};

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    friend bool operator==(const Rect&, const Rect&) = default;
};

// --- Framebuffers -----------------------------------------------------------------

constexpr int kMaxColorAttachments = 8;

// One image of a texture to draw into. `layer` picks a layer of an array, a face
// of a cube (0..5, +X -X +Y -Y +Z -Z) or a slice of a 3D texture; -1 means the
// texture is 2D -- or, for an array or cube, all of it at once (layered).
struct Attachment {
    TextureHandle texture;
    int level = 0;
    int layer = -1;
    friend bool operator==(const Attachment&, const Attachment&) = default;
};

// Colour attachments are drawn into in order (fragment output i -> color[i]);
// a framebuffer with none draws depth only. `depth` takes a depth or a
// depth-stencil texture -- the format says which.
struct FramebufferDesc {
    std::array<Attachment, kMaxColorAttachments> color{};
    Attachment depth;
};

// Which image of a framebuffer a read takes: a colour attachment by index, the
// depth, or -- the window's framebuffer only -- the picture shown last.
constexpr int kReadDepth = -1;
constexpr int kReadFront = -2;

enum ClearMask : std::uint8_t {
    ClearColor   = 1u << 0,
    ClearDepth   = 1u << 1,
    ClearStencil = 1u << 2,
};

enum BlitMask : std::uint8_t {
    BlitColor = 1u << 0,
    BlitDepth = 1u << 1,
};

// --- Programs ---------------------------------------------------------------------

// Shaders are GLSL as the engine's shader files are written: #version 330 core
// (430 for compute), plain uniforms, samplers given a texture unit through
// setUniform. A backend translates as it must -- the OpenGL one to GLSL ES for
// WebGL, a Vulkan one to SPIR-V with the loose uniforms gathered into one block.
struct ProgramSource {
    const char* vertex   = nullptr;   // with fragment: a draw program
    const char* fragment = nullptr;
    const char* compute  = nullptr;   // alone: a compute program
};

enum class UniformType : std::uint8_t {
    Int, IVec2, IVec3, IVec4,
    Float, Vec2, Vec3, Vec4,
    Mat3, Mat4,
};

// --- Compute ----------------------------------------------------------------------

enum class Access : std::uint8_t { ReadOnly, WriteOnly, ReadWrite };

// What the next reads need to see of what compute wrote.
enum Barrier : std::uint32_t {
    BarrierImageAccess  = 1u << 0,   // image load/store
    BarrierTextureFetch = 1u << 1,   // sampling
    BarrierTextureUpdate = 1u << 2,  // uploads and read-backs of textures
    BarrierStorage      = 1u << 3,   // storage buffers
    BarrierBufferUpdate = 1u << 4,   // buffer uploads and read-backs
    BarrierAll          = 0xFFFFFFFFu,
};

// --- Queries ----------------------------------------------------------------------

enum class QueryType : std::uint8_t {
    TimeElapsed,      // GPU time between begin and end, in ns
    Timestamp,        // GPU clock at timestamp(), in ns
    AnySamplesPassed, // whether anything between begin and end passed depth
};

// --- What the device can do -------------------------------------------------------

enum class Backend : std::uint8_t { OpenGL, Vulkan };

struct Caps {
    Backend backend = Backend::OpenGL;
    // Compute programs, storage buffers and image load/store. The engine draws
    // without them; one feature (the GPU path tracer) needs them.
    bool  compute = false;
    // Timer and timestamp queries.
    bool  timerQueries = false;
    int   timestampBits = 0;
    // Running in a browser (WebGL2): fewer samplers, no texture read-back, and
    // a few shader features missing.
    bool  web = false;
    int   maxTextureSize = 0;
    float maxAnisotropy = 1.0f;
    std::int64_t maxStorageBlockSize = 0;
    int   apiMajor = 0, apiMinor = 0;
    const char* apiName  = "";   // "OpenGL 4.6 ...", for a log line
    const char* apiVersion = ""; // the driver's own version string ("4.6.0 NVIDIA ...")
    const char* renderer = "";   // the GPU, as the driver names it
    const char* vendor   = "";
};

} // namespace fitzel::rhi

template <class Tag>
struct std::hash<fitzel::rhi::Handle<Tag>> {
    std::size_t operator()(fitzel::rhi::Handle<Tag> h) const noexcept {
        return std::hash<std::uint32_t>{}(h.id);
    }
};
