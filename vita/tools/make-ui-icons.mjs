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
  wifi: `<path d="M2.6 9.2 C8 4.2 16 4.2 21.4 9.2 M5.6 12.6 C9.4 9.2 14.6 9.2 18.4 12.6 M8.7 16 C10.6 14.4 13.4 14.4 15.3 16" ${S} stroke-width="2.3"/>`,
  tag: `<path d="M3.5 4.8 V11 L12.8 20.3 L20.3 12.8 L11 3.5 H4.8 Z" ${S}/><circle cx="8" cy="8" r="1.5" ${F}/>`,
  chev_l: `<path d="M14.5 5 L7.5 12 L14.5 19" ${S} stroke-width="2.6"/>`,
  chev_r: `<path d="M9.5 5 L16.5 12 L9.5 19" ${S} stroke-width="2.6"/>`,
  refresh: `<path d="M19.5 12 A7.5 7.5 0 1 1 16.8 6.2 M17.5 2.8 V6.8 H13.5" ${S}/>`,
  // the filter and order lists, the comic reader's options (22 px)
  flame: `<path d="M12 2.8 C13.2 6.4 17.8 8.6 17.8 14 A5.8 5.8 0 0 1 6.2 14 C6.2 11.2 7.8 9.6 9 8.4 C9.2 10.4 10.1 11.6 11.2 12 C10.6 8.8 11.2 5.4 12 2.8 Z" ${S}/>`,
  star: `<path d="M12 3.2 L14.6 8.7 L20.6 9.4 L16.1 13.5 L17.3 19.5 L12 16.5 L6.7 19.5 L7.9 13.5 L3.4 9.4 L9.4 8.7 Z" ${S}/>`,
  album: `<rect x="3" y="6.5" width="18" height="13.5" rx="2.6" ${S}/><path d="M6 3.8 h12" ${S}/><path d="M7.5 15.5 l3 -3.2 l2.3 2.3 l1.6 -1.6 l2.6 2.6" ${S}/>`,
  stack: `<rect x="3" y="8.5" width="14" height="11.5" rx="2.4" ${S}/><path d="M6.5 5.5 h11 a2.5 2.5 0 0 1 2.5 2.5 v9" ${S}/>`,
  grid: `<rect x="3.5" y="3.5" width="7" height="7" rx="1.8" ${S}/><rect x="13.5" y="3.5" width="7" height="7" rx="1.8" ${S}/><rect x="3.5" y="13.5" width="7" height="7" rx="1.8" ${S}/><rect x="13.5" y="13.5" width="7" height="7" rx="1.8" ${S}/>`,
  bookmark: `<path d="M6.5 3.5 h11 v17 l-5.5 -4.2 l-5.5 4.2 Z" ${S}/>`,
  save: `<path d="M12 3.5 V14.5 M7.5 10 L12 14.5 L16.5 10 M4.5 17 V20 H19.5 V17" ${S}/>`,
  eraser: `<path d="M4.2 15.2 L12.8 6.6 a2 2 0 0 1 2.8 0 l3.4 3.4 a2 2 0 0 1 0 2.8 L12 19.8 H8.8 L4.2 15.2 Z M9 10.4 L15.8 17.2 M12 19.8 H20" ${S}/>`,
  newest: `<circle cx="12" cy="12" r="8.6" ${S}/><path d="M12 7.2 V12 L15.2 14" ${S}/><circle cx="19" cy="5" r="2.6" ${F}/>`,
  oldest: `<path d="M7 3.5 h10 M7 20.5 h10 M8 3.5 C8 9 16 9 16 12 C16 15 8 15 8 20.5 M16 3.5 C16 9 8 9 8 12" ${S}/>`,
  eye: `<path d="M2.5 12 C5 7 8.3 5.2 12 5.2 C15.7 5.2 19 7 21.5 12 C19 17 15.7 18.8 12 18.8 C8.3 18.8 5 17 2.5 12 Z" ${S}/><circle cx="12" cy="12" r="3.2" ${S}/>`,
  eye_off: `<path d="M2.5 12 C5 7 8.3 5.2 12 5.2 C15.7 5.2 19 7 21.5 12 C19 17 15.7 18.8 12 18.8 C8.3 18.8 5 17 2.5 12 Z" ${S} opacity=".55"/><path d="M4 4 L20 20" ${S}/>`,
  history: `<path d="M3.8 12 A8.2 8.2 0 1 0 6.2 6.2 M3.5 3.5 V7.5 H7.5" ${S}/><path d="M12 7.8 V12 L14.8 13.8" ${S}/>`,
  hourglass: `<path d="M6.5 3.5 h11 M6.5 20.5 h11 M7.5 3.5 C7.5 9 12 9.5 12 12 C12 14.5 7.5 15 7.5 20.5 M16.5 3.5 C16.5 9 12 9.5 12 12 C12 14.5 16.5 15 16.5 20.5" ${S}/><path d="M9.5 19 h5 l-2.5 -3 Z" ${F}/>`,
  shuffle: `<path d="M3 7 h3.5 c4 0 6 10 10 10 H21 M3 17 h3.5 c1.6 0 2.8 -1.6 3.8 -3.4 M13.7 10.4 C14.7 8.6 15.9 7 17.5 7 H21 M18.5 4.5 L21 7 L18.5 9.5 M18.5 14.5 L21 17 L18.5 19.5" ${S}/>`,
  sparkle: `<path d="M11 3 Q11.8 10.2 18.5 11 Q11.8 11.8 11 19 Q10.2 11.8 3.5 11 Q10.2 10.2 11 3 Z" ${S}/><path d="M18.5 15.5 Q18.8 18.2 21 18.5 Q18.8 18.8 18.5 21 Q18.2 18.8 16 18.5 Q18.2 18.2 18.5 15.5 Z" ${F}/>`,
  pages: `<rect x="3" y="4.5" width="8" height="15" rx="1.8" ${S}/><rect x="13" y="4.5" width="8" height="15" rx="1.8" ${S}/>`,
  scroll: `<rect x="6" y="2.8" width="12" height="18.4" rx="2.2" ${S}/><path d="M6 9.5 h12 M6 15 h12" ${S}/>`,
  rotate: `<rect x="3" y="9" width="12" height="11" rx="2" ${S}/><path d="M13 3.5 A7.5 7.5 0 0 1 20.5 11 M18 8.8 L20.5 11.3 L22.8 8.6" ${S}/>`,
  swipe: `<path d="M4 12 h12 M12 7.5 L16.5 12 L12 16.5" ${S}/><path d="M19.5 5 V19" ${S} opacity=".6"/>`,
  direction: `<path d="M3.5 8 H18 M14.5 4.5 L18 8 L14.5 11.5 M20.5 16 H6 M9.5 12.5 L6 16 L9.5 19.5" ${S}/>`,
  tap: `<circle cx="12" cy="9" r="5.2" ${S} opacity=".55"/><circle cx="12" cy="9" r="2" ${F}/><path d="M12 14.5 V21 M9.2 18.4 L12 21 L14.8 18.4" ${S}/>`,
  zoom: `<circle cx="10.5" cy="10.5" r="6.5" ${S}/><path d="M15.5 15.5 L20.5 20.5 M8 10.5 h5 M10.5 8 v5" ${S}/>`,
  touchpad: `<rect x="3" y="5" width="18" height="14" rx="4" ${S}/><circle cx="9" cy="12" r="1.7" ${F}/><circle cx="15" cy="12" r="1.7" ${F}/>`,
  // the Library's "All": a bento of tiles (one tall, two small) with a sparkle in the tall one
  all: `<path d="M5.4 3 h4.2 a2.6 2.6 0 0 1 2.6 2.6 v12.8 a2.6 2.6 0 0 1 -2.6 2.6 h-4.2 a2.6 2.6 0 0 1 -2.6 -2.6 v-12.8 a2.6 2.6 0 0 1 2.6 -2.6 Z M7.5 6.8 Q8 11.3 10.6 12 Q8 12.7 7.5 17.2 Q7 12.7 4.4 12 Q7 11.3 7.5 6.8 Z" ${F} fill-rule="evenodd"/><rect x="15" y="3.4" width="6" height="7" rx="2.1" ${S}/><rect x="15" y="13.6" width="6" height="7" rx="2.1" ${S}/>`,
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
