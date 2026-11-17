// The site switcher's icons (vita/sites/<key>.png, 112 x 112): modern app icons, each an iOS-style
// squircle in a two-tone gradient of the site's colour with a bold white monogram (or a glyph), a soft
// light across its top; and add.png, the "+" tile that adds a site. Drawn as SVG, rendered by Chromium
// (Playwright) at twice the size, then made smaller with Pillow (smooth edges).
//   node vita/tools/make-site-icons.mjs
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const { chromium } = createRequire(import.meta.url)('playwright');
const root = join(dirname(fileURLToPath(import.meta.url)), '..');

// key: [light, dark, the monogram (or a glyph's name)]
const SITES = {
  danbooru: ['#e0a062', '#7a4420', 'D'],
  yandere: ['#ff86b0', '#b8305f', 'Y'],
  konachan: ['#7d96ff', '#3646bd', 'K'],
  xcomics: ['#ffb84a', '#c2560f', 'XC'],
  xbooru: ['#ffd25c', '#d47414', 'X'],
  tbib: ['#a8adbd', '#454959', 'TB'],
  hypnohub: ['#bd8bff', '#5422a6', '@spiral'],
  safebooru: ['#a9d2ff', '#5274d4', 'S'],
  dancomics: ['#d79a5c', '#6b3a1a', 'DC'],
  mangadex: ['#ff8a5c', '#c4321a', 'MD'],
  ehentai: ['#c2505c', '#4f0e18', 'EH'],
  gelbooru: ['#5a9bff', '#123ba6', 'G'],
  rule34: ['#5fd27a', '#17642a', '34'],
  sakugabooru: ['#ff7a6a', '#b11f2b', '作'],
  rule34video: ['#ff5f7e', '#7a1030', 'R34'],
  r34anime: ['#ff9ad0', '#8a2a6a', 'R34'],
  eshuushuu: ['#7fd3ff', '#1f6fb5', 'ES'],
  booruio: ['#9aa7ff', '#3a3f9e', 'io'],
  xvideos: ['#ff6a5c', '#8a1414', 'EP'],
  hentaiworld: ['#ffb36b', '#c2410c', 'HW'],
  hentaicloud: ['#8fd0ff', '#2563eb', 'HC'],
  hanime: ['#ff5aa8', '#8c1a5a', 'h'],
  missav: ['#ff8a4a', '#a3300e', 'M'],
  add: ['#3a2f3c', '#241c26', '@plus'],
};

// a squircle (superellipse, n = 5): the iOS icon's continuous corners
const squircle = (cx, cy, r) => {
  const pts = [];
  for (let k = 0; k < 96; k++) {
    const t = (k / 96) * Math.PI * 2, c = Math.cos(t), s = Math.sin(t);
    pts.push([cx + Math.sign(c) * Math.abs(c) ** 0.4 * r, cy + Math.sign(s) * Math.abs(s) ** 0.4 * r]);
  }
  return 'M' + pts.map(p => p.map(v => v.toFixed(2)).join(' ')).join(' L') + ' Z';
};
const spiral = () => {
  let d = '';
  for (let k = 0; k <= 200; k++) {
    const t = k / 200 * Math.PI * 5.4, r = 5 + t * 4.4;
    d += (k ? ' L' : 'M') + (112 + Math.cos(t) * r).toFixed(1) + ' ' + (112 + Math.sin(t) * r).toFixed(1);
  }
  return `<path d="${d}" fill="none" stroke="#fff" stroke-width="15" stroke-linecap="round"/>`;
};
const glyph = (mono) => {
  if (mono === '@spiral') return spiral();
  if (mono === '@plus') return `<path d="M112 70 V154 M70 112 H154" stroke="#fff" stroke-opacity=".85" stroke-width="16" stroke-linecap="round"/>`;
  const cjk = /[　-鿿]/.test(mono);
  const size = cjk ? 112 : mono.length > 1 ? 104 : 132;
  return `<text x="112" y="${cjk ? 152 : 112 + size * 0.36}" text-anchor="middle" font-family="${cjk ? 'IPAGothic' : 'Inter Display'}" font-weight="900" font-size="${size}" letter-spacing="${mono.length > 1 ? -6 : 0}" fill="#fff">${mono}</text>`;
};
const icon = (key, [a, b, mono]) => {
  const add = key === 'add';
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 224 224" width="224" height="224">
  <defs>
    <linearGradient id="g" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="${a}"/><stop offset="1" stop-color="${b}"/></linearGradient>
    <radialGradient id="l" cx=".28" cy=".05" r=".85"><stop offset="0" stop-color="#fff" stop-opacity=".42"/><stop offset=".55" stop-color="#fff" stop-opacity=".06"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
    <clipPath id="c"><path d="${squircle(112, 112, 104)}"/></clipPath>
    <filter id="s" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="4" stdDeviation="5" flood-color="#000" flood-opacity=".28"/></filter>
  </defs>
  <path d="${squircle(112, 112, 104)}" fill="url(#g)"/>
  <g clip-path="url(#c)"><rect width="224" height="224" fill="url(#l)"/><circle cx="196" cy="214" r="80" fill="#000" opacity=".08"/></g>
  ${add ? `<path d="${squircle(112, 112, 96)}" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width="5" stroke-dasharray="14 12"/>` : `<path d="${squircle(112, 112, 102.5)}" fill="none" stroke="#fff" stroke-opacity=".22" stroke-width="3"/>`}
  <g clip-path="url(#c)"><g filter="url(#s)">${glyph(mono)}</g></g>
</svg>`;
};

mkdirSync(join(root, 'sites'), { recursive: true });
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 224, height: 224 } });
for (const [key, spec] of Object.entries(SITES)) {
  await page.setContent(`<body style="margin:0;background:transparent">${icon(key, spec)}</body>`);
  await page.waitForTimeout(50);
  const big = join(root, 'sites', `${key}.big.png`);
  await page.screenshot({ path: big, omitBackground: true, clip: { x: 0, y: 0, width: 224, height: 224 } });
  execFileSync('python3', ['-c', 'from PIL import Image; import os, sys; Image.open(sys.argv[1]).resize((112, 112), Image.LANCZOS).save(sys.argv[2], optimize=True); os.remove(sys.argv[1])', big, join(root, 'sites', `${key}.png`)]);
}
await browser.close();
console.log(`made ${Object.keys(SITES).length} icons in vita/sites/`);
