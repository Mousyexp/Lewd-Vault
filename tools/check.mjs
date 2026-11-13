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

// The page's own inline <style> (ui/Index.html) is copied into src/index.html too, so it has to be
// checked as well as desktop.css — otherwise a stray unprefixed use there is invisible to this test.
const unpairedBackdrop = (css, where) => {
  const lines = css.split('\n');
  const bad = lines.filter((l, i) => /(^|[^-])backdrop-filter\s*:/.test(l) && !/-webkit-backdrop-filter/.test(l) && !/-webkit-backdrop-filter/.test(lines[i - 1] || ''));
  if (bad.length) throw new Error(`${where}: ${bad.length} without the prefix, e.g. ${bad[0].trim()}`);
};
step('every backdrop-filter has its -webkit- pair (iOS WebKit)', () => {
  const css = fs.readFileSync(path.join(root, 'ui', 'desktop.css'), 'utf8');
  unpairedBackdrop(css, 'desktop.css');
  if (!css.includes('--glass-blur')) throw new Error('the Liquid Glass tokens are missing');
  const html = fs.readFileSync(path.join(root, 'ui', 'Index.html'), 'utf8');
  const style = html.slice(html.indexOf('<style>'), html.indexOf('</style>'));
  unpairedBackdrop(style, 'Index.html inline style');
});

