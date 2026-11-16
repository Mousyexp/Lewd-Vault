// Fits the generated Android project (src-tauri/gen/android, made by `tauri android init`) with
// what the phone app needs beyond Tauri's template. Run by .github/workflows/android.yml after
// `tauri android init` and `tauri icon`:
//   1. Share to Lewd Vault: the main activity takes pictures and videos sent to it (one or many) and
//      copies them into files/share-inbox, where the app adds them (src-tauri/src/phone.rs, "inbox").
//   2. The app icon presets (art/icons, Settings → Phone → App icon): one launcher alias each, with
//      its own icon and name; the one chosen is switched on when the app goes to the background.
//   3. No screenshots or screen recording while "Hide from screenshots" is on (FLAG_SECURE).
//   4. Plain http to the app's own media server on 127.0.0.1 (cleartext), and the camera for the
//      break-in photo.
//   5. Chat's Nearby (Bluetooth LE, like AirDrop): mobile/android/LvNearby.kt goes in beside the
//      activity, which hands it the web view as window.LvBle; the Bluetooth permissions.
//   node tools/android-patch.mjs
import fs from 'node:fs';
import path from 'node:path';
import { execSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('..', import.meta.url));
const app = path.join(root, 'src-tauri/gen/android/app');
const res = path.join(app, 'src/main/res');
const manifestPath = path.join(app, 'src/main/AndroidManifest.xml');
const ICONS = [['default', null], ['violet', null], ['mint', null], ['midnight', null], ['mono', null],
  ['calculator', 'Calculator'], ['notes', 'Notes'], ['weather', 'Weather'], ['clock', 'Clock'], ['files', 'Files'], ['compass', 'Compass']];
const alias = id => 'Icon' + id[0].toUpperCase() + id.slice(1);
const must = (cond, what) => { if (!cond) { console.error('android-patch: ' + what); process.exit(1); } };

// ---------- the icons: each preset's launcher pictures, as mipmaps ----------
const tmp = path.join(root, 'src-tauri/gen/icon-presets');
for (const [id] of ICONS) {
  const out = path.join(tmp, id);
  fs.rmSync(out, { recursive: true, force: true });
  execSync(`npx tauri icon "${path.join(root, 'art/icons', id + '.png')}" -o "${out}"`, { cwd: root, stdio: 'inherit' });
  let copied = 0;
  const walk = d => { for (const e of fs.readdirSync(d, { withFileTypes: true })) {
    const p = path.join(d, e.name);
    if (e.isDirectory()) walk(p);
    else if (e.name === 'ic_launcher.png' && /^mipmap-/.test(path.basename(d))) {
      fs.mkdirSync(path.join(res, path.basename(d)), { recursive: true });
      fs.copyFileSync(p, path.join(res, path.basename(d), `ic_preset_${id}.png`));
      copied++;
    }
  } };
  walk(out);
  must(copied > 0, `no launcher pictures made for ${id}`);
}
fs.rmSync(tmp, { recursive: true, force: true });

// ---------- the manifest ----------
let m = fs.readFileSync(manifestPath, 'utf8');
const act = m.match(/<activity\b[^>]*android:name="\.MainActivity"[\s\S]*?<\/activity>/);
must(act, 'no .MainActivity in the manifest');
let block = act[0];
// the launcher entry moves to the aliases (exactly one of them is on)
must(/android\.intent\.category\.LAUNCHER/.test(block), 'the main activity has no LAUNCHER category');
block = block.replace(/\s*<category android:name="android\.intent\.category\.LAUNCHER"\s*\/>/, '');
const share = ['SEND', 'SEND_MULTIPLE'].map(a => `
            <intent-filter>
                <action android:name="android.intent.action.${a}" />
                <category android:name="android.intent.category.DEFAULT" />
                <data android:mimeType="image/*" />
                <data android:mimeType="video/*" />
            </intent-filter>`).join('');
