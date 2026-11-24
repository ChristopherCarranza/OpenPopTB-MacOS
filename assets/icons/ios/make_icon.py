import sys
from PIL import Image, ImageFilter, ImageDraw, ImageEnhance, ImageChops
S = sys.argv[1]
menu = Image.open(f"{S}/icon/menu.png").convert("RGB")
k = 2752 / 2000; N = 1024

# --- A: logo letters (keyed out by brightness) with a drop shadow, on the carved stone from the menu ---
# background: the left part of the menu (carvings, no menu text), square, lightly softened, vignetted
bg = menu.crop((0, int(330*k), int(700*k), int(1030*k))).resize((N, N), Image.LANCZOS)
bg = bg.filter(ImageFilter.GaussianBlur(2))
bg = ImageEnhance.Brightness(bg).enhance(0.8)
vig = Image.new("L", (N, N), 0); ImageDraw.Draw(vig).ellipse((-250, -250, N + 250, N + 250), fill=255)
vig = vig.filter(ImageFilter.GaussianBlur(160))
bg = Image.composite(bg, Image.new("RGB", (N, N), (25, 18, 8)), vig)
# letters
x0, y0, x1, y1 = int(545*k), int(20*k), int(1445*k), int(355*k)
logo = menu.crop((x0, y0, x1, y1))
w = int(N * 0.90); h = int(logo.height * w / logo.width)
logo = logo.resize((w, h), Image.LANCZOS)
lum = logo.convert("L")
alpha = lum.point(lambda v: max(0, min(255, (v - 120) * 4)))   # bright gold/cream letters only
alpha = alpha.filter(ImageFilter.GaussianBlur(0.8))
px, py = (N - w) // 2, (N - h) // 2
A = bg.copy()
shadow = Image.new("RGB", (w, h), (0, 0, 0))
sm = alpha.filter(ImageFilter.MaxFilter(9)).filter(ImageFilter.GaussianBlur(10)).point(lambda v: v * 0.85)
A.paste(shadow, (px + 8, py + 12), sm)
A.paste(logo, (px, py), alpha)
A.save(f"{S}/iconwork/A_logo.png")

# --- B: Mac 3D planet icon, full bleed, badge kept whole ---
mac = Image.open(f"{S}/icon/d3d.iconset/icon_512x512@2x.png").convert("RGBA")
inner = mac.crop((100, 100, 924, 924)).resize((N, N), Image.LANCZOS)
B = Image.new("RGB", (N, N), (20, 22, 58)); B.paste(inner, (0, 0), inner)
B.save(f"{S}/iconwork/B_planet.png")

def rounded(im, r=230):
    m = Image.new("L", im.size, 0); ImageDraw.Draw(m).rounded_rectangle((0, 0, im.width - 1, im.height - 1), r, fill=255)
    out = Image.new("RGBA", im.size, (0, 0, 0, 0)); out.paste(im, (0, 0), m); return out
pv = Image.new("RGBA", (N * 2 + 120, N + 80), (40, 40, 46, 255))
pv.paste(rounded(A), (40, 40), rounded(A)); pv.paste(rounded(B), (N + 80, 40), rounded(B))
pv.resize((pv.width // 2, pv.height // 2), Image.LANCZOS).save(f"{S}/iconwork/preview.png")
# small size check (home screen ~ 60-80 pt)
sm2 = Image.new("RGBA", (2 * 180 + 60, 200), (40, 40, 46, 255))
sm2.paste(rounded(A).resize((180, 180), Image.LANCZOS), (20, 10), rounded(A).resize((180, 180), Image.LANCZOS))
sm2.paste(rounded(B).resize((180, 180), Image.LANCZOS), (220, 10), rounded(B).resize((180, 180), Image.LANCZOS))
sm2.save(f"{S}/iconwork/preview_small.png")
