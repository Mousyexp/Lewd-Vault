// Fits the generated iOS project (src-tauri/gen/apple, made by `tauri ios init`) with what the phone
// app needs beyond Tauri's template. Run by .github/workflows/ios.yml after `tauri ios init` and
// `tauri icon`, before the build:
//   1. the app icon presets (art/icons, Settings → Phone → App icon) as alternate app icons: one
//      AppIcon-<id> set each in the asset catalog, all of them compiled in;
//   2. the share extension (mobile/ios/ShareExtension): "Lewd Vault" in the share sheet, handing
//      what was shared to the app through the app group's share-inbox (src-tauri/src/phone.rs).
//   3. Chat's Nearby (mobile/ios/Nearby: LvNearby.swift, Bluetooth LE, and LvNearbyLoad.m, which starts
//      it): compiled into the app; it finds the web view itself and adds the "lvBle" message handler
//      (and "lvAuth", Google Drive's sign-in sheet, LvAuth.swift).
// Then the Xcode project is generated again from the edited project.yml.
//   node tools/ios-patch.mjs
import fs from 'node:fs';
import path from 'node:path';
import { execSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const apple = path.join(root, 'src-tauri/gen/apple');
const must = (cond, what) => { if (!cond) { console.error('ios-patch: ' + what); process.exit(1); } };
const conf = JSON.parse(fs.readFileSync(path.join(root, 'src-tauri/tauri.conf.json'), 'utf8'));
const version = conf.version;
const minIos = (conf.bundle && conf.bundle.iOS && conf.bundle.iOS.minimumSystemVersion) || '16.0';
const PRESETS = ['violet', 'mint', 'midnight', 'mono', 'calculator', 'notes', 'weather', 'clock', 'files', 'compass'];

// ---------- 1. alternate icons ----------
const catalogs = [];
const walk = d => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) { if (e.name.endsWith('.xcassets')) catalogs.push(p); else if (!/^(build|Pods|\.)/.test(e.name)) walk(p); } } };
walk(apple);
must(catalogs.length, 'no asset catalog');
const catalog = catalogs.find(c => fs.existsSync(path.join(c, 'AppIcon.appiconset'))) || catalogs[0];
for (const id of PRESETS) {
  const set = path.join(catalog, `AppIcon-${id}.appiconset`);
  fs.mkdirSync(set, { recursive: true });
  fs.copyFileSync(path.join(root, 'art/icons', id + '.png'), path.join(set, 'icon.png'));
  fs.writeFileSync(path.join(set, 'Contents.json'), JSON.stringify({ images: [{ filename: 'icon.png', idiom: 'universal', platform: 'ios', size: '1024x1024' }], info: { author: 'xcode', version: 1 } }, null, 2));
}

// ---------- 2. the share extension's files ----------
fs.cpSync(path.join(root, 'mobile/ios/ShareExtension'), path.join(apple, 'ShareExtension'), { recursive: true });

