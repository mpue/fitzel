"""Textures for the world generator's villages, painted by ComfyUI (Z-Image Turbo).

    python3 -I tools/gen-ground-textures.py [--server http://127.0.0.1:8188] [names...]

For each surface: one prompt to the server (raw picture cached in build/ground-tex-raw/), then a
tileable colour map with the model's baked-in lighting divided out, and a normal map derived from
its brightness, into content/textures/<name>_diff_1k.jpg and <name>_nor_gl_1k.png with .meta
files (GUIDs fixed below, so a rebuild keeps every reference). A pattern of stones is cropped to a
whole number of its own repeats instead of cross-faded, so its joints wrap.

  lawn_01      a mown garden lawn with a few leaves: the ground of a village (WorldGen, Ground layer)
  setts_01     small grey granite setts in straight rows: a road's edging (RoadSystem::setts)
  plaster_01   a house's rough render, near white            } NEUTRAL: tinted per house by the
  rooftiles_01 interlocking clay tiles in courses, greyed    } material's `tint` (HouseGen), so they
  plinth_01    the grey render of a house's plinth           } are greyed and their LINEAR mean set
  gravel_01    the gravel strip round a house's walls          to NEUTRAL_MEAN -- tint = colour / that

Roof tiles run in courses across the picture, the lower edge of each tile toward its bottom: the
roof's downhill side is the picture's down. They are not painted but BUILT (rooftiles()): the
model draws a roof in perspective, its columns fanning out, and no crop of that tiles. Built from
a height field instead -- sixteen Frankfurter Pfannen across and sixteen courses down, 0.33 by 0.34 m
each, so one repeat is 5.28 x 5.44 m (HouseGen's FaceUV) -- with the normal map from the same field.
"""
import argparse, json, sys, time, urllib.parse, urllib.request, uuid
from pathlib import Path

import numpy as np
from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent
RAW = ROOT / "build" / "ground-tex-raw"
OUT = ROOT / "content" / "textures"

FLAT = (" Seamless tileable texture, photographed perfectly straight down from above, orthographic, "
        "flat even overcast lighting, no shadows, no perspective, no vignette, fills the entire frame "
        "edge to edge, no text, no objects.")

# name: (prompt, seed, normal strength, patterned, colour guid, normal guid)
SURFACES = {
    "lawn_01": ("A mown green garden lawn seen from directly above: short dense grass blades in many "
                "shades of green, a little clover, a few tiny daisies, some fallen yellow and brown "
                "autumn leaves, a few slightly yellowed dry patches, natural and a bit uneven." + FLAT,
                4101, 2.0, False, "c3f1a8e27b6d4f0c9e15a2d47b80c6e1", "d84b2e61f09a4c37b5e8a1c26f3d9b07"),
    "setts_01": ("Old hand-split grey granite setts, roughly 10 cm cobblestones of slightly uneven size with "
                 "rounded worn edges, laid in straight rows, seen from directly above, wide dark joints of "
                 "sand and dirt with a little moss, weathered speckled granite in light, mid and bluish "
                 "greys, some stones darker." + FLAT,
                 4102, 3.0, True, "e5a7c09b3d214f6e8b07c4d1a29f6e53", "f1c86d2a7e0b4953a6d8e27b10c4f9a8"),
    "plaster_01": ("Close-up of the rough exterior render of a German house wall, off-white scratched "
                   "plaster (Kratzputz) with a clearly visible coarse grain of 2 mm sand and tiny pits all "
                   "over, a little uneven, faint grey weather streaks, half a metre of wall seen straight "
                   "on." + FLAT,
                   4103, 1.6, False, "3ef3c7bae1a448d29d6dbff243d9a589", "6c694d54c402457096f2f68c3a9f8238"),
    "rooftiles_01": ("Roof covered in interlocking clay roof tiles (Frankfurter Pfanne), seen straight on "
                     "at the roof plane: neat straight horizontal courses, each course overlapping the one "
                     "below, the rounded lower edge of every tile pointing down, slight colour variation "
                     "between tiles, a little moss and weathering." + FLAT,
                     4104, 18.0, True, "af8c1f60e05147f193c7c05df7fb81a2", "66c749837cc64ca598cdf768562386e0"),
    "plinth_01": ("Grey cement render of a house plinth, slightly rough sponge-rubbed finish, faint "
                  "dirt splashes, a few darker damp patches, seen straight on." + FLAT,
                  4105, 1.4, False, "dfec66b1b0d5461aa98390a8ccd6ae25", "a9231f56edee4cc89f76144df260b83c"),
    "gravel_01": ("Light grey and beige rounded pebble gravel, 1 to 3 cm stones, loose, seen from "
                  "directly above, a few darker stones and some dry leaves." + FLAT,
                  4106, 3.0, False, "1bbda553c0024ffc8d49c6b7a8b73a48", "47414e7193814f98a0af9907c0a5dfb1"),
}

