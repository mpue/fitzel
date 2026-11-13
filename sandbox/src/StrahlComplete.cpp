// Completion for Strahl scripts: where the caret is, what fits there, and the
// signature of the call around it. See StrahlComplete.hpp.

#include "StrahlComplete.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

#include "StrahlInternal.hpp"

namespace strahl::complete {
namespace {

namespace fs = std::filesystem;

// --- What the language itself knows -------------------------------------------

struct Builtin {
    const char* name;
    const char* params;   // as the signature shows them; "…" = any number
    const char* doc;
};

// Every built-in function, with the names its arguments go by (the
// interpreter's ArgReader names, so `fbm(p, octaves: 3)` works as shown).
const Builtin kBuiltins[] = {
    {"sin", "x", "Sine of an angle in degrees. Element-wise on vectors."},
    {"cos", "x", "Cosine of an angle in degrees. Element-wise on vectors."},
    {"tan", "x", "Tangent of an angle in degrees."},
    {"asin", "x", "Arc sine, in degrees."},
    {"acos", "x", "Arc cosine, in degrees."},
    {"atan", "x", "Arc tangent, in degrees."},
    {"atan2", "y, x", "The angle of the point (x, y) in degrees, -180..180."},
    {"sqrt", "x", "Square root (0 below zero)."},
    {"pow", "x, y", "x to the power y. The operator ^ does the same."},
    {"exp", "x", "e to the power x."},
    {"log", "x", "Natural logarithm."},
    {"log2", "x", "Base-2 logarithm."},
    {"log10", "x", "Base-10 logarithm."},
    {"abs", "x", "Absolute value. Element-wise on vectors."},
    {"sign", "x", "-1, 0 or 1."},
    {"floor", "x", "Rounded down."},
    {"ceil", "x", "Rounded up."},
    {"round", "x", "Rounded to the nearest whole number."},
    {"fract", "x", "The part after the decimal point: x - floor(x)."},
    {"mod", "x, y", "Remainder of x / y, always with the sign of y (also for negative x)."},
    {"min", "values…", "The smallest -- of the arguments, or of one list. Element-wise on vectors."},
    {"max", "values…", "The largest -- of the arguments, or of one list. Element-wise on vectors."},
    {"clamp", "x, lo = 0, hi = 1", "x kept between lo and hi."},
    {"saturate", "x", "x kept between 0 and 1."},
    {"mix", "a, b, t", "From a (t = 0) to b (t = 1). Numbers, vectors and colours."},
    {"lerp", "a, b, t", "The same as mix()."},
    {"smoothstep", "edge0, edge1, x", "0 below edge0, 1 above edge1, a smooth S between."},
    {"step", "edge, x", "0 below edge, 1 from it on."},
    {"radians", "x", "Degrees to radians (the trigonometry here takes degrees)."},
    {"degrees", "x", "Radians to degrees."},
    {"vec", "values…", "A vector from numbers and smaller vectors: vec(p.xz, 1)."},
    {"rgb", "r, g, b", "A colour from linear components 0..1 (a #hex colour is sRGB)."},
    {"gray", "v", "A grey: [v, v, v]."},
    {"length", "v", "The length of a vector."},
    {"normalize", "v", "The vector scaled to length 1."},
    {"dot", "a, b", "Dot product."},
    {"cross", "a, b", "Cross product of two 3-vectors."},
    {"distance", "a, b", "The distance between two points."},
    {"reflect", "d, n", "The direction d mirrored about the normal n."},
    {"noise", "p", "Perlin noise at a point, about -1..1. Compiles for the tracer."},
    {"fbm", "p, octaves = 5", "Noise summed over octaves: clouds, terrain, about -1..1."},
    {"turbulence", "p, octaves = 5", "Summed |noise|: sharp creases, 0..1. Marble veins, fire."},
    {"voronoi", "p", "The distance to the nearest of randomly scattered cell points."},
    {"cell_id", "p", "A number 0..1 that is constant over each voronoi cell."},
    {"hash", "p", "A random number 0..1 from the exact value: the same input, the same number."},
    {"parity", "p", "0 or 1 on a 3D chequerboard of unit cells."},
    {"random", "seed, lo = 0, hi = 1", "A repeatable random number for a seed, or a vector of them for a vector seed."},
    {"ramp", "t, colors", "A colour along a ramp: [c0, c1, ...] evenly spread, or [[0.3, c], ...] at given positions."},
    {"at", "pattern, p, n, uv", "A colour or pattern at a point: a colour is itself, a pattern is asked. How patterns nest."},
    {"texture", "image, uv", "A picture from load_image() at uv (0..1, repeating)."},
    {"load_image", "file", "Read a picture (PNG, JPG, HDR, EXR) for texture(), beside the script or in the library."},
    {"len", "value", "The number of items in a list, characters in a string, components in a vector."},
    {"range", "start, end, step = 1", "The numbers from start up to (not including) end. Also written start..end."},
    {"list", "values…", "A list of the arguments."},
    {"push", "list, value", "Append to a list (changes it)."},
    {"pop", "list", "Remove and return a list's last item."},
    {"map", "list, f", "A new list of f(item) for every item."},
    {"filter", "list, f", "The items for which f(item) is true."},
    {"reverse", "list", "The list back to front."},
    {"sum", "list", "All the items added up."},
    {"contains", "list, value", "Whether the list has the value."},
    {"str", "values…", "The values as text, joined."},
    {"print", "values…", "Write to the Output pane (and the console)."},
    {"error", "message", "Stop the script with this message, at this line."},
    {"assert", "condition, message", "Stop the script when the condition is false."},
    {"type", "value", "The value's type as a string: \"number\", \"vector\", \"object\", ..."},
    {"identity", "", "The identity matrix."},
    {"translation", "v", "A matrix moving by v."},
    {"rotation", "degrees", "A matrix turning by [x, y, z] degrees, x first (as rotate: does)."},
    {"rotation_axis", "axis, degrees", "A matrix turning about an axis."},
    {"scaling", "s", "A matrix scaling by a number or a vector."},
    {"inverse", "m", "The inverse of a matrix."},
    {"apply", "m, point", "A point transformed by a matrix (the same as m * point)."},
    {"apply_dir", "m, direction", "A direction transformed by a matrix (no translation)."},
};

struct Named { const char* key; const char* doc; };

const Named kKindDocs[] = {
    {"sphere", "A sphere: center and radius."},
    {"box", "An axis-aligned box from min to max, or of size around center. Without either: a 1 m cube at the origin."},
    {"plane", "An infinite plane through offset along its normal (or through point). Solid on the side the normal points away from."},
    {"disc", "A flat disc without volume, with an optional hole. Not useful in CSG."},
    {"cylinder", "A cylinder along the axis from `from` to `to`. open: true leaves off the caps."},
    {"cone", "A cone frustum: radius at `from`, top_radius at `to` (0: a point)."},
    {"torus", "A ring in the x-z plane: major is the ring's radius, minor the tube's."},
    {"isosurface", "The surface where function(p) = threshold, solid where it is below. Sought in bound, or between min and max."},
    {"mesh", "Triangles or polygons: vertices and faces (lists of indices). Not a CSG operand."},
    {"union", "Its children together. At the top level just a group."},
    {"merge", "Like union, without the surfaces inside -- the one for glass."},
    {"intersection", "Only what lies inside all of its children."},
    {"difference", "The first child with all the others cut away."},
    {"group", "Children moved and given a material together."},
    {"object", "The same as group."},
    {"material", "A surface: colour, roughness, metal, glass, emission, bump. Every channel can be a pattern."},
    {"light", "A point light. intensity is the illumination at 1 m; it falls off with the square of the distance."},
    {"spot", "A spot light at position, pointing at look_at (or along direction)."},
    {"sun", "The sun, far away: direction points TOWARD it. One per scene."},
    {"sky", "The sky: a gradient (zenith, horizon, ground), a panorama (hdri) or the engine's procedural sky."},
    {"fog", "Height fog, as the viewport draws it."},
    {"camera", "The camera: where it is, what it looks at, the vertical fov, depth of field."},
    {"render", "How the picture is made: size, samples, bounces, tone mapping and grade."},
};

// "kind.property" first, then the property on its own.
const Named kPropDocs[] = {
    {"material", "The surface: a material { }, a colour, a pattern, or a library material like gold."},
    {"translate", "Move by [x, y, z]. Transforms apply in the order they are written."},
    {"rotate", "Turn by [x, y, z] degrees: about x first, then y, then z."},
    {"scale", "Scale by a number, or by [x, y, z] (uneven is fine)."},
    {"matrix", "Apply a matrix: translation(), rotation(), scaling(), multiplied together."},
    {"transform", "The same as matrix."},
    {"center", "The centre point [x, y, z]."},
    {"radius", "The radius in metres."},
    {"light.radius", "The bulb's radius: larger gives softer shadows (0.05)."},
    {"spot.radius", "The bulb's radius: larger gives softer shadows."},
    {"cone.radius", "The radius at `from`."},
    {"min", "The lower corner [x, y, z]."},
    {"max", "The upper corner [x, y, z]."},
    {"isosurface.min", "The lower corner of the box the surface is sought in."},
    {"isosurface.max", "The upper corner of the box the surface is sought in."},
    {"size", "The edge length: a number, or [x, y, z]."},
    {"normal", "The direction the surface faces."},
    {"offset", "How far the plane lies from the origin, along its normal."},
    {"point", "A point the plane passes through (instead of offset)."},
    {"hole", "The radius of the hole in the middle."},
    {"from", "One end of the axis."},
    {"to", "The other end of the axis."},
    {"open", "true: no caps."},
    {"isosurface.open", "true: where the box cuts the surface, it is left open instead of closed."},
    {"top_radius", "The radius at `to` (0: a point)."},
    {"major", "The radius of the ring."},
    {"minor", "The radius of the tube."},
    {"function", "fn(p) => ...: below the threshold inside, above outside. Compiled for the tracer."},
    {"bound", "The box around the origin it is sought in: a half size, a number or [x, y, z]."},
    {"threshold", "The value of the function on the surface (0)."},
    {"max_gradient", "The function's steepest slope. Left out, it is measured (and written to the log); too small leaves holes."},
    {"accuracy", "How close the search comes to the surface, in metres."},
    {"vertices", "The points: a list of [x, y, z]."},
    {"faces", "The polygons: lists of vertex indices, counter-clockwise seen from outside."},
    {"normals", "One normal per vertex (otherwise computed)."},
    {"uvs", "One [u, v] per vertex."},
    {"smooth", "true: normals averaged across faces."},
    {"color", "The light's colour."},
    {"material.color", "The base colour (#hex is sRGB), or a pattern."},
    {"roughness", "0 a mirror .. 1 matte. Or a pattern."},
    {"metallic", "0 dielectric .. 1 metal. Or a pattern."},
    {"glass", "true: a dielectric that refracts."},
    {"ior", "The index of refraction: water 1.33, glass 1.5, diamond 2.42. Sets glass when given alone."},
    {"absorption", "Light lost per metre inside the glass, per channel: colour that deepens with thickness."},
    {"medium_color", "The colour of the glass's body (water)."},
    {"emission", "The colour the surface glows (sRGB), or a pattern."},
    {"strength", "The multiplier for emission."},
    {"translucency", "The part of the light that comes out on the back side (leaves, paper)."},
    {"opacity", "Below 1: partly see-through."},
    {"bump", "A height in metres per point -- fn(p) => ... or bumps(), dents(), ... -- that tilts the normal."},
    {"bump_strength", "The multiplier for bump."},
    {"shadow", "false: casts no shadow."},
    {"additive", "true: light is added, not covered (spray, flames)."},
    {"position", "Where it is [x, y, z]."},
    {"intensity", "How bright."},
    {"light.intensity", "The illumination at 1 m (10)."},
    {"spot.intensity", "The illumination at 1 m (20)."},
    {"sun.intensity", "How bright the sunlight is (3)."},
    {"sky.intensity", "How bright the sky's light is (1)."},
    {"range", "Beyond this distance it lights nothing."},
    {"visible", "true: the bulb itself shows in the picture."},
    {"look_at", "The point it faces."},
    {"direction", "The direction it faces (instead of look_at)."},
    {"sun.direction", "The direction TOWARD the sun."},
    {"angle", "The cone's half angle in degrees (30)."},
    {"sun.angle", "The sun disc's diameter in degrees (0.53): larger gives softer shadows."},
    {"inner", "The half angle inside which the spot is at full brightness."},
    {"zenith", "The colour straight up."},
    {"horizon", "The colour at the horizon."},
    {"ground", "The colour below the horizon."},
    {"hdri", "A panorama file (.hdr, .exr) to light the scene and fill the background."},
    {"sky.rotate", "Turn the panorama about the vertical, in degrees."},
    {"background", "What the camera sees behind everything; light still comes from the sky. Not for glass scenes."},
    {"procedural", "true: the engine's own sky, sun and all."},
    {"density", "How thick the fog is."},
    {"height", "The height the fog is measured from."},
    {"render.height", "The picture's height in pixels."},
    {"falloff", "How fast the fog thins out going up."},
    {"sun_color", "The colour of the glow around the sun in the fog."},
    {"up", "Which way is up for the camera ([0, 1, 0])."},
    {"fov", "The vertical field of view in degrees (45)."},
    {"aperture", "The lens's radius in metres: depth of field. 0: everything sharp."},
    {"focus", "The distance in focus, in metres, or \"auto\" for what the centre of the picture sees."},
    {"roll", "Tilt the camera about its view, in degrees."},
    {"width", "The picture's width in pixels."},
    {"samples", "Samples per pixel: more is less noise and more time."},
    {"bounces", "How often a path may bounce."},
    {"batch", "Samples per pass between progress updates."},
    {"seed", "The random seed: another seed, another noise."},
    {"threads", "Render threads (0: all cores)."},
    {"clamp", "The brightest a single sample may be: tames fireflies."},
    {"exposure", "Exposure in stops."},
    {"tonemap", "\"aces\", \"agx\" or \"neutral\"."},
    {"saturation", "Colour saturation (1)."},
    {"contrast", "Contrast (1)."},
    {"warmth", "Shift toward warm (+) or cool (-)."},
    {"hue", "Turn the hues, in degrees."},
    {"vignette", "Darken the corners."},
    {"output", "The file the command line writes the picture to."},
};

struct Constant { const char* name; const char* detail; const char* doc; };
const Constant kConstants[] = {
    {"pi", "3.14159", "Half a turn in radians."},
    {"tau", "6.28318", "A whole turn in radians."},
    {"e", "2.71828", "Euler's number."},
    {"inf", "1e30", "Practically infinite."},
    {"x", "[1, 0, 0]", "The x axis."},
    {"y", "[0, 1, 0]", "The y axis: up."},
    {"z", "[0, 0, 1]", "The z axis: toward the default camera."},
    {"time", "0..1", "Where in the animation this frame is: 0 at the first, 1 at the last (--frames)."},
    {"frame", "0, 1, ...", "The number of the frame being rendered."},
    {"frames", "N", "How many frames the animation has."},
};

const char* kStatementKeywords[] = {"let", "fn", "if", "for", "while", "import", "return", "break", "continue"};
const char* kExprKeywords[]      = {"if", "fn", "not", "true", "false", "nil"};
const char* kInfixKeywords[]     = {"and", "or", "in", "else"};

bool isKeyword(const std::string& s) {
    static const char* all[] = {"let", "fn", "if", "else", "for", "in", "while", "return", "break",
                                "continue", "import", "true", "false", "nil", "and", "or", "not"};
    for (const char* k : all)
        if (s == k) return true;
    return false;
}

const char* lookup(const Named* table, std::size_t n, const std::string& key) {
    for (std::size_t i = 0; i < n; ++i)
        if (key == table[i].key) return table[i].doc;
    return nullptr;
}

std::string kindDoc(const std::string& kind) {
    const char* d = lookup(kKindDocs, std::size(kKindDocs), kind);
    return d ? d : "";
}

std::string propDoc(const std::string& kind, const std::string& prop) {
    if (!kind.empty())
        if (const char* d = lookup(kPropDocs, std::size(kPropDocs), kind + "." + prop)) return d;
    const char* d = lookup(kPropDocs, std::size(kPropDocs), prop);
    return d ? d : "";
}

const std::vector<KindSchema>& schemas() {
    static const std::vector<KindSchema> s = kindSchemas();
    return s;
}

const KindSchema* schema(const std::string& kind) {
    for (const KindSchema& k : schemas())
        if (k.name == kind) return &k;
    return nullptr;
}

bool isCsgOrGroup(const std::string& kind) {
    return kind == "union" || kind == "merge" || kind == "intersection" || kind == "difference" ||
           kind == "group" || kind == "object";
}

// --- Reading text ----------------------------------------------------------------

bool identStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool identChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

enum class Tk { Ident, Number, String, Colour, Punct, Comment };

struct Tok {
    Tk          t;
    std::size_t a, b;          // byte range
    int         line;
    bool        nl;            // a line break between this and the previous token
    bool        closed = true; // a string: its closing quote is there
};

// Like the language's own lexer, but it never fails: an unfinished string
// ends at its line, an unknown character is punctuation.
std::vector<Tok> lex(const std::string& s) {
    std::vector<Tok> out;
    std::size_t i = 0;
    int  line = 0;
    bool nl = true;
    const std::size_t n = s.size();
    const auto push = [&](Tk t, std::size_t a, std::size_t b, int ln) {
        Tok k{t, a, b, ln, nl};
        out.push_back(k);
        if (t != Tk::Comment) nl = false;   // a comment does not join two lines
    };
    while (i < n) {
        const char c = s[i];
        if (c == '\n') { ++line; nl = true; ++i; continue; }
        if (c == ' ' || c == '\t' || c == '\r') { ++i; continue; }
        const std::size_t a = i;
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            while (i < n && s[i] != '\n') ++i;
            push(Tk::Comment, a, i, line);
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            const int ln = line;
            i += 2;
            while (i < n && !(s[i] == '*' && i + 1 < n && s[i + 1] == '/')) { if (s[i] == '\n') ++line; ++i; }
            i = std::min(n, i + 2);
            push(Tk::Comment, a, i, ln);
            continue;
        }
        if (c == '"' || c == '\'') {
            ++i;
            bool closed = false;
            while (i < n && s[i] != '\n') {
                if (s[i] == '\\' && i + 1 < n) { i += 2; continue; }
                if (s[i] == c) { ++i; closed = true; break; }
                ++i;
            }
            push(Tk::String, a, i, line);
            out.back().closed = closed;
            continue;
        }
        if (c == '#') {
            ++i;
            while (i < n && std::isxdigit(static_cast<unsigned char>(s[i]))) ++i;
            push(Tk::Colour, a, i, line);
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            while (i < n && (std::isdigit(static_cast<unsigned char>(s[i])) ||
                             (s[i] == '.' && !(i + 1 < n && s[i + 1] == '.'))))
                ++i;
            if (i < n && (s[i] == 'e' || s[i] == 'E')) {
                std::size_t j = i + 1;
                if (j < n && (s[j] == '+' || s[j] == '-')) ++j;
                if (j < n && std::isdigit(static_cast<unsigned char>(s[j]))) {
                    i = j;
                    while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                }
            }
            push(Tk::Number, a, i, line);
            continue;
        }
        if (identStart(c)) {
            while (i < n && identChar(s[i])) ++i;
            push(Tk::Ident, a, i, line);
            continue;
        }
        static const char* two[] = {"..=", "..", "=>", "==", "!=", "<=", ">=", "&&", "||", "++",
                                    "+=", "-=", "*=", "/="};
        std::size_t len = 1;
        for (const char* t : two) {
            const std::size_t l = std::strlen(t);
            if (s.compare(i, l, t) == 0) { len = l; break; }
        }
        i += len;
        push(Tk::Punct, a, i, line);
    }
    return out;
}

// --- Definitions ---------------------------------------------------------------

struct Param {
    std::string name, def;   // def: the default as written ("" none)
};

struct Def {
    std::string        name;
    Kind               kind = Kind::Value;   // Function, Value or Parameter
    std::vector<Param> params;
    std::string        detail;   // a value's expression; a parameter's default
    std::string        doc;
    std::string        flavor;   // colour, material, pattern, bump, shape, object, ...
    int                source = 0;   // 0 the script, 1 an import, 2 the prelude
};

// A prelude section's header says what its definitions are. Its first word
// decides: "Bump maps: ..., for material { bump: ... }" holds bump maps.
std::string flavorOfGroup(const std::string& header) {
    const std::string h = lower(trim(header));
    for (const char* f : {"colour", "material", "pattern", "bump", "shape", "compound"})
        if (h.rfind(f, 0) == 0) return std::string(f) == "compound" ? "shape" : f;
    return "";
}

std::string signatureOf(const std::string& name, const std::vector<Param>& ps,
                        std::vector<std::pair<int, int>>* spans = nullptr) {
    std::string s = name + "(";
    for (std::size_t i = 0; i < ps.size(); ++i) {
        if (i) s += ", ";
        const int a = static_cast<int>(s.size());
        s += ps[i].name;
        if (!ps[i].def.empty()) s += " = " + ps[i].def;
        if (spans) spans->push_back({a, static_cast<int>(s.size())});
    }
    return s + ")";
}

std::vector<Param> splitParams(const std::string& list) {
    std::vector<Param> out;
    std::string cur;
    int depth = 0;
    const auto flush = [&] {
        const std::string t = trim(cur);
        cur.clear();
        if (t.empty()) return;
        Param p;
        const std::size_t eq = t.find('=');
        p.name = trim(t.substr(0, eq));
        if (eq != std::string::npos) p.def = trim(t.substr(eq + 1));
        out.push_back(p);
    };
    for (char c : list) {
        if (c == '(' || c == '[' || c == '{') ++depth;
        if (c == ')' || c == ']' || c == '}') --depth;
        if (c == ',' && depth == 0) { flush(); continue; }
        cur += c;
    }
    flush();
    return out;
}

// The scan: one pass over the tokens with a stack of open brackets. It
// collects the definitions a place can see, and stops to take a picture of
// the stack at the word being typed.
enum class Fr { Code, Object, Call, Params, List, Paren };

struct Frame {
    Fr                       what = Fr::Code;
    std::size_t              tok = 0;     // the opening bracket's token
    std::string              name;        // Object: its kind ("" unknown); Call: the function
    std::vector<Def>         locals;
    std::vector<std::string> given;       // Object / Call: properties or names already set
    int                      arg = 0;     // Call: commas so far
    std::size_t              argTok = 0;  // Call: the token the current argument starts at
};

struct ExprScope {   // the parameters of a fn whose body is an expression
    std::size_t      depth;
    std::vector<Def> params;
};

struct Snapshot {
    bool                   taken = false;
    std::vector<Frame>     stack;
    std::vector<ExprScope> exprs;
    int                    prev = -1, prev2 = -1;   // significant tokens before the word
    bool                   naming = false;          // after let / fn / for: a new name
};

struct Scanner {
    const std::string&      src;
    const std::vector<Tok>& toks;
    int                     source;
    std::string             docGroup;   // prelude: the section header above

