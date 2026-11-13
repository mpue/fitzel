// export3mfcheck -- the selection as a 3MF for a 3D printer (Export3mf.hpp).
//
// A slicer forgives little and says less: a corner that is two corners leaves a
// crack it "repairs", a triangle wound the wrong way turns the part inside out,
// a model lying on its back prints lying on its back. So the parts are measured
// rather than looked at: every one closed, every edge crossed once each way,
// its volume the volume of the shape (a box exactly, a sphere and a cylinder to
// within their facets), the right objects taken and the rest left out, Y up
// turned to Z up without a mirror, millimetres, on the bed, the colours along
// -- and the file a zip a reader opens, holding what modelXml says. No GL.
//
//   build/linux/bin/export3mfcheck

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "miniz.h"

#include "../src/Component.hpp"
#include "../src/Document.hpp"
#include "../src/EditMesh.hpp"
#include "../src/Export3mf.hpp"
#include "../src/ModelLibrary.hpp"
#include "../src/Modifiers.hpp"
#include "../src/SceneGraph.hpp"

namespace {

int failures = 0;
void check(bool ok, const std::string& what, const std::string& detail = {}) {
    std::printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what.c_str(), detail.empty() ? "" : "  -- ",
                detail.c_str());
    if (!ok) ++failures;
}
std::string num(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

// Closed and wound one way: every directed edge once, and its reverse once.
bool closed(const std::vector<glm::ivec3>& tris) {
    std::map<std::pair<int, int>, int> edges;
    for (const glm::ivec3& t : tris)
        for (int k = 0; k < 3; ++k) ++edges[{t[k], t[(k + 1) % 3]}];
    for (const auto& [e, n] : edges) {
        if (n != 1) return false;
        const auto back = edges.find({e.second, e.first});
        if (back == edges.end() || back->second != 1) return false;
    }
    return !tris.empty();
}

// Signed volume: positive when every triangle is counter-clockwise from outside.
template <class V>
double volume(const std::vector<V>& v, const std::vector<glm::ivec3>& tris) {
    double s = 0.0;
    for (const glm::ivec3& t : tris) {
        const glm::dvec3 a(v[t.x]), b(v[t.y]), c(v[t.z]);
        s += glm::dot(a, glm::cross(b, c));
    }
    return s / 6.0;
}

const export3mf::Part* partNamed(const export3mf::Shape& s, const std::string& name) {
    for (const export3mf::Part& p : s.parts)
        if (p.name == name) return &p;
    return nullptr;
}

glm::dvec3 centre(const export3mf::Part& p) {
    glm::dvec3 lo(1e300), hi(-1e300);
    for (const glm::dvec3& v : p.verts) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
    return 0.5 * (lo + hi);
}

// The objects of a model XML read back: corners and triangles per <object>
// that has a mesh, the attributes of each tag, the colours.
struct XmlObject {
    std::string                 name;
    std::vector<glm::dvec3>     verts;
    std::vector<glm::ivec3>     tris;
    std::vector<int>            p1;   // -1 where the triangle has none
    std::string                 pid, pindex;
};
std::string attr(const std::string& tag, const std::string& name) {
    const std::string key = " " + name + "=\"";
    const auto at = tag.find(key);
    if (at == std::string::npos) return {};
    const auto from = at + key.size();
    return tag.substr(from, tag.find('"', from) - from);
}
std::vector<XmlObject> readObjects(const std::string& x, std::vector<std::string>* colors) {
    std::vector<XmlObject> out;
    std::size_t at = 0;
    while ((at = x.find('<', at)) != std::string::npos) {
        const std::size_t end = x.find('>', at);
        const std::string tag = x.substr(at, end - at + 1);
        at = end;
        if (tag.rfind("<object ", 0) == 0) {
            out.push_back({attr(tag, "name"), {}, {}, {}, attr(tag, "pid"), attr(tag, "pindex")});
        } else if (tag.rfind("<vertex ", 0) == 0) {
            out.back().verts.emplace_back(std::atof(attr(tag, "x").c_str()), std::atof(attr(tag, "y").c_str()),
                                          std::atof(attr(tag, "z").c_str()));
        } else if (tag.rfind("<triangle ", 0) == 0) {
            out.back().tris.emplace_back(std::atoi(attr(tag, "v1").c_str()), std::atoi(attr(tag, "v2").c_str()),
                                         std::atoi(attr(tag, "v3").c_str()));
            const std::string p = attr(tag, "p1");
            out.back().p1.push_back(p.empty() ? -1 : std::atoi(p.c_str()));
        } else if (tag.rfind("<base ", 0) == 0 && colors) {
            colors->push_back(attr(tag, "displaycolor"));
        }
    }
    return out;
}

Entity solid(int id, EntityType type, glm::vec3 c, glm::vec3 half, std::string name) {
    Entity e;
    e.id = id;
    e.type = type;
    e.localCenter = c;
    e.half = half;
    e.name = std::move(name);
    return e;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    ModelLibrary models;   // no models: an imported one needs the GPU (the editor's FITZEL_EXPORT_3MF)

    Document doc;
    MaterialDef plain;
    plain.name = "Default";
    doc.materials().push_back(plain);
    const fitzel::AssetId red  = doc.addMaterial("Red & Co", glm::vec3(1.0f, 0.0f, 0.0f), 0.0f, 0.5f);
    const fitzel::AssetId blue = doc.addMaterial("Blue", glm::vec3(0.0f, 0.0f, 1.0f), 0.0f, 0.5f);

    std::vector<Entity>& ents = doc.entities();
    // A group: an empty with a box, a sphere, a cylinder, a ramp, a modelled
    // mesh through a modifier, and what must be left out -- a light, a switched
    // off box, a decal.
    Entity group = solid(1, EntityType::Empty, glm::vec3(10.0f, 2.0f, -4.0f), glm::vec3(0.5f), "Group");
    group.localRotation = glm::vec3(0.0f, 30.0f, 0.0f);
    ents.push_back(std::move(group));
    {
        Entity box = solid(2, EntityType::Box, glm::vec3(0.0f, 0.25f, 0.0f), glm::vec3(1.0f, 0.25f, 0.5f), "Crate");
        box.parent = 1;
        auto mat = std::make_unique<MaterialComponent>();
        mat->material = red;
        box.components.items.push_back(std::move(mat));
        ents.push_back(std::move(box));
    }
    {
        Entity s = solid(3, EntityType::Sphere, glm::vec3(3.0f, 1.0f, 0.0f), glm::vec3(0.5f, 0.5f, 0.5f), "Ball");
        s.parent = 1;
        ents.push_back(std::move(s));
        Entity c = solid(4, EntityType::Cylinder, glm::vec3(-3.0f, 1.0f, 0.0f), glm::vec3(0.4f, 1.0f, 0.4f), "Post");
        c.parent = 1;
        ents.push_back(std::move(c));
        Entity r = solid(5, EntityType::Ramp, glm::vec3(0.0f, 0.5f, 3.0f), glm::vec3(1.0f, 0.5f, 1.0f), "Ramp");
        r.parent = 1;
        ents.push_back(std::move(r));
    }
    {
        // A modelled box, one face of it blue, smoothed by a subdivision.
        Entity m = solid(6, EntityType::Box, glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f), "Blob");
        m.parent = 2;   // under the crate: a grandchild of the group
        auto mc = std::make_unique<MeshComponent>();
        mc->mesh = EditMesh::box(glm::vec3(0.5f));
        mc->mesh.syncFaceMat();
        mc->mesh.faceMat[0] = blue;
        m.components.items.push_back(std::move(mc));
        auto ms = std::make_unique<ModifierStackComponent>();
        ms->stack.push_back(modifiers::make("subsurf"));
        m.components.items.push_back(std::move(ms));
        ents.push_back(std::move(m));
    }
    {
        Entity light = solid(7, EntityType::Light, glm::vec3(0.0f, 4.0f, 0.0f), glm::vec3(0.2f), "Lamp");
        light.parent = 1;
        ents.push_back(std::move(light));
        Entity off = solid(8, EntityType::Box, glm::vec3(0.0f, -5.0f, 0.0f), glm::vec3(0.5f), "Off");
        off.parent = 1;
        off.active = false;
        ents.push_back(std::move(off));
        Entity decal = solid(9, EntityType::Box, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.5f), "Decal");
        decal.parent = 1;
        decal.components.items.push_back(std::make_unique<DecalComponent>());
        ents.push_back(std::move(decal));
    }
    // Outside the group: a box far from the origin, and one not selected.
    ents.push_back(solid(10, EntityType::Box, glm::vec3(100000.0f, 0.5f, 0.0f), glm::vec3(1.0f, 0.5f, 0.5f), "Far"));
    ents.push_back(solid(11, EntityType::Box, glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), "Elsewhere"));
    scenegraph::resolve(ents);

    // --- What is taken -----------------------------------------------------------------
    std::printf("What is taken\n");
    {
        const std::vector<int> ids = export3mf::objectsOf(doc, {1});
        const std::vector<int> want = {2, 6, 3, 4, 5};
        bool same = ids.size() == want.size();
        for (int w : want) same = same && std::find(ids.begin(), ids.end(), w) != ids.end();
        std::string got;
        for (int id : ids) got += std::to_string(id) + " ";
        check(same, "the group: its solids and the grandchild; not the light, the switched-off box, the decal",
              got);
        check(export3mf::objectsOf(doc, {1, 2, 6}).size() == want.size(), "a child selected with its parent counts once");
        check(export3mf::objectsOf(doc, {7}).empty(), "a light alone: nothing to print");
    }

    // --- The parts ----------------------------------------------------------------------
    std::printf("The parts\n");
    const export3mf::Shape s = export3mf::gather(doc, models, {1});
    check(s.parts.size() == 5 && s.skipped == 0, "five parts, none unread", std::to_string(s.parts.size()));
    for (const export3mf::Part& p : s.parts)
        check(closed(p.tris), p.name + " is closed, every edge crossed once each way",
              std::to_string(p.verts.size()) + " corners, " + std::to_string(p.tris.size()) + " triangles");
    const double kPi = 3.14159265358979;
    const auto vol = [&](const char* name) {
        const export3mf::Part* p = partNamed(s, name);
        return p ? volume(p->verts, p->tris) : 0.0;
    };
    if (const export3mf::Part* crate = partNamed(s, "Crate")) {
        check(crate->verts.size() == 8 && crate->tris.size() == 12,
              "the box's corners welded: 8 of them, though each face was uploaded with its own",
              std::to_string(crate->verts.size()));
        check(std::abs(vol("Crate") - 2.0 * 0.5 * 1.0) < 1e-6, "the box's volume, exactly (and outward)", num(vol("Crate")));
        const glm::dvec3 c = centre(*crate);
        const Entity& e = *doc.find(2);
        check(glm::length(c - glm::dvec3(e.center)) < 1e-5, "standing where the crate stands in the world",
              num(c.x) + " " + num(c.y) + " " + num(c.z));
    } else {
        check(false, "a part named Crate");
    }
    {
        const double ball = 4.0 / 3.0 * kPi * 0.125, post = kPi * 0.16 * 2.0, ramp = 0.5 * 2.0 * 1.0 * 2.0;
        check(std::abs(vol("Ball") / ball - 1.0) < 0.01, "the sphere's volume within a percent", num(vol("Ball") / ball));
        check(std::abs(vol("Post") / post - 1.0) < 0.002, "the cylinder's within a fifth of one", num(vol("Post") / post));
        check(std::abs(vol("Ramp") - ramp) < 1e-5, "the ramp's, half its box", num(vol("Ramp")));
        const double blob = vol("Blob");
        const export3mf::Part* bp = partNamed(s, "Blob");
        check(bp && bp->tris.size() > 12 && blob > 0.3 && blob < 1.0,
              "the modelled box through its subdivision: rounder, smaller than its cage",
              num(blob) + " m3 in " + std::to_string(bp ? bp->tris.size() : 0) + " triangles");
    }

    // --- Colours ------------------------------------------------------------------------
    std::printf("Colours\n");
    {
        bool red1 = false, blue1 = false;
        for (const export3mf::Color& c : s.colors) {
            red1  = red1  || (c.name == "Red & Co" && c.rgb == glm::vec3(1, 0, 0));
            blue1 = blue1 || (c.name == "Blue" && c.rgb == glm::vec3(0, 0, 1));
        }
        check(s.colors.size() == 3 && red1 && blue1, "three colours: the crate's red, the blob's blue face, the default",
              std::to_string(s.colors.size()));
        const export3mf::Part* bp = partNamed(s, "Blob");
        int blueTris = 0;
        if (bp)
            for (int c : bp->triColor) blueTris += s.colors[static_cast<std::size_t>(c)].name == "Blue";
        check(blueTris > 0 && bp && blueTris < static_cast<int>(bp->tris.size()),
              "the blob's blue face stays blue through the subdivision, the rest does not",
              std::to_string(blueTris));
    }

    // --- The file -----------------------------------------------------------------------
    std::printf("The file\n");
    {
        export3mf::Options o;   // as in the scene
        const std::string xml = export3mf::modelXml(s, o, "Group <test>");
        std::vector<std::string> colors;
        const std::vector<XmlObject> objs = readObjects(xml, &colors);
        check(xml.find("unit=\"millimeter\"") != std::string::npos, "in millimetres");
        check(xml.find("Group &lt;test&gt;") != std::string::npos, "the title escaped");
        check(colors.size() == 3 && std::find(colors.begin(), colors.end(), "#FF0000FF") != colors.end(),
              "the colours as basematerials, red as #FF0000FF");
        // Five meshes and the assembly that holds them, last.
        check(objs.size() == 6 && objs.back().verts.empty() &&
                  xml.find("<component objectid=") != std::string::npos,
              "five mesh objects and one of components holding them");
        glm::dvec3 lo(1e300), hi(-1e300);
        bool allClosed = true, allOutward = true;
        for (const XmlObject& ob : objs) {
            if (ob.verts.empty()) continue;
            for (const glm::dvec3& v : ob.verts) { lo = glm::min(lo, v); hi = glm::max(hi, v); }
            allClosed = allClosed && closed(ob.tris);
            allOutward = allOutward && volume(ob.verts, ob.tris) > 0.0;
        }
        check(allClosed && allOutward, "read back, still closed and still outward: Z up by turning, not mirroring");
        check(std::abs(lo.z) < 1e-4, "standing on the bed (lowest point at Z 0)", num(lo.z));
        check(std::abs(lo.x + hi.x) < 2e-4 && std::abs(lo.y + hi.y) < 2e-4, "centred over the origin");
        const glm::vec3 size = export3mf::printSize(s, o);
        check(std::abs((hi - lo).x - size.x) < 1e-2 && std::abs((hi - lo).y - size.y) < 1e-2 &&
                  std::abs((hi - lo).z - size.z) < 1e-2,
              "as big as printSize says",
              num((hi - lo).x) + " x " + num((hi - lo).y) + " x " + num((hi - lo).z) + " mm");
        // Up is up: the blob sits three metres above the crate's base.
        double crateTop = -1e300, blobLow = 1e300;
        for (const XmlObject& ob : objs) {
            for (const glm::dvec3& v : ob.verts) {
                if (ob.name == "Crate") crateTop = std::max(crateTop, v.z);
                if (ob.name == "Blob") blobLow = std::min(blobLow, v.z);
            }
        }
        check(blobLow > crateTop + 1000.0, "the scene's up is the print's up: the blob above the crate",
              num(crateTop) + " / " + num(blobLow));
        // One colour per object said once; the blob's several per triangle.
        bool crateOnce = false, blobPer = false;
        for (const XmlObject& ob : objs) {
            if (ob.name == "Crate")
                crateOnce = !ob.pid.empty() && std::all_of(ob.p1.begin(), ob.p1.end(), [](int p) { return p < 0; });
            if (ob.name == "Blob") blobPer = !ob.pid.empty() && std::all_of(ob.p1.begin(), ob.p1.end(), [](int p) { return p >= 0; });
        }
        check(crateOnce && blobPer, "a one-colour part says its colour once, the blob per triangle");

        // Scaled: the longest side to 100 mm.
        export3mf::Options half;
        half.mmPerMetre = 10.0f;
        const glm::vec3 big = export3mf::printSize(s, o), small = export3mf::printSize(s, half);
        check(std::abs(small.x * 100.0f - big.x) < 1e-2f, "the scale scales", num(small.x) + " / " + num(big.x));

        // Written, and opened again as a zip.
        const std::filesystem::path out = std::filesystem::temp_directory_path() / "export3mfcheck.3mf";
        std::string err;
        check(export3mf::write(out.string(), s, o, err), "written", err);
        mz_zip_archive zip{};
        const bool opened = mz_zip_reader_init_file(&zip, out.string().c_str(), 0);
        check(opened, "a zip that opens");
        if (opened) {
            const bool types = mz_zip_reader_locate_file(&zip, "[Content_Types].xml", nullptr, 0) >= 0;
            const bool rels  = mz_zip_reader_locate_file(&zip, "_rels/.rels", nullptr, 0) >= 0;
            std::size_t n = 0;
            void* model = mz_zip_reader_extract_file_to_heap(&zip, "3D/3dmodel.model", &n, 0);
            check(types && rels && model, "holding the content types, the relationship and the model");
            const std::string title = "export3mfcheck";
            check(model && std::string(static_cast<const char*>(model), n) == export3mf::modelXml(s, o, title),
                  "the model in it is modelXml's, titled after the file");
            if (model) mz_free(model);
            mz_zip_reader_end(&zip);
        }
        std::filesystem::remove(out);
        check(!export3mf::write(out.string(), export3mf::Shape{}, o, err) && !std::filesystem::exists(out),
              "nothing selected: no file, and a reason", err);
    }

    // --- Far from the origin -----------------------------------------------------------
    std::printf("Far from the origin\n");
    {
        const export3mf::Shape far = export3mf::gather(doc, models, {10});
        const glm::vec3 size = export3mf::printSize(far, export3mf::Options{});
        check(far.parts.size() == 1 && closed(far.parts[0].tris), "a box a hundred kilometres out, closed");
        check(std::abs(size.x - 2000.0f) < 0.05f && std::abs(size.y - 1000.0f) < 0.05f && std::abs(size.z - 1000.0f) < 0.05f,
              "keeps its size to a tenth of a millimetre", num(size.x) + " x " + num(size.y) + " x " + num(size.z));
    }

    std::printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
