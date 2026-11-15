#include "Export3mf.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>

#include "miniz.h"

#include <fitzel/Version.hpp>

// The file side of the 3MF export: a Shape to XML and the zip around it. Apart
// from Export3mf.cpp because it needs nothing of the editor, so the Strahl
// command line (StrahlPrint.hpp) writes prints with the very same code.
namespace export3mf {
namespace {

// A number in millimetres: four decimals at most, no trailing zeros. Through
// to_chars so no locale can turn the point into a comma.
void appendNumber(std::string& out, double v) {
    if (std::abs(v) < 5.0e-5) v = 0.0;   // no "-0"
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed, 4);
    char* end = r.ptr;
    while (end > buf && end[-1] == '0') --end;
    if (end > buf && end[-1] == '.') --end;
    out.append(buf, end);
}

void appendEscaped(std::string& out, const std::string& s) {
    for (const char ch : s) {
        switch (ch) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                // Control characters are not allowed in XML 1.0 at all.
                if (static_cast<unsigned char>(ch) >= 0x20 || ch == '\t') out += ch;
        }
    }
}

std::string hexColor(const glm::vec3& c) {
    static const char* digits = "0123456789ABCDEF";
    std::string s = "#";
    for (int i = 0; i < 3; ++i) {
        const int v = static_cast<int>(std::lround(std::clamp(c[i], 0.0f, 1.0f) * 255.0f));
        s += digits[v >> 4];
        s += digits[v & 15];
    }
    return s + "FF";
}

} // namespace

glm::vec3 printSize(const Shape& s, const Options& o) {
    if (s.empty()) return glm::vec3(0.0f);
    const glm::dvec3 d = s.hi - s.lo;
    return glm::vec3(glm::dvec3(d.x, d.z, d.y) * static_cast<double>(o.mmPerMetre));   // Y up becomes Z up
}