    std::vector<Def>       globals;
    std::map<std::string, std::string> varKind;   // let a = sphere {...}: a is a sphere
    std::vector<std::string> imports;

    std::vector<Frame>     stack;
    std::vector<ExprScope> exprs;
    int prev = -1, prev2 = -1;

    // The small state machines of let / fn / for headers.
    enum class FnState { None, AfterFn, AfterName, InParams, AfterParams } fn = FnState::None;
    std::vector<Def> fnParams;
    int  fnDef = -1;               // the global being defined (doc, params), or -1
    bool letName = false;          // the next identifier is a let's name
    bool letEq = false;            // ...was just read: an = should follow
    bool letRhs = false;           // the let's value starts at this token
    Def* letDef = nullptr;         // that let (valid only across these few tokens)
    bool forVars = false;
    std::vector<Def> loopVars;
    std::size_t pendingCode = static_cast<std::size_t>(-1);
    std::string lastCall;          // the name of the call whose ) was just passed

    Scanner(const std::string& s, const std::vector<Tok>& t, int src_) : src(s), toks(t), source(src_) {}

    std::string text(const Tok& t) const { return src.substr(t.a, t.b - t.a); }
    std::string text(int i) const { return i < 0 ? std::string() : text(toks[static_cast<std::size_t>(i)]); }
    bool is(int i, const char* s) const { return i >= 0 && text(i) == s; }
    bool isP(int i, const char* s) const { return i >= 0 && toks[static_cast<std::size_t>(i)].t == Tk::Punct && text(i) == s; }

