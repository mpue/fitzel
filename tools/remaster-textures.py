#!/usr/bin/env python3
"""Remaster a folder of game textures with an upscale model in ComfyUI.

Each texture goes through a super-resolution model (default 4x-UltraSharpV2) and is then
brought to its target size. Axes on which a texture tiles are padded wrap-around first,
so the model sees across the seam and the result keeps tiling. Alpha goes through the
same model as a grey picture; under fully transparent texels the colour is filled in from
the visible neighbours, so cutouts (leaves, fences) get no dark fringes in the mipmaps.
"""
import argparse, io, json, math, os, re, time, uuid, urllib.request, urllib.parse
from concurrent.futures import ThreadPoolExecutor
import numpy as np
from PIL import Image

# --- ComfyUI ------------------------------------------------------------------------
class Comfy:
    def __init__(self, url):
        self.url = url.rstrip("/")
        self.client = uuid.uuid4().hex

    def _req(self, path, data=None, headers=None):
        r = urllib.request.Request(self.url + path, data=data, headers=headers or {})
        with urllib.request.urlopen(r, timeout=600) as f:
            return f.read()

    def upload(self, img, name):
        buf = io.BytesIO(); img.save(buf, "PNG")
        b = "----remaster" + uuid.uuid4().hex
        body = (f"--{b}\r\nContent-Disposition: form-data; name=\"image\"; filename=\"{name}\"\r\n"
                f"Content-Type: image/png\r\n\r\n").encode() + buf.getvalue() + \
               (f"\r\n--{b}\r\nContent-Disposition: form-data; name=\"overwrite\"\r\n\r\ntrue"
                f"\r\n--{b}--\r\n").encode()
        r = json.loads(self._req("/upload/image", body, {"Content-Type": f"multipart/form-data; boundary={b}"}))
        return (r["subfolder"] + "/" if r.get("subfolder") else "") + r["name"]

    def run(self, graph):
        r = json.loads(self._req("/prompt", json.dumps({"prompt": graph, "client_id": self.client}).encode(),
                                 {"Content-Type": "application/json"}))
        pid = r["prompt_id"]
        while True:
            h = json.loads(self._req("/history/" + pid))
            if pid in h:
                st = h[pid].get("status", {})
                if st.get("status_str") == "error":
                    raise RuntimeError(json.dumps(st.get("messages"))[:800])
                for out in h[pid]["outputs"].values():
                    for im in out.get("images", []):
                        q = urllib.parse.urlencode({"filename": im["filename"], "subfolder": im["subfolder"],
                                                    "type": im["type"]})
                        return Image.open(io.BytesIO(self._req("/view?" + q))).convert("RGB")
                raise RuntimeError("no image in the result")
            time.sleep(0.2)

    def upscale(self, img, name, model):
        image = self.upload(img, name)
        return self.run({
            "1": {"class_type": "UpscaleModelLoader", "inputs": {"model_name": model}},
            "2": {"class_type": "LoadImage", "inputs": {"image": image}},
            "3": {"class_type": "ImageUpscaleWithModel", "inputs": {"upscale_model": ["1", 0], "image": ["2", 0]}},
            "4": {"class_type": "SaveImage", "inputs": {"images": ["3", 0], "filename_prefix": "remaster/sr"}},
        })

# --- image maths --------------------------------------------------------------------
def tiles_on(a, axis, limit=2.2):
    """Does the texture wrap around on this axis? Compares the jump across the wrap
    with the typical jump between neighbouring texels."""
    inner = np.abs(np.diff(a, axis=axis)).mean() + 1e-3
    edge = np.abs(a.take(0, axis=axis) - a.take(-1, axis=axis)).mean()
    return edge / inner < limit

def one_bit(alpha):
    """Alpha that is only on or off (DXT1 cutouts: leaves, fences)."""
    return ((alpha > 0.02) & (alpha < 0.98)).mean() < 0.01

def dark_fringe(rgb, alpha):
    """In 1-bit alpha (DXT1) the opaque texels next to a hole often came out near black.
    Those are found here: touching a hole and far darker than the opaque texels around."""
    hole = alpha < 0.5
    if not one_bit(alpha):  # real soft alpha: leave it be
        return np.zeros_like(hole)
    touch = np.zeros_like(hole)
    touch[1:] |= hole[:-1]; touch[:-1] |= hole[1:]; touch[:, 1:] |= hole[:, :-1]; touch[:, :-1] |= hole[:, 1:]
    lum = rgb.mean(axis=2)
    op = (~hole).astype(np.float32)
    around = blur(lum * op, 2.0, (False, False)) / np.maximum(blur(op, 2.0, (False, False)), 1e-3)
    return ~hole & touch & (lum < 0.5 * around)