std::string modelXml(const Shape& s, const Options& o, const std::string& title) {
    // Y up to Z up, turned rather than mirrored so every triangle keeps its
    // winding: X stays, the scene's Z (towards the viewer) becomes -Y, its Y
    // becomes Z. Centred over the origin and stood on Z 0.
    const double k  = o.mmPerMetre;
    const double cx = 0.5 * (s.lo.x + s.hi.x);
    const double cz = 0.5 * (s.lo.z + s.hi.z);
    const double y0 = s.lo.y;

    std::size_t tris = 0, verts = 0;
    for (const Part& p : s.parts) { tris += p.tris.size(); verts += p.verts.size(); }
    std::string x;
    x.reserve(1024 + verts * 64 + tris * 56);
    x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<model unit=\"millimeter\" xml:lang=\"en-US\" "
         "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n";
    x += " <metadata name=\"Title\">";
    appendEscaped(x, title);
    x += "</metadata>\n <metadata name=\"Application\">Fitzel ";
    x += fitzel::kVersion;
    x += "</metadata>\n <resources>\n";

    const bool colored = !s.colors.empty();
    const int  matId   = 1;                  // the colours' resource
    int        nextId  = colored ? 2 : 1;
    if (colored) {
        x += "  <basematerials id=\"1\">\n";
        for (const Color& c : s.colors) {
            x += "   <base name=\"";
            appendEscaped(x, c.name);
            x += "\" displaycolor=\"" + hexColor(c.rgb) + "\"/>\n";
        }
        x += "  </basematerials>\n";
    }

    std::vector<int> partIds;
    for (const Part& p : s.parts) {
        const int id = nextId++;
        partIds.push_back(id);
        x += "  <object id=\"" + std::to_string(id) + "\" type=\"model\" name=\"";
        appendEscaped(x, p.name);
        x += "\"";
        // A part in one colour says so once; one in several, per triangle.
        const bool oneColor = std::all_of(p.triColor.begin(), p.triColor.end(),
                                          [&](int c) { return c == p.triColor.front(); });
        if (colored && !p.triColor.empty() && p.triColor.front() >= 0)
            x += " pid=\"" + std::to_string(matId) + "\" pindex=\"" + std::to_string(p.triColor.front()) + "\"";
        x += ">\n   <mesh>\n    <vertices>\n";
        for (const glm::dvec3& v : p.verts) {
            x += "     <vertex x=\"";
            appendNumber(x, (v.x - cx) * k);
            x += "\" y=\"";
            appendNumber(x, -(v.z - cz) * k);
            x += "\" z=\"";
            appendNumber(x, (v.y - y0) * k);
            x += "\"/>\n";
        }
        x += "    </vertices>\n    <triangles>\n";
        for (std::size_t t = 0; t < p.tris.size(); ++t) {
            const glm::ivec3& tr = p.tris[t];
            x += "     <triangle v1=\"" + std::to_string(tr.x) + "\" v2=\"" + std::to_string(tr.y) +
                 "\" v3=\"" + std::to_string(tr.z) + "\"";
            if (colored && !oneColor && p.triColor[t] >= 0)
                x += " pid=\"" + std::to_string(matId) + "\" p1=\"" + std::to_string(p.triColor[t]) + "\"";
            x += "/>\n";
        }
        x += "    </triangles>\n   </mesh>\n  </object>\n";
    }

    // Several objects: one print of several parts, the way a slicer keeps
    // them together and in place (each a part of one object, not N objects
    // it would arrange across the bed).
    int buildId = partIds.empty() ? 0 : partIds.front();
    if (partIds.size() > 1) {
        buildId = nextId++;
        x += "  <object id=\"" + std::to_string(buildId) + "\" type=\"model\" name=\"";
        appendEscaped(x, title);
        x += "\">\n   <components>\n";
        for (int id : partIds) x += "    <component objectid=\"" + std::to_string(id) + "\"/>\n";
        x += "   </components>\n  </object>\n";
    }
    x += " </resources>\n <build>\n";
    if (buildId > 0) x += "  <item objectid=\"" + std::to_string(buildId) + "\"/>\n";
    x += " </build>\n</model>\n";
    return x;
}

bool write(const std::string& path, const Shape& s, const Options& o, std::string& err) {
    if (s.empty()) { err = "Nothing to export: the selection has no surface."; return false; }
    std::string title = path;
    if (const auto slash = title.find_last_of("/\\"); slash != std::string::npos) title.erase(0, slash + 1);
    if (const auto dot = title.rfind('.'); dot != std::string::npos && dot > 0) title.erase(dot);

    // The package around the model: what each part is, and which one is the model.
    const std::string types =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">\n"
        " <Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>\n"
        " <Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>\n"
        "</Types>\n";
    const std::string rels =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">\n"
        " <Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
        "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>\n"
        "</Relationships>\n";
    const std::string model = modelXml(s, o, title);

    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) { err = "Cannot start the zip."; return false; }
    bool ok = true;
    const std::pair<const char*, const std::string*> entries[] = {
        {"[Content_Types].xml", &types}, {"_rels/.rels", &rels}, {"3D/3dmodel.model", &model}};
    for (const auto& [name, bytes] : entries)
        if (ok && !mz_zip_writer_add_mem(&zip, name, bytes->data(), bytes->size(), MZ_DEFAULT_COMPRESSION)) {
            err = std::string("Cannot add ") + name + " to the zip.";
            ok = false;
        }
    void*  buf  = nullptr;
    size_t size = 0;
    if (ok && !mz_zip_writer_finalize_heap_archive(&zip, &buf, &size)) {
        err = "Cannot finish the zip.";
        ok = false;
    }
    mz_zip_writer_end(&zip);
    if (ok) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(static_cast<const char*>(buf), static_cast<std::streamsize>(size));
        if (!f) { err = "Cannot write " + path; ok = false; }
    }
    if (buf) mz_free(buf);
    return ok;
}

} // namespace export3mf