// The golden vault (src-tauri/tests/fixtures/golden-vault) is the Windows-on-disk shape; the page's
// own parseCsv must read it exactly. This pulls the real function out of the built page (so it tests
// the code that ships, not a copy) and checks BOM, CRLF, RFC-4180 quoting, an embedded newline and a
// non-ASCII cell.
step('the page reads the golden vault (BOM, CRLF, quoting)', () => {
  const html = fs.readFileSync(path.join(src, 'index.html'), 'utf8');
  const lines = html.split('\n');
  const at = lines.findIndex(l => /^\s*function parseCsv\(/.test(l));
  if (at < 0) throw new Error('parseCsv is not in the page');
  const end = lines.findIndex((l, i) => i > at && l === '}');
  if (end < 0) throw new Error('parseCsv has no end');
  const parseCsv = new Function(`${lines.slice(at, end + 1).join('\n')}; return parseCsv;`)();

  const read = name => fs.readFileSync(path.join(root, 'src-tauri', 'tests', 'fixtures', 'golden-vault', name), 'utf8');
  const index = read('index.csv');
  if (index.charCodeAt(0) !== 0xFEFF) throw new Error('the fixture lost its BOM');
  const rows = parseCsv(index.replace(/^\uFEFF/, ''));
  const head = rows.shift();
  if (head[0] !== 'id' || head.length !== 21) throw new Error(`header is wrong: ${head.length} columns`);
  if (rows.length !== 3) throw new Error(`expected 3 rows, got ${rows.length}`);
  const col = name => head.indexOf(name);
  const get = (r, name) => r[col(name)];
  if (get(rows[0], 'original_name') !== 'Cover, front "final".webp') throw new Error('a quoted comma / doubled quote did not parse');
  if (get(rows[1], 'collection') !== 'Set, A') throw new Error('a quoted collection did not parse');
  if (get(rows[1], 'thumb').indexOf(':') < 0) throw new Error('the packed-thumbnail value did not parse');
  if (get(rows[2], 'category') !== '日本語') throw new Error('a non-ASCII cell did not parse');
  if (get(rows[2], 'original_name') !== 'line1\nline2.gif') throw new Error('a quoted newline did not parse');
  const views = parseCsv(read('views.csv').replace(/^\uFEFF/, ''));
  if (views.length !== 4) throw new Error(`expected 3 view rows, got ${views.length - 1}`);
});

// The untraceable fixture (golden-untraceable) is encrypted by tools/chacha20.py, an implementation
// written from the format spec rather than shared with the Rust code. Its ChaCha20 core is checked
// against the RFC 8439 keystream vector, and it decrypts the fixture back to the plain golden vault's
// index.csv/views.csv. This runs both checks, so the Rust test's "a vault made elsewhere opens" claim
// is backed by an independent implementation that is itself verified. Skipped (not failed) if python3
// is missing: the Rust test still covers the fixture.
step('the untraceable fixture is independently generated and self-consistent', () => {
  let py;
  try {
    py = execFileSync('python3', ['-c', 'print(1)'], { encoding: 'utf8' }).trim();
  } catch { return; }
  const tool = path.join(root, 'tools', 'chacha20.py');
  execFileSync('python3', [tool, 'self-test'], { encoding: 'utf8' });

  const fixtures = path.join(root, 'src-tauri', 'tests', 'fixtures');
  const script = `
import sys
sys.path.insert(0, ${JSON.stringify(path.join(root, 'tools'))})
from chacha20 import MAGIC, derive, crypt
key = derive("hunter2")
root = ${JSON.stringify(path.join(fixtures, 'golden-untraceable'))}
plain = ${JSON.stringify(path.join(fixtures, 'golden-vault'))}
def open_vault(name):
    b = open(root + "/" + name, "rb").read()
    assert b[:4] == MAGIC, name + " is not an encrypted file"
    return crypt(key, b[4:16], b[16:])
for name in ["index.csv", "views.csv"]:
    assert open_vault(name) == open(plain + "/" + name, "rb").read(), name + " did not decrypt to the plain vault"
print("ok")
`;
  const out = execFileSync('python3', ['-c', script], { encoding: 'utf8' }).trim();
  if (!out.endsWith('ok')) throw new Error(`the untraceable fixture did not check out: ${out}`);
});

step('the iOS port pieces the platform turns on are in place', () => {
  // These are exactly the things that silently break the iOS build or the iOS-only behaviour, and
  // each has bitten once. They are cheap to pin here (this runs on every `npm test`, on any OS).
  const conf = JSON.parse(fs.readFileSync(path.join(root, 'src-tauri', 'tauri.conf.json'), 'utf8'));
  // The bundle key is "iOS" (capital I/O/S, matching "macOS"); "ios" is a deprecated alias. The
  // minimum version encodes the iOS-26 decision (docs/IOS-PORT.md section 11).
  if (!conf.bundle?.iOS) throw new Error('bundle.iOS is missing from tauri.conf.json');
  if (conf.bundle.iOS.minimumSystemVersion !== '26.0') throw new Error(`bundle.iOS.minimumSystemVersion is ${conf.bundle.iOS.minimumSystemVersion}, expected 26.0`);
  // The crate must build as a staticlib for iOS (docs/IOS-PORT.md section 5).
  const cargo = fs.readFileSync(path.join(root, 'src-tauri', 'Cargo.toml'), 'utf8');
  if (!/crate-type\s*=\s*\[[^\]]*staticlib/.test(cargo)) throw new Error('the crate is not a staticlib (needed for iOS)');
  // Info.plist: file sharing + the loopback ATS exception (docs/IOS-PORT.md sections 4, 8).
  const plist = fs.readFileSync(path.join(root, 'src-tauri', 'Info.plist'), 'utf8');
  for (const key of ['UIFileSharingEnabled', 'NSAllowsLocalNetworking', '127.0.0.1']) {
    if (!plist.includes(key)) throw new Error(`Info.plist is missing ${key}`);
  }
  // The iOS layer runs only under html.ios, which the Rust side adds as an initialization script
  // before the page's own scripts; if that ever disappears, ui/ios.js silently does nothing.
  const lib = fs.readFileSync(path.join(root, 'src-tauri', 'src', 'lib.rs'), 'utf8');
  if (!/classList\.add\((['"])ios\1\)/.test(lib)) throw new Error("nothing adds the html.ios class, so ui/ios.js never runs");
  if (!page.includes('src="/ios.js"')) throw new Error('the page does not load /ios.js');
  // The iOS build only runs in .github/workflows/ios.yml. Two of the ten classic CI failure modes have
  // to stay fixed there or the IPA job breaks: the aarch64-apple-ios target (without it the crate
  // cannot find `std`), and a pinned macOS runner (macos-latest drifts to a new Xcode and its SDK).
  const wf = fs.readFileSync(path.join(root, '.github', 'workflows', 'ios.yml'), 'utf8');
  if (!/aarch64-apple-ios/.test(wf)) throw new Error('ios.yml does not install the aarch64-apple-ios Rust target');
  if (/runs-on:\s*macos-latest/.test(wf)) throw new Error('ios.yml uses the floating macos-latest runner; pin it');
});

if (problems.length) {
  console.error('\n' + problems.join('\n'));
  process.exit(1);
}
console.log('\nAll checks passed.');
