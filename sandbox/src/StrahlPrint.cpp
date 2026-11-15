#include "StrahlPrint.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>

#include "TraceMesh.hpp"

namespace strahl::print {
namespace {

bool hasIso(const pathtrace::Shapes& s, int node) {
    const pathtrace::ShapeNode& n = s.nodes[static_cast<std::size_t>(node)];
    if (n.kind == pathtrace::ShapeNode::Kind::Iso) return true;
    if (n.isLeaf()) return false;
    for (int i = 0; i < n.count; ++i)
        if (hasIso(s, s.childIndex[static_cast<std::size_t>(n.first + i)])) return true;
    return false;
}

std::string hex(const glm::vec3& c) {
    char b[8];
    std::snprintf(b, sizeof b, "#%02X%02X%02X", static_cast<int>(std::lround(std::clamp(c.r, 0.0f, 1.0f) * 255.0f)),
                  static_cast<int>(std::lround(std::clamp(c.g, 0.0f, 1.0f) * 255.0f)),
                  static_cast<int>(std::lround(std::clamp(c.b, 0.0f, 1.0f) * 255.0f)));
    return b;
}

glm::dvec3 longestOf(const std::vector<Piece>& all, const std::vector<int>& picked, glm::dvec3& lo) {
    glm::dvec3 hi(0.0);
    bool first = true;
    for (int i : picked) {
        if (i < 0 || i >= static_cast<int>(all.size())) continue;
        const Piece& p = all[static_cast<std::size_t>(i)];
        lo = first ? p.lo : glm::min(lo, p.lo);
        hi = first ? p.hi : glm::max(hi, p.hi);
        first = false;
    }
    return first ? glm::dvec3(0.0) : hi - lo;
}

} // namespace

std::string Piece::label() const {
    std::string s = kind;
    if (line > 0) {
        s += ", line " + std::to_string(line);
        // Made in another file (an import, the library): say which.
        if (imported && !file.empty() && file[0] != '<')
            s += " of " + std::filesystem::path(file).filename().string();
    }
    return s;
}

std::vector<Piece> pieces(const Result& r) {
    std::vector<Piece> out;
    if (!r.ok || !r.scene || !r.scene->shapes) return out;
    const pathtrace::Shapes& s = *r.scene->shapes;
    for (std::size_t i = 0; i < s.roots.size(); ++i) {
        const int node = s.roots[i];
        const pathtrace::ShapeNode& n = s.nodes[static_cast<std::size_t>(node)];
        const Result::ShapeSource src = i < r.roots.size() ? r.roots[i] : Result::ShapeSource{};
        if (src.kind == "light" || !n.bounded) continue;
        if (n.kind == pathtrace::ShapeNode::Kind::Disc) continue;   // flat: nothing to print
        Piece p;
        p.root = static_cast<int>(i);
        p.node = node;
        p.kind = src.kind.empty() ? std::string("shape") : src.kind;
        p.file = src.file;
        p.line = src.line;
        p.imported = src.imported;
        p.iso  = hasIso(s, node);
        p.lo = p.boxLo = glm::dvec3(n.lo);
        p.hi = p.boxHi = glm::dvec3(n.hi);
        if (!(p.hi.x > p.lo.x && p.hi.y > p.lo.y && p.hi.z > p.lo.z)) continue;
        // The solid's own extent, from a quick coarse mesh: what "longest
        // side" means. A coarse crossing is still on the surface, and a part
        // thinner than the coarse cell only ever makes these smaller than
        // the real thing, never larger.
        const glm::dvec3 d = p.hi - p.lo;
        pathtrace::SolidMeshOptions mo;
        mo.cell = std::max({d.x, d.y, d.z}) / 40.0;
        mo.leaves = false;
        pathtrace::SolidMesh sm;
        std::string why;
        if (pathtrace::meshSolid(s, node, mo, sm, why)) {
            glm::dvec3 lo = sm.verts.front(), hi = lo;
            for (const glm::dvec3& v : sm.verts) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
            p.lo = glm::max(p.lo, lo - mo.cell);
            p.hi = glm::min(p.hi, hi + mo.cell);
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<int> defaultPick(const std::vector<Piece>& all) {
    std::vector<int> iso, every;
    for (int i = 0; i < static_cast<int>(all.size()); ++i) {
        every.push_back(i);
        if (all[static_cast<std::size_t>(i)].iso) iso.push_back(i);
    }
    return iso.empty() ? every : iso;
}

double mmPerUnit(const std::vector<Piece>& all, const std::vector<int>& picked, const Options& o) {
    if (o.mmPerUnit > 0.0) return o.mmPerUnit;
    glm::dvec3 lo(0.0);
    const glm::dvec3 d = longestOf(all, picked, lo);
    const double longest = std::max({d.x, d.y, d.z});
    return longest > 1e-12 ? o.longestMm / longest : 1000.0;
}

int cellsAcross(const std::vector<Piece>& all, const std::vector<int>& picked, const Options& o) {
    const double k = mmPerUnit(all, picked, o);
    int most = 0;
    for (int i : picked) {
        if (i < 0 || i >= static_cast<int>(all.size())) continue;
        const Piece& p = all[static_cast<std::size_t>(i)];
        const glm::dvec3 d = p.boxHi - p.boxLo;
        most = std::max(most, static_cast<int>(std::ceil(std::max({d.x, d.y, d.z}) * k / o.cellMm)) + 3);
    }
    return most;
}

double cellUsed(const std::vector<Piece>& all, const std::vector<int>& picked, const Options& o) {
    const int across = cellsAcross(all, picked, o);
    if (across <= o.maxCells || o.maxCells <= 8) return o.cellMm;
    return o.cellMm * static_cast<double>(across) / static_cast<double>(o.maxCells - 8);
}

bool mesh(const Result& r, const std::vector<Piece>& all, const std::vector<int>& picked, const Options& o,
          export3mf::Shape& out, double& mmUsed, std::vector<std::string>& notes, std::string& err) {
    out = export3mf::Shape{};
    if (!r.scene || !r.scene->shapes) { err = "the scene has no shapes"; return false; }
    const pathtrace::Shapes& shapes = *r.scene->shapes;
    const std::vector<pathtrace::Material>& mats = r.scene->materials;
    if (!(o.cellMm > 0.0)) { err = "the cell must be larger than nothing"; return false; }
    const double k = mmPerUnit(all, picked, o);
    const double cellMm = cellUsed(all, picked, o);
    if (cellMm > o.cellMm) {
        char line[160];
        std::snprintf(line, sizeof line, "a %.3g mm cell is more than %d across the print; meshed at %.3g mm",
                      o.cellMm, o.maxCells, cellMm);
        notes.push_back(line);
    }
    const double cell = cellMm / k;

    // The colours, one per distinct shown colour: two materials that look the
    // same are one filament.
    std::map<std::string, int> colorOf;
    const auto colorIndex = [&](const glm::vec3& c) {
        const std::string h = hex(c);
        const auto [it, fresh] = colorOf.try_emplace(h, static_cast<int>(out.colors.size()));
        if (fresh) out.colors.push_back({h, c});
        return it->second;
    };

    int done = 0;
    const int count = static_cast<int>(picked.size());
    for (int pi : picked) {
        if (pi < 0 || pi >= static_cast<int>(all.size())) continue;
        const Piece& piece = all[static_cast<std::size_t>(pi)];
        pathtrace::SolidMeshOptions mo;
        mo.cell = cell;
        mo.threads = o.threads;
        mo.maxCells = o.maxCells;
        bool stopped = false;
        if (o.progress)
            mo.progress = [&](float f) {
                if (!o.progress((static_cast<float>(done) + f * 0.95f) / static_cast<float>(std::max(1, count))))
                    stopped = true;
                return !stopped;
            };
        pathtrace::SolidMesh sm;
        std::string why;
        if (!pathtrace::meshSolid(shapes, piece.node, mo, sm, why)) {
            if (stopped) { err = "cancelled"; out = export3mf::Shape{}; return false; }
            notes.push_back("left out " + piece.label() + ": " + why);
            ++done;
            continue;
        }

        // Each material's colour as the print shows it: a pattern averaged
        // over the triangles it covers, by area.
        struct Sum { glm::dvec3 rgb{0.0}; double w = 0.0; };
        std::map<int, Sum> sums;
        std::vector<int> triMat(sm.tris.size(), -1);
        for (std::size_t t = 0; t < sm.tris.size(); ++t) {
            const int leaf = sm.triLeaf.empty() ? -1 : sm.triLeaf[t];
            if (leaf < 0) continue;
            const pathtrace::ShapeNode& ln = shapes.nodes[static_cast<std::size_t>(leaf)];
            const int mi = std::clamp(ln.material, 0, static_cast<int>(mats.size()) - 1);
            triMat[t] = mi;
            const pathtrace::Material& m = mats[static_cast<std::size_t>(mi)];
            const glm::ivec3& tr = sm.tris[t];
            const glm::dvec3 A = sm.verts[static_cast<std::size_t>(tr.x)], B = sm.verts[static_cast<std::size_t>(tr.y)],
                             C = sm.verts[static_cast<std::size_t>(tr.z)];
            const glm::dvec3 nd = glm::cross(B - A, C - A);
            const double area = 0.5 * glm::length(nd);
            Sum& s = sums[mi];
            glm::vec3 c = m.albedo;
            if (m.colorFn) {
                const glm::vec3 P((A + B + C) / 3.0);
                const glm::vec3 op(ln.toPattern * glm::vec4(P, 1.0f));
                const glm::vec3 n = area > 0.0 ? glm::vec3(nd / (2.0 * area)) : glm::vec3(0.0f, 1.0f, 0.0f);
                c = glm::clamp(m.colorFn->eval(op, n, glm::vec3(0.0f)), glm::vec3(0.0f), glm::vec3(1.0f));
                if (!(c.x == c.x && c.y == c.y && c.z == c.z)) c = m.albedo;
            }
            s.rgb += glm::dvec3(c) * area;
            s.w += area;
        }
        std::map<int, int> matColor;
        int mostMat = -1;
        double mostW = -1.0;
        for (const auto& [mi, s] : sums) {
            matColor[mi] = colorIndex(s.w > 0.0 ? glm::vec3(s.rgb / s.w) : mats[static_cast<std::size_t>(mi)].albedo);
            if (s.w > mostW) { mostW = s.w; mostMat = mi; }
        }
        // A triangle no ray found the surface of (a sliver at a crease)
        // wears the part's main colour.
        const int fallback = mostMat >= 0 ? matColor[mostMat] : colorIndex(mats.empty() ? glm::vec3(0.75f) : mats[0].albedo);

        export3mf::Part part;
        part.name = piece.label();
        part.verts = std::move(sm.verts);
        part.tris = std::move(sm.tris);
        part.triColor.resize(part.tris.size());
        for (std::size_t t = 0; t < part.tris.size(); ++t)
            part.triColor[t] = triMat[t] >= 0 ? matColor[triMat[t]] : fallback;
        out.parts.push_back(std::move(part));
        ++done;
    }
    if (out.parts.empty()) {
        err = notes.empty() ? std::string("nothing picked to print") : notes.front();
        return false;
    }
    bool first = true;
    for (const export3mf::Part& p : out.parts)
        for (const glm::dvec3& v : p.verts) {
            out.lo = first ? v : glm::min(out.lo, v);
            out.hi = first ? v : glm::max(out.hi, v);
            first = false;
        }
    mmUsed = k;
    if (!(o.mmPerUnit > 0.0)) {
        const glm::dvec3 d = out.hi - out.lo;
        const double longest = std::max({d.x, d.y, d.z});
        if (longest > 1e-12) mmUsed = o.longestMm / longest;
    }
    if (o.progress) o.progress(1.0f);
    return true;
}

} // namespace strahl::print