block = block.replace(/<\/activity>$/, share + '\n        </activity>');
const aliases = ICONS.map(([id, label]) => `
        <activity-alias
            android:name=".${alias(id)}"
            android:targetActivity=".MainActivity"
            android:enabled="${id === 'default'}"
            android:exported="true"
            android:label="${label || '@string/app_name'}"
            android:icon="@mipmap/ic_preset_${id}"
            android:roundIcon="@mipmap/ic_preset_${id}">
            <intent-filter>
                <action android:name="android.intent.action.MAIN" />
                <category android:name="android.intent.category.LAUNCHER" />
            </intent-filter>
        </activity-alias>`).join('');
m = m.replace(act[0], block + aliases);
if (!/android\.permission\.CAMERA/.test(m)) m = m.replace(/<application\b/, '<uses-permission android:name="android.permission.CAMERA" />\n    <uses-feature android:name="android.hardware.camera.front" android:required="false" />\n    <application');
if (!/BLUETOOTH_ADVERTISE/.test(m)) m = m.replace(/<application\b/, [
  '<uses-permission android:name="android.permission.BLUETOOTH" android:maxSdkVersion="30" />',
  '<uses-permission android:name="android.permission.BLUETOOTH_ADMIN" android:maxSdkVersion="30" />',
  '<uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" android:maxSdkVersion="30" />',
  '<uses-permission android:name="android.permission.BLUETOOTH_SCAN" android:usesPermissionFlags="neverForLocation" />',
  '<uses-permission android:name="android.permission.BLUETOOTH_ADVERTISE" />',
  '<uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />',
  '<uses-feature android:name="android.hardware.bluetooth_le" android:required="false" />',
  '<application'].join('\n    '));
if (!/usesCleartextTraffic/.test(m)) m = m.replace(/<application\b/, '<application android:usesCleartextTraffic="true"');
fs.writeFileSync(manifestPath, m);
// (the template sets the cleartext flag from a placeholder in build.gradle.kts)
const gradle = path.join(app, 'build.gradle.kts');
if (fs.existsSync(gradle)) fs.writeFileSync(gradle, fs.readFileSync(gradle, 'utf8').replace(/manifestPlaceholders\["usesCleartextTraffic"\] = "false"/g, 'manifestPlaceholders["usesCleartextTraffic"] = "true"'));

