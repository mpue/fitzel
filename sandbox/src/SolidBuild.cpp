#include "SolidBuild.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "PolyFill.hpp"

namespace solidbuild {

DV3 world(int axis, DV2 uv, double along) {
    switch (axis) {
    case 0:  return {along, uv.x, uv.y};
    case 1:  return {uv.y, along, uv.x};
    default: return {uv.x, uv.y, along};
    }
}

DV2 plane(int axis, const DV3& p) {
    switch (axis) {
    case 0:  return {p.y, p.z};
    case 1:  return {p.z, p.x};
    default: return {p.x, p.y};
    }
}

// --- Flat faces -----------------------------------------------------------------------

// See SolidBuild.hpp: shared edges cancel, the loops left are filled, the
// corners PolyFill drops on straight runs are put back.
void Builder::region(const std::vector<Ring>& outerIn, const std::vector<Ring>& holesIn, int axis, bool up) {
    // The plane's own (u, v), so that counter-clockwise there faces +axis.
    auto at2 = [&](int id) {
        const DV3& p = verts[static_cast<std::size_t>(id)];
        return axis == 2 ? DV2(p.x, p.y) : axis == 0 ? DV2(p.y, p.z) : DV2(p.z, p.x);
    };
    // Every outline counter-clockwise there, whichever way it came.
    auto ccw = [&](const Ring& r) {
        double a = 0.0;
        for (std::size_t i = 0; i < r.size(); ++i) {
            const DV2 p = at2(r[i]), q = at2(r[(i + 1) % r.size()]);
            a += p.x * q.y - q.x * p.y;
        }
        return a < 0.0 ? Ring(r.rbegin(), r.rend()) : r;
    };
    std::vector<Ring> outer, holes;
    for (const Ring& r : outerIn) outer.push_back(ccw(r));
    for (const Ring& r : holesIn) holes.push_back(ccw(r));
    std::map<std::pair<int, int>, int> edges;
    auto add = [&](int u, int v) {
        if (u == v) return;
        const auto back = edges.find({v, u});
        if (back != edges.end()) {
            if (--back->second == 0) edges.erase(back);
            return;
        }
        ++edges[{u, v}];
    };
    for (const Ring& r : outer)
        for (std::size_t i = 0; i < r.size(); ++i) add(r[i], r[(i + 1) % r.size()]);
    for (const Ring& r : holes)
        for (std::size_t i = 0; i < r.size(); ++i) add(r[(i + 1) % r.size()], r[i]);
    if (edges.empty()) return;

    // The loops the edges left over make. A corner two loops touch at would be
    // walked through twice; such a walk is cut into its simple loops.
    std::unordered_map<int, std::vector<int>> next;
    for (const auto& [e, n] : edges)
        for (int k = 0; k < n; ++k) next[e.first].push_back(e.second);
    std::vector<std::vector<int>> loops;
    std::vector<int> starts;
    for (const auto& [u, l] : next) starts.push_back(u);
    std::sort(starts.begin(), starts.end());
    for (int start : starts) {
        for (;;) {
            auto s = next.find(start);
            if (s == next.end() || s->second.empty()) break;
            std::vector<int> walk{start};
            for (int u = start;;) {
                auto it = next.find(u);
                if (it == next.end() || it->second.empty()) {
                    error = "an outline that does not close";
                    return;
                }
                const int v = it->second.back();
                it->second.pop_back();
                if (v == start) break;
                walk.push_back(v);
                u = v;
            }
            std::vector<int> stack;
            std::unordered_map<int, std::size_t> at;
            for (int v : walk) {
                const auto seen = at.find(v);
                if (seen != at.end()) {
                    const std::size_t p = seen->second;
                    loops.emplace_back(stack.begin() + static_cast<std::ptrdiff_t>(p), stack.end());
                    for (std::size_t k = p; k < stack.size(); ++k) at.erase(stack[k]);
                    stack.resize(p);
                }
                at[v] = stack.size();
                stack.push_back(v);
            }
            loops.push_back(std::move(stack));
        }
    }

    // Each loop's corners on a straight line set aside, the way PolyFill would
    // drop them, and remembered between the two corners kept on either side.
    std::vector<std::vector<int>> kept;
    std::map<std::pair<int, int>, std::vector<int>> between;
    for (const std::vector<int>& loop : loops) {
        std::vector<int> k = loop;
        for (bool again = true; again && k.size() >= 3;) {
            again = false;
            for (std::size_t i = 0; i < k.size() && k.size() >= 3; ++i) {
                const DV2 a = at2(k[(i + k.size() - 1) % k.size()]), b = at2(k[i]), c = at2(k[(i + 1) % k.size()]);
                const double cr = (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x);
                if (b == c || std::abs(cr) < 1e-9) {
                    k.erase(k.begin() + static_cast<std::ptrdiff_t>(i));
                    again = true;
                    break;
                }
            }
        }
        if (k.size() < 3) continue;   // no area: nothing to fill
        // What lies between consecutive kept corners, in the loop's order.
        const std::size_t n = loop.size();
        std::size_t first = 0;
        while (loop[first] != k.front()) ++first;
        std::vector<int> run;
        int from = k.front();
        for (std::size_t s = 1; s <= n; ++s) {
            const int v = loop[(first + s) % n];
            if (std::find(k.begin(), k.end(), v) != k.end()) {
                if (!run.empty()) between[{from, v}] = run;
                run.clear();
                from = v;
            } else {
                run.push_back(v);
            }
        }
        kept.push_back(std::move(k));
    }
    if (kept.empty()) return;

    std::vector<std::vector<polyfill::DV>> outlines;
    for (const std::vector<int>& k : kept) {
        outlines.emplace_back();
        for (int v : k) outlines.back().push_back(at2(v));
    }
    polyfill::Fill f;
    if (!polyfill::fill(outlines, f, true) || f.loops.size() != kept.size()) {
        char b[96];
        const DV3& p = verts[static_cast<std::size_t>(kept[0][0])];
        std::snprintf(b, sizeof b, "a face at %c %.2f mm could not be filled", "xyz"[axis], p[axis]);
        error = b;
        return;
    }
    // Fill's corners back to ours: loop by loop, kept in order or reversed (and
    // turned back from a tried turn, so by nearness, not equality).
    std::vector<int> id(f.points.size(), -1);
    for (std::size_t l = 0; l < kept.size(); ++l) {
        if (f.loops[l].size() != kept[l].size()) {
            error = "a face lost a corner in the fill";
            return;
        }
        for (int pi : f.loops[l]) {
            const DV2 p = f.points[static_cast<std::size_t>(pi)];
            double best = 1e-6;
            for (int v : kept[l]) {
                const double d = glm::length(at2(v) - p);
                if (d < best) {
                    best = d;
                    id[static_cast<std::size_t>(pi)] = v;
                }
            }
            if (id[static_cast<std::size_t>(pi)] < 0) {
                error = "a face's corners moved in the fill";
                return;
            }
        }
    }

    std::vector<std::array<int, 3>> tris;
    for (const glm::ivec3& t : f.tris)
        tris.push_back({id[static_cast<std::size_t>(t.x)], id[static_cast<std::size_t>(t.y)],
                        id[static_cast<std::size_t>(t.z)]});
    // A fill tried turned can lay a triangle flat along sides that are one line
    // here (compartments in a row, cells in a row): a corner m lying on the
    // edge a-b opposite it. The triangle across a-b is split at m instead.
    for (int guard = 0; guard < 1000; ++guard) {
        std::size_t flat = tris.size();
        int e = 0;
        for (std::size_t i = 0; i < tris.size() && flat == tris.size(); ++i) {
            const DV2 p0 = at2(tris[i][0]), p1 = at2(tris[i][1]), p2 = at2(tris[i][2]);
            if (std::abs((p1.x - p0.x) * (p2.y - p0.y) - (p1.y - p0.y) * (p2.x - p0.x)) > 1e-9) continue;
            for (int k = 0; k < 3; ++k) {
                const DV2 a = at2(tris[i][static_cast<std::size_t>(k)]);
                const DV2 b = at2(tris[i][static_cast<std::size_t>((k + 1) % 3)]);
                const DV2 m = at2(tris[i][static_cast<std::size_t>((k + 2) % 3)]);
                const double s = glm::dot(m - a, b - a) / glm::dot(b - a, b - a);
                if (s > 0.0 && s < 1.0) {
                    flat = i;
                    e = k;
                }
            }
        }
        if (flat == tris.size()) break;
        const std::array<int, 3> t = tris[flat];
        const int a = t[static_cast<std::size_t>(e)], b = t[static_cast<std::size_t>((e + 1) % 3)],
                  m = t[static_cast<std::size_t>((e + 2) % 3)];
        std::size_t across = tris.size();
        int d = -1;
        for (std::size_t i = 0; i < tris.size() && across == tris.size(); ++i)
            for (int k = 0; k < 3; ++k)
                if (tris[i][static_cast<std::size_t>(k)] == b && tris[i][static_cast<std::size_t>((k + 1) % 3)] == a) {
                    across = i;
                    d = tris[i][static_cast<std::size_t>((k + 2) % 3)];
                }
        if (across == tris.size()) {
            error = "a flat triangle on the edge of a face";
            return;
        }
        tris[across] = {b, m, d};
        tris[flat]   = {m, a, d};
    }

    auto emit = [&](int a, int b, int c) { face(up ? std::vector<int>{a, b, c} : std::vector<int>{c, b, a}); };
    for (const std::array<int, 3>& c3 : tris) {
        const std::vector<int>* run3[3] = {};
        int withRuns = 0;
        for (int e = 0; e < 3; ++e) {
            const auto it = between.find({c3[e], c3[(e + 1) % 3]});
            if (it != between.end()) {
                run3[e] = &it->second;
                ++withRuns;
            }
        }
        if (withRuns == 0) {
            emit(c3[0], c3[1], c3[2]);
            continue;
        }
        // The triangle's outline with the corners put back.
        std::vector<int> ring;
        for (int e = 0; e < 3; ++e) {
            ring.push_back(c3[e]);
            if (run3[e]) ring.insert(ring.end(), run3[e]->begin(), run3[e]->end());
        }
        if (withRuns == 1) {
            // A fan from the corner across the edge that was cut.
            int e = 0;
            while (!run3[e]) ++e;
            const int apex = c3[(e + 2) % 3];
            std::vector<int> side{c3[e]};
            side.insert(side.end(), run3[e]->begin(), run3[e]->end());
            side.push_back(c3[(e + 1) % 3]);
            for (std::size_t s = 0; s + 1 < side.size(); ++s) emit(apex, side[s], side[s + 1]);
        } else {
            DV3 g(0.0);
            for (int v : c3) g += verts[static_cast<std::size_t>(v)];
            const int mid = vid(g / 3.0);
            for (std::size_t s = 0; s < ring.size(); ++s) emit(mid, ring[s], ring[(s + 1) % ring.size()]);
        }
    }
}

// --- Rounded rectangles across Z ------------------------------------------------------

void Splits::tidy() {
    for (std::vector<double>* v : {&front, &right, &back, &left}) {
        std::sort(v->begin(), v->end());
        v->erase(std::unique(v->begin(), v->end(), [](double a, double b) { return b - a < 1e-7; }), v->end());
    }
}

Outline rrect(Builder& b, const Rect& q, double z, const Splits* s) {
    Outline out;
    const double r = q.r < 0.02 ? 0.0 : q.r;
    auto arc = [&](int c, double cx, double cy, double a0) {
        out.arc[static_cast<std::size_t>(c)] = out.ids.size();
        for (int i = 0; i <= kArc; ++i) {
            const double a = a0 + 0.5 * kPi * static_cast<double>(i) / kArc;
            out.ids.push_back(b.vid({cx + r * std::cos(a), cy + r * std::sin(a), z}));
        }
    };
    auto side = [&](const std::vector<double>* at, double lo, double hi, bool rising, auto&& point) {
        if (!at) return;
        std::vector<double> v;
        for (double t : *at)
            if (t > lo + 1e-6 && t < hi - 1e-6) v.push_back(t);
        if (!rising) std::reverse(v.begin(), v.end());
        for (double t : v) out.ids.push_back(b.vid(point(t)));
    };
    arc(0, q.x1 - r, q.y0 + r, -0.5 * kPi);
    side(s ? &s->right : nullptr, q.y0 + r, q.y1 - r, true, [&](double t) { return DV3(q.x1, t, z); });
    arc(1, q.x1 - r, q.y1 - r, 0.0);
    side(s ? &s->back : nullptr, q.x0 + r, q.x1 - r, false, [&](double t) { return DV3(t, q.y1, z); });
    arc(2, q.x0 + r, q.y1 - r, 0.5 * kPi);
    side(s ? &s->left : nullptr, q.y0 + r, q.y1 - r, false, [&](double t) { return DV3(q.x0, t, z); });
    arc(3, q.x0 + r, q.y0 + r, kPi);
    side(s ? &s->front : nullptr, q.x0 + r, q.x1 - r, true, [&](double t) { return DV3(t, q.y0, z); });
    return out;
}

void loft(Builder& b, const Outline& lo, const Outline& hi) {
    const std::size_t na = lo.ids.size(), nb = hi.ids.size();
    for (int c = 0; c < 4; ++c) {
        const std::size_t a0 = lo.arc[static_cast<std::size_t>(c)], b0 = hi.arc[static_cast<std::size_t>(c)];
        for (int m = 0; m < kArc; ++m)
            b.face({lo.ids[a0 + m], lo.ids[a0 + m + 1], hi.ids[b0 + m + 1], hi.ids[b0 + m]});
        // The side after this corner, end of corner to start of the next.
        const std::size_t a1 = c == 3 ? na : lo.arc[static_cast<std::size_t>(c + 1)];
        const std::size_t b1 = c == 3 ? nb : hi.arc[static_cast<std::size_t>(c + 1)];
        std::vector<int> L, U;
        for (std::size_t k = a0 + kArc; k <= a1; ++k) L.push_back(lo.ids[k % na]);
        for (std::size_t k = b0 + kArc; k <= b1; ++k) U.push_back(hi.ids[k % nb]);
        if (L.size() == 2 && U.size() == 2) {
            b.face({L[0], L[1], U[1], U[0]});
            continue;
        }
        static const DV2 dir[4] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
        auto along = [&](int v) { return glm::dot(DV2(b.verts[static_cast<std::size_t>(v)]), dir[c]); };
        std::size_t i = 0, j = 0;
        while (i + 1 < L.size() || j + 1 < U.size()) {
            const bool lower = j + 1 >= U.size() || (i + 1 < L.size() && along(L[i + 1]) <= along(U[j + 1]));
            if (lower) {
                b.face({L[i], L[i + 1], U[j]});
                ++i;
            } else {
                b.face({L[i], U[j + 1], U[j]});
                ++j;
            }
        }
    }
}

Ring circle(Builder& b, DV2 c, double r, double z, bool clockwise) {
    Ring out;
    for (int i = 0; i < kHole; ++i) {
        const double a = (clockwise ? -2.0 : 2.0) * kPi * static_cast<double>(i) / kHole;
        out.push_back(b.vid({c.x + r * std::cos(a), c.y + r * std::sin(a), z}));
    }
    return out;
}

Ring reversed(const Ring& r) { return Ring(r.rbegin(), r.rend()); }

// The corners of `ring` that lie on the sides of the rectangle (half sizes A, B,
// inset by d), as splits for it.
void gatherSplits(const Builder& b, const Ring& ring, double A, double B, double d, Splits& s) {
    for (int v : ring) {
        const DV3& p = b.verts[static_cast<std::size_t>(v)];
        if (std::abs(p.y - (-B + d)) < 1e-7) s.front.push_back(p.x);
        if (std::abs(p.y - (B - d)) < 1e-7) s.back.push_back(p.x);
        if (std::abs(p.x - (-A + d)) < 1e-7) s.left.push_back(p.y);
        if (std::abs(p.x - (A - d)) < 1e-7) s.right.push_back(p.y);
    }
}

std::vector<glm::vec2> plan2(const Builder& b, const Ring& r) {
    std::vector<glm::vec2> out;
    for (int v : r) {
        const glm::vec2 p(b.verts[static_cast<std::size_t>(v)]);
        if (out.empty() || out.back() != p) out.push_back(p);
    }
    if (out.size() > 1 && out.front() == out.back()) out.pop_back();
    return out;
}

double area2(const Builder& b, const Ring& r) {
    double a = 0.0;
    for (std::size_t i = 0; i < r.size(); ++i) {
        const DV3& p = b.verts[static_cast<std::size_t>(r[i])];
        const DV3& q = b.verts[static_cast<std::size_t>(r[(i + 1) % r.size()])];
        a += p.x * q.y - q.x * p.y;
    }
    return 0.5 * a;
}


// --- Holes and extrusions ------------------------------------------------------------

std::vector<Ring> Builder::openings(int axis, double at, const std::vector<DV2>& face) const {
    std::vector<Ring> out;
    for (const Bored& h : bored) {
        if (h.axis != axis || std::abs(h.at - at) > 1e-6) continue;
        bool in = false;
        for (std::size_t i = 0, j = face.size() - 1; i < face.size(); j = i++) {
            const DV2& a = face[i];
            const DV2& c = face[j];
            if ((a.y > h.middle.y) != (c.y > h.middle.y) &&
                h.middle.x < (c.x - a.x) * (h.middle.y - a.y) / (c.y - a.y) + a.x)
                in = !in;
        }
        if (in) out.push_back(h.ring);
    }
    return out;
}

namespace {

// A rounded rectangle in a plane's (u, v), counter-clockwise: kArc + 1 corners a
// corner, as many whatever its size.
std::vector<DV2> roundedRect2(DV2 c, double hx, double hy, double r) {
    r = std::clamp(r, 0.0, std::min(hx, hy));
    std::vector<DV2> out;
    const DV2 centres[4] = {{c.x + hx - r, c.y - hy + r}, {c.x + hx - r, c.y + hy - r},
                            {c.x - hx + r, c.y + hy - r}, {c.x - hx + r, c.y - hy + r}};
    for (int k = 0; k < 4; ++k)
        for (int i = 0; i <= kArc; ++i) {
            const double a = (k - 1) * 0.5 * kPi + 0.5 * kPi * static_cast<double>(i) / kArc;
            out.push_back(centres[k] + r * DV2(std::cos(a), std::sin(a)));
        }
    return out;
}

} // namespace

void bore(Builder& b, const Hole& h) {
    if (h.steps.size() < 2) return;
    // Bored along +axis the outlines run clockwise, along -axis counter-
    // clockwise: either way the walls face into the hole.
    const bool rising = h.steps.back().at > h.steps.front().at;
    std::vector<Ring> rings;
    for (const Hole::Step& s : h.steps) {
        std::vector<DV2> pts;
        if (s.ry <= 0.0) {
            for (int i = 0; i < kHole; ++i) {
                const double a = 2.0 * kPi * static_cast<double>(i) / kHole;
                pts.push_back(h.middle + s.rx * DV2(std::cos(a), std::sin(a)));
            }
        } else {
            pts = roundedRect2(h.middle, s.rx, s.ry, s.rc);
        }
        if (rising) std::reverse(pts.begin(), pts.end());
        Ring r;
        for (const DV2& p : pts) r.push_back(b.vid(world(h.axis, p, s.at)));
        rings.push_back(std::move(r));
    }
    for (std::size_t k = 0; k + 1 < rings.size(); ++k) b.loft(rings[k], rings[k + 1]);
    if (!h.through) b.cap(rings.back(), true);
    auto ccw = [&](const Ring& r) { return rising ? reversed(r) : r; };
    b.bored.push_back({h.axis, h.steps.front().at, h.middle, ccw(rings.front())});
    if (h.through) b.bored.push_back({h.axis, h.steps.back().at, h.middle, ccw(rings.back())});
}

std::vector<Ring> extrude(Builder& b, const Extrusion& e) {
    const std::size_t L = e.at.size();
    if (L < 2 || e.outline.size() != L || e.outline[0].size() < 3) {
        b.error = "an extrusion without two outlines";
        return {};
    }
    const std::size_t n = e.outline[0].size();
    // Counter-clockwise in (u, v), whichever way it was drawn.
    double area = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const DV2& p = e.outline[0][i];
        const DV2& q = e.outline[0][(i + 1) % n];
        area += p.x * q.y - q.x * p.y;
    }
    const bool flip = area < 0.0;
    auto pt = [&](std::size_t k, std::size_t i) -> const DV2& { return e.outline[k][flip ? n - 1 - i : i]; };
    std::vector<Ring> rings(L);
    for (std::size_t k = 0; k < L; ++k)
        for (std::size_t i = 0; i < n; ++i) rings[k].push_back(b.vid(world(e.axis, pt(k, i), e.at[k])));

