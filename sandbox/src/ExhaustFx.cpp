#include "ExhaustFx.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include <fitzel/rhi/Device.hpp>

#include "Component.hpp"
#include "SceneGraph.hpp"
#include "SceneTypes.hpp"

namespace rhi = fitzel::rhi;

namespace {

constexpr int kNoiseRes = 128;
// What one puff is on the GPU: 3 vec4s -- centre and radius; mass, glow, seed,
// opacity; how smoothly it merges with its neighbours (see the shaders).
constexpr std::size_t kPuffVec4 = 3;
// The coarse grid's voxel budget: the column a rocket leaves is kilometres tall
// and a few hundred metres wide, and this is what it gets to be drawn with.
constexpr glm::ivec3 kFarDims{128, 320, 128};

struct Indexed {
    char buf[32];
    Indexed(const char* base, int i) { std::snprintf(buf, sizeof(buf), "%s[%d]", base, i); }
    operator std::string_view() const { return buf; } // NOLINT
};

rhi::TextureHandle make3D(glm::ivec3 d, rhi::Format f, bool linear, bool repeat, bool storage) {
    rhi::TextureDesc desc;
    desc.type   = rhi::TextureType::Tex3D;
    desc.format = f;
    desc.width  = d.x;
    desc.height = d.y;
    desc.depth  = d.z;
    desc.usage  = rhi::UsageSampled | (storage ? rhi::UsageStorage : 0);
    desc.sampler.minFilter = desc.sampler.magFilter = linear ? rhi::Filter::Linear : rhi::Filter::Nearest;
    const rhi::Wrap w = repeat ? rhi::Wrap::Repeat : rhi::Wrap::ClampToEdge;
    desc.sampler.wrapU = desc.sampler.wrapV = desc.sampler.wrapW = w;
    return rhi::device().createTexture(desc);
}

void destroyTex(rhi::TextureHandle& h) {
    if (h) rhi::device().destroy(h);
    h = {};
}

// Quantise a voxel size up to a step of 2^(1/4): the coarse grid refits every
// frame, and a voxel that changed by a hair each time would make the whole
// column shimmer. This way it changes in noticeable but rare steps.
float quantVoxel(float v) {
    const float k = std::ceil(std::log2(std::max(v, 0.01f)) * 4.0f);
    return std::exp2(k / 4.0f);
}

} // namespace

ExhaustFx::~ExhaustFx() {
    for (Grid& g : m_grid) {
        destroyTex(g.a); destroyTex(g.b); destroyTex(g.l[0]); destroyTex(g.l[1]); destroyTex(g.occ);
        if (g.startBuf) rhi::device().destroy(g.startBuf);
        if (g.listBuf) rhi::device().destroy(g.listBuf);
    }
    destroyTex(m_noise);
    if (m_puffBuf) rhi::device().destroy(m_puffBuf);
}

