// iOS layer: makes the desktop page read as an iOS 26 app, laid out like Apple Photos.
//
// Only runs on iOS (the app sets html.ios there). It does not touch the shared page's own logic: it
// mirrors the sidebar's sections into a translucent bottom tab bar, adds the large collapsing title
// over the Library grid, floats the detail viewer's chrome over the media and hides it on tap, and
// keeps the tab bar in step with whichever section is shown. Liquid Glass itself is CSS
// (mobile/ui/ios.css); this file only supplies the structure and the small behaviours.
(() => {
  'use strict';
  if (!document.documentElement.classList.contains('ios')) return;
  const $ = id => document.getElementById(id);
  const make = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text != null) e.textContent = text; return e; };

  // ---------- the bottom tab bar (mirrors the sidebar) ----------
  const nav = document.querySelector('header nav');
  // A <div>, not a <nav>: the desktop layer's sidebar rules are written as `html.desktop nav …`
  // (90 of them), and on iOS <html> carries both `ios` and `desktop`. A <nav> here would be caught
  // by those rules — column layout, zero font-size — and collapse to a 6px-tall strip off-screen.
  // role="navigation" keeps the semantics without the selector collision.
  const tabbar = make('div', 'photos-tabbar');
  tabbar.setAttribute('role', 'navigation');
  tabbar.setAttribute('aria-label', 'Sections');
  const tabs = nav ? [...nav.querySelectorAll('button')] : [];
  for (const b of tabs) {
    const t = make('button', b.classList.contains('on') ? 'on' : '');
    t.type = 'button';
    t.dataset.tab = b.dataset.tab;
    t.textContent = (b.querySelector('.nt-title') || b).textContent.trim();
    if (b.dataset.count) t.dataset.count = b.dataset.count;
    // the icon: the sidebar gets it from `html.desktop nav [data-tab=…]` in CSS, which a <div> does
    // not match, so mirror the resolved value onto the button (its ::before masks by var(--icon))
    const icon = getComputedStyle(b).getPropertyValue('--icon').trim();
    if (icon) t.style.setProperty('--icon', icon);
    t.onclick = () => b.click(); // the shared page does the switching; this only mirrors it
    tabbar.append(t);
  }
  document.body.append(tabbar);
  // keep the bar's highlight (and the sidebar's) in step when a section changes
  const syncTabs = () => {
    const on = nav && nav.querySelector('button.on');
    tabbar.querySelectorAll('button').forEach(t => t.classList.toggle('on', !!on && t.dataset.tab === on.dataset.tab));
  };
  if (nav) new MutationObserver(syncTabs).observe(nav, { subtree: true, attributes: true, attributeFilter: ['class'] });
  syncTabs();

  // ---------- the large title over the Library, collapsing on scroll ----------
  const grid = $('grid');
  const lib = $('tab-library');
  if (grid && lib) {
    const title = make('h1', 'photos-title', 'Library');
    lib.insertBefore(title, lib.firstChild);
    const edge = make('div', 'scroll-edge');
    grid.parentNode.insertBefore(edge, grid);
    let ticking = false;
    const onScroll = () => {
      if (ticking) return;
      ticking = true;
      requestAnimationFrame(() => {
        title.classList.toggle('small', (window.scrollY || document.documentElement.scrollTop || 0) > 40);
        ticking = false;
      });
    };
    addEventListener('scroll', onScroll, { passive: true });
    onScroll();
  }

  // ---------- the viewer: chrome floats over the media and hides on tap ----------
  const detail = $('detail');
  if (detail) {
    const chrome = make('div', 'viewer-chrome');
    const close = make('button', 'vc-btn vc-close');
    close.title = 'Close';
    close.style.setProperty('--icon', "url(\"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' stroke='black' stroke-width='2.4' stroke-linecap='round'><path d='M6 6l12 12M18 6L6 18'/></svg>\")");
    close.onclick = e => { e.stopPropagation(); detail.classList.add('hidden'); };
    const title = make('div', 'vc-title');
    chrome.append(close, title);
    detail.append(chrome);
    // hide/show the chrome when the picture itself is tapped (never when a control is tapped)
    detail.addEventListener('click', e => {
      if (e.target.closest('button, a, input, select, video, .vc-btn')) return;
      chrome.classList.toggle('hidden-chrome');
    });
    // the item's name, from whatever the desktop layer puts in the detail header
    new MutationObserver(() => {
      const h = detail.querySelector('.big') || detail;
      const src = detail.querySelector('.info h2, .detail-head, h2, .name');
      title.textContent = src ? src.textContent.trim() : '';
    }).observe(detail, { subtree: true, childList: true, characterData: true });
  }
})();
