// cubefacecheck: does cubeface.glsl pick the texel a samplerCube picks?
//
// The browser build has no room for four point-shadow cubemaps (WebGL2: 16
// samplers per fragment shader), so it renders their faces into the layers of
// one 2D array and finds face and texel itself (cubeface.glsl, lit.frag). The
// layers hold exactly what the cube faces would, so the whole question is the
// lookup -- and that is asked here of the driver, not of the spec as read:
//
//  - the same numbers go into a cubemap and into a 6-layer array: texel (x, y)
//    of face f holds f * 10000 + y * 100 + x, so a value names where it came
//    from;
//  - a fragment shader turns 65536 directions over the whole sphere into one
//    texture() on the cube and one cubeFaceUv() + texture() on the array;
//  - every pair must agree. The only allowed differences are where the answer
//    is a coin toss -- a direction on a cube EDGE (two axes equal) or on a
//    texel boundary -- and those may only be off by one neighbouring texel or
//    land on the neighbouring face.
//
// Exit 0 = the lookups agree, 1 = they do not, 2 = no GL / no shader file.
//   build\release\bin\cubefacecheck.exe sandbox\assets\shaders\cubeface.glsl
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fitzel/core/Window.hpp>
#include <fitzel/graphics/Shader.hpp>
#include <fitzel/rhi/Device.hpp>

namespace rhi = fitzel::rhi;

namespace {

constexpr int kFace = 64;   // texels per cube face edge
constexpr int kOutW = 256, kOutH = 256;

} // namespace