bool ExhaustFx::init() {
    rhi::Device& gpu = rhi::device();
    if (!gpu.caps().compute) {
        std::fprintf(stderr, "Rocket exhaust needs compute shaders; it is switched off\n");
        return false;
    }
    const auto comp = [](const char* path) {
        return fitzel::Shader::computeFromSource(fitzel::Shader::readSource(path));
    };
    m_noiseBake = comp("assets/shaders/exhaustnoise.comp");
    m_splat     = comp("assets/shaders/exhaustsplat.comp");
    m_light     = comp("assets/shaders/exhaustlight.comp");
    m_march = fitzel::Shader::fromFiles("assets/shaders/sky.vert", "assets/shaders/exhaust.frag");
    m_up    = fitzel::Shader::fromFiles("assets/shaders/sky.vert", "assets/shaders/exhaustup.frag");
    if (!m_noiseBake.isValid() || !m_splat.isValid() || !m_light.isValid() ||
        !m_march.isValid() || !m_up.isValid()) {
        std::fprintf(stderr, "Failed to load rocket exhaust shaders\n");
        return false;
    }

    m_noise = make3D(glm::ivec3(kNoiseRes), rhi::Format::RGBA8, true, true, true);
    m_noiseBake.bind();
    gpu.bindImage(0, m_noise, 0, rhi::Access::WriteOnly, rhi::Format::RGBA8);
    m_noiseBake.setInt("uRes", kNoiseRes);
    gpu.dispatch(kNoiseRes / 4, kNoiseRes / 4, kNoiseRes / 4);
    gpu.memoryBarrier(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
    m_ok = true;
    return true;
}

void ExhaustFx::clear() {
    m_sim.clear();
    m_flames.clear();
    m_lastNozzle.clear();
    m_nearPlaced = false;
    for (Grid& g : m_grid) g.on = g.lit = false;
}

void ExhaustFx::allocGrid(Grid& g, glm::ivec3 dims) {
    if (g.dims == dims && g.a) return;
    destroyTex(g.a); destroyTex(g.b); destroyTex(g.l[0]); destroyTex(g.l[1]); destroyTex(g.occ);
    g.dims = dims;
    g.lit  = false;   // new textures: no light to ease from
    g.a   = make3D(dims, rhi::Format::RGBA16F, true, false, true);
    // Puff indices: nearest, and 32-bit floats so an index is exact.
    g.b   = make3D(dims, rhi::Format::RGBA32F, false, false, true);
    g.l[0] = make3D(dims, rhi::Format::RGBA16F, true, false, true);
    g.l[1] = make3D(dims, rhi::Format::RGBA16F, true, false, true);
    // Per brick, whether any puff was binned into it: written by the splat
    // alongside the rest, so it always belongs to the same frame's lists.
    g.occ = make3D(dims / 8, rhi::Format::R32F, false, false, true);
}

// What changes every frame goes into a NEW buffer every frame; the old one
// lives on until the GPU is done with it (a delete waits for that). Refilled in
// place, the brick lists were left to the driver to keep apart from the frame
// still in flight, and while the CPU ran a frame ahead of the GPU -- the
// editor's Play; a --film reads every frame back and drains the GPU -- one
// frame's splat could take the next frame's lists: puffs drawn from the wrong
// bricks, a launch cloud cut into boxes for a frame or two, which reads as
// glitter on the smoke.
bool ExhaustFx::uploadBuffer(rhi::BufferHandle& id, const void* data, std::size_t bytes) {
    rhi::Device& gpu = rhi::device();
    if (id) gpu.destroy(id);
    id = gpu.createBuffer(rhi::BufferKind::Storage, rhi::BufferUsage::Stream,
                          std::max<std::size_t>(bytes, 16), bytes ? data : nullptr);
    return static_cast<bool>(id);
}

ExhaustFx::Box ExhaustFx::inside(const Grid& finer, float margin) {
    Box b;
    if (!finer.on) return b;
    b.lo = finer.origin + margin;
    b.hi = finer.origin + glm::vec3(finer.dims) * finer.voxel - margin;
    return b;
}

// Every puff into every brick its sphere touches: a counting sort, so the
// lists come out contiguous per brick in one buffer. Puffs wholly inside a
// skip box are left out (see Box).
void ExhaustFx::binGrid(Grid& g, const std::vector<glm::vec4>& puffs, const Box& skipA, const Box& skipB) {
    const glm::ivec3 nb = g.bricks();
    const int bricks = nb.x * nb.y * nb.z;
    const float bs = g.voxel * 8.0f;
    g.start.assign(static_cast<std::size_t>(bricks) + 1, 0u);
    const std::size_t n = puffs.size() / kPuffVec4;
    const auto within = [](const Box& k, const glm::vec4& p) {
        return glm::all(glm::greaterThanEqual(glm::vec3(p) - p.w, k.lo)) &&
               glm::all(glm::lessThanEqual(glm::vec3(p) + p.w, k.hi));
    };
    const auto range = [&](const glm::vec4& p, glm::ivec3& lo, glm::ivec3& hi) {
        if (within(skipA, p) || within(skipB, p)) return false;
        const glm::vec3 a = (glm::vec3(p) - p.w - g.origin) / bs;
        const glm::vec3 b = (glm::vec3(p) + p.w - g.origin) / bs;
        lo = glm::max(glm::ivec3(glm::floor(a)), glm::ivec3(0));
        hi = glm::min(glm::ivec3(glm::floor(b)), nb - 1);
        return lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z;
    };
    for (std::size_t i = 0; i < n; ++i) {
        glm::ivec3 lo, hi;
        if (puffs[kPuffVec4 * i + 1].x < 1e-4f || !range(puffs[kPuffVec4 * i], lo, hi)) continue;
        for (int z = lo.z; z <= hi.z; ++z)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x) ++g.start[x + nb.x * (y + nb.y * z) + 1];
    }
    for (int b = 0; b < bricks; ++b) g.start[b + 1] += g.start[b];
    g.list.resize(g.start[bricks]);
    std::vector<std::uint32_t> fill(g.start.begin(), g.start.end() - 1);
    for (std::size_t i = 0; i < n; ++i) {
        glm::ivec3 lo, hi;
        if (puffs[kPuffVec4 * i + 1].x < 1e-4f || !range(puffs[kPuffVec4 * i], lo, hi)) continue;
        for (int z = lo.z; z <= hi.z; ++z)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x)
                    g.list[fill[x + nb.x * (y + nb.y * z)]++] = static_cast<std::uint32_t>(i);
    }
}