    // The comment lines straight above token i (no blank line between),
    // without section headers.
    std::string docAbove(std::size_t i) {
        std::vector<std::string> lines;
        int want = toks[i].line - 1;
        for (std::size_t k = i; k-- > 0;) {
            const Tok& c = toks[k];
            if (c.t != Tk::Comment || c.line != want) break;
            std::string t = text(c);
            if (t.rfind("//", 0) == 0) t = trim(t.substr(2));
            else break;
            if (t.rfind("---", 0) == 0) break;
            lines.push_back(t);
            --want;
        }
        std::string out;
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
            if (!out.empty()) out += ' ';
            out += *it;
        }
        return out;
    }

    void declare(Def d) {
        if (stack.empty()) globals.push_back(std::move(d));
        else stack.back().locals.push_back(std::move(d));
    }

    Def* lastDeclared() {
        std::vector<Def>& v = stack.empty() ? globals : stack.back().locals;
        return v.empty() ? nullptr : &v.back();
    }

    static bool continues(const std::string& p) {
        static const char* c[] = {":", "=", "=>", "+", "-", "*", "/", "%", "^", "<", ">", "==", "!=", "<=",
                                  ">=", "&&", "||", "!", "(", "[", ",", "..", "..=", "++", "+=", "-=", "*=",
                                  "/=", "and", "or", "not", "in", "return"};
        for (const char* k : c)
            if (p == k) return true;
        return false;
    }

