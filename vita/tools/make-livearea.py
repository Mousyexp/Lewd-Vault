# Makes the Vita's home-screen pictures from the app's icon (src-tauri/icons/icon.png):
# sce_sys/icon0.png (128 x 128), and the LiveArea's bg.png (840 x 500) and startup.png (280 x 158).
# The Vita wants them as 8-bit palette PNGs. Run with python3 (Pillow) when the icon changes.
import os, sys
from PIL import Image, ImageDraw, ImageFont
here = os.path.dirname(os.path.abspath(__file__))
root = os.path.join(here, '..')
icon = Image.open(os.path.join(root, '..', 'src-tauri', 'icons', 'icon.png')).convert('RGBA')
def pal(im, path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    bg = Image.new('RGB', im.size, (18, 14, 17))
    bg.paste(im, mask=im.split()[3] if im.mode == 'RGBA' else None)
    bg.quantize(colors=256, method=Image.Quantize.MEDIANCUT).save(path, optimize=True)
pal(icon.resize((128, 128), Image.LANCZOS), os.path.join(root, 'sce_sys', 'icon0.png'))
font = None
for f in ['/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf']:
    if os.path.exists(f): font = f
def card(w, h, isz, title_size, path):
    im = Image.new('RGBA', (w, h), (18, 14, 17, 255))
    d = ImageDraw.Draw(im)
    for y in range(h):  # a soft glow of the app's rose, from the bottom
        a = int(70 * (y / h) ** 2)
        d.line([(0, y), (w, y)], fill=(255, 92, 138, a))
    ic = icon.resize((isz, isz), Image.LANCZOS)
    im.alpha_composite(ic, ((w - isz) // 2, int(h * 0.12)))
    if font:
        ft = ImageFont.truetype(font, title_size)
        t = 'Lewd Vault'
        tw = d.textlength(t, font=ft)
        d.text(((w - tw) / 2, int(h * 0.12) + isz + title_size * 0.4), t, font=ft, fill=(247, 232, 236, 255))
    pal(im, path)
card(840, 500, 220, 44, os.path.join(root, 'sce_sys', 'livearea', 'contents', 'bg.png'))
card(280, 158, 70, 18, os.path.join(root, 'sce_sys', 'livearea', 'contents', 'startup.png'))
print('livearea pictures made')
