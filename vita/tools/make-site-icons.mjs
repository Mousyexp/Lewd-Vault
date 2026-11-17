// The Browser's site switcher tiles: vita/sites/<key>.png, 112 x 112. A site with a sharp icon of its
// own (art/favicons/, as the site serves it) has it on a white plate on a card in the site's colour;
// a site whose icon is only 16 px gets the app's mascot instead (a little cat, drawn here, holding up
// the site's name). Drawn as SVG and rendered by Chromium (Playwright), then made smaller with Pillow.
//   node vita/tools/make-site-icons.mjs
import { createRequire } from 'node:module';
import { execFileSync } from 'node:child_process';
import { readFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const { chromium } = createRequire(import.meta.url)('playwright'); // (wherever it's installed: NODE_PATH works for this)
const root = join(dirname(fileURLToPath(import.meta.url)), '..');
// key: [colour, the icon (or null: the mascot), the mascot's sign, pixel art]
const SITES = {
  danbooru: ['#a87444', null, 'Danbooru'],
  yandere: ['#e8608a', null, 'Yande.re'],
  konachan: ['#5a78d0', 'konachan.png', '', true],
  e621: ['#1f4f9a', 'e621.png'],
  xbooru: ['#d9a63a', 'xbooru.png'],
  tbib: ['#6e6e7c', null, 'TBIB'],
  hypnohub: ['#8e55c8', null, 'Hypnohub'],
  aibooru: ['#4f9f4a', 'aibooru.svg'],
  safebooru: ['#8a86c8', 'safebooru.png'],
  e926: ['#2a6fb8', 'e926.png'],
  gelbooru: ['#2a6bd8', null, 'Gelbooru'],
  rule34: ['#3d8f45', 'rule34.png', '', true],
  sakugabooru: ['#d24848', null, 'Sakuga'],
};

const shade = (hex, f) => {
  const n = parseInt(hex.slice(1), 16);
  return '#' + [n >> 16, (n >> 8) & 255, n & 255].map(v => Math.round(f > 0 ? v + (255 - v) * f : v * (1 + f))).map(v => v.toString(16).padStart(2, '0')).join('');
};
const card = (col, inner, id) => `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 224 224" width="224" height="224">
  <defs>
    <linearGradient id="bg${id}" x1="0" y1="0" x2=".3" y2="1"><stop offset="0" stop-color="${shade(col, 0.45)}"/><stop offset=".55" stop-color="${col}"/><stop offset="1" stop-color="${shade(col, -0.35)}"/></linearGradient>
    <radialGradient id="gl${id}" cx=".3" cy=".1" r=".9"><stop offset="0" stop-color="#fff" stop-opacity=".55"/><stop offset=".5" stop-color="#fff" stop-opacity=".08"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
    <filter id="sh${id}" x="-20%" y="-20%" width="140%" height="140%"><feDropShadow dx="0" dy="5" stdDeviation="6" flood-color="#000" flood-opacity=".35"/></filter>
  </defs>
  <rect x="6" y="6" width="212" height="212" rx="52" fill="url(#bg${id})"/>
  <rect x="6" y="6" width="212" height="212" rx="52" fill="url(#gl${id})"/>
  <rect x="7.5" y="7.5" width="209" height="209" rx="50.5" fill="none" stroke="#fff" stroke-opacity=".35" stroke-width="3"/>
  <g filter="url(#sh${id})">${inner}</g>
</svg>`;
const iconTile = (col, data, id, pixel) => card(col, `<rect x="40" y="40" width="144" height="144" rx="36" fill="#fff"/>
  <image href="${data}" x="52" y="52" width="120" height="120" preserveAspectRatio="xMidYMid meet" ${pixel ? 'style="image-rendering:pixelated"' : ''}/>`, id);
const mascot = (col, label, id) => {
  const base = shade(col, 0.35), light = shade(col, 0.75), dark = shade(col, -0.45), eye = shade(col, -0.2);
  const ear = `<path d="M58 92 C50 70 44 44 52 30 C62 30 86 50 100 70 Z" fill="url(#fur${id})" stroke="${dark}" stroke-width="4" stroke-linejoin="round"/>
    <path d="M63 80 C59 64 57 50 60 42 C68 46 80 58 88 68 Z" fill="#ffb4c9"/>`;
  const eyeAt = x => `<ellipse cx="${x}" cy="124" rx="14" ry="18" fill="url(#iris${id})"/>
    <ellipse cx="${x}" cy="124" rx="14" ry="18" fill="none" stroke="#2a1426" stroke-width="2.5"/>
    <circle cx="${x - 5}" cy="115" r="6" fill="#fff"/><circle cx="${x + 5}" cy="133" r="3" fill="#fff" opacity=".9"/>`;
  return card(col, `<defs>
      <radialGradient id="fur${id}" cx=".38" cy=".3" r=".8"><stop offset="0" stop-color="#fff"/><stop offset=".45" stop-color="${light}"/><stop offset="1" stop-color="${base}"/></radialGradient>
      <linearGradient id="iris${id}" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#2a1426"/><stop offset=".55" stop-color="${eye}"/><stop offset="1" stop-color="${shade(col, 0.3)}"/></linearGradient>
    </defs>
    ${ear}<g transform="translate(224,0) scale(-1,1)">${ear}</g>
    <path d="M112 62 C162 62 190 92 190 128 C190 166 156 184 112 184 C68 184 34 166 34 128 C34 92 62 62 112 62 Z" fill="url(#fur${id})" stroke="${dark}" stroke-width="4"/>
    <path d="M104 64 q8 -10 16 0" fill="none" stroke="${dark}" stroke-width="3" stroke-linecap="round"/>
    ${eyeAt(84)}${eyeAt(140)}
    <ellipse cx="62" cy="148" rx="12" ry="6.5" fill="#ff6f9c" opacity=".5"/><ellipse cx="162" cy="148" rx="12" ry="6.5" fill="#ff6f9c" opacity=".5"/>
    <path d="M101 146 q5.5 7 11 0 q5.5 7 11 0" fill="none" stroke="#2a1426" stroke-width="3.2" stroke-linecap="round" stroke-linejoin="round"/>
    <g transform="rotate(-6 112 186)">
      <rect x="60" y="164" width="104" height="40" rx="14" fill="#fff" stroke="${dark}" stroke-width="3.5"/>
      <text x="112" y="192" text-anchor="middle" font-family="DejaVu Sans, Arial" font-weight="bold" font-size="${label.length > 6 ? 17 : 21}" fill="${dark}">${label}</text>
      <ellipse cx="66" cy="168" rx="11" ry="9" fill="url(#fur${id})" stroke="${dark}" stroke-width="3"/>
      <ellipse cx="158" cy="168" rx="11" ry="9" fill="url(#fur${id})" stroke="${dark}" stroke-width="3"/>
    </g>`, id);
};

mkdirSync(join(root, 'sites'), { recursive: true });
const browser = await chromium.launch();
const page = await browser.newPage({ viewport: { width: 224, height: 224 } });
let id = 0;
for (const [key, [col, icon, label, pixel]] of Object.entries(SITES)) {
  let svg;
  if (icon) {
    const bytes = readFileSync(join(root, 'art', 'favicons', icon));
    svg = iconTile(col, `data:image/${icon.endsWith('.svg') ? 'svg+xml' : 'png'};base64,${bytes.toString('base64')}`, ++id, pixel);
  } else svg = mascot(col, label, ++id);
  await page.setContent(`<body style="margin:0;background:transparent">${svg}</body>`);
  const big = join(root, 'sites', `${key}.big.png`);
  await page.screenshot({ path: big, omitBackground: true, clip: { x: 0, y: 0, width: 224, height: 224 } });
  // (112 x 112, smooth)
  execFileSync('python3', ['-c', `from PIL import Image; import os, sys; p = sys.argv[1]; Image.open(p).resize((112, 112), Image.LANCZOS).save(sys.argv[2], optimize=True); os.remove(p)`, big, join(root, 'sites', `${key}.png`)]);
}
await browser.close();
console.log(`made ${id} tiles in vita/sites/`);