    void push(Fr what, std::size_t tok, std::string name = {}) {
        Frame f;
        f.what   = what;
        f.tok    = tok;
        f.name   = std::move(name);
        f.argTok = tok + 1;
        stack.push_back(std::move(f));
    }

    void popTo(char open, std::size_t close) {
        while (!stack.empty()) {
            const Frame f = stack.back();
            stack.pop_back();
            while (!exprs.empty() && exprs.back().depth > stack.size()) exprs.pop_back();
            const char o = src[toks[f.tok].a];
            if (o == open) {
                if (f.what == Fr::Params) {
                    fnParams.clear();
                    const std::size_t a = toks[f.tok].b;
                    const std::size_t b = toks[close].a;
                    for (const Param& p : splitParams(src.substr(a, b > a ? b - a : 0))) {
                        Def d;
                        d.name = p.name;
                        d.kind = Kind::Parameter;
                        d.detail = p.def.empty() ? "parameter" : "= " + p.def;
                        d.source = source;
                        fnParams.push_back(d);
                    }
                    fn = FnState::AfterParams;
                    if (fnDef >= 0) {
                        Def& g = globals[static_cast<std::size_t>(fnDef)];
                        for (const Def& p : fnParams)
                            g.params.push_back({p.name, p.detail == "parameter" ? "" : p.detail.substr(2)});
                    }
                } else if (f.what == Fr::Call) {
                    lastCall = f.name;
                }
                return;
            }
        }
    }

    void endExprScopes() {
        while (!exprs.empty() && exprs.back().depth >= stack.size()) exprs.pop_back();
    }

    // Handle one significant token (comments are skipped by the caller).
    void step(std::size_t i) {
        const Tok& t = toks[i];
        const std::string s = text(t);
        if (t.nl && prev >= 0 && !continues(text(prev))) {
            // A new line ends an expression body at this depth.
            while (!exprs.empty() && exprs.back().depth == stack.size()) exprs.pop_back();
        }
        if (letEq) {
            letEq = false;
            if (t.t == Tk::Punct && s == "=") letRhs = true;
            else letDef = nullptr;
        } else if (letRhs && letDef) {
            // The first token of a let's value: what kind of thing it is.
            std::string fl;
            if (t.t == Tk::Colour) fl = "colour";
            else if (t.t == Tk::String) fl = "string";
            else if (t.t == Tk::Number) fl = "number";
            else if (t.t == Tk::Ident && schema(s)) {
                fl = s == "material" ? "material" : "object";
                varKind[letDef->name] = s;
            } else if (t.t == Tk::Ident && s == "fn") fl = "function";
            else if (t.t == Tk::Ident) fl = "call:" + s;   // resolved against the definitions later
            if (letDef->flavor.empty()) letDef->flavor = fl;
            // The expression as written, up to the end of its line.
            std::size_t e = t.a;
            while (e < src.size() && src[e] != '\n') ++e;
            std::string x = trim(src.substr(t.a, e - t.a));
            if (x.size() > 48) x = x.substr(0, 45) + "...";
            letDef->detail = "= " + x;
            letRhs = false;
            letDef = nullptr;
        }

        if (t.t == Tk::Ident) {
            if (letName && !isKeyword(s)) {
                Def d;
                d.name = s;
                d.kind = Kind::Value;
                d.doc = docAbove(i > 0 ? i - 1 : i);   // above the `let`
                d.source = source;
                if (source == 2) d.flavor = flavorOfGroup(docGroup);
                declare(d);
                letDef = lastDeclared();
                letName = false;
                letEq = true;
                prev2 = prev;
                prev = static_cast<int>(i);
                return;
            } else if (fn == FnState::AfterFn && !isKeyword(s)) {
                Def d;
                d.name = s;
                d.kind = Kind::Function;
                d.doc = docAbove(i > 0 ? i - 1 : i);   // above the `fn`
                d.source = source;
                if (source == 2) d.flavor = flavorOfGroup(docGroup);
                fnDef = stack.empty() ? static_cast<int>(globals.size()) : -1;
                declare(d);
                fn = FnState::AfterName;
            } else if (forVars && s != "in") {
                Def d;
                d.name = s;
                d.kind = Kind::Value;
                d.detail = "loop variable";
                d.source = source;
                loopVars.push_back(d);
            } else if (s == "in" && forVars) {
                forVars = false;
            } else if (s == "let") {
                letName = true;
            } else if (s == "fn") {
                fn = FnState::AfterFn;
                fnDef = -1;
                fnParams.clear();
            } else if (s == "if" || s == "while") {
                pendingCode = stack.size();
            } else if (s == "for") {
                pendingCode = stack.size();
                forVars = true;
                loopVars.clear();
            } else if (s == "import") {
                // the path follows
            }
            if (s != "fn" && fn == FnState::AfterFn) fn = FnState::None;
        } else if (t.t == Tk::String) {
            if (is(prev, "import")) {
                std::string p = text(t);
                if (p.size() >= 2) p = p.substr(1, p.size() - (t.closed ? 2 : 1));
                imports.push_back(p);
            }
        } else if (t.t == Tk::Punct) {
            if (s == "(") {
                if (fn == FnState::AfterFn || fn == FnState::AfterName) {
                    push(Fr::Params, i);
                    fn = FnState::InParams;
                } else if (prev >= 0 && toks[static_cast<std::size_t>(prev)].t == Tk::Ident && !isKeyword(text(prev))) {
                    push(Fr::Call, i, text(prev));
                } else if (isP(prev, ")") || isP(prev, "]")) {
                    push(Fr::Call, i);
                } else {
                    push(Fr::Paren, i);
                }
            } else if (s == ")") {
                popTo('(', i);
            } else if (s == "{") {
                if (fn == FnState::AfterParams) {
                    push(Fr::Code, i);
                    stack.back().locals = fnParams;
                    fn = FnState::None;
                } else if (pendingCode == stack.size()) {
                    push(Fr::Code, i);
                    stack.back().locals = loopVars;
                    loopVars.clear();
                    pendingCode = static_cast<std::size_t>(-1);
                } else if (prev >= 0 && toks[static_cast<std::size_t>(prev)].t == Tk::Ident) {
                    const std::string p = text(prev);
                    if (isKeyword(p)) push(Fr::Code, i);
                    else if (schema(p)) push(Fr::Object, i, p);
                    else push(Fr::Object, i, varKind.count(p) ? varKind[p] : std::string());
                } else if (isP(prev, ")")) {
                    push(Fr::Object, i, schema(lastCall) ? lastCall : std::string());
                } else if (isP(prev, "]")) {
                    push(Fr::Object, i);
                } else {
                    push(Fr::Code, i);
                }
            } else if (s == "}") {
                popTo('{', i);
            } else if (s == "[") {
                push(Fr::List, i);
            } else if (s == "]") {
                popTo('[', i);
            } else if (s == ",") {
                endExprScopes();
                if (!stack.empty() && (stack.back().what == Fr::Call || stack.back().what == Fr::Params)) {
                    ++stack.back().arg;
                    stack.back().argTok = i + 1;
                }
            } else if (s == ":") {
                if (!stack.empty() && prev >= 0 && toks[static_cast<std::size_t>(prev)].t == Tk::Ident)
                    stack.back().given.push_back(text(prev));
            } else if (s == "=" || s == "=>") {
                if (fn == FnState::AfterParams) {
                    exprs.push_back({stack.size(), fnParams});
                    fn = FnState::None;
                }
            }
        }
        prev2 = prev;
        prev  = static_cast<int>(i);
    }

