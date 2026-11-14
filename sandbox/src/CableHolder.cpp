#include "CableHolder.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

#include <nlohmann/json.hpp>

#include "Lettering.hpp"
#include "PolyFill.hpp"
#include "SolidBuild.hpp"

using fitzel::AssetId;

namespace cableholder {

namespace {

using solidbuild::Builder;
using solidbuild::DV2;
using solidbuild::DV3;
using solidbuild::kPi;

constexpr int    kFil   = 4;     // segments a fillet
constexpr int    kSeat  = 20;    // segments a seat's round
constexpr int    kBend  = 12;    // segments a hook's bend
constexpr double kRelief = 0.6;  // how far a label stands out (mm)

// --- Outlines in a plane --------------------------------------------------------------

// Points round `c` from angle a0 to a1 (radians, either way), both ends included.
void arcTo(std::vector<DV2>& out, DV2 c, double r, double a0, double a1, int n) {
    for (int i = 0; i <= n; ++i) {
        const double a = a0 + (a1 - a0) * static_cast<double>(i) / n;
        out.push_back(c + r * DV2(std::cos(a), std::sin(a)));
    }
}

// A polygon with its corners rounded, each by its own radius (0: sharp); a
// corner turning either way is rounded the right way, a radius too big for the
// edges beside it is taken down to fit, a corner on a straight run stays a
// point (an outline cut where another one's corner lies on it).
std::vector<DV2> filleted(const std::vector<std::pair<DV2, double>>& corners) {
    std::vector<DV2> out;
    const std::size_t n = corners.size();
    for (std::size_t i = 0; i < n; ++i) {
        const DV2 A = corners[(i + n - 1) % n].first, P = corners[i].first, B = corners[(i + 1) % n].first;
        double r = corners[i].second;
        const DV2 a = glm::normalize(P - A), b = glm::normalize(B - P);
        const double cr = a.x * b.y - a.y * b.x;
        if (r <= 0.0 || std::abs(cr) < 1e-9) {
            out.push_back(P);
            continue;
        }
        const double theta = std::acos(std::clamp(glm::dot(-a, b), -1.0, 1.0));   // the corner's angle
        double t = r / std::tan(0.5 * theta);
        const double room = 0.5 * std::min(glm::length(P - A), glm::length(B - P));
        if (t > room) {
            t = room;
            r = t * std::tan(0.5 * theta);
        }
        const DV2 t1 = P - a * t, t2 = P + b * t;
        const DV2 left(-a.y, a.x);
        const DV2 c = cr > 0.0 ? t1 + left * r : t1 - left * r;
        const double a0 = std::atan2(t1.y - c.y, t1.x - c.x);
        double a1 = std::atan2(t2.y - c.y, t2.x - c.x);
        if (cr > 0.0 && a1 < a0) a1 += 2.0 * kPi;
        if (cr < 0.0 && a1 > a0) a1 -= 2.0 * kPi;
        const int seg = std::max(2, static_cast<int>(std::lround(kFil * std::abs(a1 - a0) / (0.5 * kPi))));
        const std::size_t first = out.size();
        arcTo(out, c, r, a0, a1, seg);
        // The ends exactly on the edges, so a straight edge stays exactly level.
        out[first] = t1;
        out.back() = t2;
    }
    return out;
}

// A keyhole seat cut into an edge, in the edge's own frame: s along it, d
// inward from its line.
struct Seat {
    double s  = 0.0;   // its middle along the edge
    double dc = 0.0;   // the round's centre, inward from the edge's line
    double r  = 0.0;   // the round's radius
    double hn = 0.0;   // half the neck (r and more: an open U)
    double re = 1.0;   // the mouth's rounding
    double rl = 0.5;   // the lip's rounding, where the neck widens into the round
};

// The seat, its edge drawn in by `c` (the round and the neck widened by as
// much): mouth, neck, lip, the round the long way through its deepest point,
// lip, neck, mouth. Always as many points, however it is drawn in -- an
// outline of one profile at every level of a rounded edge.
void seat(std::vector<DV2>& out, const Seat& st, double c, const std::function<DV2(double, double)>& at) {
    const double R = st.r + c;
    const double N = std::min(st.hn + c, R);
    const double re = st.re, rl = st.rl;
    std::vector<DV2> loc;
    arcTo(loc, {st.s - N - re, c + re}, re, -0.5 * kPi, 0.0, kFil);
    const double k  = std::sqrt(std::max(0.0, (R + rl) * (R + rl) - (N + rl) * (N + rl)));
    const DV2    C(st.s, st.dc);
    const double aF = std::atan2(k, N + rl);   // from a lip's centre towards the round's
    arcTo(loc, {st.s - N - rl, st.dc - k}, rl, 0.0, aF, kFil);
    const double a1 = std::atan2(-k, -(N + rl));
    double a2 = std::atan2(-k, N + rl);
    if (a2 >= a1) a2 -= 2.0 * kPi;
    arcTo(loc, C, R, a1, a2, kSeat);
    arcTo(loc, {st.s + N + rl, st.dc - k}, rl, kPi - aF, kPi, kFil);
    arcTo(loc, {st.s + N + re, c + re}, re, kPi, 1.5 * kPi, kFil);
    for (const DV2& p : loc) out.push_back(at(p.x, p.y));
}

// How deep under its edge a seat's centre must lie for the neck to keep a
// little straight between mouth and lip, drawn in by up to `cMax`.
double seatDepth(double r, double hn, double re, double rl, double cMax) {
    double need = 0.0;
    for (const double c : {0.0, cMax}) {
        const double R = r + c, N = std::min(hn + c, R);
        need = std::max(need, c + re + 0.3 + std::sqrt(std::max(0.0, (R + rl) * (R + rl) - (N + rl) * (N + rl))));
    }
    return need;
}

// The levels of an edge rounded by `rho` at both ends of [0, len]: where they
// lie, and how far the outline is drawn in at each.
void roundedLevels(double len, double rho, std::vector<double>& at, std::vector<double>& in) {
    at.clear();
    in.clear();
    if (rho <= 1e-3) {
        at = {0.0, len};
        in = {0.0, 0.0};
        return;
    }
    const int m = 4;
    for (int j = 0; j <= m; ++j) {   // the front: drawn in by rho at 0, by nothing at rho
        const double psi = 0.5 * kPi * (1.0 - static_cast<double>(j) / m);
        at.push_back(rho * (1.0 - std::sin(psi)));
        in.push_back(rho * (1.0 - std::cos(psi)));
    }
    for (int j = m; j >= 0; --j) {
        const double psi = 0.5 * kPi * (1.0 - static_cast<double>(j) / m);
        at.push_back(len - rho * (1.0 - std::sin(psi)));
        in.push_back(rho * (1.0 - std::cos(psi)));
    }
}

// --- Labels --------------------------------------------------------------------------

// A block of text raised from a face as closed solids, one a glyph: `place`
// takes a point of the block (mm, y up) and a height above the face to the
// world, keeping the block's turn (counter-clockwise seen from where it stands
// out to). A glyph whose outlines overlap is built of bands drawn apart by a
// hair, as on the cards (CardGen).
void raise(Builder& b, const lettering::Block& blk, const std::function<DV3(DV2, double)>& place) {
    auto prism = [&](const std::vector<DV2>& pts, const std::vector<glm::ivec3>& tris,
                     const std::vector<std::vector<int>>& loops) {
        std::vector<int> lo, hi;
        for (const DV2& p : pts) {
            lo.push_back(b.vid(place(p, 0.0)));
            hi.push_back(b.vid(place(p, kRelief)));
        }
        for (const glm::ivec3& t : tris) {
            b.face({hi[static_cast<std::size_t>(t.x)], hi[static_cast<std::size_t>(t.y)], hi[static_cast<std::size_t>(t.z)]});
            b.face({lo[static_cast<std::size_t>(t.x)], lo[static_cast<std::size_t>(t.z)], lo[static_cast<std::size_t>(t.y)]});
        }
        for (const std::vector<int>& l : loops)
            for (std::size_t k = 0; k < l.size(); ++k) {
                const std::size_t p = static_cast<std::size_t>(l[k]), q = static_cast<std::size_t>(l[(k + 1) % l.size()]);
                b.face({lo[p], lo[q], hi[q], hi[p]});
            }
    };
    for (const lettering::Placed& g : blk.glyphs) {
        auto mm = [&](glm::dvec2 u) { return g.at + g.scale * u; };
        if (!g.shape->fill.tris.empty()) {
            std::vector<DV2> pts;
            for (const glm::dvec2& p : g.shape->fill.points) pts.push_back(mm(p));
            prism(pts, g.shape->fill.tris, g.shape->fill.loops);
            continue;
        }
        const double inset = 0.005;
        for (const polyfill::Trap& t : g.shape->traps) {
            const DV2 a = mm({t.xl0, t.y0}), c = mm({t.xr1, t.y1});
            const DV2 bb = mm({t.xr0, t.y0}), d = mm({t.xl1, t.y1});
            if (c.y - a.y <= 4.0 * inset) continue;
            const double lo = a.y + inset, hi = c.y - inset;
            auto along = [](DV2 p, DV2 q, double y) { return p.x + (q.x - p.x) * (y - p.y) / (q.y - p.y); };
            double l0 = along(a, d, lo) + inset, r0 = along(bb, c, lo) - inset;
            double l1 = along(a, d, hi) + inset, r1 = along(bb, c, hi) - inset;
            if (r0 - l0 < 2.0 * inset && r1 - l1 < 2.0 * inset) continue;
            if (r0 < l0) l0 = r0 = 0.5 * (l0 + r0);
            if (r1 < l1) l1 = r1 = 0.5 * (l1 + r1);
            std::vector<DV2> q = {{l0, lo}, {r0, lo}, {r1, hi}, {l1, hi}};
            q.erase(std::unique(q.begin(), q.end(), [](DV2 u, DV2 v) { return glm::length(u - v) < 1e-3; }), q.end());
            if (q.size() > 1 && glm::length(q.front() - q.back()) < 1e-3) q.pop_back();
            if (q.size() < 3) continue;
            std::vector<glm::ivec3> tris;
            std::vector<int> loop;
            for (std::size_t i = 0; i < q.size(); ++i) {
                loop.push_back(static_cast<int>(i));
                if (i >= 2) tris.emplace_back(0, static_cast<int>(i - 1), static_cast<int>(i));
            }
            prism(q, tris, {loop});
        }
    }
}

// A label laid out to fit the box [lo, hi] (mm, y up), centred in it: the
// capitals as tall as asked, smaller where the box is too small. `ok` false
// with no font or nothing to write.
struct Fitted {
    lettering::Block block;
    DV2 origin{0.0};   // where the block's (0, 0) goes
    bool ok = false;
};
Fitted fit(const std::string& text, double capHeight, DV2 lo, DV2 hi) {
    Fitted f;
    if (text.empty()) return f;
    static const std::string path = lettering::defaultFontPath();
    lettering::Font* font = path.empty() ? nullptr : lettering::load(path);
    if (!font) return f;
    lettering::Style st;
    st.capHeight = static_cast<float>(capHeight);
    st.align = lettering::Align::Centre;
    f.block = lettering::layout(*font, text, st);
    const double w = f.block.width, h = f.block.height + f.block.descent;
    if (w <= 0.0 || h <= 0.0) return f;
    const double k = std::min({1.0, (hi.x - lo.x) / w, (hi.y - lo.y) / h});
    if (k < 1.0) {
        st.capHeight = static_cast<float>(capHeight * k * 0.995);
        f.block = lettering::layout(*font, text, st);
    }
    const double W = f.block.width, Ht = f.block.height + f.block.descent;
    f.origin = DV2(0.5 * (lo.x + hi.x) - 0.5 * W, 0.5 * (lo.y + hi.y) + 0.5 * Ht);
    f.ok = st.capHeight >= 0.8f;
    return f;
}

// --- A comb ----------------------------------------------------------------------------

struct Comb {
    std::vector<Seat>   seats;   // in the top edge's frame: from (W, H) going -x, inward -z
    std::vector<double> xc, r;   // the seats' centres along x, their radii
    double W = 0.0, H = 0.0;     // the bar's width and its top
    double z0 = 0.0;             // its bottom
    double base = 0.0;
    double Rc = 2.5;             // the top corners' rounding
    double rho = 0.0;            // the edges' rounding, front and back
    bool   flanges = false;
    double Lf = 0.0, tf = 0.0, Rf = 2.0, rfi = 1.0;   // flange length, thickness, roundings
};

Comb combLayout(const Params& p, double z0, bool flanges) {
    Comb L;
    L.z0 = z0;
    L.base = p.base;
    L.flanges = flanges;
    const std::vector<float> d = diametersOf(p);
    double minR = 1e9;
    for (int i = 0; i < p.slots; ++i) {
        L.r.push_back(0.5 * (d[static_cast<std::size_t>(i)] + p.clearance));
        minR = std::min(minR, L.r.back());
    }
    // Centres along the bar, a tooth between every two rounds.
    double x = p.margin + L.r[0];
    for (int i = 0; i < p.slots; ++i) {
        if (i) x += L.r[static_cast<std::size_t>(i - 1)] + p.tooth + L.r[static_cast<std::size_t>(i)];
        L.xc.push_back(x);
    }
    L.W = L.xc.back() + L.r.back() + p.margin;
    // Roundings the teeth leave room for: between two necks the mouths' roundings,
    // the edge rounding on both sides, and a sliver of flat top.
    std::vector<double> hn;
    for (int i = 0; i < p.slots; ++i) hn.push_back(std::min(1.0, static_cast<double>(p.neck)) * L.r[static_cast<std::size_t>(i)]);
    double gap = 1e9;
    for (int i = 0; i + 1 < p.slots; ++i)
        gap = std::min(gap, (L.xc[static_cast<std::size_t>(i + 1)] - hn[static_cast<std::size_t>(i + 1)]) -
                                (L.xc[static_cast<std::size_t>(i)] + hn[static_cast<std::size_t>(i)]));
    const double ends = std::min(L.xc.front() - hn.front(), L.W - L.xc.back() - hn.back());
    gap = std::min(gap, ends);
    const double re = std::clamp(0.3 * minR, 0.3, 1.2);
    const double reFit = std::min(re, 0.3 * gap);
    L.rho = std::clamp(static_cast<double>(p.rounding), 0.0,
                       std::max(0.0, std::min({0.5 * gap - reFit - 0.15, 0.5 * p.depth - 0.5, p.base - 0.6})));
    L.Rc = std::clamp(2.5, 0.0, std::max(0.0, ends - reFit - L.rho - 0.3));
    const double rl = std::clamp(0.25 * minR, 0.25, 0.8);
    double top = 0.0;
    for (int i = 0; i < p.slots; ++i) {
        const double need = seatDepth(L.r[static_cast<std::size_t>(i)], hn[static_cast<std::size_t>(i)], reFit, rl, L.rho);
        top = std::max(top, z0 + p.base + L.r[static_cast<std::size_t>(i)] + need);
    }
    L.H = std::max(top, z0 + p.base + 2.0 * L.Rc + 1.0);
    for (int i = 0; i < p.slots; ++i) {
        Seat s;
        s.s  = L.W - L.xc[static_cast<std::size_t>(i)];
        s.dc = L.H - (z0 + p.base + L.r[static_cast<std::size_t>(i)]);
        s.r  = L.r[static_cast<std::size_t>(i)];
        s.hn = hn[static_cast<std::size_t>(i)];
        s.re = reFit;
        s.rl = rl;
        L.seats.push_back(s);
    }
    std::reverse(L.seats.begin(), L.seats.end());   // along the top edge: the right-hand one first
    if (flanges) {
        L.Lf = p.screwHead + 6.0;
        L.tf = std::max(3.0, 0.5 * (p.screwHead - p.screwShank) + 1.2);
    }
    return L;
}

// The bar seen from the front (x, z), drawn in by `c` along the top and round
// the seats; counter-clockwise.
std::vector<DV2> combOutline(const Comb& L, double c) {
    std::vector<DV2> pts;
    const double W = L.W, z0 = L.z0;
    if (L.flanges) {
        pts.push_back({-L.Lf, z0});
        pts.push_back({W + L.Lf, z0});
        arcTo(pts, {W + L.Lf - L.Rf, z0 + L.tf - L.Rf}, L.Rf, 0.0, 0.5 * kPi, kFil);
        arcTo(pts, {W + L.rfi, z0 + L.tf + L.rfi}, L.rfi, 1.5 * kPi, kPi, kFil);
    } else {
        pts.push_back({0.0, z0});
        pts.push_back({W, z0});
    }
    arcTo(pts, {W - L.Rc, L.H - c - L.Rc}, L.Rc, 0.0, 0.5 * kPi, kFil);
    for (const Seat& s : L.seats) seat(pts, s, c, [&](double sx, double d) { return DV2(W - sx, L.H - d); });
    arcTo(pts, {L.Rc, L.H - c - L.Rc}, L.Rc, 0.5 * kPi, kPi, kFil);
    if (L.flanges) {
        arcTo(pts, {-L.rfi, z0 + L.tf + L.rfi}, L.rfi, 0.0, -0.5 * kPi, kFil);
        arcTo(pts, {-L.Lf + L.Rf, z0 + L.tf - L.Rf}, L.Rf, 0.5 * kPi, kPi, kFil);
    }
    return pts;
}

// The bar carried from the front (y 0) to the back (y depth), its edges rounded.
// Holes are bored before, so its faces leave them open. `glued`: its bottom
// sits on the desk clip's clamp and has no face of its own.
std::vector<double> buildComb(Builder& b, const Comb& L, double depth, bool glued) {
    std::vector<double> at, in;
    roundedLevels(depth, L.rho, at, in);
    solidbuild::Extrusion e;
    e.axis = 1;   // along y: the plane's (u, v) is (z, x)
    e.at = at;
    for (double c : in) {
        std::vector<DV2> uv;
        for (const DV2& p : combOutline(L, c)) uv.push_back({p.y, p.x});
        e.outline.push_back(std::move(uv));
    }
    if (glued) {
        const double z0 = L.z0;
        e.glued = [z0](DV2 a, DV2 q) { return a.x == z0 && q.x == z0; };
    }
    solidbuild::extrude(b, e);
    return at;
}

// The labels under the seats on the bar's front, raised towards -y.
int combLabels(Builder& b, const Params& p, const Comb& L) {
    const std::vector<std::string> text = labelsOf(p);
    int missing = 0;
    for (std::size_t i = 0; i < text.size() && i < L.xc.size(); ++i) {
        if (text[i].empty()) continue;
        const double left = i == 0 ? 0.8 : 0.5 * (L.xc[i - 1] + L.xc[i]) + 0.4;
        const double right = i + 1 == L.xc.size() ? L.W - 0.8 : 0.5 * (L.xc[i] + L.xc[i + 1]) - 0.4;
        const DV2 lo(left, L.z0 + 0.8), hi(right, L.z0 + L.base - L.rho - 0.5);
        const Fitted f = fit(text[i], p.labelHeight, lo, hi);
        if (!f.ok) continue;
        missing += f.block.missing;
        raise(b, f.block, [&](DV2 m, double h) { return DV3(f.origin.x + m.x, -h, f.origin.y + m.y); });
    }
    return missing;
}

// --- A desk clip ----------------------------------------------------------------------

// The clamp seen from the side (y, z): the plate on the desk (z 0 its top
// surface) from the front (y 0) to the back face (y D), the wall down the
// desk's edge, the lip under it leaning up by `grip` at its tip. Its top is cut
// where the bar's levels meet it.
std::vector<DV2> clampOutline(const Params& p, double D, double tp, const std::vector<double>& levels) {
    const double T = p.deskThickness, wb = p.clipWall, tl = p.clipWall;
    const double tipY = D - wb - p.reach;
    std::vector<std::pair<DV2, double>> c;
    c.push_back({{0.0, 0.0}, 0.6});
    c.push_back({{D - wb, 0.0}, 0.6});
    c.push_back({{D - wb, -T}, 1.5});
    c.push_back({{tipY, -T + p.grip}, 0.5 * tl - 0.05});
    c.push_back({{tipY, -T + p.grip - tl}, 0.5 * tl - 0.05});
    c.push_back({{D, -T - tl}, 3.0});
    c.push_back({{D, tp}, 0.0});
    for (auto it = levels.rbegin(); it != levels.rend(); ++it)
        if (*it > 1e-9 && *it < D - 1e-9) c.push_back({{*it, tp}, 0.0});
    c.push_back({{0.0, tp}, 0.0});
    return filleted(c);
}

// --- A tray under the desk -------------------------------------------------------------

struct Tray {
    double Wi, Hi, t, tfl, Lf, front;   // inside width and height, wall, flange thickness and width, front wall top
};

Tray trayLayout(const Params& p) {
    Tray T;
    T.Wi = p.trayWidth;
    T.Hi = p.trayHeight;
    T.t  = p.trayWall;
    T.tfl = std::max(static_cast<double>(p.trayWall), 0.5 * (p.screwHead - p.screwShank) + 1.2);
    T.Lf = p.screwHead + 6.0;
    T.front = p.trayOpenFront ? -0.5 * T.Hi : 0.0;
    return T;
}

// Seen from the side (y across, z up; the desk's underside at z 0), the channel
// hanging from its flanges.
std::vector<DV2> trayOutline(const Tray& T) {
    const double a = 0.5 * T.Wi, t = T.t, ri = 4.0;
    std::vector<std::pair<DV2, double>> c;
    c.push_back({{-a - t - T.Lf, -T.tfl}, 1.0});
    c.push_back({{-a - t, -T.tfl}, 2.0});
    c.push_back({{-a - t, -T.Hi - t}, ri + t});
    c.push_back({{a + t, -T.Hi - t}, ri + t});
    if (T.front < 0.0) {
        c.push_back({{a + t, T.front}, 0.5 * t - 0.02});
        c.push_back({{a, T.front}, 0.5 * t - 0.02});
    } else {
        c.push_back({{a + t, -T.tfl}, 2.0});
        c.push_back({{a + t + T.Lf, -T.tfl}, 1.0});
        c.push_back({{a + t + T.Lf, 0.0}, 1.0});
        c.push_back({{a, 0.0}, 1.0});
    }
    c.push_back({{a, -T.Hi}, ri});
    c.push_back({{-a, -T.Hi}, ri});
    c.push_back({{-a, 0.0}, 1.0});
    c.push_back({{-a - t - T.Lf, 0.0}, 1.0});
    return filleted(c);
}

// --- A hook -----------------------------------------------------------------------------

// A path of straight runs and bends, drawn with a pen `t` wide and round at both
// ends: the outline, counter-clockwise. Straight runs are single edges, so their
// faces can take screw holes.
struct Stroke {
    struct Step {
        double len = 0.0;     // a straight run
        double turn = 0.0;    // or a bend: radians, + to the left
        double radius = 0.0;  // the bend's, at the pen's middle
    };
    DV2 start{0.0}, dir{1.0, 0.0};
    std::vector<Step> steps;
};

std::vector<DV2> strokeOutline(const Stroke& s, double t) {
    std::vector<DV2> right, left;
    DV2 p = s.start, d = glm::normalize(s.dir);
    auto put = [&](DV2 q, DV2 dir) {
        const DV2 n(-dir.y, dir.x);
        right.push_back(q - 0.5 * t * n);
        left.push_back(q + 0.5 * t * n);
    };
    put(p, d);
    const DV2 d0 = d;
    for (const Stroke::Step& st : s.steps) {
        if (st.turn == 0.0) {
            p += d * st.len;
            put(p, d);
            continue;
        }
        const DV2 n(-d.y, d.x);
        const DV2 c = p + n * (st.turn > 0.0 ? st.radius : -st.radius);
        const int seg = std::max(2, static_cast<int>(std::lround(kBend * std::abs(st.turn) / (0.5 * kPi))));
        const DV2 p0 = p, dd = d;
        for (int k = 1; k <= seg; ++k) {
            const double a = st.turn * static_cast<double>(k) / seg;
            const double ca = std::cos(a), sa = std::sin(a);
            auto rot = [&](DV2 v) { return DV2(ca * v.x - sa * v.y, sa * v.x + ca * v.y); };
            p = c + rot(p0 - c);
            d = rot(dd);
            put(p, d);
        }
    }
    std::vector<DV2> out = right;
    // The end, round: from the right side over the tip to the left.
    const double ae = std::atan2(d.y, d.x);
    std::vector<DV2> cap;
    arcTo(cap, p, 0.5 * t, ae - 0.5 * kPi, ae + 0.5 * kPi, 8);
    out.insert(out.end(), cap.begin() + 1, cap.end() - 1);
    out.insert(out.end(), left.rbegin(), left.rend());
    const double as = std::atan2(d0.y, d0.x);
    cap.clear();
    arcTo(cap, s.start, 0.5 * t, as + 0.5 * kPi, as + 1.5 * kPi, 8);
    out.insert(out.end(), cap.begin() + 1, cap.end() - 1);
    return out;
}

// --- A winder ---------------------------------------------------------------------------

struct Winder {
    double L, Wd, Rc, rho, T;
    std::vector<Seat> right, top, left, bottom;   // in each edge's frame
};

Winder winderLayout(const Params& p) {
    Winder w;
    w.L = p.winderLength;
    w.Wd = p.winderWidth;
    w.T = p.winderThickness;
    w.rho = std::clamp(static_cast<double>(p.rounding), 0.0, 0.5 * w.T - 0.4);
    const double rn = 0.5 * p.notchWidth;
    // Between a corner and a notch: the corner's rounding, the notch's mouth
    // rounding and the edges' rounding, a sliver of straight left.
    const double room = 0.5 * w.Wd - rn;
    w.Rc = std::clamp(room - 1.5 - w.rho - 0.3, 0.0, 6.0);
    Seat notch;
    notch.s = 0.5 * w.Wd;
    notch.r = rn;
    notch.hn = rn;
    notch.re = std::clamp(room - w.Rc - w.rho - 0.3, 0.2, 2.0);
    notch.rl = 0.5;
    notch.dc = std::max(static_cast<double>(p.notchDepth) - rn, seatDepth(rn, rn, notch.re, notch.rl, w.rho));
    w.right = {notch};
    w.left = {notch};
    // The slits for the cable's ends: a keyhole a cable is pressed into, near
    // the ends on the long sides (one on each, turned half round).
    Seat sl;
    sl.r = 0.5 * p.slit + 0.1;
    sl.hn = 0.35 * p.slit;
    sl.re = 0.6;
    sl.rl = 0.3;
    sl.s = notch.dc + rn + 5.0 + sl.r;
    sl.dc = std::max(4.0 + sl.r, seatDepth(sl.r, sl.hn, sl.re, sl.rl, w.rho));
    w.top = {sl};
    w.bottom = {sl};
    return w;
}

// Seen from above (x, y), drawn in by `c` round the whole outline.
std::vector<DV2> winderOutline(const Winder& w, double c) {
    std::vector<DV2> pts;
    const double a = 0.5 * w.L, h = 0.5 * w.Wd, r = std::max(0.0, w.Rc - c);
    auto edge = [&](const std::vector<Seat>& seats, DV2 A, DV2 t) {
        const DV2 n(-t.y, t.x);
        for (const Seat& s : seats) seat(pts, s, c, [&](double sx, double d) { return A + sx * t + d * n; });
    };
    arcTo(pts, {a - w.Rc, -h + w.Rc}, r, -0.5 * kPi, 0.0, kFil);
    edge(w.right, {a, -h}, {0.0, 1.0});
    arcTo(pts, {a - w.Rc, h - w.Rc}, r, 0.0, 0.5 * kPi, kFil);
    edge(w.top, {a, h}, {-1.0, 0.0});
    arcTo(pts, {-a + w.Rc, h - w.Rc}, r, 0.5 * kPi, kPi, kFil);
    edge(w.left, {-a, h}, {0.0, -1.0});
    arcTo(pts, {-a + w.Rc, -h + w.Rc}, r, kPi, 1.5 * kPi, kFil);
    edge(w.bottom, {-a, -h}, {1.0, 0.0});
    return pts;
}

// --- Holes ---------------------------------------------------------------------------------

// A countersunk screw hole along `axis` from the face at `head` (its head
// sinks in there) to the face at `foot`.
solidbuild::Hole screwHole(const Params& p, int axis, DV2 middle, double head, double foot) {
    solidbuild::Hole h;
    h.axis = axis;
    h.middle = middle;
    const double s = foot > head ? 1.0 : -1.0;
    const double rh = 0.5 * p.screwHead, rs = 0.5 * p.screwShank;
    h.steps = {{head, rh}, {head + s * (rh - rs), rs}, {foot, rs}};
    return h;
}

std::vector<glm::vec2> toView(const std::vector<DV2>& pts) {
    std::vector<glm::vec2> out;
    for (const DV2& p : pts) {
        const glm::vec2 q(p);
        if (out.empty() || glm::length(out.back() - q) > 1e-4f) out.push_back(q);
    }
    return out;
}

std::vector<glm::vec2> rectView(double x0, double y0, double x1, double y1) {
    return {{float(x0), float(y0)}, {float(x1), float(y0)}, {float(x1), float(y1)}, {float(x0), float(y1)}};
}

std::vector<glm::vec2> circleView(DV2 c, double r) {
    std::vector<glm::vec2> out;
    for (int i = 0; i < 32; ++i) {
        const double a = 2.0 * kPi * i / 32.0;
        out.push_back(glm::vec2(c + r * DV2(std::cos(a), std::sin(a))));
    }
    return out;
}

std::uint32_t toByte(float v) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
}

std::vector<std::string> splitList(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == ',' || ch == ';') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    out.push_back(cur);
    for (std::string& t : out) {
        const std::size_t a = t.find_first_not_of(" \t"), b = t.find_last_not_of(" \t");
        t = a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
    }
    return out;
}

} // namespace

