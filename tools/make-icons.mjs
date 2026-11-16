// Makes the phone app's icon presets (art/icons/<id>.png, 1024 px, opaque squares: the phone rounds
// the corners itself). Settings → Phone → App icon picks one:
//   the heart in other colours: violet, mint, midnight, mono
//   disguises, each with a harmless name on Android: calculator, notes, weather, clock, files, compass
// The disguises are drawn here from scratch (plain shapes), not copied from any real app's icon.
// "default" is the normal app icon (art/app-icon.png) and isn't made here.
// The CI workflows turn these into an iOS alternate app icon each and an Android launcher alias each.
//   node tools/make-icons.mjs
import sharp from 'sharp';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const out = fileURLToPath(new URL('../art/icons/', import.meta.url));
fs.mkdirSync(out, { recursive: true });

const HEART = 'M50 90C22 68 3 50 3 28 3 13 14 3 28 3c9 0 17 5 22 13C55 8 63 3 72 3c14 0 25 10 25 25 0 22-19 40-47 62z';
// the app's heart (as tools/make-icon.mjs draws it) in other colours, on a ground
const heart = ({ bg1, bg2, top, mid, low, glass1, glass2, edge, extra = '' }) => `
<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${bg1}"/><stop offset="1" stop-color="${bg2}"/></linearGradient>
    <clipPath id="c"><path d="${HEART}"/></clipPath>
    <linearGradient id="fill" gradientUnits="userSpaceOnUse" x1="0" y1="30" x2="0" y2="94"><stop offset="0" stop-color="${top}"/><stop offset=".45" stop-color="${mid}"/><stop offset="1" stop-color="${low}"/></linearGradient>
    <linearGradient id="glass" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="${glass1}"/><stop offset="1" stop-color="${glass2}"/></linearGradient>
    <radialGradient id="shine" cx=".3" cy=".25" r=".32"><stop offset="0" stop-color="#fff" stop-opacity=".7"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
  </defs>
  <rect width="1024" height="1024" fill="url(#bg)"/>
  ${extra}
  <g transform="translate(182 196) scale(6.6)">
    <path d="${HEART}" fill="url(#glass)" stroke="${edge}" stroke-width="6" stroke-linejoin="round"/>
    <g clip-path="url(#c)">
      <path d="M-10 33Q2.5 26 15 33T40 33T65 33T90 33T115 33V120H-10Z" fill="${mid}" opacity=".55"/>
      <path d="M-20 35Q-7.5 29 5 35T30 35T55 35T80 35T105 35V120H-20Z" fill="url(#fill)"/>
      <ellipse cx="30" cy="22" rx="16" ry="11" fill="url(#shine)"/>
    </g>
  </g>
</svg>`;
const stars = [[160, 170, 7], [820, 140, 5], [880, 330, 4], [120, 420, 4], [760, 860, 6], [230, 880, 4], [600, 110, 3], [930, 620, 3]]
  .map(([x, y, r]) => `<circle cx="${x}" cy="${y}" r="${r}" fill="#fff" opacity=".85"/>`).join('');