    // Read everything; at the first token at or past `stopAt`, picture the
    // state into `snap` first.
    void run(std::size_t stopAt, Snapshot* snap) {
        for (std::size_t i = 0; i < toks.size(); ++i) {
            const Tok& t = toks[i];
            if (snap && !snap->taken && t.a >= stopAt) take(*snap);
            if (t.t == Tk::Comment) {
                if (source == 2) {
                    const std::string c = text(t);
                    if (c.rfind("// ---", 0) == 0) docGroup = c.substr(6);
                }
                continue;
            }
            step(i);
        }
        if (snap && !snap->taken) take(*snap);
    }

    void take(Snapshot& s) {
        s.taken  = true;
        s.stack  = stack;
        s.exprs  = exprs;
        s.prev   = prev;
        s.prev2  = prev2;
        s.naming = letName || fn == FnState::AfterFn || forVars ||
                   (!stack.empty() && stack.back().what == Fr::Params &&
                    (static_cast<int>(stack.back().tok) == prev || isP(prev, ",")));
    }
};

// The prelude's definitions, read once from its source.
const std::vector<Def>& preludeDefs() {
    static const std::vector<Def> defs = [] {
        const std::string src = preludeSource();
        const std::vector<Tok> toks = lex(src);
        Scanner sc(src, toks, 2);
        sc.run(src.size() + 1, nullptr);
        return sc.globals;
    }();
    return defs;
}

// An imported file's top-level definitions, kept until the file changes.
const std::vector<Def>& importDefs(const fs::path& file) {
    struct Entry { fs::file_time_type time; std::vector<Def> defs; };
    static std::map<std::string, Entry> cache;
    std::error_code ec;
    const auto t = fs::last_write_time(file, ec);
    Entry& e = cache[file.string()];
    if (ec || e.time != t || e.defs.empty()) {
        e.time = t;
        std::ifstream f(file, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        const std::string src = ss.str();
        const std::vector<Tok> toks = lex(src);
        Scanner sc(src, toks, 1);
        sc.run(src.size() + 1, nullptr);
        e.defs = sc.globals;
    }
    return e.defs;
}

fs::path resolveImport(const std::string& path, const Request& r) {
    const fs::path p(path);
    std::error_code ec;
    std::vector<fs::path> dirs;
    dirs.push_back(r.file.empty() ? fs::current_path(ec) : r.file.parent_path());
    for (const fs::path& d : r.libraryDirs) dirs.push_back(d);
    for (const fs::path& d : dirs) {
        for (const fs::path& q : {d / p, d / (path + ".strahl")})
            if (fs::is_regular_file(q, ec)) return q;
    }
    return {};
}

// --- Ranking ----------------------------------------------------------------------

// 3: the label starts with the word; 2: a part of it after _ does; 1: the
// word's letters appear in it in order, starting with its first. 0: no.
int matchScore(const std::string& label, const std::string& word) {
    if (word.empty()) return 1;
    const std::string l = lower(label), w = lower(word);
    if (l.rfind(w, 0) == 0) return 3;
    for (std::size_t j = 1; j < l.size(); ++j)
        if (l[j - 1] == '_' && l.compare(j, w.size(), w) == 0) return 2;
    if (l[0] != w[0]) return 0;
    std::size_t k = 0;
    for (char c : l)
        if (k < w.size() && c == w[k]) ++k;
    return k == w.size() ? 1 : 0;
}

// What a value that comes after `prop:` (or as a named argument) should be.
std::string wantFor(const std::string& prop) {
    static const char* colour[] = {"color", "emission", "zenith", "horizon", "ground", "medium_color",
                                   "sun_color", "background", "a", "b", "c", "base", "vein", "light",
                                   "dark", "brick", "mortar", "grout"};
    static const char* boolean[] = {"open", "glass", "visible", "smooth", "procedural", "shadow", "additive"};
    if (prop == "material") return "material";
    if (prop == "bump") return "bump";
    if (prop == "function") return "function";
    if (prop == "tonemap") return "tonemap";
    if (prop == "focus") return "focus";
    for (const char* c : colour)
        if (prop == c) return "colour";
    for (const char* b : boolean)
        if (prop == b) return "bool";
    return "";
}

struct Builder {
    const Request& req;
    std::string    after;     // the rest of the caret's line
    std::vector<Item> items;
    std::map<std::string, std::size_t> seen;   // label -> index: the first (highest) source wins

    char nextChar() const {
        for (char c : after)
            if (c != ' ' && c != '\t') return c;
        return '\0';
    }

    void add(Item it) {
        const auto f = seen.find(it.label);
        if (f != seen.end()) {
            // The same name again (a script fn shadowing the prelude's): keep
            // the first description, the better rank.
            items[f->second].prio = std::max(items[f->second].prio, it.prio);
            return;
        }
        seen[it.label] = items.size();
        items.push_back(std::move(it));
    }

    void keyword(const char* k, int prio) {
        Item it;
        it.label = k;
        it.kind = Kind::Keyword;
        it.detail = "keyword";
        const std::string s = k;
        const bool plain = s == "true" || s == "false" || s == "nil" || s == "break" || s == "continue";
        if (s == "import") { it.insert = "import \"\""; it.caret = 8; }
        else it.insert = plain ? s : s + " ";
        it.prio = prio;
        add(it);
    }

    void object(const KindSchema& k, int prio) {
        Item it;
        it.label = k.name;
        it.kind = Kind::Object;
        const char nc = nextChar();
        if (nc == '{' || nc == '(') it.insert = k.name;
        else { it.insert = k.name + " {  }"; it.caret = static_cast<int>(k.name.size()) + 3; }
        std::string props;
        for (const std::string& p : k.positional) props += (props.empty() ? "" : ", ") + p;
        it.detail = "{ " + props + (props.empty() ? "" : ", ") + "... }";
        it.doc = kindDoc(k.name);
        it.prio = prio;
        add(it);
    }

    void property(const std::string& kind, const std::string& name, bool positional, int prio) {
        Item it;
        it.label = name;
        it.kind = Kind::Property;
        it.insert = nextChar() == ':' ? name : name + ": ";
        it.detail = positional ? "property (positional)" : "property";
        it.doc = propDoc(kind, name);
        it.prio = prio;
        add(it);
    }

    void namedArg(const Param& p, int prio) {
        Item it;
        it.label = p.name;
        it.kind = Kind::Parameter;
        it.insert = nextChar() == ':' ? p.name : p.name + ": ";
        it.detail = p.def.empty() ? "parameter" : "= " + p.def;
        it.prio = prio;
        add(it);
    }

    void function(const std::string& name, const std::vector<Param>& ps, const std::string& doc, int prio) {
        Item it;
        it.label = name;
        it.kind = Kind::Function;
        if (nextChar() == '(') it.insert = name;
        else {
            it.insert = name + "()";
            if (!ps.empty()) it.caret = static_cast<int>(name.size()) + 1;
        }
        it.detail = signatureOf("", ps);
        it.doc = doc;
        it.prio = prio;
        add(it);
    }

    void value(const std::string& name, const std::string& detail, const std::string& doc, Kind k, int prio) {
        Item it;
        it.label = name;
        it.kind = k;
        it.insert = name;
        it.detail = detail;
        it.doc = doc;
        it.prio = prio;
        add(it);
    }

    void def(const Def& d, int prio) {
        if (d.kind == Kind::Function) function(d.name, d.params, d.doc, prio);
        else value(d.name, d.detail, d.doc, d.kind, prio);
    }

    void snippet(const char* label, const char* insert, const char* doc, int prio) {
        Item it;
        it.label = label;
        it.kind = Kind::Snippet;
        it.insert = insert;
        it.detail = "snippet";
        it.doc = doc;
        it.prio = prio;
        add(it);
    }
};

std::vector<Param> builtinParams(const Builtin& b) {
    return splitParams(b.params);
}

// Where the caret stands, from the scan's picture.
enum class Pos { None, Statement, Item, PropValue, ArgStart, ArgValue, Expr };

} // namespace

