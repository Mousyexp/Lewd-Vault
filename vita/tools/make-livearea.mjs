// The LiveArea page (sce_sys/livearea/contents/), drawn as one scene and cut into its pieces so they
// sit together seamlessly: bg.png (840 x 500), the gate startup.png (280 x 158), drive.png and
// local.png (the 260 x 260 frames either side of the gate: the Google Drive vault and the vault on the
// card, each opening it straight away) and strip.png (840 x 150 along the bottom, under the vault's
// numbers the app writes there). Where style a1 puts them on the 840 x 500 background: frame2 x 0-260
// and frame1 x 580-840 (y 90-350), frame3 y 350-500, the gate x 280-560 y 143-301.
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

// bokeh: the same soft flecks every time
let seed = 21;
const rnd = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
const flecks = Array.from({ length: 34 }, () => {
  const r = 4 + rnd() * 22, x = rnd() * 840, y = rnd() * 500, a = 0.05 + rnd() * 0.16;
  return `<circle cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="${r.toFixed(1)}" fill="#ffa0c0" opacity="${a.toFixed(2)}"/>`;
}).join('');

const glass = (x, y, w, h, r) => `<rect x="${x}" y="${y}" width="${w}" height="${h}" rx="${r}" fill="url(#glass)" stroke="url(#rim)" stroke-width="2"/>
  <rect x="${x + 1}" y="${y + 1}" width="${w - 2}" height="${h * 0.42}" rx="${r - 1}" fill="url(#sheen)"/>`;
const cloud = `<g transform="translate(126 200) scale(.92)">
  <path d="M-62 30 C-86 30 -92 -6 -66 -14 C-68 -46 -26 -58 -8 -36 C4 -66 58 -60 58 -22 C86 -24 92 30 60 30 Z" fill="url(#ice)" stroke="#fff" stroke-opacity=".7" stroke-width="2"/>
  <path d="M-4 20 V-24 M-20 -8 L-4 -24 L12 -8" fill="none" stroke="#e8608a" stroke-width="8" stroke-linecap="round" stroke-linejoin="round"/>
</g>`;
const card = `<g transform="translate(124 192) rotate(-8) scale(.88)">
  <path d="M-38 -54 H34 Q42 -54 42 -46 V54 Q42 62 34 62 H-34 Q-42 62 -42 54 V-38 Z" fill="url(#ice)" stroke="#fff" stroke-opacity=".7" stroke-width="2"/>
  <rect x="-28" y="-40" width="56" height="44" rx="8" fill="#e8608a" opacity=".85"/>
  <text x="0" y="-12" text-anchor="middle" font-family="DejaVu Sans" font-weight="bold" font-size="17" fill="#fff">VAULT</text>
  ${[0, 1, 2, 3, 4].map(k => `<rect x="${-28 + k * 12}" y="38" width="7" height="16" rx="2" fill="#c9a24a"/>`).join('')}
</g>`;
const label = (x, title, sub) => `<text x="${x}" y="292" text-anchor="middle" font-family="DejaVu Sans" font-weight="bold" font-size="22" fill="#fff5f8" filter="url(#ts)">${title}</text>
  <text x="${x}" y="318" text-anchor="middle" font-family="DejaVu Sans" font-size="14" fill="#f2c4d4">${sub}</text>`;

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
<rect width="840" height="500" fill="url(#sky)"/>
${flecks}
<circle cx="420" cy="222" r="190" fill="url(#halo)"/>
<text x="420" y="58" text-anchor="middle" font-family="DejaVu Sans" font-weight="bold" font-size="40" fill="#fff5f8" filter="url(#ts)">Lewd Vault</text>
<text x="420" y="84" text-anchor="middle" font-family="DejaVu Sans" font-size="15" fill="#f2c4d4" letter-spacing="1">your private collection, on the go</text>
${layer === 'gate' ? `<g filter="url(#soft)"><image href="${heart}" x="292" y="160" width="124" height="124"/></g>
  <text x="430" y="214" font-family="DejaVu Sans" font-weight="bold" font-size="32" fill="#fff5f8" filter="url(#ts)">Lewd</text>
  <text x="430" y="252" font-family="DejaVu Sans" font-weight="bold" font-size="32" fill="#fff5f8" filter="url(#ts)">Vault</text>` : ''}
${layer === 'frames' ? `
  <g>${glass(12, 102, 236, 236, 34)}<g filter="url(#soft)">${cloud}</g>${label(130, 'Google Drive', 'open that vault straight away')}
     <g filter="url(#soft)"><image href="${heart}" x="150" y="196" width="50" height="50"/></g></g>
  <g transform="translate(580 0)">${glass(12, 102, 236, 236, 34)}<g filter="url(#soft)">${card}</g>${label(130, 'On the card', 'open that vault straight away')}
     <g filter="url(#soft)"><image href="${heart}" x="148" y="194" width="50" height="50"/></g></g>
  ${glass(12, 362, 816, 126, 28)}` : ''}
</svg>`;

const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 840, height: 500 } });
const shot = async (layer, name, clip) => {
  await page.setContent(`<body style="margin:0">${scene(layer)}</body>`);
  await page.waitForTimeout(150);
  const p = join(out, name);
  await page.screenshot({ path: p, clip });
  // (the system's own format: 8-bit palette)
  execFileSync('python3', ['-c', `from PIL import Image; import sys; im = Image.open(sys.argv[1]).convert('RGB'); im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.FLOYDSTEINBERG).save(sys.argv[1], optimize=True)`, p]);
};
await shot('', 'bg.png', { x: 0, y: 0, width: 840, height: 500 });
await shot('gate', 'startup.png', { x: 280, y: 143, width: 280, height: 158 });
await shot('frames', 'drive.png', { x: 0, y: 90, width: 260, height: 260 });
await shot('frames', 'local.png', { x: 580, y: 90, width: 260, height: 260 });
await shot('frames', 'strip.png', { x: 0, y: 350, width: 840, height: 150 });
await browser.close();
console.log('LiveArea pictures made');
