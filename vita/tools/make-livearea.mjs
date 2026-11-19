// The LiveArea page (sce_sys/livearea/contents/), drawn as one scene and cut into its pieces so they
// sit together seamlessly: bg.png (840 x 500) and the gate startup.png (280 x 158: the icon alone),
// which style a1 puts at x 280-560 y 143-301 of the background. The
// title is along the bottom in a script face (Great Vibes, SIL OFL: tools/fonts), clear of the
// system's icons along the top.
// Rendered by Chromium (Playwright); the system wants 8-bit palette PNGs, made by Pillow.
//   node vita/tools/make-livearea.mjs
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { readFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const { chromium } = createRequire(import.meta.url)('playwright');
const root = join(dirname(fileURLToPath(import.meta.url)), '..');
const out = join(root, 'sce_sys', 'livearea', 'contents');
mkdirSync(out, { recursive: true });
const heart = 'data:image/png;base64,' + readFileSync(join(root, '..', 'art', 'app-icon.png')).toString('base64');
const script = 'data:font/ttf;base64,' + readFileSync(join(root, 'tools', 'fonts', 'GreatVibes-Regular.ttf')).toString('base64');

// bokeh: the same soft flecks every time
let seed = 21;
const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
const flecks = Array.from({ length: 34 }, () => {
  const r = 4 + rnd() * 22, x = rnd() * 840, y = rnd() * 500, a = 0.05 + rnd() * 0.16;
  return `<circle cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="${r.toFixed(1)}" fill="#ffa0c0" opacity="${a.toFixed(2)}"/>`;
}).join('');


const scene = (layer) => `<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 0 840 500" width="840" height="500">
<defs>
  <radialGradient id="sky" cx=".5" cy=".42" r=".8"><stop offset="0" stop-color="#9c3a64"/><stop offset=".45" stop-color="#4a1a38"/><stop offset="1" stop-color="#170b16"/></radialGradient>
  <radialGradient id="halo" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#ff6f9c" stop-opacity=".55"/><stop offset="1" stop-color="#ff6f9c" stop-opacity="0"/></radialGradient>
  <linearGradient id="glass" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".16"/><stop offset="1" stop-color="#fff" stop-opacity=".05"/></linearGradient>
  <linearGradient id="rim" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffc2d6" stop-opacity=".75"/><stop offset="1" stop-color="#ffc2d6" stop-opacity=".2"/></linearGradient>
  <linearGradient id="sheen" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".18"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
  <linearGradient id="ice" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffffff"/><stop offset="1" stop-color="#ffd3e2"/></linearGradient>
  <filter id="ts"><feDropShadow dx="0" dy="2" stdDeviation="2" flood-color="#000" flood-opacity=".6"/></filter>
  <filter id="soft"><feDropShadow dx="0" dy="6" stdDeviation="7" flood-color="#000" flood-opacity=".45"/></filter>
</defs>
<style>@font-face { font-family: 'Great Vibes'; src: url(${script}); }</style>
<rect width="840" height="500" fill="url(#sky)"/>
${flecks}
<circle cx="420" cy="222" r="190" fill="url(#halo)"/>
<text x="420" y="428" text-anchor="middle" font-family="Great Vibes" font-size="92" word-spacing="18" fill="#fff5f8" filter="url(#ts)">Lewd Vault</text>
<text x="420" y="470" text-anchor="middle" font-family="DejaVu Sans" font-size="15" fill="#f2c4d4" letter-spacing="2">your private collection, on the go</text>
${layer === 'gate' ? `<g filter="url(#soft)"><image href="${heart}" x="352" y="154" width="136" height="136"/></g>` : ''}

</svg>`;

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 840, height: 500 } });
const shot = async (layer, name, clip) => {
  await page.setContent(`<body style="margin:0">${scene(layer)}</body>`);
  await page.evaluate(() => document.fonts.ready);
  await page.waitForTimeout(300);
  const p = join(out, name);
  await page.screenshot({ path: p, clip });
  // (the system's own format: 8-bit palette)
  execFileSync('python3', ['-c', `from PIL import Image; import sys; im = Image.open(sys.argv[1]).convert('RGB'); im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(sys.argv[1], optimize=True)`, p]);
};
await shot('', 'bg.png', { x: 0, y: 0, width: 840, height: 500 });
await shot('gate', 'startup.png', { x: 280, y: 143, width: 280, height: 158 });
await browser.close();
console.log('LiveArea pictures made');
