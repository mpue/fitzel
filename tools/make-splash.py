# The splash: charcoal, one orange glow (#F28A1E, the editor accent), the hex
# mark, the name and the feature list. Drawn, not painted, so it can change.
#   python3 tools/make-splash.py images/splash.png
import math, sys
from PIL import Image, ImageDraw, ImageFont, ImageFilter
W, H, S = 1536, 1024, 2            # render at 2x, downsample for AA
w, h = W*S, H*S
ORANGE = (242, 138, 30)            # #F28A1E, the editor accent
BG     = (18, 18, 20)
F = "/usr/share/fonts/truetype/ubuntu/"
def font(name, px): return ImageFont.truetype(F+name, px*S)

# Background: flat charcoal, a faint orange glow behind the mark, soft vignette.
img = Image.new("RGB", (w, h), BG)
glow = Image.new("L", (w, h), 0)
gd = ImageDraw.Draw(glow)
cx, cy = w//2, int(h*0.40)
gd.ellipse([cx-700*S, cy-420*S, cx+700*S, cy+420*S], fill=255)
glow = glow.filter(ImageFilter.GaussianBlur(260*S))
img = Image.composite(Image.new("RGB", (w, h), (70, 40, 14)), img, glow.point(lambda v: v*0.55))
vig = Image.new("L", (w, h), 0)
ImageDraw.Draw(vig).rectangle([0, 0, w, h], fill=255)
ImageDraw.Draw(vig).rounded_rectangle([140*S, 120*S, w-140*S, h-120*S], radius=300*S, fill=0)
vig = vig.filter(ImageFilter.GaussianBlur(180*S))
img = Image.composite(Image.new("RGB", (w, h), (8, 8, 9)), img, vig.point(lambda v: v*0.7))
d = ImageDraw.Draw(img)

# Mark: an orange hexagon with a road curving through it.
def hexpts(r, cx, cy):
    return [(cx + r*math.cos(math.radians(60*i-90)), cy + r*math.sin(math.radians(60*i-90))) for i in range(6)]
mx, my, R = w//2, 250*S, 118*S
d.polygon(hexpts(R, mx, my), fill=ORANGE)
d.polygon(hexpts(R-11*S, mx, my), fill=(28, 28, 31))
# road: two parallel S-curves plus a dashed centre line
def curve(off, t):
    x = mx + (-62*S + 124*S*t) + off
    y = my + 78*S - 156*S*t + 34*S*math.sin(t*math.pi*2)
    return x, y
for off, wd in ((-22*S, 7*S), (22*S, 7*S)):
    d.line([curve(off, 0.17 + 0.66*i/60) for i in range(61)], fill=ORANGE, width=wd, joint="curve")
for i in range(12, 48, 9):
    d.line([curve(0, j/60) for j in range(i, i+4)], fill=(235, 235, 240), width=5*S)

def ctext(y, text, f, fill, spacing=0):
    if spacing == 0:
        tw = d.textlength(text, font=f); d.text(((w-tw)/2, y), text, font=f, fill=fill); return
    widths = [d.textlength(c, font=f) for c in text]
    x = (w - (sum(widths) + spacing*S*(len(text)-1))) / 2
    for c, cw in zip(text, widths):
        d.text((x, y), c, font=f, fill=fill); x += cw + spacing*S

ctext(390*S, "FITZEL", ImageFont.truetype("/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", 160*S), (240, 240, 244), spacing=14)

# Tagline: the first words in orange, the rest in grey.
ft = font("Ubuntu-M.ttf", 34)
a, b = "LIGHTNING FAST", "  3D ENGINE AND MODELER"
tw = d.textlength(a+b, font=ft); x = (w-tw)/2; y = 618*S
d.text((x, y), a, font=ft, fill=ORANGE); d.text((x + d.textlength(a, font=ft), y), b, font=ft, fill=(200, 200, 206))

d.line([(w/2-60*S, 700*S), (w/2+60*S, 700*S)], fill=ORANGE, width=3*S)

# Feature list: plain words, orange dots between them.
feats = ["PROCEDURAL VEGETATION", "SPLINE ROADS", "TERRAIN", "BUILT-IN MODELER", "ADVANCED LIGHTING"]
ff = font("Ubuntu-R.ttf", 21); gap = 34*S
parts = [d.textlength(t, font=ff) for t in feats]
x = (w - (sum(parts) + gap*2*(len(feats)-1))) / 2; y = 750*S
for i, (t, tw) in enumerate(zip(feats, parts)):
    d.text((x, y), t, font=ff, fill=(150, 150, 158)); x += tw
    if i < len(feats)-1:
        r = 4*S; dx = x + gap; dy = y + 13*S
        d.ellipse([dx-r, dy-r, dx+r, dy+r], fill=ORANGE); x += gap*2

ctext(920*S, "NATURAL. SCALABLE. UNLIMITED.", font("Ubuntu-M.ttf", 18), (120, 120, 128), spacing=6)

img.resize((W, H), Image.LANCZOS).save(sys.argv[1], optimize=True)