def fill_transparent(rgb, alpha):
    """Colour for fully transparent texels (and dark DXT1 fringes), pulled in from the
    visible ones through a pyramid of averages, so filtering and mipmaps blend no black."""
    hole = (alpha < 1.0 / 255) | dark_fringe(rgb, alpha)
    if not hole.any() or hole.all():
        return rgb
    out = rgb.copy()
    w = (~hole).astype(np.float32)
    acc, wt = rgb * w[..., None], w
    levels = []
    while min(acc.shape[:2]) > 1:
        levels.append((acc, wt))
        h2, w2 = acc.shape[0] // 2 * 2, acc.shape[1] // 2 * 2
        acc = acc[:h2, :w2].reshape(h2 // 2, 2, w2 // 2, 2, 3).sum((1, 3))
        wt = wt[:h2, :w2].reshape(h2 // 2, 2, w2 // 2, 2).sum((1, 3))
    col = acc / np.maximum(wt, 1e-6)[..., None]
    for acc, wt in reversed(levels):  # coarse to fine: keep a level's colour where it has one
        up = np.asarray(Image.fromarray(np.clip(col * 255, 0, 255).astype(np.uint8)).resize(
            (acc.shape[1], acc.shape[0]), Image.BILINEAR), dtype=np.float32) / 255
        mine = acc / np.maximum(wt, 1e-6)[..., None]
        col = np.where((wt > 0)[..., None], mine, up)
    out[hole] = col[hole]
    return out

def blur(a, sigma, wrap):
    """Gaussian blur through the FFT; wrap = (x, y) wrap-around per axis, else mirrored."""
    out = a
    for axis, wr in ((1, wrap[0]), (0, wrap[1])):
        n = out.shape[axis]
        pad = 0 if wr else min(n - 1, int(3 * sigma) + 1)
        if pad:
            widths = [(0, 0)] * out.ndim; widths[axis] = (pad, pad)
            out = np.pad(out, widths, mode="reflect")
        m = out.shape[axis]
        g = np.exp(-2 * (np.pi * np.fft.rfftfreq(m) * sigma) ** 2)
        shape = [1] * out.ndim; shape[axis] = -1
        out = np.fft.irfft(np.fft.rfft(out, axis=axis) * g.reshape(shape), n=m, axis=axis)
        if pad:
            out = out.take(range(pad, pad + n), axis=axis)
    return out

def resize(a, size, wrap):
    """Lanczos resize of a float picture; on wrapping axes the filter reaches across the
    edge to the other side, so a tiling texture stays seamless."""
    flat = a.ndim == 2
    if flat:
        a = a[..., None]
    h, w = a.shape[:2]
    tw, th = size
    def pad_for(n, m):  # source texels whose resized width is a whole number of texels
        unit = n // math.gcd(n, m)
        p = unit * max(1, math.ceil(8 / unit))
        return p if p <= n else 0
    px = pad_for(w, tw) if wrap[0] else 0
    py = pad_for(h, th) if wrap[1] else 0
    ox, oy = px * tw // w, py * th // h
    padded = np.pad(a, ((py, py), (px, px), (0, 0)), mode="wrap")
    chans = [f32(to8(padded[..., c]).resize((tw + 2 * ox, th + 2 * oy), Image.LANCZOS))
             for c in range(a.shape[2])]
    out = np.stack(chans, axis=2)[oy:oy + th, ox:ox + tw]
    return out[..., 0] if flat else out

def restore_grain(big, src, k, amount, wrap):
    """Upscale models take fine grain (concrete, rust, soil) for noise and smooth it away.
    Put the original's detail back in the band the source itself can hold (features of
    about one to three source texels); finer detail stays the model's."""
    if amount <= 0:
        return big
    up = resize(src, (big.shape[1], big.shape[0]), wrap)
    d = up - big
    band = blur(d, 0.35 * k, wrap) - blur(d, 2.5 * k, wrap)
    return np.clip(big + amount * band, 0, 1)

def smooth(t):
    return t * t * (3 - 2 * t)

def unwrap(g, w, h, px, py):
    """Cut the tile out of a wrap-padded result; the first px/py texels are cross-faded
    into the result's continuation past the far edge, so it wraps without a step."""
    def fix_x(rows):
        out = rows[:, px:px + w].copy()
        if px:
            t = smooth((np.arange(px) + 0.5) / px)[None, :, None]
            out[:, :px] = out[:, :px] * t + rows[:, px + w:px + w + px] * (1 - t)
        return out
    out = fix_x(g[py:py + h])
    if py:
        far = fix_x(g[py + h:py + h + py])
        t = smooth((np.arange(py) + 0.5) / py)[:, None, None]
        out[:py] = out[:py] * t + far * (1 - t)
    return out

def to8(a):
    return Image.fromarray(np.clip(a * 255 + 0.5, 0, 255).astype(np.uint8))

def f32(img):
    return np.asarray(img, dtype=np.float32) / 255

# --- one texture --------------------------------------------------------------------
def target_size(w, h, o):
    k = o.scale if max(w, h) * o.scale <= o.max else o.max / max(w, h)
    if max(w, h) <= o.small:  # little textures are cheap: give them the model's full 4x
        k = max(k, min(4, o.max / max(w, h)))
    return max(1, round(w * k)), max(1, round(h * k))

def remaster(comfy, path, out_path, o):
    name = os.path.basename(path)
    src = Image.open(path)
    has_alpha = src.mode in ("RGBA", "LA", "PA") or "transparency" in src.info
    a = f32(src.convert("RGBA" if has_alpha else "RGB"))
    h, w = a.shape[:2]
    tw, th = target_size(w, h, o)
    rgb = a[..., :3]
    alpha = a[..., 3] if has_alpha else None
    cutout = has_alpha and (alpha < 0.5).mean() > 0.02
    wrap = (False, False) if cutout else (tiles_on(rgb, 1), tiles_on(rgb, 0))
    if has_alpha:
        rgb = fill_transparent(rgb, alpha)
    if max(w, h) < o.min_src:  # too small for the model
        out = to8(resize(rgb, (tw, th), wrap))
        if has_alpha:
            out = out.convert("RGBA"); out.putalpha(to8(resize(alpha, (tw, th), wrap)))
        how = "resized only"
    else:
        px = min(o.pad, w) if wrap[0] else 0
        py = min(o.pad, h) if wrap[1] else 0
        tag = re.sub(r"[^a-z0-9]+", "_", name.lower())
        def sr(pic, suffix):
            padded = np.pad(pic, ((py, py), (px, px), (0, 0)), mode="wrap")
            g = f32(comfy.upscale(to8(padded), f"rm_{tag}{suffix}.png", o.model))
            s = g.shape[1] / padded.shape[1]  # the model's factor
            k = round(s)
            return unwrap(g, w * k, h * k, px * k, py * k), k
        big, k = sr(rgb, "")
        big = restore_grain(big, rgb, k, o.grain, wrap)
        out = to8(big if big.shape[1::-1] == (tw, th) else resize(big, (tw, th), wrap))
        how = f"x{k} model"
        if has_alpha and one_bit(alpha):
            # The model rings around little holes; a smooth upscale cut steeply at the
            # alpha-test threshold gives clean round contours instead.
            ab = resize(alpha, (tw, th), wrap)
            t = np.clip((ab - 0.3) / 0.4, 0, 1)
            al = to8(t * t * (3 - 2 * t))
        elif has_alpha:
            ab, _ = sr(np.repeat(alpha[..., None], 3, axis=2), "_a")
            ab = ab.mean(axis=2)
            al = to8(ab if ab.shape[1::-1] == (tw, th) else resize(ab, (tw, th), wrap))
        if has_alpha:
            out = out.convert("RGBA"); out.putalpha(al)
    if out_path.lower().endswith(".png"):
        out.save(out_path, optimize=False, compress_level=6)
    else:
        out.convert("RGB").save(out_path, quality=o.quality, subsampling=0)
    wr = ("x" if wrap[0] else "-") + ("y" if wrap[1] else "-")
    return name, f"{w}x{h} -> {tw}x{th} ({how}, wrap {wr}{', alpha' if has_alpha else ''})"

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("src", help="folder with the textures")
    ap.add_argument("out", help="folder for the results (same names)")
    ap.add_argument("--server", default="http://127.0.0.1:8188")
    ap.add_argument("--model", default="4x-UltraSharpV2.safetensors", help="file in ComfyUI/models/upscale_models")
    ap.add_argument("--only", help="regex on the file names")
    ap.add_argument("--skip", default=r"^terrain_", help="regex of files left alone")
    ap.add_argument("--scale", type=float, default=2, help="size factor of a result")
    ap.add_argument("--small", type=int, default=256, help="textures up to this edge get 4x")
    ap.add_argument("--max", type=int, default=2048, help="longest edge of a result")
    ap.add_argument("--min-src", type=int, default=16, help="smaller sources are only resized")
    ap.add_argument("--pad", type=int, default=24, help="wrap padding in source texels")
    ap.add_argument("--grain", type=float, default=0.9, help="how much of the original's fine grain comes back (0..1)")
    ap.add_argument("--quality", type=int, default=95, help="JPEG quality")
    ap.add_argument("--jobs", type=int, default=3, help="textures in flight")
    o = ap.parse_args()
    os.makedirs(o.out, exist_ok=True)
    files = sorted(f for f in os.listdir(o.src) if f.lower().endswith((".jpg", ".jpeg", ".png")))
    files = [f for f in files if (not o.only or re.search(o.only, f)) and not (o.skip and re.search(o.skip, f))]
    comfy = Comfy(o.server)
    t0 = time.time()
    failed = 0
    def one(f):
        try:
            return remaster(comfy, os.path.join(o.src, f), os.path.join(o.out, f), o)
        except Exception as e:
            return f, "FAILED " + str(e)
    with ThreadPoolExecutor(o.jobs) as ex:
        for i, (n, msg) in enumerate(ex.map(one, files), 1):
            failed += msg.startswith("FAILED")
            print(f"[{i}/{len(files)} {time.time() - t0:5.0f}s] {n}: {msg}", flush=True)
    if failed:
        raise SystemExit(f"{failed} failed")

if __name__ == "__main__":
    main()
