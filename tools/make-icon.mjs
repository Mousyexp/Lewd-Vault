// Makes the app icon (art/app-icon.png, 1024 px): the loading screen's heart, filled most of the
// way with its rose liquid (the wave, the shine and the pale outline), on a transparent ground.
// Then `tauri icon` turns it into src-tauri/icons (the .exe icon, window and taskbar icon), and the
// sidebar shows it too (sync-ui copies it as app-icon.png).
//   npm run icon
import sharp from 'sharp';
import { execSync } from 'node:child_process';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const here = new URL('..', import.meta.url);
const out = fileURLToPath(new URL('art/app-icon.png', here));

// the same path as HEART in ui/desktop.js (a 100 x 94 box)
const HEART = 'M50 90C22 68 3 50 3 28 3 13 14 3 28 3c9 0 17 5 22 13C55 8 63 3 72 3c14 0 25 10 25 25 0 22-19 40-47 62z';
const LEVEL = 30; // where the liquid's surface sits (0 = full)
const svg = `<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="-4 -7 108 108">
  <defs>
    <clipPath id="c"><path d="${HEART}"/></clipPath>
    <linearGradient id="fill" gradientUnits="userSpaceOnUse" x1="0" y1="${LEVEL}" x2="0" y2="94">
      <stop offset="0" stop-color="#F29BAE"/><stop offset=".45" stop-color="#E27589"/><stop offset="1" stop-color="#8a3550"/>
    </linearGradient>
    <linearGradient id="glass" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#5a2a3a"/><stop offset="1" stop-color="#3a1824"/></linearGradient>
    <radialGradient id="shine" cx=".3" cy=".25" r=".32"><stop offset="0" stop-color="#fff" stop-opacity=".7"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient>
  </defs>
  <path d="${HEART}" fill="url(#glass)" stroke="#2a0d17" stroke-width="7" stroke-linejoin="round"/>
  <g clip-path="url(#c)">
    <path d="M-10 ${LEVEL + 3}Q2.5 ${LEVEL - 4} 15 ${LEVEL + 3}T40 ${LEVEL + 3}T65 ${LEVEL + 3}T90 ${LEVEL + 3}T115 ${LEVEL + 3}V120H-10Z" fill="#B25B6E" opacity=".8"/>
    <path d="M-20 ${LEVEL + 5}Q-7.5 ${LEVEL - 1} 5 ${LEVEL + 5}T30 ${LEVEL + 5}T55 ${LEVEL + 5}T80 ${LEVEL + 5}T105 ${LEVEL + 5}V120H-20Z" fill="url(#fill)"/>
  </g>
  <path d="${HEART}" fill="url(#shine)"/>
  <path d="${HEART}" fill="none" stroke="#F9CDD4" stroke-width="3" stroke-linejoin="round"/>
</svg>`;
await sharp(Buffer.from(svg)).png().toFile(out);
console.log('make-icon: the heart -> art/app-icon.png');

execSync('npx tauri icon art/app-icon.png', { cwd: fileURLToPath(here), stdio: 'ignore' });
// Android isn't a target: drop its icons. The iOS catalog is kept (the iOS build copies it into
// Assets.xcassets), see docs/IOS-PORT.md.
fs.rmSync(new URL('src-tauri/icons/android', here), { recursive: true, force: true });
// Cargo doesn't watch the icon files: touching these makes the next build embed the new icon
// (build.rs puts it in the .exe; lib.rs's generate_context! uses it for the window).
const now = new Date();
for (const f of ['src-tauri/build.rs', 'src-tauri/src/lib.rs']) fs.utimesSync(new URL(f, here), now, now);
console.log('make-icon: src-tauri/icons updated (run npm run dist to build with it)');