// ---------- the activity ----------
const find = d => { for (const e of fs.readdirSync(d, { withFileTypes: true })) { const p = path.join(d, e.name); if (e.isDirectory()) { const r = find(p); if (r) return r; } else if (e.name === 'MainActivity.kt') return p; } return null; };
const ktPath = find(path.join(app, 'src/main'));
must(ktPath, 'no MainActivity.kt');
let kt = fs.readFileSync(ktPath, 'utf8');
const body = `
  // ---- Lewd Vault (tools/android-patch.mjs): share to the app, the icon presets, no screenshots ----
  override fun onNewIntent(intent: android.content.Intent) {
    super.onNewIntent(intent)
    setIntent(intent)
    lvTakeShared(intent)
  }
  override fun onResume() {
    super.onResume()
    lvTakeShared(intent)
    lvApplySecure()
  }
  override fun onStop() {
    super.onStop()
    lvNearby?.end()
    lvApplyIcon()
  }
  // Chat's Nearby (LvNearby.kt): the page reaches it as window.LvBle
  private var lvNearby: LvNearby? = null
  override fun onWebViewCreate(webView: android.webkit.WebView) {
    super.onWebViewCreate(webView)
    val n = LvNearby(this, webView)
    lvNearby = n
    webView.addJavascriptInterface(n.bridge, "LvBle")
  }
  private fun lvNative(): org.json.JSONObject =
    try { org.json.JSONObject(java.io.File(filesDir, "phone-native.json").readText()) } catch (e: Exception) { org.json.JSONObject() }
  private fun lvApplySecure() {
    val flag = android.view.WindowManager.LayoutParams.FLAG_SECURE
    if (lvNative().optBoolean("secure", false)) window.addFlags(flag) else window.clearFlags(flag)
  }
  private fun lvApplyIcon() {
    val names = listOf(${ICONS.map(([id]) => `"${id}"`).join(', ')})
    val want = lvNative().optString("icon", "default")
    if (!names.contains(want)) return
    val pm = packageManager
    val order = listOf(want) + names.filter { it != want } // (the new one on first: never no icon at all)
    for (n in order) {
      val c = android.content.ComponentName(this, packageName + ".Icon" + n.substring(0, 1).uppercase() + n.substring(1))
      val state = if (n == want) android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_ENABLED else android.content.pm.PackageManager.COMPONENT_ENABLED_STATE_DISABLED
      try { if (pm.getComponentEnabledSetting(c) != state) pm.setComponentEnabledSetting(c, state, android.content.pm.PackageManager.DONT_KILL_APP) } catch (e: Exception) {}
    }
  }
  @Suppress("DEPRECATION")
  private fun lvTakeShared(i: android.content.Intent?) {
    if (i == null) return
    val uris = ArrayList<android.net.Uri>()
    when (i.action) {
      android.content.Intent.ACTION_SEND -> (i.getParcelableExtra<android.net.Uri>(android.content.Intent.EXTRA_STREAM))?.let { uris.add(it) }
      android.content.Intent.ACTION_SEND_MULTIPLE -> (i.getParcelableArrayListExtra<android.net.Uri>(android.content.Intent.EXTRA_STREAM))?.let { uris.addAll(it) }
      else -> return
    }
    i.action = "local.imagevault.shared" // (taken: not again on the next resume)
    val dir = java.io.File(filesDir, "share-inbox")
    dir.mkdirs()
    val resolver = contentResolver
    Thread {
      for ((k, u) in uris.withIndex()) {
        try {
          var name = "shared"
          resolver.query(u, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c -> if (c.moveToFirst()) name = c.getString(0) ?: name }
          if (!name.contains('.')) {
            val ext = android.webkit.MimeTypeMap.getSingleton().getExtensionFromMimeType(resolver.getType(u)) ?: "jpg"
            name = "$name.$ext"
          }
          name = name.replace(Regex("[^A-Za-z0-9._-]"), "_")
          val part = java.io.File(dir, ".part-" + System.nanoTime())
          resolver.openInputStream(u)?.use { input -> part.outputStream().use { input.copyTo(it) } }
          part.renameTo(java.io.File(dir, System.currentTimeMillis().toString() + "-" + k + "-" + name))
        } catch (e: Exception) {}
      }
    }.start()
  }
`;
if (!kt.includes('lvTakeShared')) {
  if (/class MainActivity\s*:\s*TauriActivity\(\)\s*\{/.test(kt)) kt = kt.replace(/\}\s*$/, body + '}\n');
  else { must(/class MainActivity\s*:\s*TauriActivity\(\)\s*$/m.test(kt), 'unexpected MainActivity.kt'); kt = kt.replace(/class MainActivity\s*:\s*TauriActivity\(\)\s*$/m, 'class MainActivity : TauriActivity() {' + body + '}'); }
  must(!/override fun onResume|override fun onStop|override fun onNewIntent|override fun onWebViewCreate/.test(kt.replace(body, '')), 'MainActivity.kt already overrides onResume/onStop/onNewIntent/onWebViewCreate');
  fs.writeFileSync(ktPath, kt);
}
const pkg = (kt.match(/^package\s+([\w.]+)/m) || [])[1];
must(pkg, 'MainActivity.kt has no package line');
fs.writeFileSync(path.join(path.dirname(ktPath), 'LvNearby.kt'),
  fs.readFileSync(path.join(root, 'mobile/android/LvNearby.kt'), 'utf8').replace('package __PACKAGE__', 'package ' + pkg));
console.log('android-patch: share target, ' + ICONS.length + ' icon presets, screenshot guard, Nearby (Bluetooth)');
