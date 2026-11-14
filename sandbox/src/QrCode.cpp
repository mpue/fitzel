#include "QrCode.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>

#include <nlohmann/json.hpp>

using fitzel::AssetId;

namespace qrcode {

namespace {

std::uint32_t toByte(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

AssetId ensure(std::vector<MaterialDef>& mats, const std::string& name, glm::vec3 albedo) {
    for (MaterialDef& m : mats) {
        if (m.name != name) continue;
        m.albedo = albedo;
        if (!m.assetId.valid()) m.assetId = AssetId::generate();
        return m.assetId;
    }
    MaterialDef md;
    md.assetId      = AssetId::generate();
    md.name         = name;
    md.albedo       = albedo;
    md.reflectivity = 0.0f;
    md.roughness    = 0.55f;   // printed plastic: a dull sheen
    mats.push_back(md);
    return md.assetId;
}

// The dark cells of a square grid as closed solids. `line` holds the n+1
// positions of the grid lines, the same across x and z (cells need not be
// square: a frame's cells are as wide as the frame). Corners are shared between
// the cells that meet there, so two neighbours have one top and no wall between
// them; a corner that two dark cells share only diagonally is split, each copy
// drawn into its own cell by `pinch` (metres).
struct Cells {
    int n = 0;
    std::vector<std::uint8_t> dark;
    std::vector<float> line;
    bool at(int x, int y) const {
        return x >= 0 && y >= 0 && x < n && y < n && dark[static_cast<std::size_t>(y * n + x)] != 0;
    }
};

EditMesh solidsMesh(const Cells& c, float y0, float y1, float pinch) {
    EditMesh m;
    const int n = c.n;
    // Per grid corner: a shared corner (slot 0) or one per cell round it
    // (slots 1..4), each at two heights.
    std::vector<int> slot(static_cast<std::size_t>((n + 1) * (n + 1) * 5 * 2), -1);

    // Corner (i, j) of the grid as seen from cell (x, y), at height `top`.
    auto corner = [&](int i, int j, int x, int y, bool top) {
        const bool a = c.at(i - 1, j - 1), b = c.at(i, j - 1);
        const bool d = c.at(i - 1, j),     e = c.at(i, j);
        const bool split = (a && e && !b && !d) || (b && d && !a && !e);
        const int q = (y == j ? 2 : 0) + (x == i ? 1 : 0);   // which cell of the four
        const int s = split ? 1 + q : 0;
        int& v = slot[static_cast<std::size_t>(((j * (n + 1) + i) * 5 + s) * 2 + (top ? 1 : 0))];
        if (v < 0) {
            glm::vec3 p(c.line[static_cast<std::size_t>(i)], top ? y1 : y0, c.line[static_cast<std::size_t>(j)]);
            if (split) {
                p.x += (x == i ? 1.0f : -1.0f) * pinch;   // towards the cell's middle
                p.z += (y == j ? 1.0f : -1.0f) * pinch;
            }
            v = static_cast<int>(m.verts.size());
            m.verts.push_back(p);
        }
        return v;
    };

    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            if (!c.at(x, y)) continue;
            // Corners: 00 top-left of the code, 10 to its right, 01 below it.
            const int b00 = corner(x, y, x, y, false),         t00 = corner(x, y, x, y, true);
            const int b10 = corner(x + 1, y, x, y, false),     t10 = corner(x + 1, y, x, y, true);
            const int b01 = corner(x, y + 1, x, y, false),     t01 = corner(x, y + 1, x, y, true);
            const int b11 = corner(x + 1, y + 1, x, y, false), t11 = corner(x + 1, y + 1, x, y, true);
            m.faces.push_back({t00, t01, t11, t10});   // top, +Y
            m.faces.push_back({b00, b10, b11, b01});   // bottom, -Y
            if (!c.at(x, y - 1)) m.faces.push_back({b00, t00, t10, b10});   // -Z
            if (!c.at(x, y + 1)) m.faces.push_back({b01, b11, t11, t01});   // +Z
            if (!c.at(x - 1, y)) m.faces.push_back({b00, b01, t01, t00});   // -X
            if (!c.at(x + 1, y)) m.faces.push_back({b10, t10, t11, b11});   // +X
        }
    return m;
}

} // namespace

Params sane(const Params& p) {
    Params s = p;
    s.ecc    = static_cast<qrencode::Ecc>(std::clamp(static_cast<int>(p.ecc), 0,
                                                     static_cast<int>(qrencode::Ecc::Count) - 1));
    s.size   = std::clamp(p.size, 0.002f, 100.0f);
    s.border = std::clamp(p.border, 0, 16);
    s.plate  = std::clamp(p.plate, 1e-4f, 10.0f);
    // The code is the relief: at least a hundredth of a millimetre of it.
    s.relief = std::clamp(p.relief, 1e-5f, 10.0f);
    // A frame leaves at least a tenth of the side to the code.
    s.frameWidth = std::clamp(p.frameWidth, 1e-4f, 0.45f * s.size);
    return s;
}

Layout layoutOf(const Params& p, int codeSize) {
    Layout l;
    l.width = p.size;
    l.frame = p.frame ? p.frameWidth : 0.0f;
    l.plate = p.background ? p.plate : 0.0f;
    l.pitch = (l.width - 2.0f * l.frame) / static_cast<float>(codeSize + 2 * p.border);
    l.inset = l.frame + l.pitch * static_cast<float>(p.border);
    l.code  = l.pitch * static_cast<float>(codeSize);
    return l;
}

Model build(const Params& in) {
    const Params p = sane(in);
    Model out;
    out.code = qrencode::encode(p.text, p.ecc);
    if (!out.code.valid()) return out;
    const Layout l = layoutOf(p, out.code.size);
    out.layout = l;
    if (l.plate > 0.0f) {
        out.plate = EditMesh::box(glm::vec3(0.5f * l.width, 0.5f * l.plate, 0.5f * l.width));
        for (glm::vec3& v : out.plate.verts) v.y += 0.5f * l.plate;
    }
    // One grid for the code, its quiet zone and the frame: a ring of cells as
    // wide as the frame round the module-sized rest.
    const int f = p.frame ? 1 : 0;
    Cells c;
    c.n = out.code.size + 2 * (p.border + f);
    c.dark.assign(static_cast<std::size_t>(c.n * c.n), 0);
    const int at = p.border + f;
    for (int y = 0; y < c.n; ++y)
        for (int x = 0; x < c.n; ++x) {
            const bool rim = f && (x == 0 || y == 0 || x == c.n - 1 || y == c.n - 1);
            if (rim || out.code.at(x - at, y - at)) c.dark[static_cast<std::size_t>(y * c.n + x)] = 1;
        }
    c.line.resize(static_cast<std::size_t>(c.n + 1));
    const float o = -0.5f * l.width;
    for (int i = 0; i <= c.n; ++i)
        c.line[static_cast<std::size_t>(i)] = o + l.frame * static_cast<float>(std::min(i, f)) +
                                              l.pitch * static_cast<float>(std::clamp(i - f, 0, c.n - 2 * f));
    c.line.back() = -o;   // exactly the edge, not the sum of the steps
    out.modules = solidsMesh(c, l.plate, l.plate + p.relief, kPinch * l.pitch);
    return out;
}

EditMesh moduleSolids(const qrencode::Code& code, float pitch, float y0, float y1) {
    Cells c;
    c.n = code.size;
    c.dark = code.dark;
    c.line.resize(static_cast<std::size_t>(c.n + 1));
    for (int i = 0; i <= c.n; ++i)
        c.line[static_cast<std::size_t>(i)] = (static_cast<float>(i) - 0.5f * static_cast<float>(c.n)) * pitch;
    return solidsMesh(c, y0, y1, kPinch * pitch);
}

Palette ensurePalette(std::vector<MaterialDef>& mats, const Params& p) {
    auto colour = [&](const char* part, glm::vec3 c) {
        char name[64];
        std::snprintf(name, sizeof name, "QR Code %s#%02X%02X%02X", part, toByte(c.r), toByte(c.g),
                      toByte(c.b));
        return ensure(mats, name, c);
    };
    Palette pal;
    pal.plate = colour("", p.plateColor);
    pal.ink   = colour("Ink ", p.inkColor);
    return pal;
}

std::string autoName(const Params& p) {
    std::string t = p.text.substr(0, p.text.find('\n'));
    // Cut at 24 bytes, but not inside a UTF-8 character.
    if (t.size() > 24) {
        std::size_t k = 24;
        while (k > 0 && (static_cast<unsigned char>(t[k]) & 0xC0) == 0x80) --k;
        t = t.substr(0, k) + "...";
    }
    return "QR " + t;
}

std::vector<Entity> entities(const Params& in, const Palette& pal, int& counter, const glm::vec3& at) {
    const Params p = sane(in);
    Model mdl = build(p);
    if (!mdl.code.valid()) return {};
    std::vector<Entity> out;
    Entity root;
    root.type        = EntityType::Empty;
    root.name        = autoName(p);
    root.id          = counter++;
    root.parent      = -1;
    root.half        = glm::vec3(0.5f * p.size, 0.5f * (mdl.layout.plate + p.relief), 0.5f * p.size);
    root.localCenter = root.center = at;
    {
        auto qc = std::make_unique<QrCodeComponent>();
        qc->params = p;
        root.components.items.push_back(std::move(qc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;

    // One child per part, re-centred on its own bounds (the Modeling panel's
    // invariant: pick box, gizmo and collider describe the shape that is there).
    auto add = [&](const char* name, EditMesh m, AssetId mat, bool collide) {
        if (m.faces.empty()) return;
        const glm::vec3 shift = editmesh::recenter(m);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        Entity e;
        e.type        = EntityType::Box;
        e.name        = name;
        e.id          = counter++;
        e.parent      = rootId;
        e.half        = glm::max(0.5f * (mx - mn), glm::vec3(1e-4f));
        e.localCenter = shift;
        e.center      = at + shift;
        auto mc = std::make_unique<MeshComponent>();
        mc->mesh = std::move(m);
        mc->touch();
        e.components.items.push_back(std::move(mc));
        auto mt = std::make_unique<MaterialComponent>();
        mt->material = mat;
        e.components.items.push_back(std::move(mt));
        if (collide) {
            auto pc = std::make_unique<PhysicsComponent>();
            pc->dynamic = false;
            e.components.items.push_back(std::move(pc));
        }
        out.push_back(std::move(e));
    };
    add("Plate", std::move(mdl.plate), pal.plate, true);
    add("Code", std::move(mdl.modules), pal.ink, false);
    return out;
}

// --- Persistence ---------------------------------------------------------------

namespace {
nlohmann::json colJson(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
glm::vec3 jsonCol(const nlohmann::json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
} // namespace

void saveParams(nlohmann::json& j, const Params& p) {
    j["text"]       = p.text;
    j["ecc"]        = static_cast<int>(p.ecc);
    j["size"]       = p.size;
    j["border"]     = p.border;
    j["background"] = p.background;
    j["plate"]      = p.plate;
    j["relief"]     = p.relief;
    j["frame"]      = p.frame;
    j["frameWidth"] = p.frameWidth;
    j["plateColor"] = colJson(p.plateColor);
    j["inkColor"]   = colJson(p.inkColor);
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;
    p.text       = j.value("text", d.text);
    p.ecc        = static_cast<qrencode::Ecc>(j.value("ecc", static_cast<int>(d.ecc)));
    p.size       = j.value("size", d.size);
    p.border     = j.value("border", d.border);
    p.plate      = j.value("plate", d.plate);
    // Before the switch a plate of 0 meant none.
    p.background = j.value("background", p.plate > 0.0f);
    if (p.plate <= 0.0f) p.plate = d.plate;
    p.relief     = j.value("relief", d.relief);
    p.frame      = j.value("frame", d.frame);
    p.frameWidth = j.value("frameWidth", d.frameWidth);
    p.plateColor = jsonCol(j.value("plateColor", nlohmann::json()), d.plateColor);
    p.inkColor   = jsonCol(j.value("inkColor", nlohmann::json()), d.inkColor);
    p = sane(p);
}

namespace {
// Scene files name components by type id; this lets a saved code come back as
// a code, parameters and all.
struct RegisterQr {
    RegisterQr() {
        components::registerType({"qrCode", "QR code",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<QrCodeComponent>()); },
            /*addable=*/false});
    }
} g_registerQr;
} // namespace

} // namespace qrcode