std::string documentation(const std::string& name) {
    for (const Builtin& b : kBuiltins)
        if (name == b.name) return b.doc;
    if (const std::string d = kindDoc(name); !d.empty()) return d;
    if (const std::string d = propDoc("", name); !d.empty()) return d;
    for (const Def& d : preludeDefs())
        if (d.name == name) return !d.doc.empty() ? d.doc : d.detail;
    for (const Constant& c : kConstants)
        if (name == c.name) return c.doc;
    return {};
}

Result complete(const Request& r) {
    Result out;
    const std::string& text = r.text;
    const std::size_t caret = std::min(r.caret, text.size());
    const std::vector<Tok> toks = lex(text);

    // In a comment, or a string: nothing -- except an import's file name.
    for (std::size_t i = 0; i < toks.size(); ++i) {
        const Tok& t = toks[i];
        if (t.a >= caret) break;
        if (t.t == Tk::Comment && caret <= t.b) return out;
        if (t.t == Tk::String && (caret < t.b || (!t.closed && caret == t.b))) {
            std::size_t p = i;
            while (p > 0 && toks[p - 1].t == Tk::Comment) --p;
            if (p == 0 || text.compare(toks[p - 1].a, toks[p - 1].b - toks[p - 1].a, "import") != 0) return out;
            out.inString = true;
            out.start = t.a + 1;
            out.prefix = text.substr(out.start, caret - out.start);
            std::vector<fs::path> dirs;
            std::error_code ec;
            dirs.push_back(r.file.empty() ? fs::current_path(ec) : r.file.parent_path());
            for (const fs::path& d : r.libraryDirs) dirs.push_back(d);
            std::map<std::string, bool> seenFile;
            for (const fs::path& d : dirs) {
                for (const auto& e : fs::directory_iterator(d, ec)) {
                    if (e.path().extension() != ".strahl" || e.path() == r.file) continue;
                    const std::string stem = e.path().stem().string();
                    if (seenFile[stem] || matchScore(stem, out.prefix) == 0) continue;
                    seenFile[stem] = true;
                    Item it;
                    it.label = stem;
                    it.insert = stem;
                    it.kind = Kind::File;
                    it.detail = d.filename().string();
                    it.doc = e.path().string();
                    out.items.push_back(it);
                }
            }
            std::sort(out.items.begin(), out.items.end(),
                      [](const Item& a, const Item& b) { return a.label < b.label; });
            return out;
        }
    }

    // The word being typed: [start, caret).
    std::size_t start = caret;
    while (start > 0 && identChar(text[start - 1])) --start;
    out.start = start;
    out.prefix = text.substr(start, caret - start);
    const bool numberish = !out.prefix.empty() && std::isdigit(static_cast<unsigned char>(out.prefix[0]));
    const bool colour = start > 0 && text[start - 1] == '#';
    const bool member = start > 0 && text[start - 1] == '.';

    Scanner sc(text, toks, 0);
    Snapshot snap;
    sc.run(start, &snap);

    // The line after the caret, for "is there already a ( or : there".
    std::size_t eol = caret;
    while (eol < text.size() && text[eol] != '\n') ++eol;
    Builder b{r, text.substr(caret, eol - caret), {}, {}};

    const Frame* top = snap.stack.empty() ? nullptr : &snap.stack.back();
    const auto tokText = [&](int i) { return i < 0 ? std::string() : text.substr(toks[i].a, toks[i].b - toks[i].a); };
    const std::string P = tokText(snap.prev), P2 = tokText(snap.prev2);
    const bool pIsIdent2 = snap.prev2 >= 0 && toks[static_cast<std::size_t>(snap.prev2)].t == Tk::Ident;
    const bool newline = snap.prev < 0 || text.find('\n', toks[static_cast<std::size_t>(snap.prev)].b) < start;
    const bool opensTop = top && snap.prev == static_cast<int>(top->tok);
    const bool ends = !Scanner::continues(P);

    Pos pos = Pos::Expr;
    std::string prop;   // PropValue / ArgValue: the name before the colon
    if (numberish || colour || member || snap.naming) pos = Pos::None;
    else if (!top || top->what == Fr::Code) {
        if (snap.prev < 0 || opensTop || P == "{" || P == "}" || P == ";" || (newline && ends)) pos = Pos::Statement;
    } else if (top->what == Fr::Object) {
        if (P == ":" && pIsIdent2 && snap.prev2 > static_cast<int>(top->tok)) { pos = Pos::PropValue; prop = P2; }
        else if (opensTop || P == "," || P == ";" || (newline && ends)) pos = Pos::Item;
    } else if (top->what == Fr::Call) {
        if (P == ":" && pIsIdent2 && snap.prev2 == static_cast<int>(top->argTok)) { pos = Pos::ArgValue; prop = P2; }
        else if (opensTop || P == ",") pos = Pos::ArgStart;
    } else if (top->what == Fr::Params) {
        pos = (opensTop || P == ",") ? Pos::None : Pos::Expr;
    }

    // Every definition this place can see, nearest first.
    std::vector<Def> visible;
    for (auto f = snap.stack.rbegin(); f != snap.stack.rend(); ++f)
        for (const Def& d : f->locals) visible.push_back(d);
    for (auto e = snap.exprs.rbegin(); e != snap.exprs.rend(); ++e)
        for (const Def& d : e->params) visible.push_back(d);
    std::vector<Def> scriptGlobals;
    for (const Def& d : sc.globals)
        if (d.name != out.prefix || caret != start) scriptGlobals.push_back(d);
    std::vector<Def> imported;
    for (const std::string& im : sc.imports) {
        const fs::path p = resolveImport(im, r);
        if (!p.empty())
            for (const Def& d : importDefs(p)) imported.push_back(d);
    }
    const std::vector<Def>& prelude = preludeDefs();
    // A let's flavour from what made it: gold = metal(...) is a material.
    const auto flavorOf = [&](const Def& d) -> std::string {
        if (d.flavor.rfind("call:", 0) != 0) return d.flavor;
        const std::string head = d.flavor.substr(5);
        const std::vector<Def>* from[] = {&imported, &prelude};
        for (const std::vector<Def>* v : from)
            for (const Def& p : *v)
                if (p.name == head && p.flavor.rfind("call:", 0) != 0) return p.flavor;
        return {};
    };

    // --- Gather, ranked for the place ---------------------------------------
    const std::string want = (pos == Pos::PropValue || pos == Pos::ArgValue) ? wantFor(prop) : "";
    const auto valuePrio = [&](const Def& d, int base) {
        const std::string f = flavorOf(d);
        if (!want.empty() && f == want) return 60;
        if (want == "material" && f == "pattern") return 40;
        if (want == "material" && f == "colour") return 35;
        if (want == "colour" && f == "pattern") return 50;
        if (pos == Pos::Item && (f == "shape" || f == "object")) return std::max(base, 42);
        if (pos == Pos::Statement && (f == "shape" || f == "object") && d.kind == Kind::Function) return std::max(base, 33);
        return base;
    };

    if (pos != Pos::None) {
        // The place's own names first.
        if (pos == Pos::Item) {
            const KindSchema* k = schema(top->name);
            const auto setAlready = [&](const std::string& n) {
                return std::find(top->given.begin(), top->given.end(), n) != top->given.end();
            };
            if (k) {
                for (const std::string& p : k->props) {
                    const bool posi = std::find(k->positional.begin(), k->positional.end(), p) != k->positional.end();
                    b.property(k->name, p, posi, setAlready(p) ? 30 : posi ? 62 : 60);
                }
                if (k->shape) b.property(k->name, "material", false, setAlready("material") ? 30 : 58);
            } else {
                // A block on something whose kind is not known here (a
                // variable, a function's result): what any object takes.
                b.property("", "material", false, 50);
            }
            for (const char* t : {"translate", "rotate", "scale", "matrix"})
                b.property(k ? k->name : "", t, false, 55);
            if (!k)
                for (const std::string& p : propertyNames()) b.property("", p, false, 35);
            const bool container = !k || isCsgOrGroup(k->name);
            for (const KindSchema& o : schemas())
                b.object(o, container ? (o.shape ? 45 : 8) : (o.shape ? 25 : 5));
        } else if (pos == Pos::ArgStart) {
            std::vector<Param> ps;
            std::string kind;
            if (const KindSchema* k = schema(top->name)) {
                kind = k->name;
                for (const std::string& p : k->props) b.property(kind, p, false, 58);
                if (k->shape) b.property(kind, "material", false, 57);
            } else {
                for (const Def& d : visible) if (d.name == top->name && d.kind == Kind::Function) ps = d.params;
                if (ps.empty()) for (const Def& d : scriptGlobals) if (d.name == top->name) ps = d.params;
                if (ps.empty()) for (const Def& d : imported) if (d.name == top->name) ps = d.params;
                if (ps.empty()) for (const Def& d : prelude) if (d.name == top->name) ps = d.params;
                if (ps.empty())
                    for (const Builtin& bi : kBuiltins)
                        if (top->name == bi.name) ps = builtinParams(bi);
            }
            for (const Param& p : ps)
                if (p.name.find("…") == std::string::npos &&
                    std::find(top->given.begin(), top->given.end(), p.name) == top->given.end())
                    b.namedArg(p, 60);
        } else if (pos == Pos::PropValue || pos == Pos::ArgValue) {
            if (want == "bool") { b.keyword("true", 60); b.keyword("false", 60); }
            if (want == "tonemap")
                for (const char* s : {"\"aces\"", "\"agx\"", "\"neutral\""}) b.value(s, "tone mapping", "", Kind::Value, 60);
            if (want == "focus") b.value("\"auto\"", "focus", "Focus on what the centre of the picture sees.", Kind::Value, 60);
            if (want == "material")
                if (const KindSchema* m = schema("material")) b.object(*m, 58);
            const bool fnValued = want == "function" || want == "bump" || want == "colour" ||
                                  prop == "roughness" || prop == "metallic" || prop == "opacity" ||
                                  prop == "translucency" || prop == "emission";
            if (fnValued)
                b.snippet("fn(p) =>", "fn(p) => ", "A function of the point, compiled for the tracer.",
                          want == "function" || want == "bump" ? 60 : 30);
        }

        // Then everything a value can be.
        const bool statement = pos == Pos::Statement;
        for (const Def& d : visible) b.def(d, valuePrio(d, statement ? 20 : 36));
        for (const Def& d : scriptGlobals) b.def(d, valuePrio(d, statement && d.kind == Kind::Function ? 32 : 30));
        for (const Def& d : imported) b.def(d, valuePrio(d, statement && d.kind == Kind::Function ? 31 : 25));
        for (const Def& d : prelude) b.def(d, valuePrio(d, 20));
        for (const Builtin& bi : kBuiltins) b.function(bi.name, builtinParams(bi), bi.doc, 18);
        for (const Constant& c : kConstants) b.value(c.name, c.detail, c.doc, Kind::Value, 18);
        if (statement) {
            for (const char* k : kStatementKeywords) b.keyword(k, 40);
            for (const KindSchema& o : schemas()) b.object(o, 38);
        } else {
            for (const KindSchema& o : schemas()) b.object(o, want == "material" ? 10 : 15);
        }
        for (const char* k : kExprKeywords) b.keyword(k, 12);
        if (!statement && pos != Pos::Item && pos != Pos::ArgStart)
            for (const char* k : kInfixKeywords) b.keyword(k, 10);
    }

    // --- Filter and order -------------------------------------------------------
    struct Ranked { std::size_t i; int score; };
    std::vector<Ranked> ranked;
    if (r.manual || !out.prefix.empty())
        for (std::size_t i = 0; i < b.items.size(); ++i) {
            const int s = matchScore(b.items[i].label, out.prefix);
            if (s > 0) ranked.push_back({i, s});
        }
    std::stable_sort(ranked.begin(), ranked.end(), [&](const Ranked& x, const Ranked& y) {
        if (x.score != y.score) return x.score > y.score;
        const Item &a = b.items[x.i], &c = b.items[y.i];
        if (a.prio != c.prio) return a.prio > c.prio;
        const bool ea = a.label.compare(0, out.prefix.size(), out.prefix) == 0;   // case too
        const bool ec = c.label.compare(0, out.prefix.size(), out.prefix) == 0;
        if (ea != ec) return ea;
        return a.label < c.label;
    });
    for (const Ranked& x : ranked) out.items.push_back(b.items[x.i]);
    // Typed out already: nothing left to complete (and Enter stays a new line).
    if (!r.manual && !out.items.empty() && out.items.front().label == out.prefix)
        out.items.erase(out.items.begin());
    if (!r.manual && !out.items.empty() && out.items.front().label == out.prefix) out.items.clear();
    if (out.items.size() > 200) out.items.resize(200);

    // --- The call the caret is in ------------------------------------------------
    if (top && top->what == Fr::Call && !top->name.empty()) {
        Signature& sig = out.signature;
        std::vector<Param> ps;
        std::string doc;
        bool found = false;
        const auto fromDefs = [&](const std::vector<Def>& v) {
            for (const Def& d : v)
                if (!found && d.name == top->name && d.kind == Kind::Function) { ps = d.params; doc = d.doc; found = true; }
        };
        fromDefs(visible);
        fromDefs(scriptGlobals);
        fromDefs(imported);
        fromDefs(prelude);
        for (const Builtin& bi : kBuiltins)
            if (!found && top->name == bi.name) { ps = builtinParams(bi); doc = bi.doc; found = true; }
        if (!found)
            if (const KindSchema* k = schema(top->name)) {
                for (const std::string& p : k->positional) ps.push_back({p, ""});
                ps.push_back({"…properties", ""});
                doc = kindDoc(k->name);
                found = true;
            }
        if (found) {
            sig.label = signatureOf(top->name, ps, &sig.params);
            sig.doc = doc;
            // The argument the caret is in: by name when it has one, else by place.
            int active = top->arg;
            const std::size_t a = top->argTok;
            if (a + 1 < toks.size() && toks[a].t == Tk::Ident && toks[a + 1].b <= caret &&
                toks[a + 1].t == Tk::Punct && text.compare(toks[a + 1].a, 1, ":") == 0) {
                const std::string n = text.substr(toks[a].a, toks[a].b - toks[a].a);
                active = -1;
                for (std::size_t i = 0; i < ps.size(); ++i)
                    if (ps[i].name == n) active = static_cast<int>(i);
            } else if (active >= static_cast<int>(ps.size())) {
                active = !ps.empty() && ps.back().name.find("…") != std::string::npos
                             ? static_cast<int>(ps.size()) - 1 : -1;
            }
            sig.active = active;
            out.hasSignature = true;
        }
    }
    return out;
}

