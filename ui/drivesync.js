// Lewd Vault desktop: Google Drive over the web (Settings → Storage), see src-tauri/src/gdrive.rs.
// For PCs where Google Drive for desktop can't be installed (it needs an administrator): sign in in
// the web browser, with the built-in Google key or your own, and the vault syncs with a "Lewd Vault"
// folder in your Drive every 10 minutes and on "Sync now". The Google Drive for desktop option on
// the welcome screen stays as it was.

(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make } = window.vaultUI;
  const sites = window.vaultSites;
  const CONSOLE = 'https://console.cloud.google.com/apis/credentials';

  const box = make('div', 'st-group gd-box');
  $('storageInfo').parentElement.append(box);
  let st = null, poll = null, lastFinished = null;

  const btn = (cls, label, fn) => { const b = make('button', 'btn' + (cls ? ' ' + cls : ''), label); b.type = 'button'; b.onclick = fn; return b; };
  const call = async (name, ...args) => { try { return await api(name, ...args); } catch (e) { toast(errMsg(e), 'err'); return null; } };
  function ago(iso) {
    if (!iso) return 'not yet';
    const s = (Date.now() - new Date(iso)) / 1000;
    return s < 90 ? 'just now' : s < 3600 ? `${Math.round(s / 60)} minutes ago` : s < 86400 ? `${Math.round(s / 3600)} hours ago` : new Date(iso).toLocaleString();
  }

  async function refresh() {
    try { st = await api('driveStatus'); } catch (e) { return; }
    // a sync finished: say what happened, and show what came from Drive
    if (st.finished && st.finished !== lastFinished) {
      const first = lastFinished === null;
      lastFinished = st.finished;
      if (!first) {
        if (st.state === 'error') toast('Google Drive: ' + st.message, 'err');
        else {
          const bits = [st.uploaded && `${fmtN(st.uploaded)} up`, st.downloaded && `${fmtN(st.downloaded)} down`,
            (st.deletedHere + st.deletedThere) && `${fmtN(st.deletedHere + st.deletedThere)} deleted`,
            st.failed && `${fmtN(st.failed)} couldn't sync`].filter(Boolean);
          if (bits.length) toast(`Google Drive synced: ${bits.join(', ')}`, st.failed ? 'err' : 'ok');
        }
        if (st.downloaded) { try { loadLibrary(); } catch (e) {} }
      }
    }
    draw();
    const busy = st.state === 'syncing' || st.state === 'signing-in';
    clearTimeout(poll);
    poll = setTimeout(refresh, busy ? 800 : 30000);
  }

  function draw() {
    if (!st) return;
    box.replaceChildren(make('h3', '', 'Google Drive over the web'));
    const row = make('div', 'setting mt-row');
    const label = make('span', 's-label');
    const acts = make('div', 'mt-acts');
    row.append(label, acts);
    box.append(row);
    const say = (text, small) => { label.textContent = text; if (small) label.append(make('small', '', small)); };

    if (st.state === 'signing-in') {
      say('Waiting for you to sign in, in your web browser…', 'Didn\'t a browser tab open? Copy the link and open it yourself.');
      if (st.authUrl) acts.append(btn('', 'Copy the link', () => navigator.clipboard.writeText(st.authUrl).then(() => toast('Link copied', 'ok'))));
      acts.append(btn('', 'Cancel', () => call('driveCancel')));
      return;
    }
    if (!st.connected) {
      say('Sync your vault with Google Drive without the Drive app.',
        'For PCs where Google Drive for desktop can\'t be installed (it needs an administrator). You sign in once in your web browser; your vault stays on this PC and syncs with a "Lewd Vault" folder in your Drive.' +
        (st.hasKey ? '' : ' First add a Google key (free, about 5 minutes).'));
      if (st.hasKey) acts.append(btn('primary', 'Sign in with Google', signIn));
      acts.append(btn(st.hasKey ? '' : 'primary', st.ownKey ? 'Change key…' : st.hasKey ? 'Use your own key…' : 'Add your key…', keySheet));
      if (st.state === 'error' && st.message) box.append(make('p', 'gd-err', st.message));
      return;
    }
    if (st.state === 'syncing') {
      say(`Syncing with ${st.email || 'Google Drive'}… ${st.total ? `${fmtN(st.done || 0)} of ${fmtN(st.total)}` : 'looking for changes'}`);
      const bar = make('div', 'pv-progress'); const fill = make('i');
      fill.style.width = (st.total ? (st.done || 0) / st.total * 100 : 3) + '%';
      bar.append(fill);
      box.append(bar);
      acts.append(btn('', 'Stop', () => call('driveCancel')));
    } else {
      say(`Signed in as ${st.email || 'your Google account'}. Last synced ${ago(st.lastSync)}.`,
        st.auto ? 'Syncs by itself every 10 minutes while the app is open.' : 'Syncs only when you press Sync now.');
      acts.append(btn('primary', 'Sync now', async () => { await call('driveSync'); refresh(); }));
    }
    if (st.folderUrl) acts.append(btn('', 'Open in Drive', () => call('openLink', st.folderUrl)));
    acts.append(btn('', 'Sign out', async () => {
      if (!confirm('Stop syncing with Google Drive? Your files stay where they are, here and in Drive.')) return;
      await call('driveSignOut'); refresh();
    }));
    const auto = make('label', 'setting gd-auto');
    const cb = make('input'); cb.type = 'checkbox'; cb.checked = !!st.auto;
    cb.onchange = async () => { await call('driveAuto', cb.checked); refresh(); };
    auto.append(make('span', 's-label', 'Sync by itself every 10 minutes'), cb);
    box.append(auto);
    if (st.state === 'error' && st.message) box.append(make('p', 'gd-err', st.message));
  }

  async function signIn() {
    if (await call('driveSignIn')) { toast('Sign in in the browser tab that just opened', 'ok'); refresh(); }
  }

  // "Add your key": a Desktop-app OAuth client of your own, step by step
  function keySheet() {
    sites.sheet('Your Google key', (body, close) => {
      body.append(make('p', 'dc-sheet-lead', 'Google only lets an app into Drive with a key (an "OAuth client"). It\'s free and yours alone. Use the same key on every PC you sync.'));
      const steps = make('ol', 'gd-steps');
      for (const s of [
        'Open the Google Cloud Console and sign in with the Google account you\'ll sync to. Make a project (any name).',
        'APIs & Services → Library: find "Google Drive API" and press Enable.',
        'Google Auth Platform (OAuth consent screen): Get started, any app name, Audience "External". Under Audience, add your own Google address as a test user, or press "Publish app" (then the sign-in doesn\'t run out after 7 days; Google may show an "unverified app" warning you can click past).',
        'Clients (Credentials) → Create client → Application type "Desktop app". Copy its Client ID and Client secret into the boxes below.'
      ]) steps.append(make('li', '', s));
      body.append(steps, btn('', 'Open the Google Cloud Console', () => call('openLink', CONSOLE)));
      const id = sites.text(st && st.clientId, '1234-abc.apps.googleusercontent.com');
      const secret = sites.text('', st && st.ownKey ? '(kept; type a new one to change it)' : 'GOCSPX-…');
      const f = make('div', 'pin-form gd-form');
      f.append(sites.field('Client ID', id), sites.field('Client secret', secret));
      const acts = make('div', 'dc-actions st-actions');
      if (st && st.ownKey) acts.append(btn('danger', st.builtIn ? 'Use the built-in key' : 'Remove the key', async () => {
        if (await call('driveSetKey', '', '')) { close(true); refresh(); }
      }));
      acts.append(btn('primary', 'Save key', async () => {
        if (!id.value.trim()) { toast('Paste the Client ID', 'err'); return; }
        const keep = !secret.value.trim() && st && st.ownKey && id.value.trim() === st.clientId;
        if (keep) { close(true); return; }
        if (await call('driveSetKey', id.value, secret.value)) { close(true); toast('Key saved. Now sign in with Google.', 'ok'); refresh(); }
      }));
      body.append(f, acts);
      setTimeout(() => id.focus(), 30);
    });
  }

  // the welcome screen: point PCs without the Drive app here
  const hint = document.querySelector('#welcome .wl-choose .wl-hint');
  if (hint) hint.after(make('p', 'wl-hint', 'No Google Drive app on this PC (it needs an administrator)? Pick This PC, then Settings → Storage → Google Drive over the web.'));

  setupReady.then(() => setTimeout(refresh, 500));
  // back to the app: catch up on what the automatic sync did meanwhile
  window.addEventListener('focus', () => { clearTimeout(poll); poll = setTimeout(refresh, 600); });
})();