void ExhaustFx::splatGrid(Grid& g) {
    rhi::Device& gpu = rhi::device();
    uploadBuffer(g.startBuf, g.start.data(), g.start.size() * sizeof(std::uint32_t));
    uploadBuffer(g.listBuf, g.list.data(), g.list.size() * sizeof(std::uint32_t));

    m_splat.bind();
    gpu.bindBufferBase(rhi::BufferKind::Storage, 0, m_puffBuf);
    gpu.bindBufferBase(rhi::BufferKind::Storage, 1, g.startBuf);
    gpu.bindBufferBase(rhi::BufferKind::Storage, 2, g.listBuf);
    gpu.bindImage(0, g.a, 0, rhi::Access::WriteOnly, rhi::Format::RGBA16F);
    gpu.bindImage(1, g.b, 0, rhi::Access::WriteOnly, rhi::Format::RGBA32F);
    gpu.bindImage(2, g.occ, 0, rhi::Access::WriteOnly, rhi::Format::R32F);
    m_splat.setVec3("uOrigin", g.origin);
    m_splat.setFloat("uVoxel", g.voxel);
    m_splat.setFloat("uNoiseK", m_look.noiseK);
    const glm::ivec3 nb = g.bricks();
    gpu.setUniform(m_splat.id(), m_splat.location("uBricks"), rhi::UniformType::IVec3, &nb, 1);
    gpu.dispatch(nb.x, nb.y, nb.z);
}

