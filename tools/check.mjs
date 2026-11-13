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
  const want = ['const inTauri', 'tauriInvoke', "tauriInvoke('api'", 'vault.localhost', '/vendor/thumbhash.js', '.c3.mp4', '/extras.js', '/mobile.css'];
  const missing = want.filter(w => !page.includes(w));
  if (missing.length) throw new Error('missing ' + missing.join(', '));
});

// The page's own inline <style> (ui/Index.html) is copied into src/index.html too, so it has to be
// checked as well as the desktop CSS — otherwise a stray unprefixed use there is invisible to this test.
const unpairedBackdrop = (css, where) => {
  const lines = css.split('\n');
  const bad = lines.filter((l, i) => /(^|[^-])backdrop-filter\s*:/.test(l) && !/-webkit-backdrop-filter/.test(l) && !/-webkit-backdrop-filter/.test(lines[i - 1] || ''));
  if (bad.length) throw new Error(`${where}: ${bad.length} without the prefix, e.g. ${bad[0].trim()}`);
};
step('every backdrop-filter has its -webkit- pair (iOS WebKit)', () => {
  // The desktop look and the iOS skin are two files (ui/desktop.css and mobile/ui/ios.css) that
  // sync-ui concatenates into src/mobile.css; check the built file so neither half can drift.
  const css = fs.readFileSync(path.join(src, 'mobile.css'), 'utf8');
  unpairedBackdrop(css, 'mobile.css');
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
  // minimum version encodes the iOS-26 decision (mobile/docs/PORT.md section 11).
  if (!conf.bundle?.iOS) throw new Error('bundle.iOS is missing from tauri.conf.json');
  if (conf.bundle.iOS.minimumSystemVersion !== '26.0') throw new Error(`bundle.iOS.minimumSystemVersion is ${conf.bundle.iOS.minimumSystemVersion}, expected 26.0`);
  // The crate must build as a staticlib for iOS (mobile/docs/PORT.md section 5).
  const cargo = fs.readFileSync(path.join(root, 'src-tauri', 'Cargo.toml'), 'utf8');
  if (!/crate-type\s*=\s*\[[^\]]*staticlib/.test(cargo)) throw new Error('the crate is not a staticlib (needed for iOS)');
  // Info.plist: file sharing + the loopback ATS exception (mobile/docs/PORT.md sections 4, 8).
  const plist = fs.readFileSync(path.join(root, 'src-tauri', 'Info.plist'), 'utf8');
  for (const key of ['UIFileSharingEnabled', 'NSAllowsLocalNetworking', '127.0.0.1']) {
    if (!plist.includes(key)) throw new Error(`Info.plist is missing ${key}`);
  }
  // The iOS layer runs only under html.ios, which the Rust side adds as an initialization script
  // before the page's own scripts; if that ever disappears, mobile/ui/ios.js silently does nothing.
  const lib = fs.readFileSync(path.join(root, 'src-tauri', 'src', 'lib.rs'), 'utf8');
  if (!/classList\.add\((['"])ios\1\)/.test(lib)) throw new Error("nothing adds the html.ios class, so mobile/ui/ios.js never runs");
  if (!page.includes('src="/ios.js"')) throw new Error('the page does not load /ios.js');
  // The iOS skin (mobile/ui/ios.css) is appended after the desktop look into the single /mobile.css
  // the page loads. If sync-ui stops folding it in, iOS would run the desktop look with no iOS styling.
  if (!page.includes('href="/mobile.css"')) throw new Error('the page does not load /mobile.css');
  const builtCss = fs.readFileSync(path.join(src, 'mobile.css'), 'utf8');
  if (!builtCss.includes('.photos-tabbar')) throw new Error('mobile.css has no iOS skin (mobile/ui/ios.css was not folded in)');
  // The tab bar must NOT be a <nav>: on iOS <html> has both `ios` and `desktop` (desktop.js adds
  // `desktop` unconditionally), and the desktop sidebar's 90 `html.desktop nav …` rules then capture
  // it — column layout, font-size 0 — collapsing it to a 6px strip off the bottom of the screen.
  const iosjs = fs.readFileSync(path.join(root, 'mobile', 'ui', 'ios.js'), 'utf8');
  if (/make\(\s*['"]nav['"]\s*,\s*['"]photos-tabbar['"]/.test(iosjs)) throw new Error("mobile/ui/ios.js builds the tab bar as a <nav>, which the html.desktop nav rules clobber on iOS");
  if (!/make\(\s*['"]div['"]\s*,\s*['"]photos-tabbar['"]/.test(iosjs)) throw new Error('mobile/ui/ios.js no longer builds the tab bar as a <div class="photos-tabbar">');
  // ...and because a <div> does not match `html.desktop nav [data-tab=…]`, the icon has to be copied
  // onto each tab button explicitly, or the bar renders with blank icons.
  if (!/setProperty\(\s*['"]--icon['"]/.test(iosjs)) throw new Error('mobile/ui/ios.js no longer copies the sidebar icon onto the tab buttons (blank tab icons)');
  // The iOS app launches through `main.mm`'s `ffi::start_app()`: `mobile_entry_point` turns `run()`
  // into an exported `start_app` symbol (a #[no_mangle] extern "C"). Rename `run` and the build still
  // succeeds, then the app dies on launch with a missing symbol — so pin the attribute here.
  if (!/#\[cfg_attr\(mobile,\s*tauri::mobile_entry_point\)\]/.test(lib)) throw new Error('lib.rs lost the #[cfg_attr(mobile, tauri::mobile_entry_point)] on run(), so iOS has no start_app symbol');
  if (!/\n\s*pub fn run\(\)/.test(lib)) throw new Error('lib.rs has no `pub fn run()` for the mobile entry point to wrap');
  // The iOS build only runs in .github/workflows/ios.yml. Several classic CI failure modes have to
  // stay fixed there or the IPA job breaks. Cheap to pin here (this runs on every `npm test`, any OS):
  const wf = fs.readFileSync(path.join(root, '.github', 'workflows', 'ios.yml'), 'utf8');
  //   - a manual trigger, so the build can be kicked off from the Actions tab without a dummy commit
  if (!/workflow_dispatch:/.test(wf)) throw new Error('ios.yml has no workflow_dispatch trigger');
  //   - the Rust target for the phone, without which the crate cannot find `std`
  if (!/aarch64-apple-ios/.test(wf)) throw new Error('ios.yml does not install the aarch64-apple-ios Rust target');
  //   - a pinned runner: macos-latest drifts to a new default Xcode and SDK
  if (/runs-on:\s*macos-latest/.test(wf)) throw new Error('ios.yml uses the floating macos-latest runner; pin it');
  //   - the Xcode pin, so the iOS 26 SDK does not move under us
  if (!/xcode-select -s \/Applications\/Xcode_/.test(wf)) throw new Error('ios.yml does not pin the Xcode version');
  //   - the headless keychain, or codesign can hang on a password prompt a runner cannot answer
  if (!/set-key-partition-list/.test(wf)) throw new Error('ios.yml does not set the keychain partition list');
  //   - the fourth signing secret: the API key authorizes the export but Xcode still needs a team ID
  if (!/APPLE_DEVELOPMENT_TEAM/.test(wf)) throw new Error('ios.yml does not pass APPLE_DEVELOPMENT_TEAM for signing');
  //   - a job timeout with room for a cold aarch64 + CocoaPods build (the ipa job, not the tests)
  const mins = [...wf.matchAll(/timeout-minutes:\s*(\d+)/g)].map(m => +m[1]);
  if (!mins.some(m => m >= 60)) throw new Error('ios.yml has no generous job timeout (a cold iOS build is slow)');
  // The bundle id is the App ID the Apple portal must know about (#9): a valid reverse-DNS name, and
  // not Tauri's rejected default. It is shared with the bundle config, so a rename cannot drift.
  if (conf.identifier !== 'local.imagevault.desktop') throw new Error(`identifier is ${conf.identifier}, expected local.imagevault.desktop`);
  if (!/^[A-Za-z0-9.-]+$/.test(conf.identifier) || conf.identifier === 'com.tauri.dev') throw new Error('the bundle identifier is not a valid, non-default reverse-DNS name');
});

if (problems.length) {
  console.error('\n' + problems.join('\n'));
  process.exit(1);
}
console.log('\nAll checks passed.');
