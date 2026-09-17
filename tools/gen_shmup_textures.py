"""Generate the textures SKYSTRIKE (sandbox/scripts/shmup.lua) draws with.

  shmup_cloud_[a|b|c].png   a cumulus seen from above: lit crowns, blue-grey
                            creases, frayed see-through rims (RGBA)
  shmup_haze.png            a thin, stretched wisp for the high layer (RGBA)
  shmup_sea.png             open water, tiles seamlessly (RGB)
  shmup_sea_n.png           its wave normals, same tiling (RGB, GL convention)
  shmup_island_[a|b|c].png  an island from the air: lagoon, surf, beach, jungle
                            canopy, hills -- the lagoon fades out to nothing (RGBA)
  shmup_smoke.png           a soft smoke puff (RGBA)
  shmup_fire.png            a fireball, used as colour AND emission (RGBA)
  shmup_ring.png            a shock ring, colour AND emission (RGBA)
  shmup_puff.png            a soft white puff: contrails, wakes, spray (RGBA)
  shmup_splash.png          something hitting the water, from above (RGBA)
  shmup_storm_[a|b].png     dark storm cells for the STORM FRONT stage (RGBA)
  shmup_sea_storm.png       the same waves under a storm: grey-green, foam-streaked
  shmup_sea_front[_out].png normal sea into storm sea (and back), for the row
                            that carries the weather front across the screen
  shmup_land.png            farmland from the air: fields, hedges, woods, farms
  shmup_land_road.png       the same with a road down the middle
  shmup_coast[_out].png     sea into land (and land back into sea)

The ground tiles (sea, storm sea, land, coast, fronts) all share one 20 m tile
and are built from the same wave field and the same farmland, so any two of
them meet without a seam as the ground scrolls from one kind into the next.

The camera looks straight down on everything, so every one of these is a
picture of the thing from above, drawn on a flat quad. The light is baked in
from the upper left, which is where the scene's sun sits in the default shmup
scene; the engine's own sun still lights the quads on top of that.

Albedo is kept in the range the game's materials use (whites near 0.6): the sea
is dark, auto exposure opens up for it, and a pale texture then blooms.

Needs numpy (the PNGs are written with zlib from the standard library).

    python tools/gen_shmup_textures.py content/textures

(content/textures is not in the repository -- run this once; without the
files the game falls back to plain, untextured materials.)
"""
import os
import struct
import sys
import zlib

import numpy as np


# --- PNG ---------------------------------------------------------------------

def write_png(path, img):
    """img: float array HxWx3 or HxWx4 in 0..1."""
    img = np.clip(img, 0.0, 1.0)
    h, w, c = img.shape
    data = (img * 255.0 + 0.5).astype(np.uint8)
    raw = b"".join(b"\x00" + data[y].tobytes() for y in range(h))
    ctype = 6 if c == 4 else 2

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body +
                struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ctype, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))
    print(f"{path}: {w}x{h}")


# --- Noise -------------------------------------------------------------------

def spectral(n, beta, rng, aniso=(1.0, 1.0), lo=1.0):
    """Tileable fractal noise by filtering white noise in frequency space.

    beta sets the falloff (bigger = smoother); aniso stretches it along x/y.
    The result tiles because the FFT's basis does. Normalised to -1..1.
    """
    fx = np.fft.fftfreq(n)[None, :] * n / aniso[0]
    fy = np.fft.fftfreq(n)[:, None] * n / aniso[1]
    f = np.sqrt(fx * fx + fy * fy)
    f[0, 0] = 1.0
    amp = 1.0 / np.power(np.maximum(f, lo), beta * 0.5)
    amp[0, 0] = 0.0
    phase = rng.standard_normal((n, n)) + 1j * rng.standard_normal((n, n))
    out = np.real(np.fft.ifft2(phase * amp))
    out -= out.mean()
    return out / (np.abs(out).max() + 1e-9)


def band(n, k0, kw, rng, direction=0.0, spread=2):
    """Tileable waves: noise whose spectrum is a ring around k0 cycles per tile,
    squeezed toward `direction` (radians) -- crests run across that direction."""
    fx = np.fft.fftfreq(n)[None, :] * n
    fy = np.fft.fftfreq(n)[:, None] * n
    k = np.sqrt(fx * fx + fy * fy)
    ang = np.arctan2(fy, fx) - direction
    amp = np.exp(-((k - k0) / kw) ** 2) * np.abs(np.cos(ang)) ** spread
    amp[0, 0] = 0.0
    phase = rng.standard_normal((n, n)) + 1j * rng.standard_normal((n, n))
    out = np.real(np.fft.ifft2(phase * amp))
    out -= out.mean()
    return out / (np.abs(out).max() + 1e-9)


