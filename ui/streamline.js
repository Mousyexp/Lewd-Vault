// Lewd Vault desktop: the streamlining pass. Everyday actions stay big and obvious; occasional tools
// become icons; detailed options fold away next to where they're used, and each fold remembers
// whether you left it open (so regular users keep their full setup, new ones see a calm screen).
//   - sidebar: Stats, Clean up and Settings as a row of icons under the big tiles
//   - full view: the editing fields fold under "Edit details" (with a one-line summary)
//   - Add: Collection and the tag help under "More options"
//   - Library tag panel: "Tag all shown" folds away
//   - Clean up: its four tools in one Tools menu
//   - Settings: "Make your own" theme folds away (open when the Custom theme is on)
(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make } = window.vaultUI;
  const remember = (key, def) => { try { const v = localStorage.getItem('imageVault.fold.' + key); return v === null ? def : v === '1'; } catch (e) { return def; } };
  const keep = (key, on) => { try { localStorage.setItem('imageVault.fold.' + key, on ? '1' : '0'); } catch (e) {} };
  // a fold: <details> with a summary line; its open state remembered under key
  function fold(key, title, def, extraSummary) {
    const d = make('details', 'fold fold-' + key);
    const s = make('summary', 'fold-sum');
    s.append(make('span', 'fold-title', title));
    if (extraSummary) s.append(extraSummary);
    d.append(s);
    d.open = remember(key, def);
    d.addEventListener('toggle', () => keep(key, d.open));
    return d;
  }

  // ---------- sidebar: the occasional tools as icons ----------
  const TOOL_TIP = { stats: 'Stats: your viewing', cleanup: 'Clean up: least opened, duplicates, health', settings: 'Settings: theme, look & more' };
  for (const [tab, tip] of Object.entries(TOOL_TIP)) {
    const b = document.querySelector(`nav [data-tab="${tab}"]`);
    if (b) { b.classList.add('nav-tool'); b.title = tip; b.setAttribute('aria-label', tip); }
  }

  // ---------- full view: the editing fields fold under "Edit details" ----------
  {
    const aside = document.querySelector('#detail aside');
    const first = aside && [...aside.children].find(n => n.matches('label.f') && /category/i.test(n.textContent));
    const save = $('dSave') && $('dSave').closest('.actions');
    if (first && save) {
      const summary = make('span', 'fold-chips');
      const d = fold('detailEdit', 'Edit details', false, summary);
      const body = make('div', 'fold-body');
      // everything from Category to the Save button
      let n = first;
      const moving = [];
      while (n) { moving.push(n); if (n === save) break; n = n.nextElementSibling; }
      first.before(d);
      body.append(...moving);
      d.append(body);
      // the summary: what it is now, readable while folded
      const drawSummary = () => {
        const it = typeof current !== 'undefined' ? current : null; // (the page's own `let current`)
        if (!it) return;
        summary.replaceChildren(...[it.category, `NSFW ${it.nsfw}`, '★'.repeat(it.quality || 1), it.collection ? `in ${it.collection}` : '']
          .filter(Boolean).map(t => make('i', '', t)));
      };
      const od = window.openDetail;
      window.openDetail = function () { const r = od.apply(this, arguments); try { drawSummary(); } catch (e) {} return r; };
      $('dSave').addEventListener('click', () => setTimeout(drawSummary, 400));
    }
  }

  // ---------- Add: Collection and the tag help under "More options" ----------
  {
    const form = $('tagForm');
    const colLabel = form && [...form.querySelectorAll('label.f')].find(l => /collection/i.test(l.textContent));
    if (colLabel) {
      const d = fold('addMore', 'More options: collection, tag help', false);
      const body = make('div', 'fold-body');
      const help = form.querySelector('.tagbox ~ p.hint');
      colLabel.before(d);
      body.append(colLabel, $('collection'), $('colList'));
      if (help) body.append(help);
      d.append(body);
    }
  }

  // ---------- Library: "Tag all shown" folds away ----------
  // (the panel redraws its insides, so the fold is a class on the panel, toggled by the section's heading)
  {
    const panel = document.querySelector('.tag-panel');
    if (panel) {
      panel.classList.toggle('bulk-open', remember('tagAllShown', false));
      panel.addEventListener('click', e => {
        if (!e.target.closest('.bulk > h4')) return;
        keep('tagAllShown', panel.classList.toggle('bulk-open'));
      });
    }
  }

  // ---------- Clean up: one Tools menu ----------
  {
    const row = document.querySelector('.lv-cleanup-tools');
    if (row) {
      const tools = [...row.querySelectorAll('button')];
      const btn = make('button', 'btn ct-btn'); btn.type = 'button';
      btn.innerHTML = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M14.7 6.3a4 4 0 0 0-5.4 5.4L3 18l3 3 6.3-6.3a4 4 0 0 0 5.4-5.4l-2.5 2.5-2.4-.6-.6-2.4z"/></svg><span>Tools</span><i>▾</i>';
      btn.title = 'Find duplicates and look-alikes, compact videos, health check';
      const pop = make('div', 'ct-pop hidden');
      const WHAT = [['duplicates', 'Exact copies saved more than once'], ['look-alikes', 'Resized or re-saved copies'], ['compact', 'Shrink big videos'], ['health', 'Missing files, broken previews']];
      for (const t of tools) {
        const w = WHAT.find(([k]) => t.textContent.toLowerCase().includes(k));
        if (w) t.append(make('small', '', w[1]));
        t.classList.add('ct-item');
        t.addEventListener('click', () => pop.classList.add('hidden'));
        pop.append(t);
      }
      row.prepend(btn);
      document.body.append(pop);
      btn.onclick = () => {
        if (!pop.classList.contains('hidden')) return pop.classList.add('hidden');
        const r = btn.getBoundingClientRect();
        Object.assign(pop.style, { left: r.left + 'px', top: r.bottom + 8 + 'px' });
        pop.classList.remove('hidden');
      };
      addEventListener('pointerdown', e => { if (!pop.contains(e.target) && !btn.contains(e.target)) pop.classList.add('hidden'); }, true);
      addEventListener('keydown', e => { if (e.key === 'Escape') pop.classList.add('hidden'); });
    }
  }

  // ---------- Settings: "Make your own" folds away unless the Custom theme is on ----------
  function foldBuilder() {
    const b = document.querySelector('#settingsForm .theme-builder');
    if (!b || b.dataset.folded) return;
    b.dataset.folded = '1';
    const head = b.querySelector('.tb-head');
    const open = S('theme') === 'custom' || remember('themeBuilder', false);
    b.classList.toggle('tb-closed', !open);
    if (head) {
      head.tabIndex = 0; head.setAttribute('role', 'button');
      const flip = () => { const now = b.classList.toggle('tb-closed'); keep('themeBuilder', !now); };
      head.addEventListener('click', flip);
      head.addEventListener('keydown', e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); flip(); } });
    }
  }
  const rs = window.renderSettings;
  window.renderSettings = function () { const r = rs.apply(this, arguments); try { foldBuilder(); } catch (e) {} return r; };
  foldBuilder();
})();
