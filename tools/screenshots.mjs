// The README's screenshots (docs/screenshots/*.webp), taken from the real page and the real Rust
// backend over a fresh demo vault, so they can be retaken whenever the app changes:
//
//   node tools/screenshots.mjs <folder with the 5 demo pictures>
//
// What it does:
//   1. builds the page (sync-ui) and starts the backend as a small HTTP server over a new vault in a
//      temporary folder (main.rs tests::screenshot_server, `cargo test ... --ignored`),
//   2. opens the page in Chromium (Playwright; the same engine as WebView2) at 1400 x 900, with a
//      stand-in for Tauri's bridge that sends the page's calls to that server,
//   3. adds the demo pictures through the Add screen like you would (category, tags, NSFW, rating),
//      looks at a few, then visits each screen and saves it as lossless WebP.
//
// The demo pictures aren't in the repository (they're used with permission for the screenshots only).
// Their order and details are in DEMO below; any 5 pictures work.
// Needs: Node, Rust, Playwright with Chromium, Python with Pillow (for the WebP files).

import { execFileSync, spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const { chromium } = require('playwright');
const root = fileURLToPath(new URL('..', import.meta.url));
const args = process.argv.slice(2);
const pics = args.find(a => !a.startsWith('--'));
if (!pics) { console.error('usage: node tools/screenshots.mjs <folder with the demo pictures>'); process.exit(2); }
const files = fs.readdirSync(pics).filter(f => /\.(webp|png|jpe?g)$/i.test(f)).sort();
if (files.length < 5) { console.error(`need 5 pictures in ${pics}, found ${files.length}`); process.exit(2); }

// the demo vault: added in this order (the last one is the one the Add screenshot shows)
const DEMO = [
  { name: 'dragon-girl.webp', category: 'Spicy', nsfw: 8, q: 3, tags: 'white_hair horns red_eyes underboob crop_top choker' },
  { name: 'black-bikini.webp', category: 'Beach', nsfw: 6, q: 3, tags: 'black_hair bikini hair_ribbon armpits long_hair' },
  { name: 'beach-day.webp', category: 'Beach', nsfw: 5, q: 2, tags: 'black_hair bikini hair_flower long_hair' },
  { name: 'maid-bikini.webp', category: 'Cute', nsfw: 6, q: 3, tags: 'silver_hair bikini maid_headdress braids blue_eyes' },
  { name: 'bear-hoodie.webp', category: 'Cute', nsfw: 5, q: 3, tags: 'pink_hair hoodie wings cleavage fang' }
];
// pictures by file order: 1 bear, 2 dragon, 3 black bikini, 4 beach, 5 maid
const SOURCE = { 'bear-hoodie.webp': files[0], 'dragon-girl.webp': files[1], 'black-bikini.webp': files[2], 'beach-day.webp': files[3], 'maid-bikini.webp': files[4] };

const out = path.join(root, 'docs', 'screenshots');
const work = fs.mkdtempSync(path.join(os.tmpdir(), 'lv-shots-'));
const sleep = ms => new Promise(r => setTimeout(r, ms));
const PORT = 8790 + Math.floor(Math.random() * 100), WEB = PORT + 200;

// 1. the page and the backend
execFileSync(process.execPath, [path.join(root, 'sync-ui.mjs')], { cwd: root, stdio: 'inherit' });
const backend = spawn('cargo', ['test', '--quiet', 'screenshot_server', '--', '--ignored', '--nocapture'], {
  cwd: path.join(root, 'src-tauri'), env: { ...process.env, DEMO_DATA: path.join(work, 'vault'), DEMO_PORT: String(PORT), LOCALAPPDATA: path.join(work, 'local') },
  stdio: ['ignore', 'pipe', 'inherit']
});
await new Promise((ok, fail) => {
  backend.stdout.on('data', d => { if (String(d).includes('screenshot server on')) ok(); });
  backend.on('exit', c => fail(new Error('the backend stopped: ' + c)));
});
const types = { '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.html': 'text/html', '.png': 'image/png', '.webp': 'image/webp', '.woff2': 'font/woff2', '.ogg': 'audio/ogg' };
const site = http.createServer((q, r) => {
  let p = path.join(root, 'src', decodeURIComponent(q.url.split('?')[0]));
  if (p.endsWith(path.sep)) p += 'index.html';
  fs.readFile(p, (e, b) => { if (e) { r.writeHead(404); return r.end(); } r.writeHead(200, { 'Content-Type': types[path.extname(p)] || 'application/octet-stream' }); r.end(b); });
}).listen(WEB);

// 2. the browser, with Tauri's bridge pointed at the backend
const browser = await chromium.launch({ args: ['--autoplay-policy=no-user-gesture-required'] });
const ctx = await browser.newContext({ viewport: { width: 1400, height: 900 }, deviceScaleFactor: 1 });
await ctx.route('http://vault.localhost/**', async route => {
  const u = new URL(route.request().url());
  const headers = {};
  const range = route.request().headers().range;
  if (range) headers.range = range;
  const res = await route.fetch({ url: `http://127.0.0.1:${PORT}/vault${u.pathname}${u.search}`, headers });
  await route.fulfill({ response: res });
});
await ctx.addInitScript(({ port, fake }) => {
  const call = async (pathname, body, headers = {}) => {
    const r = await fetch(`http://127.0.0.1:${port}${pathname}`, { method: 'POST', body, headers });
    const v = await r.json();
    if ('err' in v) throw v.err;
    return v.ok;
  };
  window.__panic = [];
  const quiet = new Set(['window_ready', 'window_action', 'set_panic_key', 'set_panic_mode', 'restart_app', 'unlock']);
  window.__TAURI__ = {
    core: {
      invoke: async (cmd, a, opts) => {
        if (cmd === 'api') {
          if (a.name === 'fastLane') return null;
          if (a.name === 'lockState') return window.__lock || { untraceable: false, locked: false };
          if (fake[a.name]) return fake[a.name];
          return call('/api', JSON.stringify({ name: a.name, args: a.args }), { 'content-type': 'application/json' });
        }
        if (cmd === 'upload') return call('/upload', a, (opts && opts.headers) || {});
        if (cmd === 'unlock') return true;
        if (quiet.has(cmd)) return cmd === 'window_action' ? false : null;
        return null;
      }
    },
    event: { listen: async (name, f) => { if (name === 'panic') window.__panic.push(f); return () => {}; } }
  };
}, {
  port: PORT,
  // what a Windows PC shows on the welcome screen (the server's own paths would be this machine's)
  fake: {
    storageOptions: {
      options: [
        { id: 'gdrive', name: 'Google Drive', found: false, download: 'x' }, { id: 'onedrive', name: 'OneDrive', found: false, download: 'x' },
        { id: 'dropbox', name: 'Dropbox', found: false, download: 'x' }, { id: 'icloud', name: 'iCloud Drive', found: false, download: 'x' },
        { id: 'local', name: 'This PC', found: true, folder: 'C:\\Users\\you\\Documents\\Image Vault data', current: true, items: 5 },
        { id: 'untraceable', name: 'Untraceable', found: true, folder: 'C:\\Users\\you\\Documents\\data.temp\\vault' }],
      current: 'C:\\Users\\you\\Documents\\Image Vault data', currentItems: 5
    }
  }
});
const page = await ctx.newPage();
page.on('pageerror', e => console.warn('page error:', e.message));
const tidy = () => page.evaluate(() => {
  document.querySelectorAll('.badge-toast, #splash').forEach(e => e.remove());
  const t = document.getElementById('toast');
  if (t) t.className = 'hidden';
});
const tab = async name => { await page.click(`nav [data-tab="${name}"]`); await sleep(900); };
const shot = async (name, fullClip) => {
  await tidy();
  await sleep(250);
  const png = path.join(work, name + '.png');
  await page.screenshot({ path: png, clip: fullClip });
  execFileSync('python3', ['-c', `from PIL import Image; Image.open(r'''${png}''').convert('RGB').save(r'''${path.join(out, name + '.webp')}''', lossless=True, method=6)`]);
  console.log('saved', name);
};

console.log('opening the page');
await page.goto(`http://localhost:${WEB}/index.html`);
await page.waitForFunction(() => typeof library !== 'undefined' && library !== null, null, { timeout: 30000 });
await sleep(1500);

// 3. adding the pictures, one by one
console.log('adding the demo pictures');
for (const [k, d] of DEMO.entries()) {
  console.log('  ' + d.name);
  await tab('add');
  const copy = path.join(work, d.name);
  fs.copyFileSync(path.join(pics, SOURCE[d.name]), copy);
  await page.setInputFiles('#fileInput', copy);
  await page.waitForSelector('#tagger:not(.hidden)');
  await sleep(600);
  await page.fill('#category', d.category);
  await page.dispatchEvent('#category', 'change');
  const tagInput = page.locator('#tagForm .tagbox input').first();
  for (const t of d.tags.split(' ')) { await tagInput.type(t + ' ', { delay: 15 }); }
  await page.fill('#nsfw', String(d.nsfw));
  await page.click(`#quality [data-q="${d.q}"]`);
  await page.click('body', { position: { x: 5, y: 890 } }); // (closes the tag suggestions)
  await sleep(500);
  if (k === DEMO.length - 1) await shot('add');
  await page.click('#saveBtn');
  await page.waitForFunction(n => library && library.length >= n, k + 1, { timeout: 60000 });
  await sleep(800);
}

// looking at a few, so Stats and You have something to show
await tab('library');
for (let k = 0; k < 4; k++) {
  const cards = page.locator('#grid .card');
  await cards.nth(k).click();
  await sleep(2600);
  if (k < 2) await page.keyboard.press('a').catch(() => {});
  await page.keyboard.press('Escape');
  await sleep(600);
}

// 4. the screens
await tab('library');
await page.selectOption('#fLayout', 'rows').catch(() => {});
await page.dispatchEvent('#fLayout', 'change').catch(() => {});
await sleep(2000);
await shot('library');

if (await page.evaluate(() => !!window.vaultCoach)) {
  await page.evaluate(() => window.vaultCoach.open());
  await sleep(1200);
  await shot('coach');
  await page.click('.coach-x');
  await sleep(600);
}

await tab('stats');
await sleep(800);
await shot('stats');

await page.evaluate(() => window.vaultProfile && window.vaultProfile.open());
await sleep(1500);
await shot('you');

await tab('settings');
await shot('settings');

await tab('slideshow');
await page.click('#showStart');
await sleep(4500);
await page.mouse.move(700, 880);
await sleep(600);
await shot('slideshow');
await page.keyboard.press('Escape');
await sleep(800);

// the emergency key's screensaver
await page.evaluate(() => window.__panic.forEach(f => f({ payload: { on: true, mode: 'screensaver' } })));
await sleep(900);
await shot('emergency');
await page.evaluate(() => window.__panic.forEach(f => f({ payload: { on: false, mode: 'cover' } })));
await sleep(500);

// the welcome screen and Untraceable (Settings → Storage → Change where it lives…)
await tab('settings');
await page.evaluate(() => { const b = [...document.querySelectorAll('button')].find(b => b.textContent === 'Change where it lives…'); b.click(); });
await sleep(1200);
await page.evaluate(() => { const w = document.querySelector('#welcome'); w.querySelector('.wl-title').textContent = 'Welcome to Lewd Vault'; w.querySelector('.wl-cancel').classList.add('hidden'); w.scrollTop = 120; });
await sleep(400);
await shot('welcome');
await page.click('.wl-untraceable');
await sleep(500);
await page.fill('.wl-pw input >> nth=0', 'correct horse');
await page.fill('.wl-pw input >> nth=1', 'correct horse');
await page.evaluate(() => { const w = document.querySelector('#welcome'), h = document.querySelector('.wl-confirm-head'); w.scrollTop = h.getBoundingClientRect().top + w.scrollTop - 30; });
await sleep(400);
await shot('untraceable');

// Untraceable's password screen: a fresh page, locked
const lockPage = await ctx.newPage();
await lockPage.addInitScript(() => { window.__lock = { untraceable: true, locked: true }; });
await lockPage.goto(`http://localhost:${WEB}/index.html`);
await sleep(2500);
await lockPage.evaluate(() => document.querySelectorAll('#toast, #splash').forEach(e => e.remove()));
{
  const png = path.join(work, 'password.png');
  await lockPage.screenshot({ path: png });
  execFileSync('python3', ['-c', `from PIL import Image; Image.open(r'''${png}''').convert('RGB').save(r'''${path.join(out, 'password.webp')}''', lossless=True, method=6)`]);
  console.log('saved password');
}

await browser.close();
site.close();
backend.kill();
fs.rmSync(work, { recursive: true, force: true });
console.log('done: docs/screenshots');