const char* styleName(Style s) {
    switch (s) {
    case Style::Comb:     return "Cable comb";
    case Style::DeskClip: return "Desk cable clip";
    case Style::Tray:     return "Cable tray";
    case Style::Hook:     return "Cable hook";
    case Style::Winder:   return "Cable winder";
    default:              return "?";
    }
}

const char* mountName(Mount m) {
    switch (m) {
    case Mount::Free:     return "Free-standing";
    case Mount::Adhesive: return "Adhesive pad";
    case Mount::Screws:   return "Screws";
    case Mount::Magnets:  return "Magnets";
    default:              return "?";
    }
}

std::vector<float> diametersOf(const Params& p) {
    std::vector<float> vals;
    for (const std::string& t : splitList(p.diameters)) {
        char* end = nullptr;
        const float v = std::strtof(t.c_str(), &end);
        if (end != t.c_str() && v > 0.0f) vals.push_back(std::clamp(v, 1.0f, 30.0f));
    }
    if (vals.empty()) vals.push_back(5.0f);
    std::vector<float> out;
    for (int i = 0; i < std::max(1, p.slots); ++i)
        out.push_back(vals[std::min(static_cast<std::size_t>(i), vals.size() - 1)]);
    return out;
}

std::vector<std::string> labelsOf(const Params& p) {
    std::vector<std::string> out;
    if (p.labels.find_first_not_of(" \t,;") == std::string::npos) return out;
    out = splitList(p.labels);
    out.resize(static_cast<std::size_t>(std::max(1, p.slots)));
    return out;
}