void ExhaustFx::lightGrid(Grid& g, int which) {
    rhi::Device& gpu = rhi::device();
    m_light.bind();
    // Every grid's density, for the marches that leave this one.
    static const char* const names[3][4] = {{"uA0", "uO0", "uE0", "uV0"},
                                            {"uA1", "uO1", "uE1", "uV1"},
                                            {"uA2", "uO2", "uE2", "uV2"}};
    for (int i = 0; i < 3; ++i) {
        const Grid& o = m_grid[i].on ? m_grid[i] : g;
        gpu.bindTexture(i, o.a);
        m_light.setInt(names[i][0], i);
        m_light.setVec3(names[i][1], o.origin);
        m_light.setVec3(names[i][2], glm::vec3(o.dims) * o.voxel);
        m_light.setFloat(names[i][3], o.voxel);
    }
    m_light.setInt("uHave1", m_grid[kFar].on ? 1 : 0);
    m_light.setInt("uHave2", m_grid[kTip].on ? 1 : 0);
    m_light.setInt("uWhich", which);
    gpu.setUniform(m_light.id(), m_light.location("uDims"), rhi::UniformType::IVec3, &g.dims, 1);
    m_light.setFloat("uSigma", m_look.sigma);
    m_light.setFloat("uThresh", m_look.thresh);
    m_light.setFloat("uSoft", m_look.soft);
    m_light.setVec3("uSunDir", m_sunDir);
    m_light.setInt("uFlameOn", m_lightOn ? 1 : 0);
    m_light.setVec3("uFlameA", m_lightA);
    m_light.setVec3("uFlameB", m_lightB);
    // Eased over ~80 ms from last frame's light (see the shader) -- read where
    // the same piece of space was then, so a grid that moves with the vehicle
    // eases as well as one standing still. A grid on another lattice than last
    // frame's starts afresh.
    const bool ease = g.lit && g.litVoxel == g.voxel;
    const glm::ivec3 shift = ease ? glm::ivec3(glm::round((g.origin - g.litOrigin) / g.voxel)) : glm::ivec3(0);
    m_light.setFloat("uBlend", ease ? 1.0f - std::exp(-m_dt / 0.08f) : 1.0f);
    gpu.setUniform(m_light.id(), m_light.location("uShift"), rhi::UniformType::IVec3, &shift, 1);
    gpu.bindTexture(3, g.l[g.cur]);
    m_light.setInt("uLast", 3);
    gpu.bindImage(0, g.l[1 - g.cur], 0, rhi::Access::WriteOnly, rhi::Format::RGBA16F);
    gpu.dispatch(g.dims.x / 8, g.dims.y / 8, g.dims.z / 8);
    g.cur       = 1 - g.cur;
    g.lit       = true;
    g.litOrigin = g.origin;
    g.litVoxel  = g.voxel;
}

// The grid travelling with the vehicle hangs from its engines down the way the
// flames point -- the young trail and the column being laid down, which is what
// a camera following a launch looks at. Moved by whole voxels on the pad grid's
// lattice: a puff lands in the same voxels whichever of the two holds it, so
// where they meet nothing changes. Stays where the engines last burned.
void ExhaustFx::placeTipGrid() {
    Grid& g = m_grid[kTip];
    if (!m_lightOn) return;
    const float v = std::max(m_look.nearVoxel, 0.5f);
    const glm::ivec3 dims = glm::max(glm::ivec3(glm::ceil(m_look.tipSize / (v * 8.0f))) * 8, glm::ivec3(8));
    const glm::vec3 ext = glm::vec3(dims) * v;
    const glm::vec3 ab = m_lightB - m_lightA;
    const glm::vec3 axis = glm::length(ab) > 1e-3f ? glm::normalize(ab) : glm::vec3(0.0f, -1.0f, 0.0f);
    const glm::vec3 centre = m_lightA + axis * (ext.y * 0.5f - 64.0f);
    allocGrid(g, dims);
    g.voxel  = v;
    g.origin = glm::floor((centre - ext * 0.5f) / v) * v;
    g.on     = true;
}

