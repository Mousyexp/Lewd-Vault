# The Vita's pictures, made from the app's heart (art/app-icon.png):
#   sce_sys/icon0.png                    128 x 128  the bubble on the home screen
#   sce_sys/pic0.png                     960 x 544  the cover art behind the LiveArea
#   (the LiveArea page: tools/make-livearea.mjs)
#   splash.png                           256 x 256  the heart the app's own loading screen draws
# The system wants the sce_sys ones as 8-bit palette PNGs. Run with python3 (Pillow).
import math, os, random
from PIL import Image, ImageDraw, ImageFilter, ImageFont
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, '..')
heart = Image.open(os.path.join(root, '..', 'art', 'app-icon.png')).convert('RGBA')
BOLD = '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
REG = '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'

def backdrop(w, h, cx=0.5, cy=0.45, seed=7):
    # deep plum, a rose glow where the heart sits, and soft out-of-focus flecks of light
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            d = math.hypot((x / w - cx) * w / h, y / h - cy)
            t = max(0.0, 1 - d * 1.35)
            t = t * t
            px[x, y] = (int(22 + 120 * t), int(12 + 34 * t), int(22 + 62 * t))
    glow = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    rnd = random.Random(seed)
    for _ in range(max(6, w * h // 9000)):
        r = rnd.uniform(0.01, 0.05) * h
        x, y = rnd.uniform(0, w), rnd.uniform(0, h)
        a = int(rnd.uniform(18, 60))
        g.ellipse([x - r, y - r, x + r, y + r], fill=(255, 150, 190, a))
    glow = glow.filter(ImageFilter.GaussianBlur(max(2, h // 90)))
    im = im.convert('RGBA')
    im.alpha_composite(glow)
    return im

def put_heart(im, size, cx, cy):
    hh = heart.resize((size, size), Image.LANCZOS)
    # a soft rose halo under it
    halo = Image.new('RGBA', im.size, (0, 0, 0, 0))
    ImageDraw.Draw(halo).ellipse([cx - size * 0.62, cy - size * 0.55, cx + size * 0.62, cy + size * 0.62], fill=(255, 92, 138, 120))
    im.alpha_composite(halo.filter(ImageFilter.GaussianBlur(size // 6)))
    shadow = Image.new('RGBA', im.size, (0, 0, 0, 0))
    shadow.paste((0, 0, 0, 140), (int(cx - size / 2), int(cy - size / 2 + size * 0.05)), hh.split()[3])
    im.alpha_composite(shadow.filter(ImageFilter.GaussianBlur(max(2, size // 25))))
    im.alpha_composite(hh, (int(cx - size / 2), int(cy - size / 2)))

def title(im, text, size, x, y, anchor='la', fill=(250, 236, 241, 255), font=BOLD, shadow=True):
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(font, size)
    if shadow:
        s = Image.new('RGBA', im.size, (0, 0, 0, 0))
        ImageDraw.Draw(s).text((x, y + size * 0.06), text, font=f, fill=(0, 0, 0, 170), anchor=anchor)
        im.alpha_composite(s.filter(ImageFilter.GaussianBlur(max(1, size // 18))))
    d.text((x, y), text, font=f, fill=fill, anchor=anchor)

def pal(im, rel):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    im.convert('RGB').quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(path, optimize=True)

# the bubble
b = backdrop(128, 128, 0.5, 0.48, seed=3)
put_heart(b, 98, 64, 66)
pal(b, 'sce_sys/icon0.png')

# the cover art
c = backdrop(960, 544, 0.5, 0.42, seed=11)
put_heart(c, 300, 480, 222)
title(c, 'Lewd Vault', 64, 480, 418, anchor='mm')
title(c, 'your private collection, on the go', 24, 480, 470, anchor='mm', fill=(240, 200, 214, 255), font=REG)
pal(c, 'sce_sys/pic0.png')

# (the LiveArea page itself, bg.png, startup.png and its frames: tools/make-livearea.mjs)

# the loading screen's heart (full colour, its own alpha)
heart.resize((256, 256), Image.LANCZOS).save(os.path.join(root, 'splash.png'), optimize=True)
print('Vita pictures made')