// --- Writing the camera back --------------------------------------------------------

namespace {

std::string number(double v, int decimals = 3) {
    char b[64];
    std::snprintf(b, sizeof b, "%.*f", decimals, v);
    std::string s = b;
    if (s.find('.') != std::string::npos) {
        while (s.back() == '0') s.pop_back();
        if (s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

std::string vector3(const glm::dvec3& v) {
    return "[" + number(v.x) + ", " + number(v.y) + ", " + number(v.z) + "]";
}

// One value in a block or a call: `name: value`, or a value on its own.
struct Entry {
    std::string name;            // "" unnamed
    std::size_t first = 0;       // its first token (the name, when it has one)
    std::size_t value = 0;       // the value's first token
    std::size_t last = 0;        // its last token
};

} // namespace

CameraEdit setCamera(const std::string& text, int line, int col, const glm::dvec3& position,
                     const glm::dvec3& target, double fov) {
    CameraEdit out;
    out.text = text;
    const std::vector<Tok> all = lex(text);
    std::vector<std::size_t> sig;   // the significant tokens, as indices into `all`
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i].t != Tk::Comment) sig.push_back(i);
    const auto tx = [&](std::size_t k) { const Tok& t = all[sig[k]]; return text.substr(t.a, t.b - t.a); };
    const auto isP = [&](std::size_t k, const char* p) { return k < sig.size() && all[sig[k]].t == Tk::Punct && tx(k) == p; };

    std::string fields = "position: " + vector3(position) + ", look_at: " + vector3(target);
    if (fov > 0.0) fields += ", fov: " + number(fov, 1);

    if (line <= 0) {
        // No camera: one after render { }, or above the first statement.
        int depth = 0;
        for (std::size_t k = 0; k < sig.size(); ++k) {
            const std::string t = tx(k);
            if (depth == 0 && t == "render" && isP(k + 1, "{")) {
                int d = 0;
                for (std::size_t j = k + 1; j < sig.size(); ++j) {
                    if (isP(j, "{")) ++d;
                    else if (isP(j, "}") && --d == 0) {
                        std::size_t e = all[sig[j]].b;
                        while (e < text.size() && text[e] != '\n') ++e;
                        out.text.insert(e, "\ncamera { " + fields + " }");
                        out.ok = true;
                        return out;
                    }
                }
            }
            if (t == "(" || t == "[" || t == "{") ++depth;
            if (t == ")" || t == "]" || t == "}") --depth;
        }
        std::size_t at = sig.empty() ? text.size() : all[sig[0]].a;
        while (at > 0 && text[at - 1] != '\n') --at;
        out.text.insert(at, "camera { " + fields + " }\n");
        out.ok = true;
        return out;
    }

    // The block, where the build last found it: its { or ( (where the
    // language puts an object), or the word before it.
    std::size_t lineStart = 0;
    for (int l = 1; l < line && lineStart <= text.size(); ++l) {
        const std::size_t nl = text.find('\n', lineStart);
        if (nl == std::string::npos) { lineStart = text.size() + 1; break; }
        lineStart = nl + 1;
    }
    const std::size_t at = lineStart + static_cast<std::size_t>(std::max(0, col - 1));
    std::size_t head = sig.size();
    for (std::size_t k = 0; k < sig.size(); ++k)
        if (all[sig[k]].a == at) { head = k; break; }
    if (head < sig.size() && (isP(head, "{") || isP(head, "("))) {
        // Back from the bracket to the name: camera { }, camera(...), and the
        // block of camera(...) { }.
        std::size_t b = head;
        if (b > 0 && isP(b - 1, ")")) {
            int d = 0;
            for (std::size_t j = b - 1;; --j) {
                if (isP(j, ")")) ++d;
                else if (isP(j, "(") && --d == 0) { b = j; break; }
                if (j == 0) break;
            }
        }
        head = b > 0 ? b - 1 : sig.size();
    }
    const bool named = head < sig.size() && all[sig[head]].t == Tk::Ident && !isKeyword(tx(head));
    if (!named || !(isP(head + 1, "{") || isP(head + 1, "(")) || (schema(tx(head)) && tx(head) != "camera")) {
        out.why = "the script changed since it last ran";
        return out;
    }

    // Its brackets: camera(...), camera { }, or camera(...) { }.
    struct Group { std::size_t open, close; std::vector<Entry> entries; };
    std::vector<Group> groups;
    std::size_t k = head + 1;
    while (k < sig.size() && (isP(k, "{") || isP(k, "("))) {
        const std::string o = tx(k), c = o == "{" ? "}" : ")";
        int d = 0;
        std::size_t j = k;
        for (; j < sig.size(); ++j) {
            const std::string t = tx(j);
            if (t == "(" || t == "[" || t == "{") ++d;
            if ((t == ")" || t == "]" || t == "}") && --d == 0) break;
        }
        if (j >= sig.size() || tx(j) != c) { out.why = "the camera's " + o + " is never closed"; return out; }
        Group g{k, j, {}};
        // The entries: split at commas, semicolons and line ends at the top.
        std::size_t start = k + 1;
        int dd = 0;
        for (std::size_t m = k + 1; m <= j; ++m) {
            const std::string t = m < j ? tx(m) : std::string(",");
            const bool lineEnd = m < j && dd == 0 && all[sig[m]].nl && m > start &&
                                 !Scanner::continues(tx(m - 1));
            if (m < j && (t == "(" || t == "[" || t == "{")) ++dd;
            if (m < j && (t == ")" || t == "]" || t == "}")) --dd;
            const bool sep = m == j || (dd == 0 && (t == "," || t == ";"));
            if (sep || lineEnd) {
                const std::size_t end = m;   // exclusive
                if (end > start) {
                    Entry e;
                    e.first = start;
                    e.last = end - 1;
                    if (end - start >= 3 && all[sig[start]].t == Tk::Ident && isP(start + 1, ":")) {
                        e.name = tx(start);
                        e.value = start + 2;
                    } else {
                        e.value = start;
                    }
                    g.entries.push_back(e);
                }
                start = sep ? m + 1 : m;
            }
        }
        groups.push_back(std::move(g));
        k = j + 1;
    }

    // Each value by name; an unnamed one by the kind's order.
    const KindSchema* cam = schema("camera");
    const auto find = [&](const std::string& name) -> const Entry* {
        const Entry* found = nullptr;
        int unnamed = 0;
        for (const Group& g : groups)
            for (const Entry& e : g.entries) {
                if (e.name.empty()) {
                    if (cam && unnamed < static_cast<int>(cam->positional.size()) &&
                        cam->positional[static_cast<std::size_t>(unnamed)] == name)
                        found = &e;
                    ++unnamed;
                } else if (e.name == name) {
                    found = &e;   // the later one wins, as in the language
                }
            }
        return found;
    };
    // Numbers only: [1, -2.5, 3], or one number.
    const auto plain = [&](const Entry& e, bool vec) {
        std::size_t m = e.value;
        const auto num = [&]() {
            if (isP(m, "-")) ++m;
            if (m > e.last || all[sig[m]].t != Tk::Number) return false;
            ++m;
            return true;
        };
        if (!vec) return num() && m == e.last + 1;
        if (!isP(m, "[")) return false;
        ++m;
        for (int i = 0; i < 3; ++i) {
            if (!num()) return false;
            if (i < 2 && !isP(m++, ",")) return false;
        }
        return isP(m, "]") && m == e.last;
    };

    struct Edit { std::size_t a, b; std::string with; };
    std::vector<Edit> edits;
    std::vector<std::string> added;
    const auto set = [&](const std::string& name, const std::string& value, bool vec) {
        const Entry* e = find(name);
        if (!e) { added.push_back(name + ": " + value); return true; }
        if (!plain(*e, vec)) {
            out.why = name + " is computed in the script (" + text.substr(all[sig[e->value]].a, all[sig[e->last]].b - all[sig[e->value]].a) + ")";
            return false;
        }
        edits.push_back({all[sig[e->value]].a, all[sig[e->last]].b, value});
        return true;
    };
    if (!set("position", vector3(position), true)) return out;
    if (find("direction")) {
        const glm::dvec3 d = target - position;
        if (!set("direction", vector3(glm::length(d) > 1e-12 ? glm::normalize(d) : glm::dvec3(0, 0, -1)), true)) return out;
    } else if (!set("look_at", vector3(target), true)) {
        return out;
    }
    if (fov > 0.0 && !set("fov", number(fov, 1), false)) return out;

    if (!added.empty()) {
        std::string list;
        for (const std::string& a : added) list += (list.empty() ? "" : ", ") + a;
        const Group& g = groups.back();
        const bool block = tx(g.open) == "{";
        if (g.entries.empty())
            edits.push_back({all[sig[g.open]].b, all[sig[g.close]].a, block ? " " + list + " " : list});
        else {
            const std::size_t e = all[sig[g.entries.back().last]].b;
            edits.push_back({e, e, ", " + list});
        }
    }
    std::sort(edits.begin(), edits.end(), [](const Edit& x, const Edit& y) { return x.a > y.a; });
    for (const Edit& e : edits) out.text.replace(e.a, e.b - e.a, e.with);
    out.ok = true;
    return out;
}

} // namespace strahl::complete