// ---------- project.yml ----------
const ymlPath = path.join(apple, 'project.yml');
must(fs.existsSync(ymlPath), 'no project.yml');
let lines = fs.readFileSync(ymlPath, 'utf8').split('\n');
const indent = l => l.match(/^ */)[0].length;
const at = lines.findIndex(l => /^targets:\s*$/.test(l));
must(at >= 0, 'no targets: in project.yml');
let end = lines.findIndex((l, k) => k > at && l.trim() && indent(l) === 0);
if (end < 0) end = lines.length;
// the targets, by their two-space keys
const targets = [];
for (let k = at + 1; k < end; k++) if (/^ {2}[^ #][^:]*:\s*$/.test(lines[k])) targets.push(k);
const blockEnd = k => { const n = targets.find(t => t > k); return n === undefined ? end : n; };
const appAt = targets.find(t => lines.slice(t, blockEnd(t)).some(l => /^\s+type:\s*application\s*$/.test(l)));
must(appAt !== undefined, 'no application target');
const appName = lines[appAt].trim().replace(/:$/, '');
if (!lines.some(l => /^ {2}ShareExtension:\s*$/.test(l))) {
  // (edits from the bottom up, so the line numbers above stay right)
  const share = `  ShareExtension:
    type: app-extension
    platform: iOS
    deploymentTarget: "${minIos}"
    sources:
      - path: ShareExtension
        excludes:
          - Info.plist
    settings:
      base:
        INFOPLIST_FILE: ShareExtension/Info.plist
        PRODUCT_BUNDLE_IDENTIFIER: ${conf.identifier}.share
        PRODUCT_NAME: ShareExtension
        MARKETING_VERSION: "${version}"
        CURRENT_PROJECT_VERSION: "${version}"
        SWIFT_VERSION: "5.0"
        TARGETED_DEVICE_FAMILY: "1,2"
        SKIP_INSTALL: YES
        GENERATE_INFOPLIST_FILE: NO`.split('\n');
  lines.splice(end, 0, ...share);
  const aEnd = blockEnd(appAt);
  const block = lines.slice(appAt, aEnd);
  // the app: depends on (embeds) the extension
  const dep = block.findIndex(l => /^ {4}dependencies:\s*$/.test(l));
  if (dep >= 0) lines.splice(appAt + dep + 1, 0, '      - target: ShareExtension');
  else lines.splice(aEnd, 0, '    dependencies:', '      - target: ShareExtension');
  // the app: every AppIcon-* set compiled in, as alternate icons
  const keys = ['        ASSETCATALOG_COMPILER_INCLUDE_ALL_APPICON_ASSETS: YES', `        ASSETCATALOG_COMPILER_ALTERNATE_APPICON_NAMES: "${PRESETS.map(p => 'AppIcon-' + p).join(' ')}"`];
  const block2 = lines.slice(appAt, blockEnd(appAt) + 3);
  const set = block2.findIndex(l => /^ {4}settings:\s*$/.test(l));
  if (set < 0) lines.splice(appAt + 1, 0, '    settings:', '      base:', ...keys);
  else {
    const base = block2.findIndex((l, k) => k > set && /^ {6}base:\s*$/.test(l));
    if (base >= 0) lines.splice(appAt + base + 1, 0, ...keys);
    else lines.splice(appAt + set + 1, 0, '      base:', ...keys);
  }
}
// ---------- 3. Nearby: its files in the app's sources ----------
fs.cpSync(path.join(root, 'mobile/ios/Nearby'), path.join(apple, 'Nearby'), { recursive: true });
{
  const tEnd = (() => { let e = lines.findIndex(l => /^targets:\s*$/.test(l)); e = lines.findIndex((l, k) => k > e && l.trim() && indent(l) === 0); return e < 0 ? lines.length : e; })();
  const app2 = lines.findIndex(l => l === `  ${appName}:`);
  must(app2 >= 0, 'the app target went missing');
  let aEnd = lines.findIndex((l, k) => k > app2 && /^ {2}[^ #][^:]*:\s*$/.test(l));
  if (aEnd < 0 || aEnd > tEnd) aEnd = tEnd;
  const block = lines.slice(app2, aEnd);
  if (!block.some(l => /^\s+- path: Nearby\s*$/.test(l))) {
    const src = block.findIndex(l => /^ {4}sources:\s*$/.test(l));
    must(src >= 0, 'the app target has no sources: list');
    lines.splice(app2 + src + 1, 0, '      - path: Nearby');
  }
  const block3 = lines.slice(app2, aEnd + 1);
  if (!block3.some(l => /SWIFT_VERSION/.test(l))) {
    const base = block3.findIndex(l => /^ {6}base:\s*$/.test(l));
    if (base >= 0) lines.splice(app2 + base + 1, 0, '        SWIFT_VERSION: "5.0"');
  }
}
fs.writeFileSync(ymlPath, lines.join('\n'));
console.log(`ios-patch: ${PRESETS.length} alternate icons in ${path.relative(apple, catalog)}, the share extension, Nearby, for target ${appName}`);
execSync('xcodegen generate', { cwd: apple, stdio: 'inherit' });