void ExhaustFx::fitFarGrid() {
    Grid& g = m_grid[kFar];
    const Box near = inside(m_grid[kNear], 0.0f), tip = inside(m_grid[kTip], 0.0f);
    const auto within = [](const Box& k, const glm::vec3& a, const glm::vec3& b) {
        return glm::all(glm::greaterThanEqual(a, k.lo)) && glm::all(glm::lessThanEqual(b, k.hi));
    };
    glm::vec3 lo(1e30f), hi(-1e30f);
    bool outside = false;
    for (const exhaust::Puff& p : m_sim.puffs()) {
        const glm::vec3 a = p.pos - p.r, b = p.pos + p.r;
        lo = glm::min(lo, a);
        hi = glm::max(hi, b);
        if (!within(near, a, b) && !within(tip, a, b)) outside = true;
    }
    if (!outside) {
        g.on = false;
        return;
    }
    // The grid stays where it is for as long as everything still fits in it.
    // Refitted every frame it slid under the cloud -- a growing column moved
    // its box by a fraction of a voxel each time, every sample landed somewhere
    // new in the lattice, and the whole column shimmered at its edges. Now it
    // moves only when something has left it, onto a lattice of its own voxel,
    // with room for the column to grow into: up and downrange, where it goes.
    const glm::vec3 extG = glm::vec3(kFarDims) * g.voxel;
    if (g.on && glm::all(glm::greaterThanEqual(lo, g.origin)) &&
        glm::all(glm::lessThanEqual(hi, g.origin + extG)))
        return;
    const glm::vec3 ext = hi - lo;
    const glm::vec3 lo2 = lo - ext * 0.1f - 40.0f;
    const glm::vec3 hi2 = hi + ext * 0.4f + 300.0f;
    const glm::vec3 need = hi2 - lo2;
    const float v = quantVoxel(std::max({need.x / kFarDims.x, need.y / kFarDims.y, need.z / kFarDims.z,
                                         m_look.nearVoxel * 1.25f}));
    allocGrid(g, kFarDims);
    g.voxel  = v;
    g.origin = glm::floor(lo2 / v) * v;
    g.on     = true;
}