Params sane(const Params& in) {
    Params p = in;
    p.style = static_cast<Style>(std::clamp(static_cast<int>(in.style), 0, static_cast<int>(Style::Count) - 1));
    p.mount = static_cast<Mount>(std::clamp(static_cast<int>(in.mount), 0, static_cast<int>(Mount::Count) - 1));
    p.hookMount = static_cast<HookMount>(std::clamp(static_cast<int>(in.hookMount), 0, static_cast<int>(HookMount::Count) - 1));
    p.slots       = std::clamp(in.slots, 1, 24);
    p.clearance   = std::clamp(in.clearance, -0.5f, 2.0f);
    p.neck        = std::clamp(in.neck, 0.3f, 1.0f);
    p.tooth       = std::clamp(in.tooth, 1.0f, 30.0f);
    p.margin      = std::clamp(in.margin, 1.5f, 50.0f);
    p.depth       = std::clamp(in.depth, 6.0f, 100.0f);
    p.rounding    = std::clamp(in.rounding, 0.0f, 5.0f);
    p.labelHeight = std::clamp(in.labelHeight, 1.5f, 15.0f);
    p.screwShank  = std::clamp(in.screwShank, 2.0f, 8.0f);
    p.screwHead   = std::clamp(in.screwHead, p.screwShank + 1.0f, 20.0f);
    p.magnetDiameter = std::clamp(in.magnetDiameter, 2.0f, 20.0f);
    p.magnetHeight   = std::clamp(in.magnetHeight, 0.5f, 10.0f);
    // The base under the seats holds what goes in it: the labels' band (the
    // front drawn in by the rounding above them), a magnet, a pad's recess.
    float base = std::clamp(in.base, 1.0f, 30.0f);
    if (!labelsOf(p).empty()) base = std::max(base, 1.3f + p.rounding + 1.3f * p.labelHeight);
    if (p.style == Style::Comb && p.mount == Mount::Magnets) base = std::max(base, p.magnetHeight + 1.0f);
    if (p.style == Style::Comb && p.mount == Mount::Adhesive) base = std::max(base, 2.2f);
    p.base = base;

    p.deskThickness = std::clamp(in.deskThickness, 5.0f, 80.0f);
    p.grip          = std::clamp(in.grip, 0.0f, 3.0f);
    p.reach         = std::clamp(in.reach, 5.0f, 60.0f);
    p.clipWall      = std::clamp(in.clipWall, 2.0f, 10.0f);

    p.trayLength = std::clamp(in.trayLength, 40.0f, 600.0f);
    p.trayWidth  = std::clamp(in.trayWidth, 12.0f, 150.0f);
    p.trayHeight = std::clamp(in.trayHeight, 12.0f, 150.0f);
    p.trayWall   = std::clamp(in.trayWall, 1.2f, 6.0f);
    p.outletDiameter = std::clamp(in.outletDiameter, 5.0f, p.trayWidth - 10.0f);
    p.outletSpacing  = std::clamp(in.outletSpacing, p.outletDiameter + 5.0f, 600.0f);

    p.hookWidth     = std::clamp(in.hookWidth, p.screwHead + 3.0f, 100.0f);
    p.hookThickness = std::clamp(in.hookThickness, 0.5f * (p.screwHead - p.screwShank) + 1.5f, 15.0f);
    p.hookPlate     = std::clamp(in.hookPlate, p.screwHead + 8.0f, 200.0f);
    p.hookReach     = std::clamp(in.hookReach, 5.0f, 200.0f);
    p.hookRadius    = std::clamp(in.hookRadius, 2.0f, 80.0f);
    p.hookTip       = std::clamp(in.hookTip, 0.0f, 100.0f);

    p.winderLength    = std::clamp(in.winderLength, 35.0f, 200.0f);
    p.winderWidth     = std::clamp(in.winderWidth, 16.0f, 100.0f);
    p.winderThickness = std::clamp(in.winderThickness, 1.5f, 10.0f);
    p.notchWidth      = std::clamp(in.notchWidth, 3.0f, p.winderWidth - 8.0f);
    p.slit            = std::clamp(in.slit, 0.8f, 8.0f);
    p.notchDepth      = std::clamp(in.notchDepth, 0.5f * p.notchWidth + 1.0f,
                                   0.5f * p.winderLength - 2.0f * p.slit - 12.0f);
    return p;
}