    const int U = e.axis == 2 ? 0 : e.axis == 0 ? 1 : 2;   // the world axes of u and v
    const int V = e.axis == 2 ? 1 : e.axis == 0 ? 2 : 0;
    auto loopIn = [&](int axis, const Ring& loop) {
        std::vector<DV2> poly;
        for (int id : loop) poly.push_back(plane(axis, b.verts[static_cast<std::size_t>(id)]));
        return poly;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t j = (i + 1) % n;
        const DV2 p = pt(0, i), q = pt(0, j);
        if (e.glued && e.glued(p, q)) continue;
        bool fixed = true;
        for (std::size_t k = 1; k < L && fixed; ++k) fixed = pt(k, i) == p && pt(k, j) == q;
        // (A hair's tolerance: a rounded corner's end comes out of a sine.)
        const bool alongU = fixed && std::abs(p.y - q.y) < 1e-9 && std::abs(p.x - q.x) > 1e-9;
        const bool alongV = fixed && std::abs(p.x - q.x) < 1e-9 && std::abs(p.y - q.y) > 1e-9;
        if (alongU || alongV) {
            // One flat face from the first level to the last, its sides cut at
            // every level between.
            Ring loop{rings[0][i], rings[0][j]};
            for (std::size_t k = 1; k < L; ++k) loop.push_back(rings[k][j]);
            for (std::size_t k = L - 1; k >= 1; --k) loop.push_back(rings[k][i]);
            const int across = alongU ? V : U;
            const double at  = alongU ? p.y : p.x;
            // Outward from a counter-clockwise outline: (dv, -du).
            const bool positive = alongU ? q.x < p.x : q.y > p.y;
            b.region({loop}, b.openings(across, at, loopIn(across, loop)), across, positive);
        } else {
            for (std::size_t k = 0; k + 1 < L; ++k)
                b.face({rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i]});
        }
    }
    if (e.capLo) b.region({rings.front()}, b.openings(e.axis, e.at.front(), loopIn(e.axis, rings.front())), e.axis, false);
    if (e.capHi) b.region({rings.back()}, b.openings(e.axis, e.at.back(), loopIn(e.axis, rings.back())), e.axis, true);
    return rings;
}

