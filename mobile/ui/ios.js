// The iOS layer: what the shared page needs to be a phone app.
//
// Only runs on iOS (the app sets html.ios there, before any script). The shared page keeps doing all
// the work; this file adds the pieces a phone needs and a desktop doesn't:
//   1. a bottom tab bar (Library, Browser, Add, Slideshow, More) in place of the sidebar, and the
//      More sheet with everything else,
//   2. the Library's large title, and its Tags panel as a bottom sheet,
//   3. confirm() and prompt(): WKWebView silently drops them (confirm is always "no"), so every
//      "are you sure?" would block its action for good; they become in-app sheets,
//   4. long-press where the desktop uses right-click,
//   5. privacy: "Hide now" (the emergency key has no global shortcut on iOS) and a cover while the
//      app is in the background, so the app switcher shows nothing.
// The look is mobile/ui/ios.css.
(() => {
  'use strict';
  const html = document.documentElement;
  if (!html.classList.contains('ios')) return;
  const $ = id => document.getElementById(id);
  const make = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };
  const ICON = {
    more: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='black'><circle cx='5' cy='12' r='2.2'/><circle cx='12' cy='12' r='2.2'/><circle cx='19' cy='12' r='2.2'/></svg>",
    close: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.4' stroke-linecap='round'><path d='M6 6l12 12M18 6L6 18'/></svg>",
    you: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round'><circle cx='12' cy='8' r='4'/><path d='M4.5 20c1.2-3.6 4-5.5 7.5-5.5s6.3 1.9 7.5 5.5'/></svg>",
    coach: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M12 20s-7-4.4-7-10a4 4 0 0 1 7-2.6A4 4 0 0 1 19 10c0 5.600-7 10-7 10z'/></svg>",
    hide: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M3 3l18 18M10.6 6.200A9.700 9.700 0 0 1 12 6c5 0 8.500 4 9.500 6-.4.800-1.200 2-2.400 3.100M6.500 7.600C4.500 9 3.100 10.800 2.500 12c1 2 4.500 6 9.500 6 1.300 0 2.500-.3 3.600-.7M9.900 9.900a3 3 0 0 0 4.200 4.200'/></svg>",
    search: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.2' stroke-linecap='round'><circle cx='11' cy='11' r='6.5'/><path d='M16 16l4.5 4.5'/></svg>",
    tags: "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'><path d='M3 12V4h8l10 10-8 8z'/><circle cx='7.500' cy='8.500' r='1.300' fill='black'/></svg>"
  };
  const icon = name => `url("data:image/svg+xml;utf8,${ICON[name].replace(/"/g, "'").replace(/#/g, '%23')}")`;

  // ---------- sheets (the More list, confirm/prompt, the Tags panel's backdrop) ----------
  // One at a time, from the bottom, closed by its backdrop. Returns { el, body, close }.
  function sheet(cls, onClose) {
    const back = make('div', 'ios-back');
    const el = make('div', 'ios-sheet glass ' + (cls || ''));
    el.setAttribute('role', 'dialog');
    el.append(make('div', 'ios-grab'));
    const body = make('div', 'ios-sheet-body');
    el.append(body);
    back.append(el);
    let open = true;
    const close = result => {
      if (!open) return;
      open = false;
      back.classList.remove('in');
      setTimeout(() => back.remove(), 220);
      if (onClose) onClose(result);
    };
    back.addEventListener('click', e => { if (e.target === back) close(); });
    document.body.append(back);
    requestAnimationFrame(() => back.classList.add('in'));
    return { el, body, close };
  }

  // ---------- 1. the tab bar ----------
  const nav = document.querySelector('header nav');
  const sideTabs = nav ? [...nav.querySelectorAll('button[data-tab]')] : [];
  const side = tab => sideTabs.find(b => b.dataset.tab === tab);
  const label = b => (b.querySelector('.nt-title') || b).textContent.trim();
  const iconOf = b => getComputedStyle(b).getPropertyValue('--icon').trim(); // (the sidebar's own icon)
  const PRIMARY = ['library', 'discover', 'add', 'slideshow'].filter(side);
  // (a <div>, not a <nav>: the desktop's sidebar rules are `html.desktop nav …`, and iOS carries both classes)
  const tabbar = make('div', 'ios-tabbar glass');
  tabbar.setAttribute('role', 'navigation');
  tabbar.setAttribute('aria-label', 'Sections');
  const tabButton = (text, iconValue, onTap) => {
    const t = make('button', '', text);
    t.type = 'button';
    if (iconValue) t.style.setProperty('--icon', iconValue);
    t.onclick = onTap;
    tabbar.append(t);
    return t;
  };
  for (const tab of PRIMARY) tabButton(label(side(tab)), iconOf(side(tab)), () => side(tab).click()).dataset.tab = tab;
  const moreBtn = tabButton('More', icon('more'), openMore);
  moreBtn.dataset.tab = 'more';
  document.body.append(tabbar);

  function openMore() {
    const s = sheet('ios-more');
    s.body.append(make('h2', 'ios-sheet-title', 'More'));
    const list = make('div', 'ios-list');
    const row = (text, iconValue, onTap, cls) => {
      const r = make('button', 'ios-row' + (cls ? ' ' + cls : ''), text);
      r.type = 'button';
      if (iconValue) r.style.setProperty('--icon', iconValue);
      r.onclick = () => { s.close(); onTap(); };
      list.append(r);
    };
    for (const b of sideTabs) if (!PRIMARY.includes(b.dataset.tab)) row(label(b), iconOf(b), () => b.click());
    if (window.vaultProfile) row('You', icon('you'), () => window.vaultProfile.open());
    if (window.vaultCoach) row('Coach', icon('coach'), () => window.vaultCoach.open());
    row('Hide now', icon('hide'), hideNow, 'danger');
    s.body.append(list);
  }

  // the bar follows whichever section is showing (More lights up for everything it holds)
  const syncTabs = () => {
    const on = nav && nav.querySelector('button.on');
    const tab = on ? on.dataset.tab : '';
    const inMore = !PRIMARY.includes(tab);
    tabbar.querySelectorAll('button').forEach(t => t.classList.toggle('on', t.dataset.tab === tab || (t === moreBtn && inMore)));
  };
  if (nav) new MutationObserver(syncTabs).observe(nav, { subtree: true, attributes: true, attributeFilter: ['class'] });
  syncTabs();
  // a new section starts at its top
  tabbar.addEventListener('click', () => scrollTo(0, 0));

  // ---------- 2. the Library, laid out like Photos ----------
  // The grid runs edge to edge under everything. Over it float: the title and count (top left), Select
  // and two round buttons (top right: search & filters, tags), and a pill switcher above the tab bar
  // (All / Pictures / GIFs / Videos). Search, sort and filters live in one sheet, Tags in another.
  const lib = $('tab-library');
  if (lib) {
    const head = make('div', 'ios-libhead');
    const title = make('h1', 'ios-title', 'Library');
    const sub = make('span', 'ios-sub');
    const words = make('div', 'ios-libwords');
    words.append(title, sub);
    const tools = make('div', 'ios-libtools');
    const round = (name, label, onTap) => {
      const b = make('button', 'ios-round');
      b.type = 'button';
      b.title = label;
      b.setAttribute('aria-label', label);
      b.style.setProperty('--icon', icon(name));
      b.onclick = onTap;
      return b;
    };
    const select = make('button', 'ios-pill', 'Select');
    select.type = 'button';
    head.append(words, tools);
    lib.insertBefore(head, lib.firstChild);
    // the count under the title is the page's own ("5 of 5 items")
    const count = $('count');
    const showCount = () => { sub.textContent = count ? count.textContent.replace(/^(\d[\d,.]*) of \1 /, '$1 ') : ''; };
    if (count) new MutationObserver(showCount).observe(count, { childList: true, characterData: true, subtree: true });
    showCount();
    // scrolled: the title shrinks (the head stays, like Photos' date header)
    let ticking = false;
    addEventListener('scroll', () => {
      if (ticking) return;
      ticking = true;
      requestAnimationFrame(() => { head.classList.toggle('small', scrollY > 24); ticking = false; });
    }, { passive: true });

    // a sheet made of the page's own controls: `nodes` move into it once and stay there
    const dock = (cls, nodes) => {
      const panel = make('div', 'ios-dock ' + cls);
      panel.append(make('div', 'ios-grab'), ...nodes.filter(Boolean));
      const back = make('div', 'ios-back ios-dock-back');
      lib.append(back, panel);
      const show = on => { panel.classList.toggle('in', on); back.classList.toggle('in', on); html.classList.toggle('ios-dock-open', on); };
      back.onclick = () => show(false);
      return show;
    };
    const showFilters = dock('ios-filters', [make('h2', 'ios-sheet-title', 'Search & filters'), lib.querySelector('.searchbar'), lib.querySelector('.lib-filterrow')]);
    const panel = lib.querySelector('.tag-panel');
    let showTags = () => {};
    if (panel) {
      const back = make('div', 'ios-back ios-dock-back');
      panel.before(back); // (beside the panel, so the panel stacks above it)
      panel.prepend(make('div', 'ios-grab'));
      showTags = on => { html.classList.toggle('ios-tags-open', on); back.classList.toggle('in', on); html.classList.toggle('ios-dock-open', on); };
      back.onclick = () => showTags(false);
      // picking a tag filters the grid behind the sheet: close it so the result shows
      panel.addEventListener('click', e => { if (e.target.closest('.tl-name')) setTimeout(() => showTags(false), 120); });
    }
    tools.append(select, round('search', 'Search and filters', () => showFilters(true)), round('tags', 'Tags', () => showTags(true)));

    // Select: the desktop picks with Ctrl+click; here Select makes the next taps pick instead of open
    // (after the first pick the page's own selection takes over: its bar has Done)
    const grid = $('grid');
    let selecting = false;
    const setSelecting = on => { selecting = on; select.textContent = on ? 'Cancel' : 'Select'; select.classList.toggle('on', on); };
    select.onclick = () => {
      if (selecting) dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape', bubbles: true })); // (clears the page's selection)
      setSelecting(!selecting);
    };
    if (grid) grid.addEventListener('click', e => {
      if (!selecting || e.ctrlKey || !e.isTrusted) return;
      const card = e.target.closest('.card');
      if (!card) return;
      e.stopImmediatePropagation(); e.preventDefault();
      card.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true, ctrlKey: true }));
    }, true);
    const bar = document.querySelector('.ms-bar');
    if (bar) new MutationObserver(() => { if (bar.classList.contains('hidden') && selecting && !grid.querySelector('.card.picked-many')) setSelecting(false); })
      .observe(bar, { attributes: true, attributeFilter: ['class'] });

    // the switcher above the tab bar: the page's own Type filter, as a segmented control
    const kind = $('fKind');
    if (kind) {
      const seg = make('div', 'ios-seg glass');
      const thumb = make('i', 'ios-seg-thumb');
      seg.append(thumb);
      const opts = [['', 'All'], ['image', 'Pictures'], ['gif', 'GIFs'], ['video', 'Videos']];
      const draw = () => {
        const k = Math.max(0, opts.findIndex(([v]) => v === kind.value));
        seg.style.setProperty('--at', k);
        seg.querySelectorAll('button').forEach((b, i) => b.classList.toggle('on', i === k));
      };
      for (const [v, text] of opts) {
        const b = make('button', '', text);
        b.type = 'button';
        b.onclick = () => { kind.value = v; kind.dispatchEvent(new Event('change', { bubbles: true })); draw(); scrollTo({ top: 0, behavior: 'smooth' }); };
        seg.append(b);
      }
      seg.style.setProperty('--n', opts.length);
      kind.addEventListener('change', draw);
      lib.append(seg);
      draw();
    }

    // Photos shows squares: unless a layout was chosen, the Library starts as the even grid here
    if (typeof setupReady !== 'undefined') setupReady.then(s => {
      if (s && !(s.settings && 'libraryLayout' in s.settings) && matchMedia('(max-width: 820px)').matches) {
        settings.libraryLayout = 'grid';
        const pick = $('fLayout');
        if (pick) pick.value = 'grid';
        setTimeout(() => { if (typeof renderLibrary === 'function') renderLibrary(); }, 0);
      }
    });
  }

  // ---------- the viewer: swipe sideways for the next one, down to close ----------
  const detail = $('detail');
  if (detail) {
    const big = detail.querySelector('.big');
    let from = null;
    const media = () => big && big.querySelector('img:not(.hidden), video:not(.hidden)');
    // the next / previous one: the neighbour in the filmstrip under the picture (ui/extras.js), else
    // the page's own arrows (a collection)
    const step = d => {
      const on = detail.querySelector('.filmstrip:not(.hidden) .fs-item.on');
      const near = on && (d > 0 ? on.nextElementSibling : on.previousElementSibling);
      if (near && near.classList.contains('fs-item')) { near.click(); return true; }
      const b = $(d > 0 ? 'dNext' : 'dPrev');
      if (b && !b.classList.contains('hidden')) { b.click(); return true; }
      return false;
    };
    if (big) {
      big.addEventListener('touchstart', e => {
        from = e.touches.length === 1 && !e.target.closest('button, input, .dstrip, [class*="strip"]') ? { x: e.touches[0].clientX, y: e.touches[0].clientY, axis: '' } : null;
      }, { passive: true });
      big.addEventListener('touchmove', e => {
        if (!from) return;
        const dx = e.touches[0].clientX - from.x, dy = e.touches[0].clientY - from.y;
        if (!from.axis && Math.hypot(dx, dy) > 12) from.axis = Math.abs(dx) > Math.abs(dy) ? 'x' : (dy > 0 && detail.scrollTop <= 0 ? 'y' : 'scroll');
        const m = media();
        if (!m || !from.axis || from.axis === 'scroll') return;
        m.style.transition = 'none';
        m.style.transform = from.axis === 'x' ? `translateX(${dx}px)` : `translateY(${Math.max(0, dy)}px) scale(${Math.max(.8, 1 - Math.max(0, dy) / 900)})`;
        if (from.axis === 'y') detail.style.setProperty('--fade', String(Math.max(.35, 1 - Math.max(0, dy) / 500)));
      }, { passive: true });
      const end = e => {
        if (!from) return;
        const t = e.changedTouches[0], dx = t.clientX - from.x, dy = t.clientY - from.y, axis = from.axis;
        from = null;
        const m = media();
        const settle = () => { if (m) { m.style.transition = 'transform .28s cubic-bezier(.2, .8, .2, 1)'; m.style.transform = ''; } detail.style.removeProperty('--fade'); };
        if (axis === 'x' && Math.abs(dx) > 64 && step(dx < 0 ? 1 : -1)) { if (m) { m.style.transition = 'none'; m.style.transform = ''; } return; }
        if (axis === 'y' && dy > 110) { const c = $('closeDetail'); if (c) c.click(); settle(); return; }
        settle();
      };
      big.addEventListener('touchend', end, { passive: true });
      big.addEventListener('touchcancel', end, { passive: true });
    }
  }

  // the viewer's "Open" (in another app): no such thing on a phone ("Show in folder" is #dOpen, in the CSS)
  const openInApp = [...document.querySelectorAll('#detail aside button')].find(b => b.textContent.trim() === 'Open');
  if (openInApp) openInApp.classList.add('ios-no');

  // Settings' jump chips are made from its headings: drop the one for the section hidden on iOS
  const settingsTab = $('tab-settings');
  if (settingsTab) new MutationObserver(() => {
    for (const b of settingsTab.querySelectorAll('.st-jump')) if (/Google Drive/.test(b.textContent)) b.remove();
  }).observe(settingsTab, { childList: true, subtree: true });

  // the same two in the long-press menu (it is rebuilt each time it opens)
  const menu = document.querySelector('.lv-menu');
  if (menu) new MutationObserver(() => {
    for (const b of menu.querySelectorAll('.lv-item')) if (/Open with its program|Show in folder/.test(b.textContent)) b.remove();
  }).observe(menu, { childList: true });

  // ---------- 3. confirm() and prompt() ----------
  // WKWebView's defaults are confirm -> false and prompt -> "" with nothing shown, and the page calls
  // them synchronously ("if (!confirm(...)) return;"), which no sheet can answer in time. So: the
  // first call shows the sheet and answers "no" (the action stops, as if cancelled); when you choose
  // OK, the tap that started it is replayed and the same question is answered at once with what you
  // chose. Every one of these questions comes from a tap, so the replay is exact.
  let lastTap = null;           // what was last tapped (the thing to tap again)
  let answered = null;          // { kind, text, value }: the answer waiting for the replayed question
  addEventListener('click', e => { if (e.isTrusted) lastTap = e.target; }, true);
  const replay = target => {
    if (!target || !target.isConnected) return;
    if (typeof target.click === 'function') target.click();
    else target.dispatchEvent(new MouseEvent('click', { bubbles: true, cancelable: true }));
  };
  function ask(kind, text, def) {
    const key = String(text);
    if (answered && answered.kind === kind && answered.text === key) {
      const v = answered.value;
      answered = null;
      return v;
    }
    const target = lastTap;
    const s = sheet('ios-ask');
    s.body.append(make('p', 'ios-ask-text', key));
    let input = null;
    if (kind === 'prompt') {
      input = make('input', 'ios-ask-input');
      Object.assign(input, { type: 'text', value: def == null ? '' : String(def), autocapitalize: 'off', spellcheck: false });
      s.body.append(input);
      setTimeout(() => input.focus(), 260);
    }
    const acts = make('div', 'ios-ask-acts');
    const no = make('button', 'btn', 'Cancel'), yes = make('button', 'btn primary', 'OK');
    no.type = yes.type = 'button';
    no.onclick = () => s.close();
    yes.onclick = () => {
      s.close();
      answered = { kind, text: key, value: kind === 'prompt' ? input.value : true };
      replay(target);
      // (if the replayed tap asked nothing after all, don't let the answer leak into a later question)
      setTimeout(() => { answered = null; }, 400);
    };
    if (input) input.addEventListener('keydown', e => { if (e.key === 'Enter') yes.click(); });
    acts.append(no, yes);
    s.body.append(acts);
    return kind === 'prompt' ? null : false;
  }
  window.confirm = text => ask('confirm', text);
  window.prompt = (text, def) => ask('prompt', text, def);
  window.alert = text => { const s = sheet('ios-ask'); s.body.append(make('p', 'ios-ask-text', String(text))); const ok = make('button', 'btn primary', 'OK'); ok.type = 'button'; ok.onclick = () => s.close(); const a = make('div', 'ios-ask-acts'); a.append(ok); s.body.append(a); };

  // ---------- 4. long-press = right-click ----------
  let press = null;
  const cancelPress = () => { if (press) { clearTimeout(press.timer); press = null; } };
  addEventListener('touchstart', e => {
    cancelPress();
    if (e.touches.length !== 1 || e.target.closest('input, textarea, select, video, .ios-sheet')) return;
    const t = e.touches[0], target = e.target;
    press = { x: t.clientX, y: t.clientY, timer: setTimeout(() => {
      press = null;
      const ev = new MouseEvent('contextmenu', { bubbles: true, cancelable: true, clientX: t.clientX, clientY: t.clientY });
      // (only swallow the tap that follows when something actually took the menu)
      if (!target.dispatchEvent(ev)) addEventListener('click', c => { c.preventDefault(); c.stopPropagation(); }, { capture: true, once: true });
    }, 480) };
  }, { passive: true });
  addEventListener('touchmove', e => { const t = e.touches[0]; if (press && t && Math.hypot(t.clientX - press.x, t.clientY - press.y) > 10) cancelPress(); }, { passive: true });
  addEventListener('touchend', cancelPress, { passive: true });
  addEventListener('touchcancel', cancelPress, { passive: true });

  // ---------- 5. privacy ----------
  const events = window.__TAURI__ && window.__TAURI__.event;
  const panic = on => {
    // the same message the desktop's emergency key sends (ui/privacy.js listens); "hide" has no meaning
    // on a phone (there is no window to hide), so it shows the screensaver
    const mode = typeof S === 'function' && S('panicMode') === 'video' ? 'video' : 'screensaver';
    if (events && events.emit) events.emit('panic', on ? { on: true, mode } : { on: false, mode: 'cover' });
  };
  function hideNow() { panic(true); }
  // the screensaver ignores taps on purpose; holding a finger on it for a second brings the vault back
  // (behind your PIN, if you set one)
  let hold = null;
  addEventListener('touchstart', e => {
    if (!e.target.closest || !e.target.closest('.pv-screen')) return;
    clearTimeout(hold);
    hold = setTimeout(() => { panic(false); lockIfPin(); }, 1000);
  }, { passive: true, capture: true });
  for (const ev of ['touchend', 'touchcancel', 'touchmove']) addEventListener(ev, () => clearTimeout(hold), { passive: true, capture: true });
  // (the PIN lock listens for Ctrl+L, see ui/extras.js; it does nothing when no PIN is set)
  const lockIfPin = () => dispatchEvent(new KeyboardEvent('keydown', { key: 'l', ctrlKey: true, bubbles: true }));

  // in the background (the app switcher, a notification pulled down): a plain cover over everything,
  // so the switcher's snapshot shows nothing; back in front it lifts, to the PIN lock if there is one
  const cover = make('div', 'ios-cover');
  document.body.append(cover);
  let coveredAt = 0;
  const setCover = on => {
    if (on) { coveredAt = Date.now(); cover.classList.add('on'); document.querySelectorAll('video, audio').forEach(m => { try { m.pause(); } catch (e) {} }); }
    else { cover.classList.remove('on'); if (coveredAt && Date.now() - coveredAt > 1500) lockIfPin(); coveredAt = 0; }
  };
  document.addEventListener('visibilitychange', () => setCover(document.visibilityState !== 'visible'));
  addEventListener('pagehide', () => setCover(true));
  addEventListener('pageshow', () => { if (document.visibilityState === 'visible') setCover(false); });
})();