Model build(const Params& in) {
    const Params p = sane(in);
    Model out;
    Builder b, lb;   // the holder and its labels
    View front, side;
    switch (p.style) {
    case Style::Comb: {
        const Comb L = combLayout(p, 0.0, p.mount == Mount::Screws);
        const double D = p.depth;
        if (p.mount == Mount::Adhesive) {
            solidbuild::Hole h;
            h.axis = 2;
            h.middle = {0.5 * L.W, 0.5 * D};
            const double hx = 0.5 * L.W - 2.5, hy = 0.5 * D - 2.5;
            h.steps = {{0.0, hx, hy, 2.0}, {0.6, hx, hy, 2.0}};
            h.through = false;
            if (hx > 2.0 && hy > 2.0) solidbuild::bore(b, h);
        } else if (p.mount == Mount::Magnets) {
            std::vector<double> xs{L.xc.front()};
            if (L.xc.size() > 1) xs.push_back(L.xc.back());
            for (const double x : xs) {
                solidbuild::Hole h;
                h.axis = 2;
                h.middle = {x, 0.5 * D};
                const double r = 0.5 * p.magnetDiameter + 0.1;
                h.steps = {{0.0, r}, {p.magnetHeight + 0.2, r}};
                h.through = false;
                solidbuild::bore(b, h);
            }
        } else if (p.mount == Mount::Screws) {
            for (const double x : {-0.5 * L.Lf, L.W + 0.5 * L.Lf})
                solidbuild::bore(b, screwHole(p, 2, {x, 0.5 * D}, L.tf, 0.0));
        }
        buildComb(b, L, D, false);
        out.missingGlyphs = combLabels(lb, p, L);
        front.name = "Front";
        front.loops.push_back(toView(combOutline(L, 0.0)));
        out.printHint = "Prints standing as it is, its bottom on the bed, without support.";
        break;
    }
    case Style::DeskClip: {
        const double tp = p.clipWall;
        const double D = std::max(static_cast<double>(p.depth), p.clipWall + 8.0);
        const Comb L = combLayout(p, tp, false);
        const std::vector<double> levels = buildComb(b, L, D, true);
        solidbuild::Extrusion e;
        e.axis = 0;   // along x: (u, v) is (y, z)
        e.at = {0.0, L.W};
        const std::vector<DV2> clamp = clampOutline(p, D, tp, levels);
        e.outline = {clamp, clamp};
        e.glued = [tp, D](DV2 a, DV2 q) { return a.y == tp && q.y == tp && a.x <= D && q.x <= D; };
        solidbuild::extrude(b, e);
        out.missingGlyphs = combLabels(lb, p, L);
        front.name = "Front";
        front.solids = side.solids = 2;
        front.loops.push_back(rectView(0.0, -p.deskThickness - p.clipWall, L.W, tp));
        front.loops.push_back(toView(combOutline(L, 0.0)));
        side.name = "Side";
        side.loops.push_back(toView(clamp));
        side.loops.push_back(rectView(0.0, tp, D, L.H));
        out.printHint = "Prints best lying on its side (the side view flat on the bed): the clamp needs no support.";
        break;
    }
    case Style::Tray: {
        const Tray T = trayLayout(p);
        const double len = p.trayLength;
        // Screws through the flanges, heads sunk in from below.
        const int nScrew = std::max(2, static_cast<int>(len / 100.0) + 1);
        std::vector<double> ys{-0.5 * T.Wi - T.t - 0.5 * T.Lf};
        if (!p.trayOpenFront) ys.push_back(0.5 * T.Wi + T.t + 0.5 * T.Lf);
        for (int k = 0; k < nScrew; ++k) {
            const double x = 15.0 + (len - 30.0) * k / (nScrew - 1);
            for (const double y : ys) solidbuild::bore(b, screwHole(p, 2, {x, y}, -T.tfl, 0.0));
        }
        std::vector<DV2> outlets;
        if (p.trayOutlets) {
            const int n = std::max(1, static_cast<int>((len - p.outletDiameter - 20.0) / p.outletSpacing) + 1);
            const double span = (n - 1) * p.outletSpacing;
            for (int k = 0; k < n; ++k) {
                solidbuild::Hole h;
                h.axis = 2;
                h.middle = {0.5 * len - 0.5 * span + k * p.outletSpacing, 0.0};
                h.steps = {{-T.Hi, 0.5 * p.outletDiameter}, {-T.Hi - T.t, 0.5 * p.outletDiameter}};
                solidbuild::bore(b, h);
                outlets.push_back(h.middle);
            }
        }
        solidbuild::Extrusion e;
        e.axis = 0;
        e.at = {0.0, len};
        const std::vector<DV2> prof = trayOutline(T);
        e.outline = {prof, prof};
        solidbuild::extrude(b, e);
        side.name = "Side";
        side.loops.push_back(toView(prof));
        front.name = "From below";
        const double y0 = -0.5 * T.Wi - T.t - T.Lf, y1 = p.trayOpenFront ? 0.5 * T.Wi + T.t : 0.5 * T.Wi + T.t + T.Lf;
        front.loops.push_back(rectView(0.0, y0, len, y1));
        for (const DV2& o : outlets) front.loops.push_back(circleView(o, 0.5 * p.outletDiameter));
        for (int k = 0; k < nScrew; ++k)
            for (const double y : ys)
                front.loops.push_back(circleView({15.0 + (len - 30.0) * k / (nScrew - 1), y}, 0.5 * p.screwShank));
        out.printHint = "Prints upside down, the flanges on the bed; the open front needs no support.";
        break;
    }
    case Style::Hook: {
        const double t = p.hookThickness, w = p.hookWidth;
        Stroke s;
        const double rb = 0.5 * t + 3.0, rh = p.hookRadius + 0.5 * t;
        const int nHoles = p.hookPlate >= 2.0 * p.screwHead + 14.0 ? 2 : 1;
        if (p.hookMount == HookMount::Wall) {
            // Down the wall, out along the arm, up at the end.
            s.start = {0.5 * t, p.hookPlate + rb};
            s.dir = {0.0, -1.0};
            s.steps = {{p.hookPlate, 0.0, 0.0}, {0.0, 0.5 * kPi, rb}, {p.hookReach, 0.0, 0.0},
                       {0.0, 0.6 * kPi, rh}, {p.hookTip, 0.0, 0.0}};
            for (int k = 0; k < nHoles; ++k) {
                const double z = rb + p.hookPlate * (nHoles == 1 ? 0.55 : (k == 0 ? 0.3 : 0.78));
                solidbuild::bore(b, screwHole(p, 1, {z, 0.5 * w}, t, 0.0));
            }
            out.printHint = "Prints lying on its side (the side view flat on the bed).";
        } else {
            // Along the desk's underside, down, and round into a J.
            s.start = {0.5 * t, -0.5 * t};
            s.dir = {1.0, 0.0};
            s.steps = {{p.hookPlate, 0.0, 0.0}, {0.0, -0.5 * kPi, rb}, {p.hookReach, 0.0, 0.0},
                       {0.0, kPi, rh}, {p.hookTip, 0.0, 0.0}};
            for (int k = 0; k < nHoles; ++k) {
                const double y = 0.5 * t + p.hookPlate * (nHoles == 1 ? 0.45 : (k == 0 ? 0.22 : 0.7));
                solidbuild::bore(b, screwHole(p, 2, {0.5 * w, y}, -t, 0.0));
            }
            out.printHint = "Prints lying on its side (the side view flat on the bed).";
        }
        const std::vector<DV2> prof = strokeOutline(s, t);
        solidbuild::Extrusion e;
        e.axis = 0;
        e.at = {0.0, w};
        e.outline = {prof, prof};
        solidbuild::extrude(b, e);
        side.name = "Side";
        side.loops.push_back(toView(prof));
        break;
    }
    case Style::Winder: {
        const Winder W = winderLayout(p);
        std::vector<double> at, in;
        roundedLevels(W.T, W.rho, at, in);
        solidbuild::Extrusion e;
        e.axis = 2;
        e.at = at;
        for (double c : in) e.outline.push_back(winderOutline(W, c));
        solidbuild::extrude(b, e);
        if (!p.winderLabel.empty()) {
            const double half = 0.5 * W.L - p.notchDepth - 4.0;
            const double hh = 0.5 * W.Wd - W.rho - 2.5 - 2.0 * W.top[0].r - 4.0;
            const Fitted f = fit(p.winderLabel, p.labelHeight, {-half, -hh}, {half, hh});
            if (f.ok) {
                out.missingGlyphs = f.block.missing;
                raise(lb, f.block, [&](DV2 m, double h) { return DV3(f.origin.x + m.x, f.origin.y + m.y, W.T + h); });
            }
        }
        front.name = "From above";
        front.loops.push_back(toView(winderOutline(W, 0.0)));
        out.printHint = "Prints flat as it lies.";
        break;
    }
    default:
        break;
    }
    out.ok = b.error.empty() && lb.error.empty();
    out.error = !b.error.empty() ? b.error : lb.error;
    out.volume = solidbuild::volume(b);
    out.body = solidbuild::toEditMesh(b);
    out.labels = solidbuild::toEditMesh(lb);
    DV3 lo(1e30), hi(-1e30);
    for (const Builder* bb : {&b, &lb})
        for (const DV3& v : bb->verts) {
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
    if (!b.verts.empty()) out.size = glm::vec3(hi - lo);
    if (!front.loops.empty()) out.views.push_back(std::move(front));
    if (!side.loops.empty()) out.views.push_back(std::move(side));
    return out;
}

Palette ensurePalette(std::vector<MaterialDef>& mats, const Params& p) {
    auto colour = [&](const char* part, glm::vec3 c) {
        char name[64];
        std::snprintf(name, sizeof name, "Cable holder %s#%02X%02X%02X", part, toByte(c.r), toByte(c.g), toByte(c.b));
        for (MaterialDef& m : mats) {
            if (m.name != name) continue;
            m.albedo = c;
            if (!m.assetId.valid()) m.assetId = AssetId::generate();
            return m.assetId;
        }
        MaterialDef md;
        md.assetId      = AssetId::generate();
        md.name         = name;
        md.albedo       = c;
        md.reflectivity = 0.0f;
        md.roughness    = 0.55f;   // printed plastic: a dull sheen
        mats.push_back(md);
        return md.assetId;
    };
    Palette pal;
    pal.body = colour("", p.color);
    pal.labels = colour("Label ", p.labelColor);
    return pal;
}

std::string autoName(const Params& in) {
    const Params p = sane(in);
    char b[96];
    switch (p.style) {
    case Style::Comb:
    case Style::DeskClip: {
        const std::vector<float> d = diametersOf(p);
        const bool same = std::all_of(d.begin(), d.end(), [&](float v) { return v == d.front(); });
        if (same) std::snprintf(b, sizeof b, "%s %d x %.4g mm", styleName(p.style), p.slots, d.front());
        else std::snprintf(b, sizeof b, "%s %d seats", styleName(p.style), p.slots);
        break;
    }
    case Style::Tray:   std::snprintf(b, sizeof b, "%s %.0f mm", styleName(p.style), p.trayLength); break;
    case Style::Hook:   std::snprintf(b, sizeof b, "%s %s", styleName(p.style),
                                      p.hookMount == HookMount::Wall ? "for the wall" : "under the desk"); break;
    default:            std::snprintf(b, sizeof b, "%s %.0f mm", styleName(p.style), p.winderLength); break;
    }
    return b;
}

std::vector<Entity> entities(const Params& in, const Palette& pal, int& counter, const glm::vec3& at) {
    const Params p = sane(in);
    Model mdl = build(p);
    if (mdl.body.faces.empty()) return {};
    std::vector<Entity> out;
    Entity root;
    root.type = EntityType::Empty;
    root.name = autoName(p);
    root.id = counter++;
    root.parent = -1;
    // The root in the middle of the part, so its pick box is the part's: `at` is
    // where the part's bottom stands. The part keeps its own place round the
    // root's middle, its bottom on `at`.
    glm::vec3 lo(1e9f), hi(-1e9f);
    for (const EditMesh* m : {&mdl.body, &mdl.labels})
        for (const glm::vec3& v : m->verts) {
            lo = glm::min(lo, v);
            hi = glm::max(hi, v);
        }
    const glm::vec3 mid(0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), 0.5f * (lo.z + hi.z));
    root.half = glm::max(0.5f * (hi - lo), glm::vec3(1e-4f));
    root.localCenter = root.center = at + glm::vec3(0.0f, root.half.y, 0.0f);
    {
        auto cc = std::make_unique<CableHolderComponent>();
        cc->params = p;
        root.components.items.push_back(std::move(cc));
    }
    out.push_back(std::move(root));
    const int rootId = out.front().id;
    const glm::vec3 rootC = out.front().center;
    auto add = [&](const char* name, EditMesh m, const AssetId& mat, bool collide) {
        if (m.faces.empty()) return;
        // Into the root's frame: the part's middle on the root's.
        for (glm::vec3& v : m.verts) v -= mid;
        const glm::vec3 shift = editmesh::recenter(m);
        glm::vec3 mn, mx;
        m.bounds(mn, mx);
        Entity e;
        e.type = EntityType::Box;
        e.name = name;
        e.id = counter++;
        e.parent = rootId;
        e.half = glm::max(0.5f * (mx - mn), glm::vec3(1e-4f));
        e.localCenter = shift;
        e.center = rootC + shift;
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
    add("Holder", std::move(mdl.body), pal.body, true);
    add("Labels", std::move(mdl.labels), pal.labels, false);
    return out;
}

// --- Persistence -------------------------------------------------------------------

namespace {
nlohmann::json colJson(const glm::vec3& v) { return nlohmann::json::array({v.x, v.y, v.z}); }
glm::vec3 jsonCol(const nlohmann::json& j, const glm::vec3& def) {
    if (!j.is_array() || j.size() != 3) return def;
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}
} // namespace

void saveParams(nlohmann::json& j, const Params& p) {
    j["style"]           = static_cast<int>(p.style);
    j["slots"]           = p.slots;
    j["diameters"]       = p.diameters;
    j["clearance"]       = p.clearance;
    j["neck"]            = p.neck;
    j["tooth"]           = p.tooth;
    j["margin"]          = p.margin;
    j["depth"]           = p.depth;
    j["base"]            = p.base;
    j["rounding"]        = p.rounding;
    j["labels"]          = p.labels;
    j["labelHeight"]     = p.labelHeight;
    j["mount"]           = static_cast<int>(p.mount);
    j["deskThickness"]   = p.deskThickness;
    j["grip"]            = p.grip;
    j["reach"]           = p.reach;
    j["clipWall"]        = p.clipWall;
    j["trayLength"]      = p.trayLength;
    j["trayWidth"]       = p.trayWidth;
    j["trayHeight"]      = p.trayHeight;
    j["trayWall"]        = p.trayWall;
    j["trayOpenFront"]   = p.trayOpenFront;
    j["trayOutlets"]     = p.trayOutlets;
    j["outletDiameter"]  = p.outletDiameter;
    j["outletSpacing"]   = p.outletSpacing;
    j["hookMount"]       = static_cast<int>(p.hookMount);
    j["hookWidth"]       = p.hookWidth;
    j["hookThickness"]   = p.hookThickness;
    j["hookPlate"]       = p.hookPlate;
    j["hookReach"]       = p.hookReach;
    j["hookRadius"]      = p.hookRadius;
    j["hookTip"]         = p.hookTip;
    j["winderLength"]    = p.winderLength;
    j["winderWidth"]     = p.winderWidth;
    j["winderThickness"] = p.winderThickness;
    j["notchWidth"]      = p.notchWidth;
    j["notchDepth"]      = p.notchDepth;
    j["slit"]            = p.slit;
    j["winderLabel"]     = p.winderLabel;
    j["screwShank"]      = p.screwShank;
    j["screwHead"]       = p.screwHead;
    j["magnetDiameter"]  = p.magnetDiameter;
    j["magnetHeight"]    = p.magnetHeight;
    j["color"]           = colJson(p.color);
    j["labelColor"]      = colJson(p.labelColor);
}

void loadParams(const nlohmann::json& j, Params& p) {
    const Params d;
    p.style           = static_cast<Style>(j.value("style", static_cast<int>(d.style)));
    p.slots           = j.value("slots", d.slots);
    p.diameters       = j.value("diameters", d.diameters);
    p.clearance       = j.value("clearance", d.clearance);
    p.neck            = j.value("neck", d.neck);
    p.tooth           = j.value("tooth", d.tooth);
    p.margin          = j.value("margin", d.margin);
    p.depth           = j.value("depth", d.depth);
    p.base            = j.value("base", d.base);
    p.rounding        = j.value("rounding", d.rounding);
    p.labels          = j.value("labels", d.labels);
    p.labelHeight     = j.value("labelHeight", d.labelHeight);
    p.mount           = static_cast<Mount>(j.value("mount", static_cast<int>(d.mount)));
    p.deskThickness   = j.value("deskThickness", d.deskThickness);
    p.grip            = j.value("grip", d.grip);
    p.reach           = j.value("reach", d.reach);
    p.clipWall        = j.value("clipWall", d.clipWall);
    p.trayLength      = j.value("trayLength", d.trayLength);
    p.trayWidth       = j.value("trayWidth", d.trayWidth);
    p.trayHeight      = j.value("trayHeight", d.trayHeight);
    p.trayWall        = j.value("trayWall", d.trayWall);
    p.trayOpenFront   = j.value("trayOpenFront", d.trayOpenFront);
    p.trayOutlets     = j.value("trayOutlets", d.trayOutlets);
    p.outletDiameter  = j.value("outletDiameter", d.outletDiameter);
    p.outletSpacing   = j.value("outletSpacing", d.outletSpacing);
    p.hookMount       = static_cast<HookMount>(j.value("hookMount", static_cast<int>(d.hookMount)));
    p.hookWidth       = j.value("hookWidth", d.hookWidth);
    p.hookThickness   = j.value("hookThickness", d.hookThickness);
    p.hookPlate       = j.value("hookPlate", d.hookPlate);
    p.hookReach       = j.value("hookReach", d.hookReach);
    p.hookRadius      = j.value("hookRadius", d.hookRadius);
    p.hookTip         = j.value("hookTip", d.hookTip);
    p.winderLength    = j.value("winderLength", d.winderLength);
    p.winderWidth     = j.value("winderWidth", d.winderWidth);
    p.winderThickness = j.value("winderThickness", d.winderThickness);
    p.notchWidth      = j.value("notchWidth", d.notchWidth);
    p.notchDepth      = j.value("notchDepth", d.notchDepth);
    p.slit            = j.value("slit", d.slit);
    p.winderLabel     = j.value("winderLabel", d.winderLabel);
    p.screwShank      = j.value("screwShank", d.screwShank);
    p.screwHead       = j.value("screwHead", d.screwHead);
    p.magnetDiameter  = j.value("magnetDiameter", d.magnetDiameter);
    p.magnetHeight    = j.value("magnetHeight", d.magnetHeight);
    p.color           = jsonCol(j.value("color", nlohmann::json()), d.color);
    p.labelColor      = jsonCol(j.value("labelColor", nlohmann::json()), d.labelColor);
    p = sane(p);
}

namespace {
// Scene files name components by type id; this lets a saved holder come back
// as one, parameters and all.
struct RegisterCableHolder {
    RegisterCableHolder() {
        components::registerType({"cableHolder", "Cable holder",
            [] { return std::unique_ptr<ComponentBase>(std::make_unique<CableHolderComponent>()); },
            /*addable=*/false});
    }
} g_registerCableHolder;
} // namespace

} // namespace cableholder