// --- Cut-away and hand-over ----------------------------------------------------------

std::vector<std::vector<glm::vec2>> section(const EditMesh& m, float x) {
    auto P = [&](int i) {
        const glm::vec3& v = m.verts[static_cast<std::size_t>(i)];
        return glm::vec3(v.x, -v.z, v.y) * 1000.0f;
    };
    std::vector<std::pair<glm::vec2, glm::vec2>> segs;
    for (const std::vector<int>& f : m.faces) {
        glm::vec2 hit[4];
        int n = 0;
        for (std::size_t k = 0; k < f.size() && n < 4; ++k) {
            const glm::vec3 a = P(f[k]), b = P(f[(k + 1) % f.size()]);
            if ((a.x < x) == (b.x < x)) continue;
            const glm::vec3 p = a + (b - a) * ((x - a.x) / (b.x - a.x));
            hit[n++] = glm::vec2(p.y, p.z);
        }
        if (n >= 2) segs.emplace_back(hit[0], hit[1]);
    }
    auto key = [](glm::vec2 p) { return std::make_pair(std::lround(p.x * 1000.0f), std::lround(p.y * 1000.0f)); };
    std::multimap<std::pair<long, long>, std::size_t> ends;
    for (std::size_t i = 0; i < segs.size(); ++i) {
        ends.emplace(key(segs[i].first), i);
        ends.emplace(key(segs[i].second), i);
    }
    std::vector<bool> used(segs.size(), false);
    std::vector<std::vector<glm::vec2>> loops;
    for (std::size_t s = 0; s < segs.size(); ++s) {
        if (used[s]) continue;
        used[s] = true;
        std::vector<glm::vec2> loop{segs[s].first, segs[s].second};
        for (;;) {
            const auto range = ends.equal_range(key(loop.back()));
            std::size_t next = segs.size();
            for (auto it = range.first; it != range.second; ++it)
                if (!used[it->second]) { next = it->second; break; }
            if (next == segs.size()) break;
            used[next] = true;
            const bool fwd = key(segs[next].first) == key(loop.back());
            loop.push_back(fwd ? segs[next].second : segs[next].first);
        }
        // Once round, without the corner it came back to, the points too close
        // to tell apart and those on a straight run: what a polygon filler wants.
        std::vector<glm::vec2> clean;
        for (glm::vec2 v : loop)
            if (clean.empty() || glm::length(v - clean.back()) > 1e-3f) clean.push_back(v);
        while (clean.size() > 1 && glm::length(clean.front() - clean.back()) <= 1e-3f) clean.pop_back();
        for (bool again = true; again && clean.size() > 3;) {
            again = false;
            for (std::size_t i = 0; i < clean.size(); ++i) {
                const glm::vec2 a = clean[(i + clean.size() - 1) % clean.size()], b = clean[i],
                                c = clean[(i + 1) % clean.size()];
                if (std::abs((b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x)) < 1e-6f) {
                    clean.erase(clean.begin() + static_cast<std::ptrdiff_t>(i));
                    again = true;
                    break;
                }
            }
        }
        if (clean.size() >= 3) loops.push_back(std::move(clean));
    }
    return loops;
}

EditMesh toEditMesh(const Builder& b) {
    EditMesh m;
    m.verts.reserve(b.verts.size());
    for (const DV3& v : b.verts)
        m.verts.emplace_back(static_cast<float>(v.x * 1e-3), static_cast<float>(v.z * 1e-3),
                             static_cast<float>(-v.y * 1e-3));
    m.faces = b.faces;
    return m;
}

double volume(const Builder& b) {
    double v = 0.0;
    for (const std::vector<int>& f : b.faces)
        for (std::size_t k = 1; k + 1 < f.size(); ++k)
            v += glm::dot(b.verts[static_cast<std::size_t>(f[0])],
                          glm::cross(b.verts[static_cast<std::size_t>(f[k])], b.verts[static_cast<std::size_t>(f[k + 1])])) / 6.0;
    return v;
}

} // namespace solidbuild