# Tinted surfaces: (how much of the colour goes, 0..1). Their linear mean is set to
# NEUTRAL_MEAN on every channel, so a material's tint is its colour / NEUTRAL_MEAN.
NEUTRAL = {"plaster_01": 0.95, "rooftiles_01": 0.75, "plinth_01": 0.95}
NEUTRAL_MEAN = 0.5

GRAPH = {
    "1": {"class_type": "UNETLoader", "inputs": {"unet_name": "z_image_turbo_bf16.safetensors", "weight_dtype": "default"}},
    "2": {"class_type": "CLIPLoader", "inputs": {"clip_name": "qwen_3_4b.safetensors", "type": "lumina2", "device": "default"}},
    "3": {"class_type": "VAELoader", "inputs": {"vae_name": "ae.safetensors"}},
    "4": {"class_type": "ModelSamplingAuraFlow", "inputs": {"model": ["1", 0], "shift": 3}},
    "5": {"class_type": "CLIPTextEncode", "inputs": {"text": "", "clip": ["2", 0]}},
    "6": {"class_type": "ConditioningZeroOut", "inputs": {"conditioning": ["5", 0]}},
    "7": {"class_type": "EmptySD3LatentImage", "inputs": {"width": 1024, "height": 1024, "batch_size": 1}},
    "8": {"class_type": "KSampler", "inputs": {"model": ["4", 0], "seed": 0, "steps": 8, "cfg": 1,
                                               "sampler_name": "res_multistep", "scheduler": "simple",
                                               "positive": ["5", 0], "negative": ["6", 0],
                                               "latent_image": ["7", 0], "denoise": 1}},
    "9": {"class_type": "VAEDecode", "inputs": {"samples": ["8", 0], "vae": ["3", 0]}},
    "10": {"class_type": "SaveImage", "inputs": {"images": ["9", 0], "filename_prefix": "fitzel/ground"}},
}


def generate(server, prompt, seed, dst):
    g = json.loads(json.dumps(GRAPH))
    g["5"]["inputs"]["text"] = prompt
    g["8"]["inputs"]["seed"] = seed
    body = json.dumps({"prompt": g, "client_id": str(uuid.uuid4())}).encode()
    r = json.load(urllib.request.urlopen(urllib.request.Request(server + "/prompt", body,
                                                                {"Content-Type": "application/json"})))
    pid = r["prompt_id"]
    while True:
        time.sleep(2)
        h = json.load(urllib.request.urlopen(f"{server}/history/{pid}"))
        if pid in h:
            st = h[pid].get("status", {})
            if st.get("status_str") == "error":
                raise SystemExit(json.dumps(st, indent=1)[:3000])
            img = h[pid]["outputs"]["10"]["images"][0]
            break
    q = urllib.parse.urlencode({"filename": img["filename"], "subfolder": img.get("subfolder", ""),
                                "type": img.get("type", "output")})
    dst.write_bytes(urllib.request.urlopen(f"{server}/view?{q}").read())


def even_light(a, sigma):
    """Divide out the slow brightness changes (the model's lighting), keep the mean."""
    img = Image.fromarray((a * 255).clip(0, 255).astype(np.uint8))
    low = np.asarray(img.filter(ImageFilter.GaussianBlur(sigma))).astype(np.float32) / 255.0
    mean = a.reshape(-1, 3).mean(0)
    return np.clip(a / np.maximum(low, 0.03) * mean, 0, 1)


