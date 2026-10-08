# Desktop wallpaper in the splash's style (tools/make-splash.py): charcoal, one
# orange glow, the hex mark and the name. Laid out on a 1920x1080 grid and
# scaled, so every size is the same picture.
#   python3 tools/make-wallpaper.py images/wallpaper-3840x2160.png 3840 2160
import math, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter
out, W, H = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
S = 2 * min(W / 1920, H / 1080)    # supersample x grid scale
w, h = W * 2, H * 2
ORANGE = (242, 138, 30)            # #F28A1E, the editor accent
BG     = (18, 18, 20)
U = "/usr/share/fonts/truetype/ubuntu/"
def px(v): return int(v * S)

img = Image.new("RGB", (w, h), BG)
cx, cy = w // 2, h // 2 - px(40)
glow = Image.new("L", (w, h), 0)
ImageDraw.Draw(glow).ellipse([cx - px(820), cy - px(460), cx + px(820), cy + px(460)], fill=255)
glow = glow.filter(ImageFilter.GaussianBlur(px(300)))
img = Image.composite(Image.new("RGB", (w, h), (70, 40, 14)), img, glow.point(lambda v: v * 0.5))
vig = Image.new("L", (w, h), 255)
ImageDraw.Draw(vig).rounded_rectangle([px(160), px(130), w - px(160), h - px(130)], radius=px(340), fill=0)
vig = vig.filter(ImageFilter.GaussianBlur(px(200)))
img = Image.composite(Image.new("RGB", (w, h), (8, 8, 9)), img, vig.point(lambda v: v * 0.7))
d = ImageDraw.Draw(img)

def hexpts(r, x, y):
    return [(x + r * math.cos(math.radians(60 * i - 90)), y + r * math.sin(math.radians(60 * i - 90))) for i in range(6)]
mx, my, R = cx, cy - px(170), px(92)
d.polygon(hexpts(R, mx, my), fill=ORANGE)
d.polygon(hexpts(R - px(8.5), mx, my), fill=(28, 28, 31))
k = R / px(118)                    # road drawn for the splash's 118px hexagon
def curve(off, t):
    return (mx + k * (px(-62) + px(124) * t) + off,
            my + k * (px(78) - px(156) * t + px(34) * math.sin(t * math.pi * 2)))
for off in (-k * px(22), k * px(22)):
    d.line([curve(off, 0.17 + 0.66 * i / 60) for i in range(61)], fill=ORANGE, width=int(k * px(7)), joint="curve")
for i in range(12, 48, 9):
    d.line([curve(0, j / 60) for j in range(i, i + 4)], fill=(235, 235, 240), width=int(k * px(5)))

def spaced(y, text, f, fill, spacing):
    widths = [d.textlength(c, font=f) for c in text]
    x = (w - (sum(widths) + px(spacing) * (len(text) - 1))) / 2
    for c, cw in zip(text, widths):
        d.text((x, y), c, font=f, fill=fill); x += cw + px(spacing)

spaced(cy - px(40), "FITZEL", ImageFont.truetype("/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", px(120)),
       (240, 240, 244), 11)
ft = ImageFont.truetype(U + "Ubuntu-M.ttf", px(26))
a, b = "LIGHTNING FAST", "  3D ENGINE AND MODELER"
x = (w - d.textlength(a + b, font=ft)) / 2; y = cy + px(130)
d.text((x, y), a, font=ft, fill=ORANGE)
d.text((x + d.textlength(a, font=ft), y), b, font=ft, fill=(200, 200, 206))
d.line([(cx - px(48), cy + px(190)), (cx + px(48), cy + px(190))], fill=ORANGE, width=max(2, px(2.5)))
spaced(h - px(90), "NATURAL. SCALABLE. UNLIMITED.", ImageFont.truetype(U + "Ubuntu-M.ttf", px(15)),
       (110, 110, 118), 5)

img.resize((W, H), Image.LANCZOS).save(out, optimize=True)
