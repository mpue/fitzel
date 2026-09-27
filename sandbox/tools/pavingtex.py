# pavingtex.py -- bakes the towns' paving-slab texture (content/textures/paving_slabs_01_*).
# Re-run only to change the look; the output is committed.
# Paving slabs for the towns' pavements: 4 x 4 slabs of 50 cm in a 2 m tile,
# seamless. Albedo (sRGB) + a tangent-space normal map (OpenGL convention, +Y up).
import json, math, random
from PIL import Image, ImageFilter

N = 512               # 256 px per metre
SL = N // 4           # one slab
random.seed(1451)
import os
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "content", "textures")

def smoothstep(a, b, x):
    t = min(max((x - a) / (b - a), 0.0), 1.0)
    return t * t * (3 - 2 * t)

# Height: 1 on a slab, down into the joint with a small chamfer.
JOINT, BEVEL = 1.5, 3.5
height = Image.new("F", (N, N))
hp = height.load()
for y in range(N):
    dy = min(y % SL, SL - 1 - (y % SL))
    for x in range(N):
        dx = min(x % SL, SL - 1 - (x % SL))
        d = min(dx, dy)
        hp[x, y] = smoothstep(JOINT, JOINT + BEVEL, d)

# Per-slab tone: concrete slabs differ a little in shade and warmth.
tones = {}
for sy in range(4):
    for sx in range(4):
        tones[(sx, sy)] = (random.uniform(-0.05, 0.05), random.uniform(-0.012, 0.012))

# Grain: a few octaves of value noise, tiling at N.
def noise(cells, seed):
    rnd = random.Random(seed)
    g = [[rnd.random() for _ in range(cells)] for _ in range(cells)]
    img = Image.new("F", (N, N))
    p = img.load()
    for y in range(N):
        fy = y * cells / N
        y0 = int(fy) % cells; y1 = (y0 + 1) % cells; ty = fy - int(fy)
        ty = ty * ty * (3 - 2 * ty)
        for x in range(N):
            fx = x * cells / N
            x0 = int(fx) % cells; x1 = (x0 + 1) % cells; tx = fx - int(fx)
            tx = tx * tx * (3 - 2 * tx)
            a = g[y0][x0] + (g[y0][x1] - g[y0][x0]) * tx
            b = g[y1][x0] + (g[y1][x1] - g[y1][x0]) * tx
            p[x, y] = a + (b - a) * ty
    return p

n1 = noise(16, 1); n2 = noise(64, 2); n3 = noise(256, 3); stain = noise(6, 4)
alb = Image.new("RGB", (N, N))
ap = alb.load()
for y in range(N):
    for x in range(N):
        h = hp[x, y]
        tone, warm = tones[(x // SL, y // SL)]
        grain = 0.55 * n3[x, y] + 0.30 * n2[x, y] + 0.15 * n1[x, y] - 0.5
        v = 0.47 + tone + 0.07 * grain - 0.06 * max(stain[x, y] - 0.6, 0.0)
        v = v * (0.55 + 0.45 * h)             # joints dark
        # speckles of aggregate
        if n3[x, y] > 0.93: v += 0.05
        r = v * (1.0 + warm); g = v; b = v * (1.0 - warm) * 0.98
        ap[x, y] = tuple(int(max(0, min(1, c)) ** (1 / 2.2) * 255 + 0.5) for c in (r, g, b))
alb.save(os.path.join(OUT, "paving_slabs_01_diff_512.png"))

# Normal map from the height (plus a whisper of grain), central differences, wrapping.
nor = Image.new("RGB", (N, N))
npx = nor.load()
S = 3.0
def H(x, y):
    x %= N; y %= N
    return hp[x, y] + 0.04 * n3[x, y]
for y in range(N):
    for x in range(N):
        dx = (H(x + 1, y) - H(x - 1, y)) * S
        dy = (H(x, y + 1) - H(x, y - 1)) * S
        nx, ny, nz = -dx, dy, 1.0              # image y runs down; GL normal +Y up
        l = math.sqrt(nx * nx + ny * ny + nz * nz)
        npx[x, y] = tuple(int((c / l * 0.5 + 0.5) * 255 + 0.5) for c in (nx, ny, nz))
nor.save(os.path.join(OUT, "paving_slabs_01_nor_gl_512.png"))

for name, guid, srgb in [("paving_slabs_01_diff_512.png", "5f2a9c1e7b3d4e60a1c8f09d2b6e7a31", True),
                         ("paving_slabs_01_nor_gl_512.png", "8d41e6b2c9a74f15b3e02c7d9a6f1e84", False)]:
    with open(os.path.join(OUT, name + ".meta"), "w", newline="\n") as f:
        json.dump({"guid": guid, "importer": {"flipVertically": True, "sRGB": srgb}, "type": "Texture"},
                  f, indent=2)
        f.write("\n")
print("done")