def blur(a, px):
    """Gaussian blur, periodic (FFT)."""
    n0, n1 = a.shape
    fy = np.fft.fftfreq(n0)[:, None]
    fx = np.fft.fftfreq(n1)[None, :]
    g = np.exp(-2.0 * (np.pi * px) ** 2 * (fx * fx + fy * fy))
    return np.real(np.fft.ifft2(np.fft.fft2(a) * g))


def smooth(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def grid(n):
    y, x = np.mgrid[0:n, 0:n].astype(np.float64)
    return (x + 0.5) / n, (y + 0.5) / n


def blobs(n, rng, count, spread, size, stretch=(1.0, 1.0)):
    """A lumpy mass: gaussians scattered around the middle."""
    x, y = grid(n)
    d = np.zeros((n, n))
    for _ in range(count):
        cx = 0.5 + rng.uniform(-spread, spread) * stretch[0]
        cy = 0.5 + rng.uniform(-spread, spread) * stretch[1]
        s = rng.uniform(*size)
        d += rng.uniform(0.6, 1.0) * np.exp(-(((x - cx) / stretch[0]) ** 2 +
                                              ((y - cy) / stretch[1]) ** 2) / (2 * s * s))
    return d / d.max()


def shade(h, strength, light=(-0.55, -0.62, 0.56), periodic=False):
    """Lambert of a height field lit from the upper left (image y runs down).
    periodic: take the slopes across the wrap too, for textures that tile."""
    if periodic:
        gx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5
        gy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5
    else:
        gy, gx = np.gradient(h)
    nx, ny, nz = -gx * strength, -gy * strength, np.ones_like(h)
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    l = np.array(light) / np.linalg.norm(light)
    return np.clip((nx * l[0] + ny * l[1] + nz * l[2]) * inv, 0.0, 1.0)


def mix(a, b, t):
    t = t[..., None] if np.ndim(t) == 2 else t
    return a + (b - a) * t


def rgb(*c):
    return np.array(c, dtype=np.float64)


# --- Clouds ------------------------------------------------------------------

def puff(h, cx, cy, cz, rad):
    """Raise h to a dome of radius rad whose centre sits at height cz."""
    n = h.shape[0]
    x0, x1 = max(0, int(cx - rad)), min(n, int(cx + rad) + 1)
    y0, y1 = max(0, int(cy - rad)), min(n, int(cy + rad) + 1)
    if x0 >= x1 or y0 >= y1:
        return
    yy, xx = np.mgrid[y0:y1, x0:x1]
    d2 = (xx - cx) ** 2 + (yy - cy) ** 2
    dome = cz + np.sqrt(np.maximum(rad * rad - d2, 0.0))
    dome[d2 > rad * rad] = -1e9
    np.maximum(h[y0:y1, x0:x1], dome, out=h[y0:y1, x0:x1])


def self_shadow(h, light_xy, rise, steps=28, step=3.0):
    """How much of the sky toward the light each pixel sees over the heights:
    marched along the light's direction, softened by how far the blocker is."""
    n = h.shape[0]
    pad = int(steps * step) + 2
    hp = np.pad(h, pad, mode="constant", constant_values=0.0)
    lit = np.ones_like(h)
    for i in range(1, steps + 1):
        t = i * step
        dx, dy = int(round(light_xy[0] * t)), int(round(light_xy[1] * t))
        ahead = hp[pad + dy:pad + dy + n, pad + dx:pad + dx + n]
        over = ahead - (h + t * rise)
        lit = np.minimum(lit, np.clip(1.0 - over / (4.0 + t * 0.35), 0.0, 1.0))
    return lit


def cloud(seed, n=512, crown=(0.66, 0.67, 0.68), lee=(0.27, 0.32, 0.44),
          rim=(0.46, 0.50, 0.57)):
    """A cumulus from above: towers of rounded domes, crowns lit from the upper
    left, the lee sides and the creases between the towers in blue shadow."""
    rng = np.random.default_rng(seed)
    h = np.full((n, n), -1e9)
    ax, ay = rng.uniform(0.9, 1.05), rng.uniform(0.6, 0.8)     # footprint ellipse
    towers = []
    for _ in range(rng.integers(8, 12)):                        # the big towers
        a = rng.uniform(0, 2 * np.pi)
        d = np.sqrt(rng.uniform(0, 1)) * 0.29
        cx = n * (0.5 + np.cos(a) * d * ax)
        cy = n * (0.5 + np.sin(a) * d * ay)
        rad = n * rng.uniform(0.08, 0.15) * (1.0 - 0.8 * d)
        cz = rng.uniform(-0.45, 0.1) * rad
        puff(h, cx, cy, cz, rad)
        towers.append((cx, cy, rad))
    # Bulges on the towers, then smaller ones on those: each generation sits on
    # the surface the one before it left.
    for count, rmin, rmax in ((45, 0.035, 0.07), (120, 0.015, 0.032)):
        for _ in range(count):
            tx, ty, tr = towers[rng.integers(len(towers))]
            a = rng.uniform(0, 2 * np.pi)
            d = rng.uniform(0.3, 1.0) * tr
            cx, cy = tx + np.cos(a) * d, ty + np.sin(a) * d
            ix, iy = int(np.clip(cx, 0, n - 1)), int(np.clip(cy, 0, n - 1))
            if h[iy, ix] < 0:
                continue
            rad = n * rng.uniform(rmin, rmax)
            puff(h, cx, cy, h[iy, ix] - rad * rng.uniform(0.55, 0.85), rad)
    h = np.where(h > 0, h, 0.0)
    # Soften the seams between domes and push the surfaces around with noise,
    # or it reads as a pile of balls instead of a cloud.
    warp = spectral(n, 3.2, rng)
    hs = blur(h, 3.0) + warp * n * 0.02 * smooth(0.0, n * 0.04, h)
    hs = np.maximum(np.maximum(hs, 0.0), blur(h, 10.0) * 0.8)   # no pinholes
    lx, ly = -0.66, -0.52                                        # toward the light
    norm = np.hypot(lx, ly)
    light = (lx / norm, ly / norm)
    lam = blur(shade(hs, 1.0, light=(lx, ly, 0.42)), 0.8)
    sky = self_shadow(hs, light, rise=0.4)
    ao = np.clip(1.0 - np.maximum(blur(hs, 12.0) - hs, 0.0) * 0.06, 0.0, 1.0)
    light_amt = np.clip(lam * (0.25 + 0.75 * sky), 0.0, 1.0)
    crown, lee = rgb(*crown), rgb(*lee)  # sun-side / lit by the sky only
    col = mix(lee, crown, smooth(0.05, 0.85, light_amt))
    col = col * (0.7 + 0.3 * ao[..., None])
    # Rims: thin cloud, frayed by noise and lit through (flatter, bluer).
    fray = spectral(n, 2.0, rng)
    thick = smooth(0.0, n * 0.045, hs + fray * n * 0.012)
    alpha = smooth(0.0, 0.6, thick)
    col = mix(rgb(*rim), col, smooth(0.15, 0.8, thick))
    return np.dstack([col, alpha])


def haze(seed, n=512):
    rng = np.random.default_rng(seed)
    x, y = grid(n)
    streak = spectral(n, 3.0, rng, aniso=(0.25, 1.0))
    fine = spectral(n, 2.0, rng, aniso=(0.4, 1.0))
    env = np.exp(-(((x - 0.5) / 0.3) ** 2 + ((y - 0.5) / 0.16) ** 2))
    dens = env * (0.55 + 0.45 * streak + 0.2 * fine) - 0.12
    alpha = smooth(0.0, 0.5, dens) * 0.85
    col = np.ones((n, n, 3)) * rgb(0.60, 0.62, 0.66)
    return np.dstack([col, alpha])


# --- Sea ---------------------------------------------------------------------

def sea_field(seed, n=1024):
    rng = np.random.default_rng(seed)
    # A wave spectrum rather than fractal noise: from 40 m up the sea is a
    # field of individual waves a few metres long (the tile is 20 m), crests
    # running across the wind, with shorter chop riding on them.
    wind = 0.35
    swell = band(n, 5.0, 2.5, rng, wind, spread=2)
    waves = band(n, 13.0, 5.0, rng, wind + 0.2, spread=2)
    chop = band(n, 32.0, 12.0, rng, wind - 0.3, spread=1)
    return swell, waves, chop


def sea_colour(field, storm=False):
    swell, waves, chop = field
    h = swell * 0.4 + waves * 0.4 + chop * 0.2
    lit = shade(h * 120.0, 1.0, light=(-0.4, -0.5, 0.7), periodic=True)
    if storm:
        deep, mid, foam = rgb(0.05, 0.09, 0.11), rgb(0.10, 0.16, 0.18), rgb(0.42, 0.46, 0.47)
    else:
        deep, mid, foam = rgb(0.025, 0.11, 0.21), rgb(0.04, 0.18, 0.30), rgb(0.46, 0.52, 0.56)
    col = mix(deep, mid, smooth(-0.8, 0.8, swell + 0.3 * waves))
    col = col * (0.8 + 0.34 * lit[..., None])
    # Whitecaps: sparse, where a crest and the chop agree -- everywhere in a gale.
    if storm:
        caps = smooth(0.2, 0.7, waves) * smooth(0.1, 0.7, chop)
        streak = smooth(0.3, 0.9, band_streaks(swell))
        col = mix(col, foam, np.clip(caps * 0.9 + streak * 0.25, 0, 1))
    else:
        caps = smooth(0.45, 0.8, waves) * smooth(0.35, 0.85, chop) * smooth(0.1, 0.6, swell)
        col = mix(col, foam, caps * 0.9)
    return col


def band_streaks(swell):
    """Wind-blown foam lanes: the swell smeared along the wind."""
    n = swell.shape[0]
    acc = np.zeros_like(swell)
    for k in range(-12, 13, 3):
        acc += np.roll(np.roll(swell, k * 3, axis=1), k, axis=0)
    return acc / 9.0


def sea(seed, n=1024):
    field = sea_field(seed, n)
    swell, waves, chop = field
    col = sea_colour(field)
    # Normals: from the same heights, gradients taken periodically so the map
    # tiles like the colour does.
    # Gentle: the engine's sun turns every steep facet into a glint, and a sea
    # of glints reads as crumpled foil. Only the long waves get to tilt.
    hn = blur(swell * 0.7 + waves * 0.3, 2.0) * 2.5
    gx = (np.roll(hn, -1, 1) - np.roll(hn, 1, 1)) * 0.5
    gy = (np.roll(hn, -1, 0) - np.roll(hn, 1, 0)) * 0.5
    nx, ny, nz = -gx, gy, np.ones_like(hn)   # GL: +y up the texture
    inv = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
    nrm = np.dstack([nx * inv, ny * inv, nz * inv]) * 0.5 + 0.5
    return col, nrm


# --- Ground: land, coast, weather fronts ------------------------------------

def wrap_dist(x, y, px, py):
    """Distance to a point on a torus (the tile wraps), for tileable cells."""
    dx = np.abs(x - px)
    dy = np.abs(y - py)
    dx = np.minimum(dx, 1.0 - dx)
    dy = np.minimum(dy, 1.0 - dy)
    return np.sqrt(dx * dx + dy * dy)


def canopy_dots(n, rng, mask, count, rmin, rmax):
    """Tree crowns as shaded dots (lit upper left, shadow lower right), placed
    where mask > 0.5 -- wrapped, so the tile still tiles."""
    canopy = np.zeros((n, n))
    shadow = np.zeros((n, n))
    for _ in range(count):
        ix, iy = rng.integers(0, n, 2)
        if mask[iy, ix] < 0.5:
            continue
        rad = rng.uniform(rmin, rmax)
        span = int(rad * 2) + 3
        yy, xx = np.mgrid[-span:span + 1, -span:span + 1]
        d2 = (xx ** 2 + yy ** 2) / (rad * rad)
        dome = np.clip(1.0 - d2, 0.0, 1.0)
        lx = np.clip(1.0 - ((xx + rad * 0.35) ** 2 + (yy + rad * 0.35) ** 2) / (rad * rad * 0.8), 0, 1)
        sd = np.clip(1.0 - ((xx - rad * 0.55) ** 2 + (yy - rad * 0.55) ** 2) / (rad * rad), 0, 1)
        rows = (iy + yy[:, 0]) % n
        cols = (ix + xx[0, :]) % n
        sub = np.ix_(rows, cols)
        canopy[sub] = np.maximum(canopy[sub], dome * (0.55 + 0.45 * lx))
        shadow[sub] = np.maximum(shadow[sub], sd)
    return canopy, shadow


def land(seed, n=1024, road=False):
    """Farmland from the air: a patchwork of fields (tileable Voronoi cells),
    each with its crop and its furrows, hedgerows along the borders, woods,
    and a farm here and there."""
    rng = np.random.default_rng(seed)
    x, y = grid(n)
    cells = rng.uniform(0, 1, (11, 2))
    d = np.stack([wrap_dist(x, y, cx, cy) for cx, cy in cells])
    order = np.argsort(d, axis=0)
    owner = order[0]
    d1 = np.take_along_axis(d, order[:1], 0)[0]
    d2 = np.take_along_axis(d, order[1:2], 0)[0]
    edge = d2 - d1                                        # 0 on a border
    crops = [rgb(0.44, 0.40, 0.25), rgb(0.20, 0.31, 0.13), rgb(0.15, 0.25, 0.10),
             rgb(0.29, 0.24, 0.17), rgb(0.28, 0.36, 0.17), rgb(0.39, 0.37, 0.23),
             rgb(0.23, 0.28, 0.14)]
    col = np.zeros((n, n, 3))
    nz = spectral(n, 2.2, rng)
    for i in range(len(cells)):
        m = owner == i
        c = crops[rng.integers(len(crops))]
        # Furrows: a whole number of them across and along the tile in each
        # direction, or the stripes would break at the tile edge.
        a = rng.uniform(0, np.pi)
        f = rng.uniform(40, 70)
        kx, ky = round(f * np.cos(a)), round(f * np.sin(a))
        stripes = np.sin((x * kx + y * ky) * 2 * np.pi)
        shade_ = 1.0 + 0.07 * stripes + 0.06 * nz
        col[m] = c * shade_[m][:, None]
    # Hedgerows and tracks along the borders.
    hedge = smooth(0.012, 0.004, edge)
    track = smooth(0.006, 0.0, np.abs(edge - 0.02)) * (rng.uniform() > 0.5)
    col = mix(col, rgb(0.42, 0.37, 0.28), track * 0.6)
    # Woods: a few patches, crowded with crowns.
    big = spectral(n, 3.0, rng)
    wood = smooth(0.35, 0.45, big)
    trees = np.maximum(hedge, wood)
    canopy, shadow = canopy_dots(n, rng, trees, 5000, 3.5, 8.0)
    col = mix(col, col * 0.55, np.clip(shadow - canopy, 0, 1) * 0.8)
    col = mix(col, rgb(0.07, 0.15, 0.05), wood * 0.6)
    has = smooth(0.0, 0.2, canopy)
    col = mix(col, mix(rgb(0.03, 0.10, 0.03), rgb(0.13, 0.28, 0.08), canopy), has)
    # Farms: a roof and its shadow at a few field corners.
    for _ in range(3):
        fx, fy = rng.uniform(0.1, 0.9, 2)
        w, h = rng.uniform(0.02, 0.035), rng.uniform(0.012, 0.02)
        roof = (np.abs(x - fx) < w) & (np.abs(y - fy) < h)
        sh = (np.abs(x - fx - 0.006) < w) & (np.abs(y - fy - 0.008) < h)
        col[sh & ~roof] *= 0.5
        rc = rgb(0.45, 0.20, 0.14) if rng.uniform() < 0.6 else rgb(0.38, 0.38, 0.40)
        ridge = 1.0 + 0.18 * np.sign(y - fy)
        col[roof] = (rc * ridge[roof][:, None])
    if road:
        # Down the middle, winding once per tile (so it joins itself).
        cx = 0.5 + 0.045 * np.sin(y * 2 * np.pi)
        dx = np.abs(x - cx)
        verge = smooth(0.026, 0.018, dx)
        asphalt = smooth(0.018, 0.015, dx)
        col = mix(col, rgb(0.36, 0.33, 0.26), verge)
        col = mix(col, rgb(0.14, 0.14, 0.15) * (1.0 + 0.05 * nz[..., None]), asphalt)
        dash = (smooth(0.0016, 0.0008, dx) * (np.sin(y * 2 * np.pi * 24) > 0.3))
        col = mix(col, rgb(0.55, 0.52, 0.40), dash)
    return col


def coast(sea_col, land_col, seed, outward=False):
    """Sea at one end of the tile, land at the other, a beach between: built
    from the sea and land tiles themselves, so both ends meet their neighbours.
    In the game the image's first rows lie DOWN the screen (the plane's UV
    runs that way), so the sea is at the top rows -- toward the sea rows below
    it -- unless outward (land ends, sea begins)."""
    n = sea_col.shape[0]
    rng = np.random.default_rng(seed)
    x, y = grid(n)
    wobble = spectral(n, 3.0, rng)[0]                  # periodic along x
    line = 0.5 + 0.12 * wobble[None, :] + 0.03 * spectral(n, 2.2, rng)
    t = y if not outward else (1.0 - y)                # 0 at the sea end
    land_amt = t - line                                # > 0 on land
    sand = smooth(-0.004, 0.004, land_amt)
    grass = smooth(0.03, 0.06, land_amt)
    shallow = smooth(-0.12, -0.005, land_amt)
    col = mix(sea_col, sea_col * 0.6 + rgb(0.05, 0.22, 0.24), shallow)
    col = mix(col, rgb(0.50, 0.44, 0.30), sand)
    surf = np.exp(-((land_amt + 0.01) / 0.007) ** 2)
    col = mix(col, rgb(0.62, 0.66, 0.66), surf * 0.85)
    col = mix(col, land_col, grass)
    return col


def front(calm, storm, seed, outward=False):
    """A weather front across the tile: calm sea at one end, storm at the other."""
    n = calm.shape[0]
    rng = np.random.default_rng(seed)
    x, y = grid(n)
    wobble = spectral(n, 2.6, rng)[0]
    t = y if not outward else (1.0 - y)                # 0 at the calm end (down-screen)
    k = smooth(0.25, 0.75, t + 0.12 * wobble[None, :])
    return mix(calm, storm, k)


def puff_tex(seed, n=128):
    rng = np.random.default_rng(seed)
    x, y = grid(n)
    r = np.sqrt((x - 0.5) ** 2 + (y - 0.5) ** 2)
    nz = spectral(n, 2.4, rng)
    dens = smooth(0.5, 0.05, r + 0.07 * nz)
    col = np.ones((n, n, 3)) * rgb(0.62, 0.64, 0.66)
    return np.dstack([col, dens * 0.85])


def splash_tex(seed, n=256):
    """Something hitting the water, from above: a foam ring, a churned middle,
    droplets thrown out in spokes."""
    rng = np.random.default_rng(seed)
    x, y = grid(n)
    r = np.sqrt((x - 0.5) ** 2 + (y - 0.5) ** 2)
    ang = np.arctan2(y - 0.5, x - 0.5)
    nz = spectral(n, 2.0, rng)
    ringb = np.exp(-((r - 0.3 + 0.02 * nz) / 0.05) ** 2)
    core = smooth(0.22, 0.0, r) * (0.5 + 0.5 * nz)
    spokes = np.clip(np.sin(ang * 23 + nz * 3) * 0.5 + 0.5, 0, 1) ** 6 * smooth(0.46, 0.3, r) * smooth(0.25, 0.33, r)
    alpha = np.clip(ringb + core * 0.8 + spokes * 0.7, 0, 1) * smooth(0.5, 0.44, r)
    col = np.ones((n, n, 3)) * rgb(0.64, 0.68, 0.70)
    return np.dstack([col, alpha])


# --- Islands -----------------------------------------------------------------

def island(seed, n=1024, kind="green"):
    rng = np.random.default_rng(seed)
    x, y = grid(n)
    r = np.sqrt((x - 0.5) ** 2 + (y - 0.5) ** 2)
    mass = blobs(n, rng, rng.integers(6, 10), 0.17, (0.065, 0.125),
                 stretch=(rng.uniform(1.0, 1.3), rng.uniform(0.75, 1.0)))
    big = spectral(n, 3.0, rng)
    mid = spectral(n, 2.4, rng)
    fine = spectral(n, 1.8, rng)
    land = mass * 1.1 + 0.22 * big + 0.06 * mid - 0.5          # 0 = the coast
    land = land - smooth(0.36, 0.5, r) * 2.0                    # fade before the edge

    # Lagoon: shallow turquoise falling off into the open sea.
    lagoon = smooth(-0.34, -0.02, land)
    reef = smooth(0.55, 0.8, mid) * smooth(-0.3, -0.12, land) * smooth(-0.02, -0.12, land)
    water = mix(rgb(0.03, 0.22, 0.30), rgb(0.12, 0.46, 0.46), smooth(-0.16, -0.01, land))
    water = mix(water, rgb(0.20, 0.42, 0.36), reef * 0.6)
    alpha = np.power(lagoon, 1.6) * 0.92

    # Beach: wet sand, dry sand, and the surf line on the water side.
    sand = mix(rgb(0.36, 0.31, 0.21), rgb(0.52, 0.45, 0.31), smooth(0.0, 0.05, land))
    sand = sand * (0.93 + 0.07 * fine[..., None])
    surf = np.exp(-((land + 0.012) / 0.008) ** 2) * (0.6 + 0.4 * fine)
    col = mix(water, sand, smooth(-0.004, 0.004, land))
    col = mix(col, rgb(0.62, 0.66, 0.66), surf * 0.85)
    alpha = np.maximum(alpha, smooth(-0.02, 0.0, land))

    # Relief: hills in the middle, shaded from the upper left.
    inland = smooth(0.04, 0.5, land)
    hills = np.power(np.maximum(land, 0.0), 1.2) * 90.0 + mid * 6.0 * inland + big * 10 * inland
    lit = shade(hills, 1.0)
    if kind == "rock":
        ground = mix(rgb(0.30, 0.27, 0.22), rgb(0.44, 0.40, 0.33), smooth(-0.5, 0.8, mid))
        veg = mix(rgb(0.10, 0.20, 0.07), rgb(0.18, 0.30, 0.10), smooth(-0.6, 0.6, big))
        veg_amount = smooth(0.06, 0.12, land) * smooth(0.55, 0.3, land) * smooth(-0.2, 0.3, mid)
    else:
        ground = mix(rgb(0.16, 0.26, 0.09), rgb(0.26, 0.34, 0.13), smooth(-0.5, 0.8, mid))
        veg = mix(rgb(0.05, 0.17, 0.05), rgb(0.12, 0.27, 0.07), smooth(-0.6, 0.6, big))
        veg_amount = smooth(0.06, 0.14, land)
    top = mix(ground, veg, veg_amount * 0.7)
    rockcap = smooth(0.42, 0.62, land + 0.1 * mid)
    top = mix(top, rgb(0.40, 0.38, 0.34), rockcap * (0.7 if kind == "rock" else 0.35))
    top = top * (0.55 + 0.75 * lit[..., None])
    col = mix(col, top, smooth(0.035, 0.07, land))

    # Canopy: tree crowns as shaded dots with a shadow to the lower right.
    canopy = np.zeros((n, n))
    shadow = np.zeros((n, n))
    count = 1400 if kind != "rock" else 500
    for _ in range(count):
        cx, cy = rng.uniform(0.12, 0.88, 2)
        ix, iy = int(cx * n), int(cy * n)
        if land[iy, ix] < 0.09 or (kind == "rock" and veg_amount[iy, ix] < 0.3):
            continue
        rad = rng.uniform(3.5, 8.0)
        x0, x1 = max(0, ix - 14), min(n, ix + 15)
        y0, y1 = max(0, iy - 14), min(n, iy + 15)
        yy, xx = np.mgrid[y0:y1, x0:x1]
        d2 = ((xx - ix) ** 2 + (yy - iy) ** 2) / (rad * rad)
        dome = np.clip(1.0 - d2, 0.0, 1.0)
        lx = np.clip(1.0 - ((xx - ix + rad * 0.35) ** 2 + (yy - iy + rad * 0.35) ** 2) /
                     (rad * rad * 0.8), 0.0, 1.0)
        canopy[y0:y1, x0:x1] = np.maximum(canopy[y0:y1, x0:x1], dome * (0.55 + 0.45 * lx))
        sd = ((xx - ix - rad * 0.55) ** 2 + (yy - iy - rad * 0.55) ** 2) / (rad * rad)
        shadow[y0:y1, x0:x1] = np.maximum(shadow[y0:y1, x0:x1], np.clip(1.0 - sd, 0, 1))
    tree_dark = rgb(0.03, 0.11, 0.03)
    tree_lit = rgb(0.13, 0.30, 0.08)
    col = mix(col, col * 0.55, np.clip(shadow - canopy, 0, 1) * 0.8)
    has = smooth(0.0, 0.2, canopy)
    col = mix(col, mix(tree_dark, tree_lit, canopy), has)
    return np.dstack([col, alpha])


# --- Effects -----------------------------------------------------------------

def smoke(seed, n=256):
    rng = np.random.default_rng(seed)
    x, y = grid(n)
    r = np.sqrt((x - 0.5) ** 2 + (y - 0.5) ** 2)
    nz = spectral(n, 2.6, rng)
    dens = smooth(0.48, 0.1, r + 0.08 * nz)
    lit = shade(blur(dens, 4.0) * 30.0, 1.0)
    col = mix(rgb(0.10, 0.10, 0.11), rgb(0.34, 0.33, 0.32), lit)
    return np.dstack([col, dens * 0.9])


def fire(seed, n=256):
    rng = np.random.default_rng(seed)
    x, y = grid(n)
    r = np.sqrt((x - 0.5) ** 2 + (y - 0.5) ** 2)
    nz = spectral(n, 2.4, rng)
    t = np.clip(r * 2.1 + 0.18 * nz, 0.0, 1.0)            # 0 core .. 1 rim
    col = mix(rgb(1.0, 0.95, 0.75), rgb(1.0, 0.62, 0.18), smooth(0.0, 0.35, t))
    col = mix(col, rgb(0.75, 0.18, 0.04), smooth(0.35, 0.8, t))
    alpha = smooth(1.0, 0.55, t)
    return np.dstack([col, alpha])


def ring(seed, n=256):
    rng = np.random.default_rng(seed)
    x, y = grid(n)
    r = np.sqrt((x - 0.5) ** 2 + (y - 0.5) ** 2)
    nz = spectral(n, 2.2, rng)
    band = np.exp(-((r - 0.42 + 0.01 * nz) / 0.028) ** 2)
    inner = np.exp(-((r - 0.36) / 0.08) ** 2) * 0.25
    alpha = np.clip(band + inner, 0.0, 1.0) * smooth(0.5, 0.47, r)
    col = mix(rgb(0.55, 0.75, 1.0), rgb(1.0, 1.0, 1.0), band)
    return np.dstack([col, alpha])


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "content/textures"
    os.makedirs(out, exist_ok=True)
    p = lambda name: os.path.join(out, name)
    for i, s in enumerate((11, 23, 37)):
        write_png(p(f"shmup_cloud_{'abc'[i]}.png"), cloud(s))
    write_png(p("shmup_haze.png"), haze(5))
    col, nrm = sea(7)
    write_png(p("shmup_sea.png"), col)
    write_png(p("shmup_sea_n.png"), nrm)
    for i, (s, k) in enumerate(((3, "green"), (19, "green"), (41, "rock"))):
        write_png(p(f"shmup_island_{'abc'[i]}.png"), island(s, kind=k))
    write_png(p("shmup_smoke.png"), smoke(2))
    write_png(p("shmup_fire.png"), fire(4))
    write_png(p("shmup_ring.png"), ring(6))
    write_png(p("shmup_puff.png"), puff_tex(8))
    write_png(p("shmup_splash.png"), splash_tex(9))
    storm = dict(crown=(0.50, 0.52, 0.55), lee=(0.19, 0.21, 0.26), rim=(0.36, 0.38, 0.42))
    write_png(p("shmup_storm_a.png"), cloud(51, **storm))
    write_png(p("shmup_storm_b.png"), cloud(67, **storm))
    field = sea_field(7)
    calm = col
    rough = sea_colour(field, storm=True)
    write_png(p("shmup_sea_storm.png"), rough)
    write_png(p("shmup_sea_front.png"), front(calm, rough, 13))
    write_png(p("shmup_sea_front_out.png"), front(rough, calm, 13))
    fields = land(21)
    write_png(p("shmup_land.png"), fields)
    write_png(p("shmup_land_road.png"), land(21, road=True))
    write_png(p("shmup_coast.png"), coast(calm, fields, 31))
    write_png(p("shmup_coast_out.png"), coast(calm, fields, 31, outward=True))


if __name__ == "__main__":
    main()