def tileable(a, hard=False):
    """Cross-fade a half-offset copy over the seams: the result wraps without a line.
    `hard`: for loose stones a cross-fade is a blurred double exposure, so the copy is
    cut in along a ragged line instead (a seam through the pebbles, not a ghost of them)."""
    h, w, _ = a.shape
    b = np.roll(a, (h // 2, w // 2), (0, 1))
    y = np.abs(np.linspace(-1, 1, h))[:, None]
    x = np.abs(np.linspace(-1, 1, w))[None, :]
    if hard:
        rng = np.random.default_rng(7)
        n = Image.fromarray((rng.random((h, w)) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(24))
        n = np.asarray(n).astype(np.float32) / 255.0
        n = (n - n.mean()) / max(n.std(), 1e-4)
        m = (np.maximum(x, y) + 0.06 * n > 0.78).astype(np.float32)
        m = np.asarray(Image.fromarray((m * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2)))
        return a * (1 - m[..., None] / 255.0) + b * (m[..., None] / 255.0)
    m = np.clip((np.maximum(x, y) - 0.55) / 0.4, 0, 1)[..., None]
    m = m * m * (3 - 2 * m)
    return a * (1 - m) + b * m


HARD_SEAM = {"gravel_01"}
# Built, not painted: name -> () -> (colour, height).
BUILT = {"rooftiles_01": lambda: rooftiles()}


def period(profile, lo=12, hi=400):
    """The strongest repeat (pixels) of a 1-D profile, by autocorrelation."""
    p = profile - profile.mean()
    n = len(p)
    f = np.fft.rfft(p, 2 * n)
    ac = np.fft.irfft(f * np.conj(f))[:n]
    ac /= ac[0] + 1e-9
    hi = min(hi, n // 2)
    k = lo + int(np.argmax(ac[lo:hi]))
    if k // 2 >= lo and ac[k // 2] > 0.8 * ac[k]:
        k = k // 2
    return k


def period_crop(a):
    """Crop to a whole number of pattern repeats both ways, then back to size: the joints wrap."""
    g = a @ np.array([0.3, 0.59, 0.11], np.float32)
    px = period(np.abs(np.diff(g, axis=1)).mean(0))
    py = period(np.abs(np.diff(g, axis=0)).mean(1))
    h, w = g.shape
    nx, ny = max(1, w // px), max(1, h // py)
    img = Image.fromarray((a[:ny * py, :nx * px] * 255).astype(np.uint8)).resize((w, h), Image.LANCZOS)
    print(f"   period {px} x {py} px -> {nx} x {ny} repeats")
    return np.asarray(img).astype(np.float32) / 255.0


def normal_from(a, strength):
    g = a @ np.array([0.3, 0.59, 0.11], np.float32)
    g = np.asarray(Image.fromarray((g * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.0))) / 255.0
    dx = (np.roll(g, -1, 1) - np.roll(g, 1, 1)) * strength
    dy = (np.roll(g, -1, 0) - np.roll(g, 1, 0)) * strength
    n = np.dstack([-dx, dy, np.ones_like(g)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return ((n * 0.5 + 0.5) * 255).astype(np.uint8)


def neutral(a, desat):
    """Grey it by `desat` and scale its linear mean to NEUTRAL_MEAN on every channel."""
    lum = (a @ np.array([0.2126, 0.7152, 0.0722], np.float32))[..., None]
    a = a * (1 - desat) + lum * desat
    lin = np.power(np.clip(a, 0, 1), 2.2)
    lin = lin * (NEUTRAL_MEAN / np.maximum(lin.reshape(-1, 3).mean(0), 1e-4))
    return np.clip(np.power(np.clip(lin, 0, 1), 1 / 2.2), 0, 1)


def periodic_blur(a, sigma):
    """Gaussian blur that wraps round the edges (so a blurred noise still tiles)."""
    h, w = a.shape
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    k = np.exp(-2 * (np.pi * sigma) ** 2 * (fx * fx + fy * fy))
    return np.real(np.fft.ifft2(np.fft.fft2(a) * k)).astype(np.float32)


def rooftiles(size=1024, tiles=16, courses=16, seed=4104):
    """Frankfurter Pfannen from a height field: (colour sRGB HxWx3, height HxW in tile-heights).
    Each course's tiles lie tilted on the course below, their rounded lower edge on top of it,
    so every course boundary is a step with a shadow under the lip; across, the big roll, the
    small roll and the interlock groove. Per tile its own shade of clay, dirt in the channels the
    rain runs down, fine grain and the odd lichen spot. Everything wraps."""
    rng = np.random.default_rng(seed)
    tw, ch = size // tiles, size // courses
    py, px = np.mgrid[0:size, 0:size]
    fx = (px % tw + 0.5) / tw                    # across a tile, 0..1
    fy = 1.0 - (py % ch + 0.5) / ch              # up a course: 0 at its lower edge (picture bottom)
    col_i, row_j = px // tw, py // ch

    def hump(x, c, w):
        d = np.clip(np.abs(x - c) / w, 0, 1)
        return 0.5 + 0.5 * np.cos(np.pi * d)

    roll = 0.62 * hump(fx, 0.40, 0.30) + 0.30 * hump(fx, 0.84, 0.12)
    groove = 1.0 - 0.6 * hump(fx, 0.01, 0.035) - 0.6 * hump(fx, 1.01, 0.035)
    tilt = 0.32 * (1.0 - fy)                     # the lower edge rests on the course below
    lip = 1.0 - 0.35 * (1.0 - np.clip(fy / 0.07, 0, 1)) ** 2   # rounded off at the very edge
    height = (tilt + 0.45 * roll * groove) * lip
    height += 0.01 * periodic_blur(rng.standard_normal((size, size)).astype(np.float32), 1.0)

    # Colour: clay, a shade per tile, a little per course.
    shade = rng.normal(1.0, 0.045, (courses, tiles)).astype(np.float32)
    hue = rng.normal(0.0, 0.03, (courses, tiles)).astype(np.float32)
    base = np.array([0.60, 0.31, 0.21], np.float32)
    t_shade = shade[row_j, col_i][..., None]
    t_hue = hue[row_j, col_i][..., None]
    c = base * t_shade + np.array([1.0, 0.2, -0.6], np.float32) * t_hue * 0.3
    # Dirt in the channels between the rolls, thickest toward the lower edge where it collects.
    rn = roll / roll.max()
    dirt = 1.0 - 0.38 * (1.0 - rn) * (0.4 + 0.6 * (1.0 - fy))
    # The shadow under the next course's lip, at the top of this course, and the
    # lip itself catching the light.
    # Its edge follows the rolls of the tile above, so the shadow under it is wavy:
    # deep under the troughs, shallow under the rolls.
    edge = 0.66 + 0.16 * rn
    ao = 1.0 - 0.65 * np.clip((fy - edge) / (1.0 - edge), 0, 1) ** 1.3
    lipLit = 1.0 + 0.12 * (1.0 - np.clip(fy / 0.08, 0, 1))
    # The side joint of every tile: a dark line where the next one's lip overlaps.
    ao = ao * (1.0 - 0.45 * hump(fx, 0.0, 0.025) - 0.45 * hump(fx, 1.0, 0.025))
    # Grain, and weathering patches a few tiles big.
    def unit_noise(sigma):
        n = periodic_blur(rng.standard_normal((size, size)).astype(np.float32), sigma)
        return n / max(float(n.std()), 1e-6)
    grain = 1.0 + 0.05 * unit_noise(0.7)
    patches = np.clip(1.0 + 0.07 * unit_noise(40.0), 0.85, 1.15)
    # Lichen: a few pale spots, not a coat.
    spots = unit_noise(1.5)
    lichen = np.clip((spots - np.quantile(spots, 0.993)) * 3.0, 0, 1)
    c = c * (dirt * ao * lipLit * grain * patches)[..., None]
    c = c * (1 - 0.6 * lichen[..., None]) + np.array([0.62, 0.60, 0.48], np.float32) * 0.6 * lichen[..., None]
    return np.clip(c, 0, 1), height.astype(np.float32)


def normal_from_height(h, strength):
    """GL normal map (green up) from a height field in tile-heights; `strength` in pixels per unit."""
    dx = (np.roll(h, -1, 1) - np.roll(h, 1, 1)) * 0.5 * strength
    dy = (np.roll(h, -1, 0) - np.roll(h, 1, 0)) * 0.5 * strength
    n = np.dstack([-dx, dy, np.ones_like(h)])
    n /= np.linalg.norm(n, axis=2, keepdims=True)
    return ((n * 0.5 + 0.5) * 255).astype(np.uint8)


def meta(path, guid, srgb):
    path.with_name(path.name + ".meta").write_text(json.dumps(
        {"guid": guid, "importer": {"flipVertically": True, "sRGB": srgb}, "type": "Texture"}, indent=2) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server", default="http://127.0.0.1:8188")
    ap.add_argument("--regen", action="store_true", help="paint again even if a raw picture is cached")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    RAW.mkdir(parents=True, exist_ok=True)
    for name, (prompt, seed, strength, patterned, gc, gn) in SURFACES.items():
        if a.names and name not in a.names:
            continue
        col = OUT / f"{name}_diff_1k.jpg"
        nor = OUT / f"{name}_nor_gl_1k.png"
        if name in BUILT:
            img, height = BUILT[name]()
            normals = normal_from_height(height, strength)
        else:
            raw = RAW / f"{name}.png"
            if a.regen or not raw.exists():
                generate(a.server, prompt, seed, raw)
                print(name, "painted", flush=True)
            img = np.asarray(Image.open(raw).convert("RGB")).astype(np.float32) / 255.0
            img = period_crop(even_light(img, 160)) if patterned else tileable(even_light(img, 160),
                                                                                 name in HARD_SEAM)
            normals = None
        if name in NEUTRAL:
            img = neutral(img, NEUTRAL[name])
        if normals is None:
            normals = normal_from(img, strength)
        Image.fromarray((img * 255).astype(np.uint8)).save(col, quality=92)
        Image.fromarray(normals).save(nor)
        meta(col, gc, True)
        meta(nor, gn, False)
        print(name, "->", col.name, nor.name, flush=True)


if __name__ == "__main__":
    main()