const ICONS = {
  violet: heart({ bg1: '#3b2160', bg2: '#1c0f33', top: '#d5b3ff', mid: '#a06cf0', low: '#5b2aa0', glass1: '#3a2560', glass2: '#22143c', edge: '#120822' }),
  mint: heart({ bg1: '#0f4a46', bg2: '#062724', top: '#b9f5e2', mid: '#4fd3b0', low: '#1b7e68', glass1: '#16514a', glass2: '#0b302c', edge: '#03201d' }),
  midnight: heart({ bg1: '#141a3c', bg2: '#070a1c', top: '#ffb3c6', mid: '#ff6f96', low: '#a3325a', glass1: '#2a2346', glass2: '#171230', edge: '#05030f', extra: stars }),
  mono: heart({ bg1: '#1d1d1f', bg2: '#050505', top: '#ffffff', mid: '#d4d4d8', low: '#8a8a90', glass1: '#2a2a2e', glass2: '#151517', edge: '#000000' }),
  calculator: `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024">
    <rect width="1024" height="1024" fill="#26262a"/>
    ${[0, 1, 2].map(r => [0, 1, 2].map(c => `<rect x="${170 + c * 180}" y="${330 + r * 180}" width="150" height="150" rx="75" fill="${c === 2 ? '#f39a2b' : '#5d5d63'}"/>`).join('')).join('')}
    <rect x="170" y="150" width="690" height="140" rx="36" fill="#3a3a40"/>
    <rect x="680" y="200" width="140" height="22" rx="11" fill="#e8e8ec"/><rect x="680" y="240" width="140" height="22" rx="11" fill="#e8e8ec"/>
    <rect x="512" y="690" width="22" height="80" fill="#fff" opacity="0"/>
    <path d="M740 380v54M713 407h54" stroke="#fff" stroke-width="18" stroke-linecap="round"/>
    <path d="M713 587h54" stroke="#fff" stroke-width="18" stroke-linecap="round"/>
    <path d="M713 754h54M713 780h54" stroke="#fff" stroke-width="14" stroke-linecap="round"/>
  </svg>`,
  notes: `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024">
    <rect width="1024" height="1024" fill="#fffaf0"/>
    <rect width="1024" height="230" fill="#ffd34d"/>
    ${[0, 1, 2, 3, 4].map(k => `<rect x="150" y="${340 + k * 120}" width="${k === 4 ? 420 : 724}" height="16" rx="8" fill="#d9d2c3"/>`).join('')}
    <path d="M600 760l230-230 60 60-230 230-90 30z" fill="#f2a33a"/><path d="M600 760l-30 90 90-30z" fill="#4a3a2a"/>
  </svg>`,
  weather: `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024">
    <defs><linearGradient id="s" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#3d8bfd"/><stop offset="1" stop-color="#7cc0ff"/></linearGradient></defs>
    <rect width="1024" height="1024" fill="url(#s)"/>
    <circle cx="420" cy="400" r="170" fill="#ffd54a"/>
    <path d="M330 700a130 130 0 0 1 20-258 180 180 0 0 1 340 30 120 120 0 0 1 30 228z" fill="#fff"/>
  </svg>`,
  clock: `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024">
    <rect width="1024" height="1024" fill="#1b1b1e"/>
    <circle cx="512" cy="512" r="370" fill="#fafafa"/>
    ${Array.from({ length: 12 }, (_, k) => { const a = k * Math.PI / 6; return `<rect x="504" y="170" width="16" height="${k % 3 ? 34 : 60}" rx="8" fill="#1b1b1e" transform="rotate(${k * 30} 512 512)"/>`; }).join('')}
    <path d="M512 512V300" stroke="#1b1b1e" stroke-width="26" stroke-linecap="round"/><path d="M512 512l150 90" stroke="#1b1b1e" stroke-width="26" stroke-linecap="round"/>
    <path d="M512 560V240" stroke="#ff8a1f" stroke-width="10" stroke-linecap="round" transform="rotate(200 512 512)"/>
    <circle cx="512" cy="512" r="24" fill="#ff8a1f"/>
  </svg>`,
  files: `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024">
    <rect width="1024" height="1024" fill="#f4f6fb"/>
    <path d="M190 330a50 50 0 0 1 50-50h170l60 70h314a50 50 0 0 1 50 50v354a50 50 0 0 1-50 50H240a50 50 0 0 1-50-50z" fill="#2f7cf6"/>
    <path d="M190 430h644v324a50 50 0 0 1-50 50H240a50 50 0 0 1-50-50z" fill="#5a9bff"/>
  </svg>`,
  compass: `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024">
    <rect width="1024" height="1024" fill="#16181d"/>
    <circle cx="512" cy="512" r="360" fill="none" stroke="#e9e9ee" stroke-width="22"/>
    ${Array.from({ length: 36 }, (_, k) => `<rect x="508" y="170" width="8" height="${k % 9 ? 26 : 48}" fill="#e9e9ee" transform="rotate(${k * 10} 512 512)"/>`).join('')}
    <path d="M512 250l70 262H442z" fill="#ff4b3e"/><path d="M512 774l-70-262h140z" fill="#e9e9ee"/>
    <circle cx="512" cy="512" r="26" fill="#16181d" stroke="#e9e9ee" stroke-width="10"/>
  </svg>`
};

for (const [id, svg] of Object.entries(ICONS)) {
  await sharp(Buffer.from(svg)).png().toFile(out + id + '.png');
  console.log('made', id);
}
// the normal icon too, on its own dark ground (the phone needs an opaque square)
await sharp({ create: { width: 1024, height: 1024, channels: 4, background: '#2a1d24' } })
  .composite([{ input: await sharp(fileURLToPath(new URL('../art/app-icon.png', import.meta.url))).resize(820, 820).toBuffer(), gravity: 'center' }])
  .png().toFile(out + 'default.png');
console.log('made default');
// small copies for the page's picker (sync-ui copies art/icons/small into the app)
fs.mkdirSync(out + 'small', { recursive: true });
for (const f of fs.readdirSync(out).filter(f => f.endsWith('.png'))) await sharp(out + f).resize(144, 144).png().toFile(out + 'small/' + f);
console.log('made the small copies');
