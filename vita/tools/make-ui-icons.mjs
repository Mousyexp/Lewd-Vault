// The app's icons (vita/ui/*.png): white glyphs on transparent, in the manner of iOS's symbols
// (rounded 2 px strokes; a filled variant for a selected tab), rendered at exactly the size the Vita
// draws them (the tab bar's at 30 px, the small ones at 22 px), so nothing is scaled. The app tints
// them (vita2d_draw_texture_tint). Rendered by Chromium (Playwright).
//   node vita/tools/make-ui-icons.mjs
import { createRequire } from 'node:module';
import { mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
const { chromium } = createRequire(import.meta.url)('playwright');
const out = join(dirname(fileURLToPath(import.meta.url)), '..', 'ui');
mkdirSync(out, { recursive: true });

const S = 'fill="none" stroke="#fff" stroke-width="1.9" stroke-linecap="round" stroke-linejoin="round"';
const F = 'fill="#fff"';
// a gear: 8 rounded teeth around a ring
const gear = (fill) => {
  const pts = [];
  for (let k = 0; k < 16; k++) {
    const a0 = (k / 16) * Math.PI * 2 - Math.PI / 16 * 0.55, a1 = (k / 16) * Math.PI * 2 + Math.PI / 16 * 0.55;
    const r = k % 2 ? 7.4 : 9.6;
    pts.push([12 + Math.cos(a0) * r, 12 + Math.sin(a0) * r], [12 + Math.cos(a1) * r, 12 + Math.sin(a1) * r]);
  }
  const d = 'M' + pts.map(p => p.map(v => v.toFixed(2)).join(' ')).join(' L') + ' Z';
  return fill ? `<path d="${d} M12 8.6 a3.4 3.4 0 1 0 0.01 0 Z" ${F} fill-rule="evenodd"/>`
              : `<path d="${d}" ${S}/><circle cx="12" cy="12" r="3.2" ${S}/>`;
};
const ICONS = {
  // the tab bar
  photos: `<rect x="5.5" y="3.5" width="15" height="12" rx="2.6" ${S} opacity=".55"/><rect x="2.5" y="7" width="16" height="13" rx="2.8" ${S}/><path d="M5 17.5 l4 -4.2 l3 3 l2 -2 l3 3.2" ${S}/><circle cx="8" cy="11.2" r="1.3" ${F}/>`,
  photos_fill: `<rect x="5.5" y="3.5" width="15" height="12" rx="2.6" ${F} opacity=".5"/><path d="M5.3 7 h10.9 a2.8 2.8 0 0 1 2.8 2.8 v7.4 a2.8 2.8 0 0 1 -2.8 2.8 h-10.9 a2.8 2.8 0 0 1 -2.8 -2.8 v-7.4 a2.8 2.8 0 0 1 2.8 -2.8 Z M5 17.5 l4 -4.2 l3 3 l2 -2 l3 3.2 Z M8 9.9 a1.3 1.3 0 1 0 0.01 0 Z" ${F} fill-rule="evenodd"/>`,
  compass: `<circle cx="12" cy="12" r="9.3" ${S}/><path d="M15.6 8.4 L13.3 13.3 L8.4 15.6 L10.7 10.7 Z" ${S}/><circle cx="12" cy="12" r="1" ${F}/>`,
  compass_fill: `<path d="M12 2.7 a9.3 9.3 0 1 0 0.01 0 Z M15.6 8.4 L13.3 13.3 L8.4 15.6 L10.7 10.7 Z" ${F} fill-rule="evenodd"/><path d="M15.6 8.4 L13.3 13.3 L10.7 10.7 Z" ${F} opacity=".45"/>`,
  play: `<rect x="2.5" y="5" width="19" height="14" rx="3.4" ${S}/><path d="M10.2 9.2 v5.6 l4.8 -2.8 Z" ${F}/>`,
  play_fill: `<path d="M5.9 5 h12.2 a3.4 3.4 0 0 1 3.4 3.4 v7.2 a3.4 3.4 0 0 1 -3.4 3.4 h-12.2 a3.4 3.4 0 0 1 -3.4 -3.4 v-7.2 a3.4 3.4 0 0 1 3.4 -3.4 Z M10.2 9.2 v5.6 l4.8 -2.8 Z" ${F} fill-rule="evenodd"/>`,
  gear: gear(false),
  gear_fill: gear(true),
  // small ones
  search: `<circle cx="10.5" cy="10.5" r="6.3" ${S}/><path d="M15.2 15.2 L20.5 20.5" ${S} stroke-width="2.3"/>`,
  sliders: `<path d="M3.5 7 h17 M3.5 12 h17 M3.5 17 h17" ${S}/><circle cx="8" cy="7" r="2.1" ${F}/><circle cx="15.5" cy="12" r="2.1" ${F}/><circle cx="10" cy="17" r="2.1" ${F}/>`,
  sort: `<path d="M8 19 V5 M4.5 8.5 L8 5 L11.5 8.5 M16 5 V19 M12.5 15.5 L16 19 L19.5 15.5" ${S}/>`,
  back: `<path d="M15 4.5 L7.5 12 L15 19.5" ${S} stroke-width="2.4"/>`,
  heart: `<path d="M12 20 C5.5 15.5 2.8 12.4 2.8 8.7 C2.8 6 4.9 4 7.5 4 C9.4 4 11 5.1 12 6.6 C13 5.1 14.6 4 16.5 4 C19.1 4 21.2 6 21.2 8.7 C21.2 12.4 18.5 15.5 12 20 Z" ${S}/>`,
  heart_fill: `<path d="M12 20 C5.5 15.5 2.8 12.4 2.8 8.7 C2.8 6 4.9 4 7.5 4 C9.4 4 11 5.1 12 6.6 C13 5.1 14.6 4 16.5 4 C19.1 4 21.2 6 21.2 8.7 C21.2 12.4 18.5 15.5 12 20 Z" ${F}/>`,
  info: `<circle cx="12" cy="12" r="9.3" ${S}/><path d="M12 11 v6" ${S} stroke-width="2.2"/><circle cx="12" cy="7.6" r="1.25" ${F}/>`,
  plus: `<path d="M12 4.5 V19.5 M4.5 12 H19.5" ${S} stroke-width="2.3"/>`,
  xmark: `<path d="M6 6 L18 18 M18 6 L6 18" ${S} stroke-width="2.3"/>`,
  more: `<circle cx="5.5" cy="12" r="1.8" ${F}/><circle cx="12" cy="12" r="1.8" ${F}/><circle cx="18.5" cy="12" r="1.8" ${F}/>`,
  film: `<rect x="3" y="4" width="18" height="16" rx="2.8" ${S}/><path d="M7.5 4 V20 M16.5 4 V20 M3 9.3 H7.5 M3 14.7 H7.5 M16.5 9.3 H21 M16.5 14.7 H21" ${S}/>`,
  book: `<path d="M12 6.5 C9.8 4.9 6.8 4.4 3 4.6 V18.6 C6.8 18.4 9.8 18.9 12 20.5 C14.2 18.9 17.2 18.4 21 18.6 V4.6 C17.2 4.4 14.2 4.9 12 6.5 Z M12 6.5 V20.5" ${S}/>`,
  image: `<rect x="3" y="4.5" width="18" height="15" rx="2.8" ${S}/><path d="M5.5 17 l4.5 -4.6 l3.2 3.2 l2.2 -2.2 l3.2 3.4" ${S}/><circle cx="8.6" cy="9.2" r="1.4" ${F}/>`,
};
const TAB = ['photos', 'photos_fill', 'compass', 'compass_fill', 'play', 'play_fill', 'gear', 'gear_fill'];

const browser = await chromium.launch();
const page = await browser.newPage();
for (const [name, body] of Object.entries(ICONS)) {
  const px = TAB.includes(name) ? 30 : 22;
  await page.setViewportSize({ width: px, height: px });
  await page.setContent(`<body style="margin:0;background:transparent"><svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="${px}" height="${px}">${body}</svg></body>`);
  await page.screenshot({ path: join(out, `${name}.png`), omitBackground: true, clip: { x: 0, y: 0, width: px, height: px } });
}
await browser.close();
console.log(`${Object.keys(ICONS).length} icons in vita/ui/`);
