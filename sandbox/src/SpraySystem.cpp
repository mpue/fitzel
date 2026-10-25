#include "SpraySystem.hpp"

#include <fitzel/rhi/Device.hpp>

void SprayPool::update(float dt, const SurfaceFn& surface, glm::vec3 wind) {
    for (std::size_t i = 0; i < m_parts.size();) {
        SprayP& p = m_parts[i];
        if (p.flat > 1.5f) {
            // Mist: hangs, drifts with the wind, swells and thins.
            p.vel += (wind - p.vel) * glm::min(1.0f, dt * 1.2f);
            p.vel.y += dt * 0.15f;
            p.pos += p.vel * dt;
            p.size += dt * 1.8f;
            p.life -= dt;
            if (p.life <= 0.0f) { p = m_parts.back(); m_parts.pop_back(); continue; }
        } else if (p.flat > 0.5f) {
            // Surface foam: rides the water, drifts and spreads, no gravity.
            p.pos += p.vel * dt;
            p.pos.y = surface(p.pos.x, p.pos.z) + 0.03f;
            p.vel *= glm::max(0.0f, 1.0f - dt * 1.6f); // drag out
            p.size += dt * 2.5f;                       // spread
            p.life -= dt;
            if (p.life <= 0.0f) { p = m_parts.back(); m_parts.pop_back(); continue; }
        } else {
            // Airborne droplet: arcs and falls back into the water.
            p.vel.y -= 9.81f * dt;
            p.vel += (wind * 0.35f - glm::vec3(p.vel.x, 0.0f, p.vel.z)) * glm::min(1.0f, dt * 0.25f);
            p.vel *= glm::max(0.0f, 1.0f - dt * 0.6f); // air drag
            p.pos += p.vel * dt;
            p.life -= dt;
            // The 0.3 m of slack lets a droplet break the surface before it dies,
            // so it reads as going *in* rather than winking out on contact.
            // Only asked of droplets on the way down: one coming up out of a
            // crest starts below where the crest is a moment later.
            if (p.life <= 0.0f ||
                (p.vel.y < 0.0f && p.pos.y < surface(p.pos.x, p.pos.z) - 0.3f)) {
                p = m_parts.back(); m_parts.pop_back(); continue;
            }
        }
        ++i;
    }
}

void SprayPool::pack(std::vector<float>& out) const {
    out.clear();
    out.reserve(m_parts.size() * kStride);
    for (const SprayP& p : m_parts)
        out.insert(out.end(), {p.pos.x, p.pos.y, p.pos.z, p.life / p.life0, p.size,
                               p.flat});
}

SpraySystem::~SpraySystem() {
    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    gpu.destroy(m_vbo);
    gpu.destroy(m_vao);
}

bool SpraySystem::init() {
    m_shader = fitzel::Shader::fromFiles("assets/shaders/spray.vert",
                                         "assets/shaders/spray.frag");
    if (!m_shader.isValid()) return false;

    using namespace fitzel::rhi;
    Device& gpu = device();
    // Sized for a full pool once; every frame then just overwrites the front of it.
    m_vbo = gpu.createBuffer(BufferKind::Vertex, BufferUsage::Stream,
                             SprayPool::kMax * SprayPool::kStride * sizeof(float));
    const std::uint32_t stride = SprayPool::kStride * sizeof(float);
    const VertexAttribute attrs[] = {
        {0, m_vbo, VertexFormat::Float3, 0, stride, 0},
        {1, m_vbo, VertexFormat::Float, 3 * sizeof(float), stride, 0},
        {2, m_vbo, VertexFormat::Float, 4 * sizeof(float), stride, 0},
        {3, m_vbo, VertexFormat::Float, 5 * sizeof(float), stride, 0},
    };
    VertexArrayDesc desc;
    desc.attributes     = attrs;
    desc.attributeCount = 4;
    m_vao = gpu.createVertexArray(desc);
    return true;
}

void SpraySystem::draw(const FrameContext& ctx) {
    if (!m_vao || m_pool.empty()) return;
    m_pool.pack(m_scratch);
    if (m_scratch.empty()) return;

    fitzel::rhi::Device& gpu = fitzel::rhi::device();
    gpu.bufferSubData(m_vbo, 0, m_scratch.size() * sizeof(float), m_scratch.data());
    gpu.setBlendEnabled(true);
    gpu.setBlendFunc(fitzel::rhi::BlendFactor::SrcAlpha, fitzel::rhi::BlendFactor::OneMinusSrcAlpha);
    gpu.setDepthWrite(false);
    m_shader.bind();
    m_shader.setMat4("uViewProj", ctx.viewProj);
    m_shader.setVec3("uCam", ctx.camPos);
    m_shader.setFloat("uSize", sizeScale);
    // Lit like the water it came off: dim at dusk, not a white sticker at night.
    m_shader.setVec3("uLight", ctx.lightColor * glm::max(ctx.lightDir.y, 0.0f));
    m_shader.setVec3("uAmbient", ctx.ambient);
    gpu.bindVertexArray(m_vao);
    gpu.draw(fitzel::rhi::Primitive::Points, 0,
             static_cast<int>(m_scratch.size() / SprayPool::kStride));
    gpu.bindVertexArray({});
    gpu.setDepthWrite(true);
    gpu.setBlendEnabled(false);
}
