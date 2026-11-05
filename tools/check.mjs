// npm test: checks that the page still builds and hangs together, without starting the app.
//   1. sync-ui runs (each of its patches must still match exactly once, or it stops)
//   2. every script and stylesheet the built page loads is in src/
//   3. every script parses: the copied ui/*.js and the page's own inline scripts
//   4. the desktop hooks are in the built page
// Exit code 1 on the first kind of failure, with every problem listed.
import { execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const src = path.join(root, 'src');
const problems = [];
const step = (name, fn) => { try { fn(); console.log('ok   ' + name); } catch (e) { problems.push(`${name}: ${e.message}`); console.log('FAIL ' + name); } };
const check = file => execFileSync(process.execPath, ['--check', file], { stdio: 'pipe' });

step('sync-ui applies every patch', () => {
  execFileSync(process.execPath, [path.join(root, 'sync-ui.mjs')], { cwd: root, stdio: 'pipe' });
});

if (problems.length) {
  console.error('\n' + problems.join('\n') + (fs.existsSync(path.join(root, 'node_modules')) ? '' : '\n(node_modules is missing: run npm ci first)'));
  process.exit(1);
}
const page = fs.readFileSync(path.join(src, 'index.html'), 'utf8');

step('every file the page loads exists', () => {
  const refs = [...page.matchAll(/<(?:script|link)[^>]+(?:src|href)="\/([^"]+)"/g)].map(m => m[1]);
  const missing = refs.filter(r => !fs.existsSync(path.join(src, r)));
  if (missing.length) throw new Error('missing ' + missing.join(', '));
  if (refs.length < 10) throw new Error(`only ${refs.length} files referenced`);
});

step('every ui script is copied and loaded', () => {
  const ui = fs.readdirSync(path.join(root, 'ui')).filter(f => f.endsWith('.js'));
  const notLoaded = ui.filter(f => !page.includes(`src="/${f}"`));
  if (notLoaded.length) throw new Error('not loaded by the page: ' + notLoaded.join(', '));
});

step('every script parses', () => {
  const bad = [];
  for (const f of fs.readdirSync(src).filter(f => f.endsWith('.js'))) {
    try { check(path.join(src, f)); } catch (e) { bad.push(`${f}: ${String(e.stderr || e.message).split('\n').find(l => /Error/.test(l)) || 'syntax error'}`); }
  }
  // the page's own inline scripts (classic ones as .cjs, modules as .mjs)
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'lv-check-'));
  let k = 0;
  for (const m of page.matchAll(/<script(\s[^>]*)?>([\s\S]*?)<\/script>/g)) {
    const attrs = m[1] || '', code = m[2];
    if (/\ssrc=/.test(attrs) || !code.trim() || /type="(?!module)[^"]+"/.test(attrs)) continue;
    const file = path.join(tmp, `inline-${++k}.${/type="module"/.test(attrs) ? 'mjs' : 'cjs'}`);
    fs.writeFileSync(file, code);
    try { check(file); } catch (e) { bad.push(`inline script ${k}: ${String(e.stderr || e.message).split('\n').find(l => /Error/.test(l)) || 'syntax error'}`); }
  }
  fs.rmSync(tmp, { recursive: true, force: true });
  if (!k) throw new Error('no inline scripts found in the page');
  if (bad.length) throw new Error(bad.join('; '));
});

step('the desktop hooks are in the page', () => {
  const want = ['const inTauri', 'tauriInvoke', "tauriInvoke('api'", 'vault.localhost', '/vendor/thumbhash.js', '.c3.mp4', '/extras.js', '/desktop.css'];
  const missing = want.filter(w => !page.includes(w));
  if (missing.length) throw new Error('missing ' + missing.join(', '));
});

if (problems.length) {
  console.error('\n' + problems.join('\n'));
  process.exit(1);
}
console.log('\nAll checks passed.');
