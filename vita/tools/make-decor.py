#!/usr/bin/env python3
"""The little anime girls of the PC and the phones (art/decor), made Vita-sized for the VPK (vita/decor):
at most 340 px tall, WebP with their transparency. The Vita turns them to the accent's colour as the
PC does (they're drawn in rose), src/decor.c.

    python3 vita/tools/make-decor.py
"""
import glob
import os
from PIL import Image

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, '..', '..', 'art', 'decor')
out = os.path.join(here, '..', 'decor')
os.makedirs(out, exist_ok=True)
for f in sorted(glob.glob(os.path.join(src, '*.webp'))):
    im = Image.open(f).convert('RGBA')
    h = 340
    if im.height > h:
        im = im.resize((round(im.width * h / im.height), h), Image.LANCZOS)
    p = os.path.join(out, os.path.basename(f))
    im.save(p, 'WEBP', quality=88, method=6)
    print(os.path.basename(f), im.size, os.path.getsize(p))