void ExhaustFx::update(const std::vector<Entity>& entities, float dt, std::uint64_t frame,
                       const std::function<float(float, float)>& groundAt, bool simulate,
                       const glm::vec3& sunDir) {
    if (!m_ok || frame == m_lastFrame) return;
    m_lastFrame   = frame;
    m_sunDir      = sunDir;
    m_dt          = dt;

    std::vector<exhaust::Engine> engines;
    std::vector<exhaust::Vent>   vents;
    std::vector<const RocketEngineComponent*> engineComps;
    m_trenches.clear();
    std::vector<std::pair<int, glm::vec3>> nozzles;
    for (const Entity& e : entities) {
        if (!e.activeInHierarchy) continue;
        const RocketEngineComponent* rc = e.components.get<RocketEngineComponent>();
        const ExhaustVentComponent*  vc = e.components.get<ExhaustVentComponent>();
        const FlameTrenchComponent*  tc = e.components.get<FlameTrenchComponent>();
        if (!rc && !vc && !tc) continue;
        const glm::mat3 rot = glm::mat3(scenegraph::compose(glm::vec3(0.0f), e.rotation, glm::vec3(1.0f)));
        if (rc) {
            exhaust::Engine en;
            en.id     = e.id;
            en.nozzle = e.center;
            en.dir    = glm::normalize(rot * glm::vec3(0.0f, -1.0f, 0.0f));
            for (const auto& [id, pos] : m_lastNozzle)
                if (id == e.id && dt > 1e-5f) en.vel = (e.center - pos) / dt;
            nozzles.push_back({e.id, e.center});
            en.radius = rc->radius;
            en.thrust = rc->thrust;
            en.length = rc->length;
            en.spread = glm::radians(rc->spread);
            en.reach  = rc->reach;
            en.smoke  = rc->smoke;
            en.heat   = rc->heat;
            engines.push_back(en);
            engineComps.push_back(rc);
        }
        if (vc && vc->on) {
            exhaust::Vent v;
            v.id       = e.id;
            v.pos      = e.center;
            v.dir      = glm::normalize(rot * glm::vec3(0.0f, 1.0f, 0.0f));
            v.rate     = vc->rate;
            v.speed    = vc->speed;
            v.spread   = glm::radians(vc->spread);
            v.size     = vc->size;
            v.growth   = vc->growth;
            v.buoyancy = vc->buoyancy;
            v.heat     = vc->heat;
            v.life     = vc->life;
            v.density  = vc->density;
            vents.push_back(v);
        }
        if (tc) {
            exhaust::Trench t;
            t.model     = scenegraph::compose(e.center, e.rotation, glm::max(e.half * 2.0f, glm::vec3(0.01f)));
            t.hole      = tc->hole;
            t.reach     = tc->reach;
            t.drop      = tc->drop;
            t.twoSided  = tc->twoSided;
            t.exitSpeed = tc->exitSpeed;
            t.steam     = tc->steam;
            m_trenches.push_back(t);
        }
    }
    m_lastNozzle = std::move(nozzles);

    exhaust::World world;
    world.groundAt = groundAt;
    if (simulate) m_sim.step(dt, engines, vents, m_trenches, world);

    // --- The flames this frame -------------------------------------------
    m_flames.clear();
    float wSum = 0.0f;
    glm::vec3 aSum(0.0f), bSum(0.0f);
    for (std::size_t i = 0; i < engines.size() && m_flames.size() < kMaxFlames; ++i) {
        const exhaust::Engine& en = engines[i];
        const RocketEngineComponent& rc = *engineComps[i];
        if (en.thrust <= 0.01f) continue;
        const exhaust::FlameShape f = exhaust::Sim::flameOf(en, m_trenches, world);
        FlameGpu g;
        g.a        = f.a;
        g.dir      = f.dir;
        g.len      = f.len;
        g.r0       = f.r0;
        g.r1       = f.r1;
        g.bright   = rc.brightness * m_look.flameBright * (0.35f + 0.65f * en.thrust);
        g.core     = rc.core;
        g.diamonds = rc.diamonds;
        g.seed     = std::fmod(static_cast<float>(en.id) * 0.3713f, 1.0f);
        g.light    = 20000.0f * en.radius * en.radius * rc.light * en.thrust;
        g.color    = rc.color;
        m_flames.push_back(g);
        aSum += f.a * g.light;
        bSum += (f.a + f.dir * f.len) * g.light;
        wSum += g.light;
        if (!m_nearPlaced) {
            const glm::vec3 foot = f.a + f.dir * std::max(f.hitT, 0.0f);
            m_nearCentre = glm::vec3(foot.x, groundAt ? groundAt(foot.x, foot.z) : 0.0f, foot.z);
            m_nearPlaced = true;
        }
    }
    m_lightOn = wSum > 0.0f;
    if (m_lightOn) {
        m_lightA = aSum / wSum;
        m_lightB = bSum / wSum;
    }
    if (!m_nearPlaced && !m_trenches.empty()) {
        const glm::vec3 top = glm::vec3(m_trenches[0].model * glm::vec4(0.0f, 0.5f, 0.0f, 1.0f));
        m_nearCentre = glm::vec3(top.x, groundAt ? groundAt(top.x, top.z) : top.y, top.z);
        m_nearPlaced = true;
    }
    if (!m_nearPlaced && !vents.empty()) {
        m_nearCentre = vents[0].pos;
        m_nearPlaced = true;
    }

    // --- The grids -------------------------------------------------------
    const auto& puffs = m_sim.puffs();
    if (!m_nearPlaced || puffs.empty()) {
        for (Grid& g : m_grid) g.on = g.lit = false;
        return;
    }
    Grid& g0 = m_grid[kNear];
    const float v0 = std::max(m_look.nearVoxel, 0.5f);
    const glm::ivec3 dims0 = glm::max(glm::ivec3(glm::ceil(m_look.nearSize / (v0 * 8.0f))) * 8, glm::ivec3(8));
    const glm::vec3 origin0 = glm::floor((m_nearCentre - glm::vec3(dims0.x, 0, dims0.z) * v0 * 0.5f -
                                          glm::vec3(0.0f, 24.0f, 0.0f)) / v0) * v0;
    allocGrid(g0, dims0);
    g0.voxel  = v0;
    g0.origin = origin0;
    g0.on     = true;
    placeTipGrid();
    fitFarGrid();
    for (Grid& g : m_grid)
        if (!g.on) g.lit = false;

    m_puffData.resize(puffs.size() * kPuffVec4);
    for (std::size_t i = 0; i < puffs.size(); ++i) {
        const exhaust::Puff& p = puffs[i];
        glm::vec4* d = &m_puffData[kPuffVec4 * i];
        d[0] = glm::vec4(p.pos, std::max(p.r, 0.05f));
        d[1] = glm::vec4(p.mass * p.fade(), p.heat,
                         static_cast<float>(p.seed >> 8) / 16777216.0f, p.opacity);
        d[2] = glm::vec4(p.merge, 0.0f, 0.0f, 0.0f);
    }
    uploadBuffer(m_puffBuf, m_puffData.data(), m_puffData.size() * sizeof(glm::vec4));

    // A coarser grid leaves out what lies well inside a finer one (two of its
    // own voxels in, so every voxel a sample outside could blend or pick from
    // is still complete): a launch cloud is held once, not three times.
    rhi::Device& gpu = rhi::device();
    Grid& tip = m_grid[kTip];
    Grid& far = m_grid[kFar];
    binGrid(g0, m_puffData, Box{}, Box{});
    splatGrid(g0);
    if (tip.on) {
        binGrid(tip, m_puffData, inside(g0, tip.voxel * 2.0f), Box{});
        splatGrid(tip);
    }
    if (far.on) {
        binGrid(far, m_puffData, inside(g0, far.voxel * 2.0f), inside(tip, far.voxel * 2.0f));
        splatGrid(far);
    }
    gpu.memoryBarrier(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
    for (int i = 0; i < 3; ++i)
        if (m_grid[i].on) lightGrid(m_grid[i], i);
    gpu.memoryBarrier(rhi::BarrierImageAccess | rhi::BarrierTextureFetch);
}

void ExhaustFx::resize(int w, int h) {
    w = std::max(1, w);
    h = std::max(1, h);
    if (w == m_w && h == m_h) return;
    m_w = w;
    m_h = h;
    m_rt = fitzel::RenderTarget(w, h, fitzel::RenderTarget::Format::RGBA16F);
}

void ExhaustFx::render(const fitzel::RenderTarget& hdr, const Params& p, fitzel::Mesh& fsQuad) {
    if (!m_ok || (m_flames.empty() && !m_grid[kNear].on && !m_grid[kFar].on && !m_grid[kTip].on)) return;
    rhi::Device& gpu = rhi::device();
    using rhi::BlendFactor;

    const int scale = std::clamp(m_look.resScale, 1, 4);
    resize(hdr.width() / scale, hdr.height() / scale);

    m_rt.bind();
    gpu.setDepthTest(false);
    gpu.setDepthWrite(false);
    gpu.setCullEnabled(false);
    gpu.setBlendEnabled(false);

    m_march.bind();
    hdr.bindDepthTexture(0);
    m_march.setInt("uDepth", 0);
    // Grid i on units 1 + 4i .. 4 + 4i; something of the right type on every
    // sampler even when a grid is off.
    static const char* const names[3][8] = {
        {"uA0", "uB0", "uL0", "uOcc0", "uO0", "uE0", "uV0", "uHave0"},
        {"uA1", "uB1", "uL1", "uOcc1", "uO1", "uE1", "uV1", "uHave1"},
        {"uA2", "uB2", "uL2", "uOcc2", "uO2", "uE2", "uV2", "uHave2"}};
    const Grid* any = nullptr;
    for (const Grid& g : m_grid)
        if (g.a && !any) any = &g;
    for (int i = 0; i < 3; ++i) {
        const Grid& g = m_grid[i];
        const Grid* s = g.on ? &g : any;
        const int u = 1 + 4 * i;
        if (s) {
            gpu.bindTexture(u, s->a); gpu.bindTexture(u + 1, s->b);
            gpu.bindTexture(u + 2, s->l[s->cur]); gpu.bindTexture(u + 3, s->occ);
        } else {
            for (int k = 0; k < 4; ++k) gpu.bindTexture(u + k, m_noise);
        }
        for (int k = 0; k < 4; ++k) m_march.setInt(names[i][k], u + k);
        m_march.setVec3(names[i][4], g.origin);
        m_march.setVec3(names[i][5], glm::vec3(g.dims) * g.voxel);
        m_march.setFloat(names[i][6], g.voxel);
        m_march.setInt(names[i][7], g.on ? 1 : 0);
    }
    gpu.bindTexture(13, m_noise);
    m_march.setInt("uNoise", 13);

    m_march.setMat4("uInvViewProjRel", glm::inverse(p.viewProjRel));
    m_march.setVec3("uCamPos", p.camPos);
    m_march.setFloat("uTime", p.time);
    // Golden-ratio steps: each frame's pattern as far from the last ones as it
    // can be. (Once taken from the clock in microseconds, as a float: past a
    // few seconds that has no fraction left, the pattern froze, and frozen
    // under a moving cloud the noise glittered.)
    m_march.setFloat("uJitter", p.temporal ? static_cast<float>(std::fmod((p.frame % 4096u) * 0.6180339887, 1.0)) : 0.0f);
    m_march.setInt("uMaxSteps", m_look.maxSteps);
    m_march.setFloat("uNoiseK", m_look.noiseK);
    gpu.bindBufferBase(rhi::BufferKind::Storage, 0, m_puffBuf);
    static const int debug = [] {
        const char* e = std::getenv("EXHAUST_DEBUG");
        return e ? std::atoi(e) : 0;
    }();
    m_march.setInt("uDebug", debug);
    static const int stable = [] {
        const char* e = std::getenv("EXHAUST_STABLE");
        return e ? std::atoi(e) : 0;
    }();
    m_march.setInt("uStable", stable);

    m_march.setFloat("uSigma", m_look.sigma);
    m_march.setFloat("uThresh", m_look.thresh);
    m_march.setFloat("uSoft", m_look.soft);
    m_march.setFloat("uErode", m_look.erode);
    m_march.setFloat("uAlbedo", m_look.albedo);
    m_march.setVec3("uTint", m_look.tint);
    m_march.setVec3("uSunDir", p.sunDir);
    m_march.setVec3("uSunColor", p.sunColor);
    m_march.setVec3("uSkyColor", p.skyColor);
    m_march.setVec3("uGroundColor", p.groundColor);
    m_march.setFloat("uSunI", m_look.sun);
    m_march.setFloat("uSkyI", m_look.sky);
    m_march.setFloat("uFlameI", m_look.flame);
    m_march.setFloat("uHeatI", m_look.heat);

    const int nf = static_cast<int>(m_flames.size());
    m_march.setInt("uFlameCount", nf);
    for (int i = 0; i < nf; ++i) {
        const FlameGpu& f = m_flames[i];
        m_march.setVec3(Indexed("uFA", i), f.a);
        m_march.setVec3(Indexed("uFD", i), f.dir);
        m_march.setVec4(Indexed("uFP", i), glm::vec4(f.len, f.r0, f.r1, f.bright));
        m_march.setVec4(Indexed("uFQ", i), glm::vec4(f.core, f.diamonds, f.seed, f.light));
        m_march.setVec3(Indexed("uFC", i), f.color);
    }
    fsQuad.draw();

    // --- Onto the scene: src + dst * srcAlpha -------------------------------
    hdr.bind();
    gpu.setBlendEnabled(true);
    gpu.setBlendFuncSeparate(BlendFactor::One, BlendFactor::SrcAlpha, BlendFactor::Zero,
                             BlendFactor::One);
    m_up.bind();
    m_rt.bindColorTexture(0);
    m_up.setInt("uFx", 0);
    m_up.setVec2("uTexel", {1.0f / static_cast<float>(m_w), 1.0f / static_cast<float>(m_h)});
    m_up.setInt("uScaled", scale > 1 ? 1 : 0);
    fsQuad.draw();

    gpu.setBlendEnabled(false);
    gpu.setBlendFunc(BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);
    gpu.setCullFace(rhi::CullMode::Back);
    gpu.setDepthWrite(true);
    gpu.setDepthTest(true);
    gpu.setCullEnabled(true);
}
