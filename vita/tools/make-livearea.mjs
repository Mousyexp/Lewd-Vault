// The LiveArea page (sce_sys/livearea/contents/), drawn as one scene and cut into its pieces so they
// sit together seamlessly (style a5): bg.png (840 x 500); the gate, startup.png (280 x 158), is the
// heart that starts the app; drive.png and local.png are two slim launchers (480 x 68, the right
// column's first two frames) that open the Google Drive vault and the vault on the card straight
// away; stats.png (360 x 210, frame7 under the gate) is the panel the app writes the vault's
// numbers on.
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
  <rect x="${x + 1}" y="${y + 1}" width="${w - 2}" height="${h * 0.45}" rx="${r - 1}" fill="url(#sheen)"/>`;
// the small launchers: a pill with a glyph and a name
const cloudGlyph = (cx, cy) => `<g transform="translate(${cx} ${cy - 2}) scale(.19)"><path d="M-62 30 C-86 30 -92 -6 -66 -14 C-68 -46 -26 -58 -8 -36 C4 -66 58 -60 58 -22 C86 -24 92 30 60 30 Z" fill="#fff"/></g>`;
const cardGlyph = (cx, cy) => `<g transform="translate(${cx} ${cy - 1}) scale(.22)"><path d="M-38 -54 H34 Q42 -54 42 -46 V54 Q42 62 34 62 H-34 Q-42 62 -42 54 V-38 Z" fill="#fff"/>${[0, 1, 2, 3].map(k => `<rect x="${-28 + k * 14}" y="34" width="8" height="18" rx="2" fill="#c84a7a"/>`).join('')}</g>`;
const launcher = (y, glyph, title, sub) => `${glass(374, y + 8, 300, 52, 26)}
  <circle cx="404" cy="${y + 34}" r="18" fill="#e8608a"/>${glyph(404, y + 34)}
  <text x="434" y="${y + 31}" font-family="Inter" font-weight="700" font-size="17" fill="#fff5f8">${title}</text>
  <text x="434" y="${y + 49}" font-family="Inter" font-size="12" fill="#f2c4d4">${sub}</text>
  <text x="652" y="${y + 40}" text-anchor="middle" font-family="Inter" font-weight="700" font-size="20" fill="#f2c4d4">›</text>`;

// Style a5 on the 840 x 500 background: the gate x 40-320 y 109-267; frame7 (bottom left) x 0-360
// y 290-500; frames 1-6 (the right column) x 360-840, each 68 high from y 92.
const scene = (layer) => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 840 500" width="840" height="500">
<defs>
  <radialGradient id="sky" cx=".22" cy=".38" r=".95"><stop offset="0" stop-color="#a3406b"/><stop offset=".45" stop-color="#4a1a38"/><stop offset="1" stop-color="#140a13"/></radialGradient>
  <radialGradient id="halo" cx=".5" cy=".5" r=".5"><stop offset="0" stop-color="#ff6f9c" stop-opacity=".6"/><stop offset="1" stop-color="#ff6f9c" stop-opacity="0"/></radialGradient>
  <linearGradient id="glass" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".17"/><stop offset="1" stop-color="#fff" stop-opacity=".06"/></linearGradient>
  <linearGradient id="rim" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffc2d6" stop-opacity=".75"/><stop offset="1" stop-color="#ffc2d6" stop-opacity=".2"/></linearGradient>
  <linearGradient id="sheen" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#fff" stop-opacity=".16"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></linearGradient>
  <filter id="ts"><feDropShadow dx="0" dy="2" stdDeviation="2" flood-color="#000" flood-opacity=".6"/></filter>
  <filter id="soft"><feDropShadow dx="0" dy="8" stdDeviation="9" flood-color="#000" flood-opacity=".5"/></filter>
</defs>
<rect width="840" height="500" fill="url(#sky)"/>
${flecks}
<circle cx="180" cy="188" r="170" fill="url(#halo)"/>
<text x="374" y="52" font-family="Inter Display" font-weight="800" font-size="36" fill="#fff5f8" filter="url(#ts)">Lewd Vault</text>
<text x="376" y="76" font-family="Inter" font-size="14" fill="#f2c4d4" letter-spacing=".5">your private collection, on the go</text>
${layer === 'gate' ? `<g filter="url(#soft)"><image href="${heart}" x="104" y="112" width="152" height="152"/></g>` : ''}
${layer === 'frames' ? `${launcher(92, cloudGlyph, 'Google Drive vault', 'Open it straight away')}${launcher(160, cardGlyph, 'Vault on the card', 'Open it straight away')}
  ${glass(14, 300, 332, 186, 26)}` : ''}
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
await shot('gate', 'startup.png', { x: 40, y: 109, width: 280, height: 158 });
await shot('frames', 'drive.png', { x: 360, y: 92, width: 480, height: 68 });
await shot('frames', 'local.png', { x: 360, y: 160, width: 480, height: 68 });
await shot('frames', 'stats.png', { x: 0, y: 290, width: 360, height: 210 });
await browser.close();
console.log('LiveArea pictures made');