int main(int argc, char** argv) {
    const std::string glslPath = argc > 1 ? argv[1] : "assets/shaders/cubeface.glsl";
    std::ifstream in(glslPath);
    if (!in) {
        std::printf("[FAIL] cannot read %s\n", glslPath.c_str());
        return 2;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string cubeFace = ss.str();

    // A hidden window for the context, alive until the end of main.
    std::unique_ptr<fitzel::Window> window;
    try {
        fitzel::WindowConfig wc;
        wc.width   = 64;
        wc.height  = 64;
        wc.title   = "cubefacecheck";
        wc.vsync   = false;
        wc.visible = false;
        window = std::make_unique<fitzel::Window>(wc);
    } catch (const std::exception& e) {
        std::printf("[FAIL] no GL context: %s\n", e.what());
        return 2;
    }
    rhi::Device& gpu = rhi::device();

    // The same numbers into both: the six faces one after another, which is
    // both a cube's face order and an array's layer order.
    std::vector<float> faces(static_cast<std::size_t>(kFace) * kFace * 6);
    for (int f = 0; f < 6; ++f)
        for (int y = 0; y < kFace; ++y)
            for (int x = 0; x < kFace; ++x)
                faces[(static_cast<std::size_t>(f) * kFace + y) * kFace + x] =
                    static_cast<float>(f * 10000 + y * 100 + x);
    // Nearest, clamped across the face; the third axis keeps OpenGL's default.
    rhi::SamplerDesc nearest = rhi::SamplerDesc::nearestClamp();
    nearest.wrapW = rhi::Wrap::Repeat;
    rhi::TextureDesc cd;
    cd.type    = rhi::TextureType::Cube;
    cd.format  = rhi::Format::R32F;
    cd.width   = kFace;
    cd.height  = kFace;
    cd.sampler = nearest;
    const rhi::TextureHandle cube = gpu.createTexture(cd, faces.data(), rhi::Format::R32F);
    rhi::TextureDesc ad = cd;
    ad.type  = rhi::TextureType::Tex2DArray;
    ad.depth = 6;
    const rhi::TextureHandle arr = gpu.createTexture(ad, faces.data(), rhi::Format::R32F);

    // Directions on a Fibonacci sphere, one per output pixel; out = (cube,
    // array, how far from a cube edge: 0 = exactly on one).
    const std::string vs =
        "#version 330 core\n"
        "void main() {\n"
        "    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);\n"
        "    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
        "}\n";
    const std::string fs =
        "#version 330 core\n"
        "uniform samplerCube uCube;\n"
        "uniform sampler2DArray uArr;\n"
        "out vec4 o;\n" + cubeFace +
        "\nvoid main() {\n"
        "    float i = floor(gl_FragCoord.y) * 256.0 + floor(gl_FragCoord.x);\n"
        "    float n = 65536.0;\n"
        "    float z = 1.0 - 2.0 * (i + 0.5) / n;\n"
        "    float r = sqrt(max(0.0, 1.0 - z * z));\n"
        "    float phi = i * 2.39996323;\n"
        "    vec3 d = vec3(r * cos(phi), z, r * sin(phi));\n"
        "    vec3 f = cubeFaceUv(d);\n"
        "    vec3 a = abs(d);\n"
        "    float hi = max(a.x, max(a.y, a.z));\n"
        "    float mid = a.x + a.y + a.z - hi - min(a.x, min(a.y, a.z));\n"
        "    o = vec4(texture(uCube, d).r, texture(uArr, vec3(f.xy, f.z)).r,\n"
        "             (hi - mid) / hi, 1.0);\n"
        "}\n";
    // The compiler's log goes to stderr on a failure.
    const fitzel::Shader prog = fitzel::Shader::fromSource(vs, fs);
    if (!prog.isValid()) { std::printf("[FAIL] shader/link\n"); return 2; }

    rhi::TextureDesc od;
    od.format = rhi::Format::RGBA32F;
    od.width  = kOutW;
    od.height = kOutH;
    const rhi::TextureHandle out = gpu.createTexture(od);
    rhi::FramebufferDesc fd;
    fd.color[0] = rhi::Attachment{out};
    const rhi::FramebufferHandle fbo = gpu.createFramebuffer(fd);
    gpu.bindFramebuffer(fbo);
    // No attributes: the vertex shader makes its triangle from gl_VertexID.
    const rhi::VertexArrayHandle vao = gpu.createVertexArray(rhi::VertexArrayDesc{});
    gpu.bindVertexArray(vao);
    gpu.setViewport(0, 0, kOutW, kOutH);
    prog.bind();
    gpu.bindTexture(0, cube);
    gpu.bindTexture(1, arr);
    prog.setInt("uCube", 0);
    prog.setInt("uArr", 1);
    gpu.draw(rhi::Primitive::Triangles, 0, 3);

    std::vector<float> px(static_cast<std::size_t>(kOutW) * kOutH * 4);
    gpu.readPixels(fbo, 0, rhi::Rect{0, 0, kOutW, kOutH}, rhi::Format::RGBA32F, px.data());

    int same = 0, edge = 0, neighbour = 0, wrong = 0;
    for (std::size_t i = 0; i < px.size(); i += 4) {
        const int c = static_cast<int>(px[i]), a = static_cast<int>(px[i + 1]);
        const float margin = px[i + 2];
        if (c == a) { ++same; continue; }
        const int cf = c / 10000, af = a / 10000;
        const int dx = std::abs(c % 100 - a % 100), dy = std::abs((c / 100) % 100 - (a / 100) % 100);
        if (cf != af) {
            if (margin < 1e-3f) { ++edge; continue; }
        } else if (dx <= 1 && dy <= 1) {
            ++neighbour;
            continue;
        }
        if (wrong < 8)
            std::printf("  mismatch: cube %d (face %d), array %d (face %d), edge margin %g\n",
                        c, cf, a, af, margin);
        ++wrong;
    }
    const int total = kOutW * kOutH;
    std::printf("%s\n", gpu.caps().apiName);
    std::printf("%d directions: %d identical, %d on a cube edge, %d one texel off, %d wrong\n",
                total, same, edge, neighbour, wrong);
    // A texel boundary falls between the two by float rounding now and then;
    // more than a sliver of those means the (s, t) mapping is not the same.
    const bool ok = wrong == 0 && neighbour < total / 200;
    std::printf("%s\n", ok ? "OK -- cubeface.glsl matches the driver's cube lookup"
                           : "FAIL -- cubeface.glsl does not pick the cube's texels");
    gpu.bindVertexArray({});
    gpu.destroy(vao);
    gpu.bindFramebuffer({});
    gpu.destroy(fbo);
    gpu.destroy(out);
    gpu.destroy(cube);
    gpu.destroy(arr);
    return ok ? 0 : 1;
}
