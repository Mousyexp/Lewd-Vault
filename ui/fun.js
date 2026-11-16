// Lewd Vault: the "fun features": appreciating (the ♥ button and its counts), star ratings (1 to 3)
// and NSFW levels (1 to 10), with the slideshow's level stages that are built on them.
//
// The welcome screen asks once, on a page of its own with a moving preview of each, whether to keep
// them (yes is ticked, and recommended). Without them the app is plainer: no hearts, stars or levels
// anywhere, a slideshow that just plays, and new things saved with neutral values behind the scenes
// (level 5, two stars) so the vault's files and names stay the same either way. Settings → Look →
// Fun features turns them back on (or off) at any time.
(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make } = window.vaultUI;
  const html = document.documentElement;
  const KEY = 'lv.fun', ASKED = 'lv.funAsked';
  const local = k => { try { return localStorage.getItem(k); } catch (e) { return null; } };
  const keep = (k, v) => { try { localStorage.setItem(k, v); } catch (e) {} };
  const on = () => (typeof settings.fun === 'boolean' ? settings.fun : local(KEY) !== '0');
  // (on the class at once, so nothing flashes before the settings arrive)
  html.classList.toggle('no-fun', local(KEY) === '0');

  // ---------- the setting ----------
  SETTINGS.push({ section: 'Look', key: 'fun', label: 'Fun features', help: 'The Appreciate button, star ratings and NSFW levels, and the slideshow\'s levels. Off: a plainer app and a simple slideshow.', type: 'toggle', def: true });
  let applied = null;
  const save0 = saveSettingsSoon;
  saveSettingsSoon = function () { const r = save0.apply(this, arguments); if (on() !== applied) apply(); return r; };

  // ---------- off: what goes ----------
  const SHOW_ALL = SHOW_OPTIONS.slice();
  const LEVEL_KEYS = new Set(['startLevel', 'firstUpTo', 'endLevel', 'minutesPerLevel', 'levelMix', 'skipEmpty', 'whenOut', 'atEnd', 'clock', 'lockLibrary', 'minRating', 'levelFlash']);
  const PLAIN_SHOW = { startLevel: 1, firstUpTo: 10, endLevel: 10, minutesPerLevel: 600, levelMix: 'below', skipEmpty: true, whenOut: 'repeat', atEnd: 'hold', lockLibrary: false, minRating: 0, levelFlash: false };
  let sortOpts = null;
  function apply() {
    const fun = on();
    applied = fun;
    html.classList.toggle('no-fun', !fun);
    keep(KEY, fun ? '1' : '0');
    // the slideshow: one stage with everything, no level settings
    SHOW_OPTIONS.splice(0, SHOW_OPTIONS.length, ...(fun ? SHOW_ALL : SHOW_ALL.filter(s => !LEVEL_KEYS.has(s.key))));
    if (!fun) Object.assign(showOpts, PLAIN_SHOW);
    // the Library: no level or rating filters, no ordering by them
    const sort = $('fSort');
    if (sort) {
      if (!sortOpts) sortOpts = [...sort.options].map(o => [o.value, o.text]);
      const want = fun ? sortOpts : sortOpts.filter(([v]) => !['best', 'nsfw', 'sfw'].includes(v));
      const v = sort.value;
      sort.replaceChildren(...want.map(([value, text]) => new Option(text, value)));
      sort.value = want.some(([x]) => x === v) ? v : 'new';
    }
    if (!fun) {
      // (everything shown: the highest level; the options run from 10 down)
      const n = $('fNsfw'); if (n && n.options.length) n.value = String(Math.max(...[...n.options].map(o => +o.value || 0)));
      const q = $('fQ'); if (q) q.value = '0';
    }
    try { if (typeof renderLibrary === 'function') renderLibrary(); } catch (e) {}
    dispatchEvent(new Event('vault-fun'));
  }
  // saving without them: a neutral level and two stars, filled in just before the form checks
  const form = $('tagForm');
  if (form) form.addEventListener('submit', () => {
    if (on()) return;
    if (!$('nsfw').value) { $('nsfw').value = 5; $('nsfw').dispatchEvent(new Event('input', { bubbles: true })); }
    const two = $('quality') && $('quality').querySelector('[data-q="2"]');
    if (two && !$('quality').querySelector('.on')) two.click();
  }, true);
  // the Browser's question when saving: the same neutral values, chosen for you
  new MutationObserver(() => {
    if (on()) return;
    const card = document.querySelector('.ask-card:not(.fun-filled)');
    if (!card) return;
    card.classList.add('fun-filled');
    const lv = card.querySelector('.ask-lv.suggested') || card.querySelectorAll('.ask-lv')[4];
    if (lv && !card.querySelector('.ask-lv.on')) lv.click();
    const star = card.querySelectorAll('.ask-star')[1];
    if (star && !card.querySelector('.ask-star.on')) star.click();
  }).observe(document.body, { childList: true });
  // the right-click (long-press) menu: no Rating or NSFW sub-menus
  const menu = document.querySelector('.lv-menu');
  if (menu) new MutationObserver(() => {
    if (on()) return;
    for (const it of menu.querySelectorAll(':scope > .lv-item')) if (/^(Rating|NSFW)/.test((it.querySelector('.lv-label') || it).textContent.trim())) it.remove();
  }).observe(menu, { childList: true });

  // ---------- the welcome screen's page: keep them? ----------
  const welcome = $('welcome');
  const card = welcome && welcome.querySelector('.wl-card');
  if (card) {
    const page = make('div', 'wl-step wl-fun hidden');
    page.innerHTML = `
      <p class="fun-lead">Three little extras make collecting more fun. They're on unless you say otherwise.</p>
      <div class="fun-cards">
        <div class="fun-card"><div class="fun-prev fun-heart" aria-hidden="true">
            <span class="fun-hbtn"><i>♥</i> Appreciate <em class="fun-count"></em></span>
            <span class="fun-float">♥</span><span class="fun-float">♥</span><span class="fun-float">♥</span>
          </div><b>Appreciate</b><span>Tap ♥ whenever something hits the spot. The counts add up, and your favourites rise to the top.</span></div>
        <div class="fun-card"><div class="fun-prev fun-stars" aria-hidden="true"><i>★</i><i>★</i><i>★</i></div>
          <b>Star ratings</b><span>One to three stars for how good it is: sort by them, filter by them, slideshow only your best.</span></div>
        <div class="fun-card"><div class="fun-prev fun-levels" aria-hidden="true"></div>
          <b>NSFW levels</b><span>From 1 (tame) to 10 (wild). Slideshows climb through them; filters keep things calm when you want.</span></div>
      </div>
      <label class="fun-keep"><input type="checkbox" checked>
        <span><b>Keep the fun features <em>Recommended</em></b><small>Untick for a plainer app: no hearts, stars or levels, and a simple slideshow. You can change this later in Settings.</small></span></label>
      <div class="fun-actions"><button class="btn primary fun-go" type="button">Continue</button></div>`;
    card.insertBefore(page, card.querySelector('.wl-step'));
    // the levels' preview: the app's own faces, a highlight going up and down them
    const lv = page.querySelector('.fun-levels');
    for (let n = 1; n <= 10; n++) {
      const b = make('span', 'fun-lv'); b.style.setProperty('--n', n);
      const F = window.vaultFaces;
      if (F && F.faceFor) { const f = make('span', 'fun-face'); f.innerHTML = F.faceFor(n); b.append(f); }
      b.append(make('b', '', String(n)));
      lv.append(b);
    }
    // the heart's count goes up while you look
    const count = page.querySelector('.fun-count');
    let c = 11, timer = null;
    const tick = () => { c = c >= 99 ? 12 : c + 1; count.textContent = '· ' + c; };
    const box = page.querySelector('.fun-keep input');
    const steps = () => card.querySelectorAll('.wl-step');
    const showFun = () => {
      steps().forEach(s => s.classList.toggle('hidden', s !== page));
      welcome.classList.add('on-fun');
      welcome.scrollTop = 0;
      box.checked = on();
      clearInterval(timer); timer = setInterval(tick, 1600); tick();
    };
    page.querySelector('.fun-go').onclick = () => {
      clearInterval(timer);
      settings.fun = box.checked;
      settings.funAsked = true;
      keep(ASKED, '1');
      saveSettingsSoon();
      apply();
      welcome.classList.remove('on-fun');
      steps().forEach(s => s.classList.toggle('hidden', !s.classList.contains('wl-choose')));
      dispatchEvent(new Event('vault-fun-asked'));
    };
    // the first time the welcome screen opens, this page comes first
    let shown = false;
    const check = () => {
      const open = !welcome.classList.contains('hidden') && welcome.classList.contains('first');
      if (!open) { shown = false; return; }
      if (shown || settings.funAsked || local(ASKED) === '1') return;
      shown = true;
      showFun();
    };
    new MutationObserver(check).observe(welcome, { attributes: true, attributeFilter: ['class'] });
    check(); // (it may have opened already, before this file loaded)
    setupReady.then(() => setTimeout(check, 0)); // (and once the settings are in: funAsked)
  }

  setupReady.then(s => {
    const saved = s && s.settings;
    if (saved && typeof saved.fun === 'boolean') settings.fun = saved.fun;
    else if (local(KEY) !== null) { settings.fun = local(KEY) !== '0'; }
    if (saved && saved.funAsked) settings.funAsked = true;
    apply();
  });
  window.vaultFun = { on, apply };
})();
