# The Browser's site switcher tiles: sites/<key>.png, 112 x 112, from each site's own icon
# (art/favicons/<key>.png, the favicon as the site serves it). A sharp icon (32 px or more) is shown on
# a soft card in the site's colour; a tiny one (16 px) is held up by the app's own mascot, a round
# little creature with ears drawn here in the site's colour, so every tile looks finished.
# Run with python3 (Pillow): python3 vita/tools/make-site-icons.py
import math, os
from PIL import Image, ImageDraw, ImageFilter

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, '..')
S = 112
K = 4  # (drawn at 4x, then made smaller: smooth edges)

SITES = {  # key: the site's colour
    'danbooru': (164, 116, 70), 'yandere': (238, 130, 160), 'konachan': (120, 150, 230), 'e621': (30, 80, 160),
    'xbooru': (220, 170, 60), 'tbib': (110, 110, 120), 'hypnohub': (150, 90, 200), 'aibooru': (90, 180, 90),
    'safebooru': (240, 190, 120), 'e926': (40, 110, 190), 'gelbooru': (40, 110, 220), 'rule34': (60, 150, 70),
    'sakugabooru': (220, 60, 60),
}

def lighter(c, f):
    return tuple(int(v + (255 - v) * f) for v in c)

def darker(c, f):
    return tuple(int(v * (1 - f)) for v in c)

def card(col):
    im = Image.new('RGBA', (S * K, S * K), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    top, bot = lighter(col, 0.35), darker(col, 0.25)
    # a rounded card with a soft top-to-bottom shade
    mask = Image.new('L', im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, S * K - 1, S * K - 1], radius=26 * K, fill=255)
    grad = Image.new('RGBA', im.size)
    gd = ImageDraw.Draw(grad)
    for y in range(S * K):
        t = y / (S * K)
        gd.line([(0, y), (S * K, y)], fill=tuple(int(top[i] * (1 - t) + bot[i] * t) for i in range(3)) + (255,))
    im.paste(grad, (0, 0), mask)
    # a glossy highlight
    gl = Image.new('RGBA', im.size, (0, 0, 0, 0))
    ImageDraw.Draw(gl).ellipse([-20 * K, -60 * K, (S + 20) * K, 40 * K], fill=(255, 255, 255, 46))
    im.alpha_composite(Image.composite(gl, Image.new('RGBA', im.size, (0, 0, 0, 0)), mask))
    return im, mask

def icon_on(im, fav, size, cx, cy, plate=True):
    # the site's icon, on a little white plate (pixel icons kept crisp: nearest)
    resample = Image.NEAREST if fav.width < 32 else Image.LANCZOS
    f = fav.convert('RGBA').resize((size, size), resample)
    if plate:
        p = int(size * 0.22)
        sh = Image.new('RGBA', im.size, (0, 0, 0, 0))
        ImageDraw.Draw(sh).rounded_rectangle([cx - size / 2 - p, cy - size / 2 - p + 3 * K, cx + size / 2 + p, cy + size / 2 + p + 3 * K], radius=p * 1.4, fill=(0, 0, 0, 90))
        im.alpha_composite(sh.filter(ImageFilter.GaussianBlur(3 * K)))
        ImageDraw.Draw(im).rounded_rectangle([cx - size / 2 - p, cy - size / 2 - p, cx + size / 2 + p, cy + size / 2 + p], radius=p * 1.4, fill=(255, 255, 255, 255))
    im.alpha_composite(f, (int(cx - size / 2), int(cy - size / 2)))

def mascot(im, col, cx, cy, r):
    # the mascot: a round body, two ears, big shiny eyes, blush and a small smile
    d = ImageDraw.Draw(im)
    body, edge = lighter(col, 0.55), darker(col, 0.15)
    for side in (-1, 1):
        ex = cx + side * r * 0.62
        ear = [(ex - side * r * 0.42, cy - r * 0.55), (ex + side * r * 0.18, cy - r * 1.28), (ex + side * r * 0.4, cy - r * 0.35)]
        d.polygon(ear, fill=body, outline=edge)
        inner = [(ex - side * r * 0.24, cy - r * 0.6), (ex + side * r * 0.12, cy - r * 1.05), (ex + side * r * 0.26, cy - r * 0.5)]
        d.polygon(inner, fill=(255, 170, 190))
    d.ellipse([cx - r, cy - r * 0.92, cx + r, cy + r * 0.98], fill=body, outline=edge, width=K * 2)
    for side in (-1, 1):
        ex, ey = cx + side * r * 0.38, cy - r * 0.08
        d.ellipse([ex - r * 0.17, ey - r * 0.22, ex + r * 0.17, ey + r * 0.22], fill=(40, 26, 40))
        d.ellipse([ex - r * 0.07, ey - r * 0.16, ex + r * 0.05, ey - r * 0.03], fill=(255, 255, 255))
        d.ellipse([ex + r * 0.03, ey + r * 0.06, ex + r * 0.09, ey + r * 0.12], fill=(255, 255, 255))
        bx = cx + side * r * 0.62
        d.ellipse([bx - r * 0.16, cy + r * 0.2, bx + r * 0.16, cy + r * 0.34], fill=(255, 140, 170))
    d.arc([cx - r * 0.13, cy + r * 0.12, cx + r * 0.01, cy + r * 0.3], 20, 160, fill=(40, 26, 40), width=K * 2)
    d.arc([cx - r * 0.01, cy + r * 0.12, cx + r * 0.13, cy + r * 0.3], 20, 160, fill=(40, 26, 40), width=K * 2)

def tile(key, col):
    fav = Image.open(os.path.join(root, 'art', 'favicons', key + '.png'))
    im, mask = card(col)
    if fav.width >= 32:
        icon_on(im, fav, 56 * K, S * K / 2, S * K / 2)
    else:
        # the mascot, holding up the site's little icon
        mascot(im, col, S * K * 0.42, S * K * 0.6, 30 * K)
        icon_on(im, fav, 26 * K, S * K * 0.74, S * K * 0.36)
        # its paws on the plate
        d = ImageDraw.Draw(im)
        for dx in (-10, 8):
            d.ellipse([S * K * 0.74 + dx * K - 5 * K, S * K * 0.5 - 4 * K, S * K * 0.74 + dx * K + 5 * K, S * K * 0.5 + 5 * K], fill=lighter(col, 0.55), outline=darker(col, 0.15), width=K)
    out = im.resize((S, S), Image.LANCZOS)
    out.putalpha(Image.composite(out.split()[3], Image.new('L', out.size, 0), mask.resize((S, S), Image.LANCZOS)))
    return out

os.makedirs(os.path.join(root, 'sites'), exist_ok=True)
for key, col in SITES.items():
    tile(key, col).save(os.path.join(root, 'sites', key + '.png'), optimize=True)
print('made', len(SITES), 'tiles in vita/sites/')
