// Lewd Vault desktop: sharing (Phase 9 of PLAN.md), a first step. On the You page:
//   - Make a share pack: a collection, a category, your original content or your most appreciated,
//     as one .zip (the files as they are, each with its tags, scores and source, and a "vault card"
//     picture), to give to a friend however you like (src-tauri/src/pack.rs)
//   - Open a friend's pack: see its card and what's in it, then add it all (with its tags and sources)
//     into your vault, into their categories or one of your own
// A shared online space (accounts, a feed, likes) would need a server, sign-ups and age checks, so
// it's not part of this.

(() => {
  'use strict';
  if (!window.vaultUI || !inTauri) return;
  const { make, thumbSrc, tagsOf } = window.vaultUI;
  const LIKED = 50;

  // ---------- what can go in a pack ----------
  function sources() {
    const lib = library || [];
    const cols = [...new Set(lib.map(i => i.collection).filter(Boolean))].sort((a, b) => a.localeCompare(b));
    const cats = [...new Set(lib.map(i => i.category))].sort((a, b) => a.localeCompare(b));
    return [
      ...cols.map(c => ({ key: 'col:' + c, label: `Collection: ${c}`, title: c, items: () => lib.filter(i => i.collection === c) })),
      ...cats.map(c => ({ key: 'cat:' + c, label: `Category: ${c}`, title: c, items: () => lib.filter(i => i.category === c) })),
      { key: 'oc', label: 'Your original content', title: 'Original content', items: () => lib.filter(i => tagsOf(i).includes('meta:oc')) },
      { key: 'liked', label: `Your ${LIKED} most appreciated`, title: 'Favourites', items: () => [...lib].sort((a, b) => likesOf(b.id) - likesOf(a.id) || timeOf(b.id) - timeOf(a.id)).slice(0, LIKED) }
    ];
  }

  // ---------- the vault card: a picture of the pack ----------
  const loadImg = src => new Promise(ok => { const im = new Image(); im.crossOrigin = 'anonymous'; im.onload = () => ok(im); im.onerror = () => ok(null); im.src = src; });
  async function card(title, items, who) {
    const W = 1200, H = 630, c = document.createElement('canvas');
    c.width = W; c.height = H;
    const g = c.getContext('2d');
    const accent = getComputedStyle(document.documentElement).getPropertyValue('--accent').trim() || '#E27589';
    const bg = g.createLinearGradient(0, 0, W, H);
    bg.addColorStop(0, '#1d0f17'); bg.addColorStop(0.55, '#3a1626'); bg.addColorStop(1, accent);
    g.fillStyle = bg; g.fillRect(0, 0, W, H);
    // a fan of pictures on the left
    const pics = (await Promise.all(items.slice(0, 5).map(i => loadImg(thumbSrc(i, 480))))).filter(Boolean);
    pics.forEach((im, k) => {
      g.save();
      g.translate(250 + k * 46, 330 + (k % 2) * 16);
      g.rotate((-12 + k * 6) * Math.PI / 180);
      const w = 250, h = 330, s = Math.max(w / im.width, h / im.height);
      g.shadowColor = 'rgba(0,0,0,.55)'; g.shadowBlur = 30; g.shadowOffsetY = 12;
      g.fillStyle = '#fff'; g.fillRect(-w / 2 - 8, -h / 2 - 8, w + 16, h + 16);
      g.shadowColor = 'transparent';
      g.beginPath(); g.rect(-w / 2, -h / 2, w, h); g.clip();
      g.drawImage(im, -im.width * s / 2, -im.height * s / 2, im.width * s, im.height * s);
      g.restore();
    });
    // the words on the right
    g.fillStyle = '#fff';
    g.font = '700 30px Nunito, sans-serif';
    g.fillText('♥ Lewd Vault share pack', 640, 140);
    g.font = '700 78px "Dancing Script", cursive';
    const t = title.length > 22 ? title.slice(0, 21) + '…' : title;
    g.fillText(t, 636, 250);
    g.font = '600 30px Nunito, sans-serif';
    g.fillStyle = 'rgba(255,255,255,.85)';
    const kinds = { image: 0, gif: 0, video: 0 }; items.forEach(i => { kinds[i.kind] = (kinds[i.kind] || 0) + 1; });
    g.fillText(`${items.length} ${items.length === 1 ? 'item' : 'items'} · ${[kinds.image && `${kinds.image} pictures`, kinds.gif && `${kinds.gif} GIFs`, kinds.video && `${kinds.video} videos`].filter(Boolean).join(', ')}`, 640, 320);
    const tags = {}; items.forEach(i => tagsOf(i).forEach(x => { if (!x.startsWith('meta:')) tags[x] = (tags[x] || 0) + 1; }));
    const top = Object.entries(tags).sort((a, b) => b[1] - a[1]).slice(0, 5).map(([x]) => '#' + x.replace(/^\w+:/, '').replace(/_/g, ' '));
    g.font = '600 24px Nunito, sans-serif';
    g.fillStyle = 'rgba(255,255,255,.7)';
    g.fillText(top.join('  '), 640, 370);
    g.fillStyle = '#fff';
    g.font = '700 28px Nunito, sans-serif';
    g.fillText(`from ${who.name}`, 640, 470);
    g.font = '600 22px Nunito, sans-serif';
    g.fillStyle = 'rgba(255,255,255,.75)';
    g.fillText(`Level ${who.level} · ${who.title || ''}`, 640, 506);
    return c.toDataURL('image/png');
  }

  // ---------- the section on the You page ----------
  function section(tab, who) {
    const box = make('section', 'sh-box');
    box.append(make('h3', 'pf-h', 'Share'));
    const grid = make('div', 'sh-grid');
    // make a pack
    const mk = make('div', 'sh-card');
    mk.append(make('h4', '', 'Make a share pack'), make('p', 'sh-sub', 'One .zip with the files, their tags, scores and sources, and a picture of the pack, to give to a friend however you like.'));
    const sel = make('select');
    const srcs = sources();
    srcs.forEach(s => { const n = s.items().length; if (n) sel.add(new Option(`${s.label} (${n})`, s.key)); });
    const title = make('input'); Object.assign(title, { type: 'text', placeholder: 'The pack\'s name', maxLength: 40 });
    const preview = make('img', 'sh-preview'); preview.alt = '';
    const go = make('button', 'btn primary', 'Save the pack…');
    let lastCard = '';
    const redraw = async () => {
      const s = srcs.find(x => x.key === sel.value);
      if (!s) { go.disabled = true; return; }
      if (!title.dataset.touched) title.value = s.title;
      go.disabled = false;
      lastCard = await card(title.value || s.title, s.items(), who);
      preview.src = lastCard;
    };
    sel.onchange = () => { title.dataset.touched = ''; redraw(); };
    let timer = null;
    title.oninput = () => { title.dataset.touched = '1'; clearTimeout(timer); timer = setTimeout(redraw, 300); };
    go.onclick = async () => {
      const s = srcs.find(x => x.key === sel.value);
      if (!s) return;
      go.disabled = true; go.textContent = 'Packing…';
      try {
        const r = await tauriInvoke('export_pack', { ids: s.items().map(i => i.id), card: lastCard, title: title.value || s.title });
        toast(`Pack saved: ${r.count} items`, 'ok');
      } catch (e) { if (!/Cancelled/.test(String(e))) toast(String(e), 'err'); }
      go.disabled = false; go.textContent = 'Save the pack…';
    };
    if (!sel.options.length) mk.append(make('p', 'sh-sub', 'Add something to your vault first.'));
    else mk.append(sel, title, preview, go);
    // open a pack
    const op = make('div', 'sh-card');
    const openBtn = make('button', 'btn primary', 'Open a pack…');
    openBtn.onclick = openPack;
    op.append(make('h4', '', 'Open a friend\'s pack'), make('p', 'sh-sub', 'See what\'s in it, then add it all to your vault with its tags and sources.'), openBtn,
      make('p', 'sh-note', 'A shared online space (accounts, a feed, likes) would need a server, sign-ups and age checks, so packs are the way to share for now.'));
    grid.append(mk, op);
    box.append(grid);
    tab.append(box);
    if (sel.options.length) redraw();
  }

  async function openPack() {
    let pack;
    try { pack = await tauriInvoke('open_pack'); }
    catch (e) { if (!/Cancelled/.test(String(e))) toast(String(e), 'err'); return; }
    const back = make('div', 'dc-sheet');
    const box = make('div', 'dc-sheet-card');
    box.innerHTML = '<div class="dc-sheet-head"><h2></h2><button class="btn" type="button">Close</button></div><div class="dc-sheet-body"></div>';
    box.querySelector('h2').textContent = pack.title || 'A share pack';
    const close = () => { back.remove(); api('tidyPack', pack.token).catch(() => {}); };
    box.querySelector('.btn').onclick = close;
    const body = box.querySelector('.dc-sheet-body');
    if (pack.card) { const im = make('img', 'sh-preview'); im.src = vaultUrl(pack.card); body.append(im); }
    const cats = {}; pack.items.forEach(i => { cats[i.category] = (cats[i.category] || 0) + 1; });
    body.append(make('p', 'dc-sheet-lead', `${pack.items.length} items${pack.made ? ' · made ' + new Date(pack.made).toLocaleDateString() : ''} · ${Object.entries(cats).map(([c, n]) => `${c} (${n})`).join(', ')}`));
    const where = make('select');
    where.add(new Option('Into their categories', '')); where.add(new Option(`Into one category: "${pack.title || 'Shared'}"`, pack.title || 'Shared'));
    const add = make('button', 'btn primary', `Add all ${pack.items.length} to my vault`);
    add.onclick = async () => {
      add.disabled = true;
      let n = 0;
      for (const it of pack.items) {
        add.textContent = `Adding ${n + 1} of ${pack.items.length}…`;
        try {
          const blob = await (await fetch(vaultUrl(it.id))).blob();
          const ext = (it.file.match(/\.[a-z0-9]+$/i) || ['.bin'])[0];
          const file = new File([blob], (it.name || 'shared').replace(/\.[^.]+$/, '') + ext, { type: blob.type || '' });
          const category = where.value || it.category || 'Shared';
          jobs.push({ file, category, nsfw: Math.max(1, Math.min(10, +it.nsfw || 1)), quality: Math.max(1, Math.min(3, +it.quality || 2)), collection: it.collection || '', tags: it.tags || '', source: it.source || '' });
          addCategory(category);
          n++;
        } catch (e) { toast(errMsg(e), 'err'); }
      }
      pumpUploads();
      toast(`${n} items on their way into your vault`, 'ok');
      close();
    };
    body.append(where, add);
    back.append(box);
    document.body.append(back);
  }

  window.vaultShare = { section, openPack };
})();
