// Puts the built app where it's easy to find and start: dist/Image Vault/Lewd Vault.exe
// (with its data folder appearing next to it on first start). Run by `npm run dist` after the build.
// The folder keeps its old name, so a library kept next to the app ("Image Vault data") stays put.
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const here = new URL('..', import.meta.url);
const built = fileURLToPath(new URL('src-tauri/target/release/image-vault.exe', here));
const dir = fileURLToPath(new URL('dist/Image Vault/', here));
fs.mkdirSync(dir, { recursive: true });
const exe = dir + 'Lewd Vault.exe';
try {
  fs.copyFileSync(built, exe);
} catch (e) {
  if (e.code === 'EBUSY' || e.code === 'EPERM') throw new Error('Lewd Vault is running from dist: close it, then run this again.');
  throw e;
}
// the app's old name
try { fs.rmSync(dir + 'Image Vault.exe', { force: true }); } catch (e) { console.warn('dist: the old Image Vault.exe is still running, so it stays for now'); }
console.log(`dist: ${exe} (${(fs.statSync(exe).size / 1e6).toFixed(1)} MB)`);
