// The prelude: the library every Strahl script starts with -- colours,
// materials, patterns, bump maps and compound shapes -- written in Strahl
// itself. So everything here is also an example of the language, and every
// pattern compiles for the tracer exactly as one of the script's own would:
// nothing in this file is privileged.
//
// Patterns take (p, n, uv) with the last two defaulted, and read their colour
// arguments through at(): a colour is a colour, a function is asked at the
// point. That is what lets patterns nest -- checker(marble(), wood()).

#include "StrahlInternal.hpp"

namespace strahl {

const char* preludeSource() {
    // Split in parts: one string literal of this size is past what MSVC accepts.
    static const std::string src = std::string(R"STRAHL(
// --- Colours (sRGB, as a colour picker shows them) ---------------------------
let white   = #ffffff
let black   = #000000
let red     = #c8282a
let green   = #3a9a3c
let blue    = #2a56c8
let yellow  = #f2d02c
let orange  = #f08424
let cyan    = #2cc4d4
let magenta = #c43a9c
let purple  = #74389c
let brown   = #7a4a28
let pink    = #f0a0b4
let ivory   = #f4eee0

// Hue in degrees, saturation and value 0..1.
fn hsv(h, s = 1, v = 1) {
    let k = mod(h / 60 + [5, 3, 1], 6)
    v - v * s * max(min(min(k, 4 - k), 1), 0)
}

// The colour of a black body at `k` kelvin, normalised: 1800 a candle,
// 2700 a household bulb, 5500 daylight, 7500 an overcast sky.
fn kelvin(k) {
    let t = clamp(k, 1000, 40000) / 100
    let r = if t <= 66 { 1 } else { clamp(1.292936186 * pow(t - 60, -0.1332047592), 0, 1) }
    let g = if t <= 66 { clamp(0.390081579 * log(t) - 0.631841444, 0, 1) }
            else { clamp(1.129890861 * pow(t - 60, -0.0755148492), 0, 1) }
    let b = if t >= 66 { 1 } else { if t <= 19 { 0 } else { clamp(0.543206789 * log(t - 10) - 1.196254089, 0, 1) } }
    [r, g, b]
}

// --- Materials ---------------------------------------------------------------
// A dull surface: chalk, paper, unglazed clay.
fn matte(color = #bfbfbf) = material { color: color, roughness: 0.95 }
// Coloured plastic with a clear coat; `roughness` 0 is a mirror finish.
fn plastic(color = #bfbfbf, roughness = 0.3) = material { color: color, roughness: roughness }
// A metal: `color` tints its reflection, `roughness` blurs it.
fn metal(color = #c8c8cc, roughness = 0.12) = material { color: color, metallic: 1, roughness: roughness }
// Glass with refraction. `absorption` (per metre and channel) colours it by
// thickness: thin glass stays light, thick glass turns deep.
fn glass(color = white, ior = 1.5, absorption = 0) =
    material { glass: true, color: color, ior: ior, absorption: absorption }
// A surface that glows: lights the scene only through bounces (a light { }
// with a radius is the quieter lamp).
fn emissive(color = white, strength = 5) = material { color: black, emission: color, strength: strength }

let chrome    = metal(#e2e2e6, 0.02)
let mirror    = metal(white, 0)
let silver    = metal(#fbf8f2, 0.08)
let gold      = metal(#ffd890, 0.12)
let copper    = metal(#fad0c0, 0.16)
let brass     = metal(#f4e2aa, 0.2)
let steel     = metal(#c4c6c8, 0.25)
let aluminium = metal(#f4f4f4, 0.3)
let water     = glass(white, 1.33)
let crystal   = glass(white, 1.55)
let diamond   = glass(white, 2.42)

// --- Patterns: functions of the point, used as a colour ----------------------
// Colours or patterns in a 3D chequerboard of `size`-metre cells.
fn checker(a = white, b = black, size = 1) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) => mix(at(a, p, n, uv), at(b, p, n, uv), parity(p / size))

// Bands across `axis`, each `width` wide.
fn stripes(a = white, b = black, width = 0.5, axis = [1, 0, 0]) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) =>
        mix(at(a, p, n, uv), at(b, p, n, uv), step(0.5, fract(dot(p, axis) / (2 * width))))

// Rings around the y axis.
fn rings(a = white, b = black, width = 0.25) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) =>
        mix(at(a, p, n, uv), at(b, p, n, uv), step(0.5, fract(length([p.x, p.z]) / (2 * width))))

// From `a` to `b` along `axis`, between the distances `from` and `to`.
fn gradient(a = black, b = white, from = 0, to = 1, axis = [0, 1, 0]) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) => mix(at(a, p, n, uv), at(b, p, n, uv), smoothstep(from, to, dot(p, axis)))

// A cloudy blend of two colours.
fn clouds(a = white, b = #7a8ca0, scale = 1, octaves = 5) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) => mix(at(a, p, n, uv), at(b, p, n, uv), 0.5 + 0.5 * fbm(p / scale, octaves))

// Marble: thin dark veins wandering through a pale stone.
fn marble(base = #ebe6dd, vein = #46474e, scale = 0.5, swirl = 2.5, sharpness = 6) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) {
        let q = p / scale
        let t = 0.5 + 0.5 * sin((q.x + 0.3 * q.y + swirl * turbulence(q, 6)) * 180)
        mix(at(base, p, n, uv), at(vein, p, n, uv), pow(1 - t, sharpness))
    }

// Wood: growth rings around the y axis, wobbling, with grain along them.
fn wood(light = #d6a66c, dark = #8a582e, scale = 0.04, swirl = 0.5) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) {
        let q = p / scale
        let r = length([q.x, q.z]) + swirl * noise(q * [0.6, 0.08, 0.6])
        let t = smoothstep(0.15, 0.85, fract(r))
        let grain = 0.85 + 0.15 * noise(q * [3, 40, 3])
        mix(at(light, p, n, uv), at(dark, p, n, uv), t) * grain
    }

// Granite: speckled grains in a mottled ground.
fn granite(a = #c2bab0, b = #2e2c2b, c = #8e7f73, scale = 0.012) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) {
        let q = p / scale
        let ground = mix(at(a, p, n, uv), at(c, p, n, uv), smoothstep(0.35, 0.75, 0.5 + 0.5 * fbm(q * 0.25, 3)))
        mix(ground, at(b, p, n, uv), step(0.78, cell_id(q)))
    }

// Round spots of `b` on `a`, about `size` across.
fn spots(a = white, b = black, size = 0.2, amount = 0.35) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) =>
        mix(at(a, p, n, uv), at(b, p, n, uv), 1 - smoothstep(amount - 0.04, amount + 0.04, voronoi(p / size)))

// A brick wall in the x-y plane (rotate the material for other walls):
// running bond, `size` = [length, height], a mortar `gap` and a little
// variation brick to brick.
fn bricks(brick = #9c4a34, mortar = #d4cbbd, size = [0.24, 0.075], gap = 0.01) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) {
        let row = floor(p.y / size.y)
        let u = p.x / size.x + 0.5 * mod(row, 2)
        let du = min(fract(u), 1 - fract(u)) * size.x
        let dv = min(fract(p.y / size.y), 1 - fract(p.y / size.y)) * size.y
        let joint = 1 - step(gap * 0.5, min(du, dv))
        let tone = 0.78 + 0.4 * hash([floor(u), row, 0])
        mix(at(brick, p, n, uv) * tone, at(mortar, p, n, uv), joint)
    }

// Square tiles on the floor (the x-z plane), alternating a and b, with grout.
fn tiles(a = #e6e1d8, b = #e6e1d8, grout = #8a847a, size = 0.3, gap = 0.006) =
    fn(p, n = [0, 1, 0], uv = [0, 0]) {
        let u = p.x / size
        let v = p.z / size
        let du = min(fract(u), 1 - fract(u)) * size
        let dv = min(fract(v), 1 - fract(v)) * size
        let tile = mix(at(a, p, n, uv), at(b, p, n, uv), parity([floor(u), 0, floor(v)]))
        mix(tile, at(grout, p, n, uv), 1 - step(gap * 0.5, min(du, dv)))
    }
)STRAHL") + std::string(R"STRAHL(
// A picture, laid on the surface by its uv (a sphere's, a box face's, a
// mesh's own). `scale` repeats it.
fn image(file, scale = [1, 1]) {
    let img = load_image(file)
    fn(p, n = [0, 1, 0], uv = [0, 0]) => texture(img, uv * scale)
}

// A picture projected straight down onto the x-z plane, `size` metres a tile.
fn planar(file, size = 1) {
    let img = load_image(file)
    fn(p, n = [0, 1, 0], uv = [0, 0]) => texture(img, [p.x, -p.z] / size)
}

// --- Bump maps: heights in metres, for material { bump: ... } ----------------
// Soft noise bumps, `scale` metres across: hammered metal, orange peel.
fn bumps(amount = 0.004, scale = 0.05) = fn(p) => amount * noise(p / scale)
// Sharp creases: crumpled foil, leather.
fn wrinkles(amount = 0.002, scale = 0.04) = fn(p) => amount * turbulence(p / scale, 5)
// Round dents about `size` across: beaten copper, golf ball.
fn dents(amount = 0.003, size = 0.05) = fn(p) => amount * min(voronoi(p / size), 1)
// Rings spreading from `center`, `wavelength` apart: a drop in water.
fn ripples(amount = 0.003, wavelength = 0.12, center = [0, 0, 0]) =
    fn(p) => amount * sin(distance(p, center) / wavelength * 360)
// Rolling waves across the x-z plane, for a water surface.
fn waves(amount = 0.02, scale = 1) = fn(p) => amount * fbm([p.x, 0, p.z] / scale, 4)

// --- Compound shapes ---------------------------------------------------------
// A box with its edges rounded off by `radius`: an exact distance field.
fn rounded_box(size = 1, radius = 0.1, center = [0, 0, 0]) {
    let h = size / 2 - radius
    isosurface {
        function: fn(p) {
            let q = abs(p - center) - h
            length(max(q, 0)) + min(max(q.x, max(q.y, q.z)), 0) - radius
        }
        min: center - size / 2 - 0.01
        max: center + size / 2 + 0.01
        max_gradient: 1
    }
}

// A cylinder with a hemisphere on each end.
fn capsule(from = [0, 0, 0], to = [0, 1, 0], radius = 0.25) =
    merge { sphere(from, radius); sphere(to, radius); cylinder(from, to, radius) }

// Metaballs: spheres of influence around `centers` that run together.
fn blob(centers, radius = 0.5, threshold = 0.4) {
    let lo = min(centers) - radius
    let hi = max(centers) + radius
    isosurface {
        function: fn(p) {
            let s = 0
            for c in centers {
                let d = distance(p, c) / radius
                s += max(1 - d * d, 0) ^ 2
            }
            threshold - s
        }
        min: lo
        max: hi
    }
}

// A surface of revolution: `points` is the profile, [radius, height] pairs
// from the bottom up, turned around the y axis.
fn lathe(points, segments = 64) {
    let vs = []
    let fs = []
    let m = len(points)
    for i in 0..segments {
        let a = i * 360 / segments
        for pt in points {
            push(vs, [pt[0] * cos(a), pt[1], pt[0] * sin(a)])
        }
    }
    for i in 0..segments {
        let j = (i + 1) % segments
        for k in 0..m - 1 {
            push(fs, [i * m + k, i * m + k + 1, j * m + k + 1, j * m + k])
        }
    }
    mesh { vertices: vs, faces: fs, smooth: true }
}

// A height field: y = function(x, z) over `size` [width, depth] metres
// centred on the origin, `resolution` cells across.
fn heightfield(function, size = [10, 10], resolution = 128) {
    let vs = []
    let fs = []
    let n = resolution
    for j in 0..=n {
        for i in 0..=n {
            let px = (i / n - 0.5) * size[0]
            let pz = (j / n - 0.5) * size[1]
            push(vs, [px, function(px, pz), pz])
        }
    }
    for j in 0..n {
        for i in 0..n {
            let a = j * (n + 1) + i
            push(fs, [a, a + n + 1, a + n + 2, a + 1])
        }
    }
    mesh { vertices: vs, faces: fs, smooth: true }
}
)STRAHL");
    return src.c_str();
}

} // namespace strahl
