// The Library's own tools, as the phone has them: filters (type, NSFW up to, stars, smart albums,
// collections, card size), saved filters, the tags panel, a fast-scroll handle with the month, quick
// actions on a card (hold ✕ or press and hold), selecting many, and editing in the full view's
// details (stars, the level dial, category, tags, delete). Edits are written to index.csv whole (after
// the day's backup), a moment after the last one. Compiled inside main.c (#include "lib.c").

static const char *KIND4[] = {"Everything", "Pictures", "GIFs", "Videos"};
static const char *ALBUMS[] = {"All", "Never opened", "Forgotten", "Long videos", "Most loved", "1 star", "Continue watching"};
#define NALBUMS 7
static int resume_of(const LvItem *it); // (show.c)
static const char *SIZES[] = {"Small", "Medium", "Large"};
static const int SIZE_H[] = {120, 168, 220};
static struct { int nsfw_max, stars_min, album, size; char coll[64]; } lf = {0, 0, 0, 1, ""};
static LvSearch lsearch;
static char forgotten_key[16]; // ("forgotten": last seen before this, yyyymmdd...)
static unsigned edits_at;       // (an edit waiting to be written: when the last one was)

static int comic_n = -1; // (the comics' flags are worked out again: an edit can make a page a comic's)
static void side_set(int open);
static void edited(void) { edits_at = frame_no ? frame_no : 1; comic_n = -1; }
// (stars and levels can't make a page a comic's or not: the comics aren't worked out again for them)
static void edited_plain(void) { edits_at = frame_no ? frame_no : 1; }
static void save_edits_now(void) {
  if (!vault.index_dirty) return;
  if (index_save_async() != 0) { sfx(SFX_ERROR); say("The changes couldn't be written to the card"); }
  edits_at = 0;
}

// ---------- which items show ----------
static int lib_pass(int i) {
  const LvHot *h = &vault.hot[i];
  if (cfg.lmode == 3) {} // (Comics: pages are pictures whatever the type filter says)
  else if (cfg.kind == 1 && h->kind != LV_IMAGE) return 0;
  if (cfg.kind == 2 && h->kind != LV_GIF) return 0;
  if (cfg.kind == 3 && h->kind != LV_VIDEO) return 0;
  const LvItem *it = &vault.items[i];
  if (lf.nsfw_max && it->nsfw > lf.nsfw_max) return 0;
  if (lf.stars_min && it->quality < lf.stars_min) return 0;
  if (lf.coll[0] && strcmp(it->collection, lf.coll)) return 0;
  switch (lf.album) {
    case 1: if (lv_views_of(&vault, it) > 0) return 0; break;
    case 2: {
      if (lv_views_of(&vault, it) == 0 || it->view < 0) return 0;
      const char *last = vault.views[it->view].last;
      if (!last[0] || strncmp(last, forgotten_key, 10) >= 0) return 0;
      break;
    }
    case 3: if (h->kind != LV_VIDEO || it->duration < 300) return 0; break;
    case 4: if (lv_likes_of(&vault, it) < 2) return 0; break;
    case 5: if (it->quality != 1) return 0; break;
    case 6: if (resume_of(it) <= 0) return 0; break;
  }
  if (lsearch.n && !lv_search_match(&lsearch, it)) return 0;
  return 1;
}
static void lib_prepare(void) {
  lv_search_compile(query, &lsearch);
  time_t t = time(NULL) - 60 * 86400;
  struct tm g;
  gmtime_r(&t, &g);
  strftime(forgotten_key, sizeof forgotten_key, "%Y-%m-%d", &g);
  ROW_H = SIZE_H[lf.size < 0 || lf.size > 2 ? 1 : lf.size];
}

// The cards made a new size: only laid out again, not filtered and sorted again (the same ones)
static void relayout(void) {
  ROW_H = SIZE_H[lf.size < 0 || lf.size > 2 ? 1 : lf.size];
  if (book_shelf && ROW_H < 220) ROW_H = 220;
  grid_build(&lgrid, nshown, lib_aspect);
  lhero_offset(); // (the row's the same: only the grid's place under it)
}

// ---------- the modes: everything, pictures, videos, comics ----------
// A comic is a collection whose pages are marked as one: the category Comics (or Manga, Doujinshi,
// Webtoon, Manhwa), or one of those words as a tag. Pages saved from the Manga tab are made that way.
// Pictures leaves comics' pages out; Comics shows one card per book (its first page, its pages
// counted), and a book opens at its first page, or where it was left.
static const char *LMODES[] = {"All", "Pictures", "Videos", "Comics"};
static const char *LMODE_ICONS[] = {"all", "image", "film", "book"};
static const char *COMIC_WORDS[] = {"comic", "comics", "manga", "doujinshi", "doujin", "webtoon", "manhwa", "manhua", NULL};
static unsigned char *comic_flag; // per item: 1 a comic's page, 2 its book's cover (its first page)
static int *cover_pages;          // per item (a cover): its book's pages
static void comics_covers(void);
static int comic_vgen;
static int comic_word(const char *w, size_t n) {
  for (int k = 0; COMIC_WORDS[k]; k++)
    if (strlen(COMIC_WORDS[k]) == n && !strncasecmp(w, COMIC_WORDS[k], n)) return 1;
  return 0;
}
// (any word of s, split at anything not a letter or digit)
static int has_comic_word(const char *s) {
  if (!s) return 0;
  while (*s) {
    while (*s && !isalnum((unsigned char)*s)) s++;
    const char *b = s;
    while (*s && isalnum((unsigned char)*s)) s++;
    if (s > b && comic_word(b, (size_t)(s - b))) return 1;
  }
  return 0;
}
static void comics_scan(void) {
  if (comic_n == vault.n && comic_vgen == vgen && comic_flag) return;
  free(comic_flag);
  comic_flag = calloc(vault.n + 1, 1);
  comic_n = vault.n; comic_vgen = vgen;
  if (!comic_flag) return;
  for (int i = 0; i < vault.n; i++) {
    const LvItem *it = &vault.items[i];
    if (!it->collection || !it->collection[0] || vault.hot[i].kind == LV_VIDEO) continue;
    comic_flag[i] = has_comic_word(it->category) || has_comic_word(it->tags);
  }
  comics_covers(); // (once a change of the vault, not each filter)
}
static int is_comic(int i) { return comic_flag && i < comic_n && comic_flag[i]; }
// The slideshow's own list: what the Pictures tab has (pictures and GIFs, no comic pages) and clips of
// half a minute or less; not longer videos, not books. On while the Slideshow page or a slideshow is up.
#define SHOW_CLIP_SECS 30
static int mode_pass(int i) {
  if (show_pool) {
    if (is_comic(i)) return 0;
    if (vault.hot[i].kind != LV_VIDEO) return 1;
    double d = vault.items[i].duration;
    return d > 0 && d <= SHOW_CLIP_SECS;
  }
  switch (cfg.lmode) {
    case 0: return !is_comic(i) || comic_flag[i] == 2; // (All: a book once, as its cover; its pages in Comics)
    case 1: return vault.hot[i].kind != LV_VIDEO && !is_comic(i);
    case 2: return vault.hot[i].kind == LV_VIDEO;
    case 3: return is_comic(i);
  }
  return 1;
}
// "page 2" before "page 10": runs of digits compared as numbers
static int natcmp(const char *a, const char *b) {
  while (*a && *b) {
    if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
      while (*a == '0') a++;
      while (*b == '0') b++;
      const char *x = a, *y = b;
      while (isdigit((unsigned char)*x)) x++;
      while (isdigit((unsigned char)*y)) y++;
      if (x - a != y - b) return (int)((x - a) - (y - b));
      int c = strncmp(a, b, (size_t)(x - a));
      if (c) return c;
      a = x; b = y;
      continue;
    }
    int c = tolower((unsigned char)*a) - tolower((unsigned char)*b);
    if (c) return c;
    a++; b++;
  }
  return (unsigned char)*a - (unsigned char)*b;
}
static const char *page_key(const LvItem *it) { return it->name && it->name[0] ? it->name : it->file; }
static int cmp_pages(const void *x, const void *y) {
  const LvItem *a = &vault.items[*(const int *)x], *b = &vault.items[*(const int *)y];
  int c = strcmp(a->collection, b->collection);
  if (c) return c;
  c = natcmp(page_key(a), page_key(b));
  return c ? c : *(const int *)x - *(const int *)y;
}
// Each book's cover: its first page, as the pages are ordered (cmp_pages), marked 2 in comic_flag
static void comics_covers(void) {
  free(cover_pages);
  cover_pages = calloc(vault.n + 1, sizeof *cover_pages);
  int *idx = malloc((vault.n + 1) * sizeof *idx), m = 0;
  if (!idx || !cover_pages) { free(idx); return; }
  for (int i = 0; i < vault.n; i++) if (comic_flag[i]) idx[m++] = i;
  qsort(idx, m, sizeof *idx, cmp_pages);
  for (int k = 0; k < m; ) {
    int e = k + 1;
    while (e < m && !strcmp(vault.items[idx[e]].collection, vault.items[idx[k]].collection)) e++;
    comic_flag[idx[k]] = 2;
    cover_pages[idx[k]] = e - k;
    k = e;
  }
  free(idx);
}
static int is_cover_in_all(int i) { return cfg.lmode == 0 && comic_flag && i < comic_n && comic_flag[i] == 2; }
static int *book_count; // per item (a book's first page): its pages
static int *book_read;  // per item (a book's first page): the page last looked at, 1.. (0: not opened)
static int nbook_count;
static unsigned char *book_new; // per item (a book's first page): pages added in the last three days that come after where it was read
// The Comics mode's list: the books (one card each), or the pages of the book open. 0: not that mode.
static int lib_books(void) {
  comics_scan();
  book_shelf = book_inside = 0;
  if (cfg.lmode != 3 || show_pool) return 0;
  int m = 0;
  for (int i = 0; i < vault.n; i++) {
    if (!is_comic(i)) continue;
    if (cat >= 0 && vault.hot[i].cat != cat) continue;
    if (!lib_pass(i)) continue;
    shown[m++] = i;
  }
  qsort(shown, m, sizeof *shown, cmp_pages);
  if (lf.coll[0]) { nshown = m; book_inside = 1; return 1; } // (a book: its pages, first to last)
  if (nbook_count < vault.n + 1) {
    int *nb = realloc(book_count, (vault.n + 1) * sizeof *nb);
    if (!nb) { nshown = m; return 1; }
    book_count = nb;
    int *nr = realloc(book_read, (vault.n + 1) * sizeof *nr);
    if (!nr) { nshown = m; return 1; }
    book_read = nr;
    unsigned char *nn = realloc(book_new, vault.n + 1);
    if (!nn) { nshown = m; return 1; }
    book_new = nn; nbook_count = vault.n + 1;
  }
  nshown = 0;
  for (int k = 0; k < m; ) {
    int e = k + 1;
    while (e < m && !strcmp(vault.items[shown[e]].collection, vault.items[shown[k]].collection)) e++;
    book_count[shown[k]] = e - k;
    // (how far it's been read: the page looked at last)
    const char *best = "";
    int at = 0;
    for (int j = k; j < e; j++) {
      const LvItem *p = &vault.items[shown[j]];
      if (p->view >= 0 && vault.views[p->view].last && strcmp(vault.views[p->view].last, best) > 0) { best = vault.views[p->view].last; at = j - k + 1; }
    }
    book_read[shown[k]] = at;
    // (a new chapter: a page added in the last three days, past the page read)
    { char since[24]; time_t t3 = time(NULL) - 3 * 86400; struct tm l; lv_local_tm(t3, &l); strftime(since, sizeof since, "%Y-%m-%d", &l);
      int nw = 0; for (int j = k + (at > 0 ? at : 0); j < e && !nw; j++) if (strcmp(vault.items[shown[j]].added, since) >= 0) nw = 1;
      book_new[shown[k]] = (unsigned char)nw; }
    shown[nshown++] = shown[k];
    k = e;
  }
  book_shelf = 1;
  if (ROW_H < 220) ROW_H = 220; // (covers big enough to tell apart)
  sort_mode = cfg.sort == 5 || cfg.sort == 6 ? 0 : cfg.sort;
  qsort(shown, nshown, sizeof *shown, cmp_items);
  return 1;
}
// Opening a book from the shelf: its pages, at the one after the page last looked at (unless that was
// the last).
static int shelf_sel; static float shelf_scroll;
static void open_book(int k) {
  const LvItem *it = &vault.items[shown[k]];
  shelf_sel = k; shelf_scroll = scroll;
  snprintf(lf.coll, sizeof lf.coll, "%s", it->collection);
  refilter();
  int at = 0;
  const char *best = "";
  for (int j = 0; j < nshown; j++) {
    const LvItem *p = &vault.items[shown[j]];
    if (p->view >= 0 && vault.views[p->view].last && strcmp(vault.views[p->view].last, best) > 0) { best = vault.views[p->view].last; at = j; }
  }
  if (best[0] && at > 0 && at < nshown - 1) {
    char m[64];
    snprintf(m, sizeof m, "Carrying on at page %d of %d", at + 1, nshown);
    say(m);
  } else at = 0;
  sel = at; scroll = 0;
  if (nshown) open_viewer(at, 0);
}
// A book opened from All: read as from the Comics shelf, and back to All (at its card) after
static int book_from_all, from_all_item;
static float from_all_scroll;
static void open_book_from_all(int item) {
  from_all_item = item; from_all_scroll = scroll;
  cfg.lmode = 3; lf.coll[0] = 0;
  refilter();
  for (int k = 0; k < nshown; k++)
    if (shown[k] == item) { book_from_all = 1; open_book(k); return; }
  // (not on the shelf, filtered out there: just its cover)
  cfg.lmode = 0; refilter();
  for (int k = 0; k < nshown; k++) if (shown[k] == item) { open_viewer(k, 0); return; }
}
static void book_back_to_all(void) {
  if (!book_from_all) return;
  book_from_all = 0;
  lf.coll[0] = 0; cfg.lmode = 0;
  refilter();
  sel = 0;
  for (int k = 0; k < nshown; k++) if (shown[k] == from_all_item) { sel = k; break; }
  scroll = from_all_scroll;
}
// A card opened: a book on the shelf or in All, else the full view
static void lib_open(int k) {
  if (k < 0 || k >= nshown) return;
  if (book_shelf) { sfx(SFX_OPEN); open_book(k); }
  else if (is_cover_in_all(shown[k])) { sfx(SFX_OPEN); open_book_from_all(shown[k]); }
  else { sfx(SFX_SELECT); open_viewer(k, 0); }
}
static void close_book(void) {
  lf.coll[0] = 0;
  refilter();
  sel = shelf_sel < nshown ? shelf_sel : 0;
  scroll = shelf_scroll;
}
static void set_mode(int m) {
  if (m == cfg.lmode) return;
  sfx(SFX_TAB);
  if (cfg.lmode == 3 && book_inside) lf.coll[0] = 0;
  cfg.lmode = m;
  cfg_save();
  refilter();
  sel = 0; scroll = 0; scroll_v = 0;
}
// The modes, as one control: an icon and a word each; returns its width.
static int draw_modes(int x, int y) {
  int x0 = x;
  int ws[4], tot = 0;
  for (int k = 0; k < 4; k++) { ws[k] = text_w(15, LMODES[k]) + 46; tot += ws[k]; }
  rrect(x, y, tot + 8, 40, 20, SURF2);
  x += 4;
  { // (the glass under the chosen mode slides over to a new one, its width following)
    static float gx = -1, gw = 0;
    float tx = x, tw2 = ws[cfg.lmode < 0 || cfg.lmode > 3 ? 0 : cfg.lmode];
    for (int k = 0; k < cfg.lmode && k < 4; k++) tx += ws[k];
    if (gx < 0) { gx = tx; gw = tw2; }
    gx += (tx - gx) * ease(0.25f); gw += (tw2 - gw) * ease(0.25f);
    glass(gx, y + 4, gw, 32, 0);
  }
  for (int k = 0; k < 4; k++) {
    int on = k == cfg.lmode;
    unsigned col = on ? TEXT : DIM;
    icon(LMODE_ICONS[k], x + 20, y + 20, col);
    text(x + 34, y + 26, col, 15, LMODES[k]);
    if (hit(x, y, ws[k], 40)) set_mode(k);
    x += ws[k];
  }
  return x + 4 - x0;
}

// "For you": what you come back to and like, the stars, a little of what's new, and some chance
static float *foryou;
static int cmp_foryou(const void *a, const void *b) {
  float x = foryou[*(const int *)a], y = foryou[*(const int *)b];
  return x < y ? 1 : x > y ? -1 : 0;
}
static void sort_foryou(void) {
  free(foryou);
  foryou = malloc((vault.n + 1) * sizeof *foryou);
  if (!foryou) return;
  for (int k = 0; k < nshown; k++) {
    int i = shown[k];
    const LvItem *it = &vault.items[i];
    float score = lv_likes_of(&vault, it) * 3.0f + logf(1 + lv_views_of(&vault, it)) * 2.0f + it->quality * 1.5f;
    score += (float)(vault.hot[i].added / 1000000 % 100000) * 0.00002f; // (a little for what's newer)
    score += (rnd() % 1000) / 250.0f;                                  // (and some chance)
    foryou[i] = score;
  }
  qsort(shown, nshown, sizeof *shown, cmp_foryou);
}

// ---------- saved filters (ux0:data/LWDV00001/filters.txt) ----------
typedef struct { char name[40], query[128], coll[64]; int kind, nsfw, stars, album; } Saved;
static Saved saved[16];
static int nsaved_f, saved_loaded;
static void saved_load(void) {
  saved_loaded = 1;
  FILE *f = fopen(DATA "/filters.txt", "r");
  if (!f) return;
  char line[400];
  while (nsaved_f < 16 && fgets(line, sizeof line, f)) {
    line[strcspn(line, "\r\n")] = 0;
    Saved s;
    memset(&s, 0, sizeof s);
    char *p[8] = {0};
    int n = 0;
    for (char *t = line; n < 8; ) { p[n++] = t; char *tab = strchr(t, '\t'); if (!tab) break; *tab = 0; t = tab + 1; }
    if (n < 7) continue;
    snprintf(s.name, sizeof s.name, "%.39s", p[0]);
    snprintf(s.query, sizeof s.query, "%s", p[1]);
    s.kind = atoi(p[2]); s.nsfw = atoi(p[3]); s.stars = atoi(p[4]); s.album = atoi(p[5]); if (s.album < 0 || s.album >= NALBUMS) s.album = 0;
    snprintf(s.coll, sizeof s.coll, "%s", p[6]);
    saved[nsaved_f++] = s;
  }
  fclose(f);
}
static void saved_write(void) {
  FILE *f = fopen(DATA "/filters.txt.part", "w");
  if (!f) return;
  for (int k = 0; k < nsaved_f; k++)
    fprintf(f, "%s\t%s\t%d\t%d\t%d\t%d\t%s\n", saved[k].name, saved[k].query, saved[k].kind, saved[k].nsfw, saved[k].stars, saved[k].album, saved[k].coll);
  fclose(f);
  sceIoRemove(DATA "/filters.txt");
  sceIoRename(DATA "/filters.txt.part", DATA "/filters.txt");
}
static void saved_add_current(const char *name) {
  if (nsaved_f >= 16) { sfx(SFX_ERROR); say("Sixteen saved filters at most"); return; }
  Saved s;
  memset(&s, 0, sizeof s);
  snprintf(s.name, sizeof s.name, "%.39s", name);
  snprintf(s.query, sizeof s.query, "%s", query);
  snprintf(s.coll, sizeof s.coll, "%s", lf.coll);
  s.kind = cfg.kind; s.nsfw = lf.nsfw_max; s.stars = lf.stars_min; s.album = lf.album;
  for (char *c = s.name; *c; c++) if (*c == '\t') *c = ' ';
  saved[nsaved_f++] = s;
  saved_write();
  sfx(SFX_SAVED);
  say("Filters saved");
}
static void saved_apply(int k) {
  Saved *s = &saved[k];
  snprintf(query, sizeof query, "%s", s->query);
  snprintf(lf.coll, sizeof lf.coll, "%s", s->coll);
  cfg.kind = s->kind; lf.nsfw_max = s->nsfw; lf.stars_min = s->stars; lf.album = s->album;
}

// ---------- the sheets ----------
enum { LS_NONE, LS_FILTERS, LS_TAGS, LS_QUICK, LS_BATCH, LS_CATS, LS_SORT };
static int lsheet, lsheet_sel, quick_item = -1, delete_armed;
static unsigned delete_armed_at;
static unsigned char *picked; // per item: chosen (selecting many)
static int npicked, selecting;
static int shown_custom; // (the list is a chosen few, for a slideshow: refiltered once it's done)
// Smart collections: lists that keep themselves up to date, first in the categories' list
static const char *const SMART[] = {"Unseen", "Hearted this month", "Most viewed", "Short clips", "New this week"};
#define NSMART 5
static long long smart_days_ago(int days) { // (yyyymmddhhmmss of that many days ago, as hot.added has it)
  time_t t = time(NULL) - (time_t)days * 86400;
  struct tm g;
  gmtime_r(&t, &g);
  return (long long)(g.tm_year + 1900) * 10000000000LL + (long long)(g.tm_mon + 1) * 100000000LL + g.tm_mday * 1000000LL + g.tm_hour * 10000 + g.tm_min * 100 + g.tm_sec;
}
static int smart_pass(int i) {
  LvItem *it = &vault.items[i];
  switch (smart) {
    case 1: return lv_views_of(&vault, it) == 0;
    case 2: {
      if (it->view < 0 || it->view >= vault.nviews || !vault.views[it->view].last_like || !vault.views[it->view].last_like[0]) return 0;
      char when[40];
      time_t t = time(NULL) - 30 * 86400;
      struct tm g;
      gmtime_r(&t, &g);
      strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", &g);
      return strcmp(vault.views[it->view].last_like, when) >= 0;
    }
    case 3: return lv_views_of(&vault, it) >= 3;
    case 4: return vault.hot[i].kind == LV_GIF || (vault.hot[i].kind == LV_VIDEO && it->duration > 0 && it->duration <= 30);
    case 5: return vault.hot[i].added >= smart_days_ago(7);
  }
  return 1;
}
static int cmp_views_desc(const void *a, const void *b) {
  long long x = lv_views_of(&vault, &vault.items[*(const int *)a]), y = lv_views_of(&vault, &vault.items[*(const int *)b]);
  return x < y ? 1 : x > y ? -1 : 0;
}
// The categories: a list opening under their button, big chips that wrap (✕ or a tap picks, ○ closes);
// the smart collections first
static void draw_cats_sheet(void) {
  int n = vault.ncats + 1 + NSMART;
  int x0 = cats_x > W - 520 ? W - 520 : cats_x, y0 = TOP_FULL + 2, pw = 510;
  // (the chips laid out first, for the panel's height)
  int xs[65], ys[65], ws[65], x = 12, y = 12, rows = 1;
  for (int k = 0; k < n && k < 65; k++) {
    const char *nm = k < NSMART ? SMART[k] : k == NSMART ? "All categories" : vault.cats[k - 1 - NSMART];
    int w = text_w(17, nm) + 40 + (k < NSMART ? 16 : 0); // (a smart one: its icon too)
    if (w > pw - 24) w = pw - 24;
    if (x + w > pw - 12) { x = 12; y += 52; rows++; }
    xs[k] = x; ys[k] = y; ws[k] = w;
    x += w + 8;
  }
  int ph = y + 56;
  if (ph > H - y0 - 20) ph = H - y0 - 20;
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, 120));
  rrect(x0, y0, pw, ph, 22, SURF);
  if (lsheet_sel >= n) lsheet_sel = n - 1;
  for (int k = 0; k < n && k < 65; k++) {
    if (ys[k] + 48 > ph) break;
    const char *nm = k < NSMART ? SMART[k] : k == NSMART ? "All categories" : vault.cats[k - 1 - NSMART];
    int on = k < NSMART ? smart == k + 1 : !smart && k == cat + 1 + NSMART, cur = k == lsheet_sel;
    int cx = x0 + xs[k], cy = y0 + ys[k];
    if (cur) rrect(cx - 3, cy - 3, ws[k] + 6, 50, 25, with_alpha(TEXT, 60));
    if (on) glass(cx, cy, ws[k], 44, 0); else rrect(cx, cy, ws[k], 44, 22, k < NSMART ? mix_col(with_alpha(SURF2, 255), ACC, 0.12f) : SURF2);
    if (k < NSMART) icon("sparkle", cx + 18, cy + 22, on ? TEXT : ACC);
    text_fit(cx + (k < NSMART ? 34 : 20), cy + 29, TEXT, 17, nm, ws[k] - (k < NSMART ? 46 : 32));
    if (hit(cx, cy, ws[k], 44)) { lsheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  if (in.pressed & SCE_CTRL_RIGHT) { lsheet_sel = (lsheet_sel + 1) % n; sfx(SFX_TICK); }
  if (in.pressed & SCE_CTRL_LEFT) { lsheet_sel = (lsheet_sel + n - 1) % n; sfx(SFX_TICK); }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) {
    // (the nearest chip in the row below or above)
    int k0 = lsheet_sel < 65 ? lsheet_sel : 0, best = k0, bd = 1 << 30;
    int dir = (in.pressed & SCE_CTRL_DOWN) ? 1 : -1;
    for (int k = 0; k < n && k < 65; k++) {
      if ((ys[k] - ys[k0]) * dir <= 0) continue;
      int d = abs(ys[k] - ys[k0]) * 4 + abs(xs[k] + ws[k] / 2 - xs[k0] - ws[k0] / 2);
      if (d < bd) { bd = d; best = k; }
    }
    lsheet_sel = best; sfx(SFX_TICK);
  }
  if (in.pressed & SCE_CTRL_CROSS) {
    sfx(SFX_TAB);
    if (lsheet_sel < NSMART) { smart = lsheet_sel + 1; cat = -1; }
    else { smart = 0; cat = lsheet_sel - 1 - NSMART; }
    refilter(); sel = 0; scroll = 0; lsheet = LS_NONE;
  }
  else if ((in.pressed & SCE_CTRL_CIRCLE) || (in.tap && !(in.tap_x >= x0 && in.tap_x < x0 + pw && in.tap_y >= y0 && in.tap_y < y0 + ph))) { sfx(SFX_CLOSE); lsheet = LS_NONE; }
  in.pressed = 0; in.tap = 0;
}


static void lib_after_change(void) { refilter(); if (sel >= nshown) sel = nshown ? nshown - 1 : 0; }

// A sheet's backdrop and body. Opening (not drawn the frame before): the dim comes in, and an
// outline springs out from it and settles, a little past and back, as it lands.
static unsigned sheet_clip_at; // (a sheet opening: what's drawn after it is cut at the edge it has reached)
static void sheet_cover_draw(void) { if (sheet_clip_at == frame_no) vita2d_disable_clipping(); } // (main loop, after the screen)
// (a sheet at the screen's side opening: how far off to the right it still is, sliding in, easing)
static int sheet_off(void) {
  static unsigned last, opened;
  if (frame_no - last > 2) opened = frame_no;
  last = frame_no;
  float u = fminf(1, (frame_no - opened) / 13.0f), e = (1 - u) * (1 - u) * (1 - u);
  return (int)(e * 420);
}
static void sheet_frame(int x, int y, int w, int h) {
  static unsigned last, opened;
  if (frame_no - last > 2) opened = frame_no;
  last = frame_no;
  unsigned age = frame_no - opened;
  float p = fminf(1, age / 14.0f);
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, (int)(150 * fminf(1, age / 6.0f))));
  // (opening: it unrolls from its left edge; what is past the edge reached is not drawn yet)
  float e = 1 - (1 - p) * (1 - p) * (1 - p);
  (void)e; // (it slides in from the side now: sheet_off)
  rrect(x, y, w, h, 16, SURF);
}
static int sheet_row(int k, int x, int y, int w, int rh, const char *name, const char *value) {
  if (k == lsheet_sel) rrect(x + 6, y, w - 12, rh - 4, 10, SURF2);
  text(x + 20, y + rh / 2 + 6, TEXT, 17, name);
  if (value && value[0]) text(x + w - 22 - text_w(16, value), y + rh / 2 + 6, ACC, 16, value);
  if (hit(x, y, w, rh)) { lsheet_sel = k; in.pressed |= SCE_CTRL_CROSS; return 1; }
  return 0;
}
// A row with its icon: a small tinted tile at the left, the name after it, the value at the right
static int sheet_row_i(int k, int x, int y, int w, int rh, const char *ic, const char *name, const char *value) {
  int on = k == lsheet_sel;
  if (on) rrect(x + 6, y, w - 12, rh - 4, 12, SURF2);
  int cy = y + (rh - 4) / 2;
  rrect(x + 16, cy - 15, 30, 30, 9, with_alpha(ACC, on ? 255 : 45));
  icon(ic, x + 31, cy, on ? RGBA(20, 12, 16, 255) : ACC);
  text(x + 58, cy + 6, TEXT, 17, name);
  if (value && value[0]) text_fit(x + w - 22 - (text_w(16, value) < w / 2 ? text_w(16, value) : w / 2), cy + 6, ACC, 16, value, w / 2);
  if (on) focus_ring(x + 6, y, w - 12, rh - 4);
  if (hit(x, y, w, rh)) { lsheet_sel = k; in.pressed |= SCE_CTRL_CROSS; return 1; }
  return 0;
}
static int sheet_nav(int n) {
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) lsheet_sel = (lsheet_sel + 1) % n;
  if (in.pressed & SCE_CTRL_UP) lsheet_sel = (lsheet_sel + n - 1) % n;
  return (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : (in.pressed & SCE_CTRL_CROSS) ? 1 : 0;
}
static void sheet_close(void) { sfx(SFX_CLOSE); lsheet = LS_NONE; save_edits_now(); }
// The order, as a card (the dating apps' look): at the top the first of the Library in that order,
// big and rounded with its name over it; under it the orders as round coloured buttons with their
// names, four to a row. ←→↑↓ move, ✕ or a tap picks, ○ or a tap outside closes.
static void draw_sort_sheet(void) {
  static const char *const SORT_ICONS[NSORTS] = {"newest", "oldest", "eye", "star", "heart", "shuffle", "sparkle", "eye_off", "history", "hourglass", "stack"}; // (one per order: a missing one was a crash)
  static const unsigned SORT_COL[NSORTS] = {RGBA(240, 84, 84, 255), RGBA(150, 96, 240, 255), RGBA(80, 196, 110, 255), RGBA(244, 170, 60, 255), RGBA(240, 100, 170, 255),
                                            RGBA(70, 150, 232, 255), RGBA(60, 196, 196, 255), RGBA(150, 150, 160, 255), RGBA(206, 150, 96, 255), RGBA(120, 128, 240, 255), RGBA(232, 120, 90, 255)};
  const int w = 372, h = H - 20, x = W - w - 12 + sheet_off(), y = 10, cols = 4;
  static unsigned opened_at; static int was;
  if (!was) opened_at = frame_no; was = 1;
  float op = fminf(1, (frame_no - opened_at) / 14.0f);
  sheet_frame(x, y, w, h);
  // the head: a round badge, two lines (as the reference's greeting)
  circle(x + 34, y + 32, 18, with_alpha(ACC, 60));
  icon("sort", x + 34, y + 32, ACC);
  text(x + 60, y + 26, FAINT, 12, "Sort your Library");
  text(x + 60, y + 45, TEXT, 17, "Order");
  // the card: the first one in the order now
  const int cx = x + 14, cy = y + 62, cw = w - 28, ch = 176;
  int sel_k = lsheet_sel >= 0 && lsheet_sel < NSORTS ? lsheet_sel : cfg.sort;
  vita2d_texture *t = nshown ? thumb_of(shown[0]) : NULL;
  rrect_round(cx, cy + 4, cw, ch, 22, RGBA(0, 0, 0, 90));
  if (t) draw_cover_round(t, cx, cy, cw, ch, 22, RGBA(255, 255, 255, 255));
  else rrect_round(cx, cy, cw, ch, 22, mix_col(SORT_COL[sel_k], RGBA(0, 0, 0, 255), 0.55f));
  grad(cx, cy + ch * 0.4f, cw, ch * 0.6f, RGBA(0, 0, 0, 255), 0, 210, 0);
  { char b[32]; snprintf(b, sizeof b, "%d in the Library", nshown); int bw = text_w(12, b) + 20;
    rrect_round(cx + 12, cy + 12, bw, 24, 12, RGBA(0, 0, 0, 150)); text(cx + 22, cy + 29, TEXT, 12, b); }
  text_fit(cx + 18, cy + ch - 34, TEXT, 24, SORTS[sel_k], cw - 40);
  circle(cx + 18 + text_w(24, SORTS[sel_k]) + 12 > cx + cw - 30 ? cx + cw - 30 : cx + 18 + text_w(24, SORTS[sel_k]) + 12, cy + ch - 42, 5, sel_k == cfg.sort ? RGBA(90, 214, 120, 255) : FAINT);
  text(cx + 18, cy + ch - 14, DIM, 13, sel_k == cfg.sort ? "In use now" : "✕ to sort this way");
  // the orders: round buttons
  const float gw = (w - 28) / (float)cols, gy0 = cy + ch + 22, rhg = 78;
  for (int k = 0; k < NSORTS; k++) {
    float bx = x + 14 + (k % cols) * gw + gw / 2, by = gy0 + (k / cols) * rhg + 24;
    // (they come in one after another as it opens)
    float e = fminf(1, fmaxf(0, op * 1.6f - k * 0.05f)); e = 1 - (1 - e) * (1 - e);
    by += (1 - e) * 18;
    int on = k == lsheet_sel, cur = k == cfg.sort;
    float r = (on ? 25 : 21) * (0.6f + 0.4f * e);
    circle(bx, by + 3, r, RGBA(0, 0, 0, (int)(90 * e)));
    circle(bx, by, r, with_alpha(SORT_COL[k], (int)(255 * e)));
    circle(bx, by - r * 0.32f, r * 0.6f, RGBA(255, 255, 255, (int)(40 * e))); // (glass light on top)
    icon(SORT_ICONS[k], bx, by, RGBA(255, 255, 255, (int)(255 * e)));
    if (on) { ring9(bx - r - 5, by - r - 5, 2 * r + 10, 2 * r + 10, with_alpha(TEXT, 220)); }
    if (cur) { circle(bx + r * 0.72f, by - r * 0.72f, 8, RGBA(90, 214, 120, 255)); icon("check", bx + r * 0.72f, by - r * 0.72f, RGBA(20, 12, 16, 255)); }
    text_fit((int)(bx - gw / 2 + 4 + fmaxf(0, (gw - 8 - text_w(11, SORTS[k])) / 2)), (int)(by + r + 18), on ? TEXT : DIM, 11, SORTS[k], (int)gw - 8);
    if (hit((int)(bx - gw / 2), (int)(by - 28), (int)gw, (int)rhg)) { lsheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
  }
  // (the D-pad: across the row, and between rows)
  int nsel = lsheet_sel < 0 ? cfg.sort : lsheet_sel;
  if (in.pressed & SCE_CTRL_RIGHT) nsel++;
  if (in.pressed & SCE_CTRL_LEFT) nsel--;
  if (in.pressed & SCE_CTRL_DOWN) nsel += cols;
  if (in.pressed & SCE_CTRL_UP) nsel -= cols;
  if (nsel < 0) nsel = 0;
  if (nsel >= NSORTS) nsel = NSORTS - 1;
  if (nsel != lsheet_sel && (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_LEFT | SCE_CTRL_DOWN | SCE_CTRL_UP))) { lsheet_sel = nsel; sfx(SFX_TICK); }
  if ((in.pressed & SCE_CTRL_CROSS) && lsheet_sel >= 0 && lsheet_sel < NSORTS) {
    sfx(SFX_TOGGLE);
    cfg.sort = lsheet_sel; refilter(); cfg_save(); sel = 0; scroll = 0;
    lsheet = LS_NONE; was = 0;
  } else if ((in.pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_TRIANGLE)) || (in.tap && !(in.tap_x >= x && in.tap_y >= y && in.tap_y < y + h))) { sheet_close(); was = 0; }
}

// the vault's collections, for cycling through
static int coll_next(int d) {
  // (a small list made each time: collections are few)
  const char *names[64];
  int n = 0;
  for (int i = 0; i < vault.n && n < 64; i++) {
    const char *c = vault.items[i].collection;
    if (!c[0]) continue;
    int seen = 0;
    for (int k = 0; k < n; k++) if (!strcmp(names[k], c)) { seen = 1; break; }
    if (!seen) names[n++] = c;
  }
  int cur = -1;
  for (int k = 0; k < n; k++) if (!strcmp(names[k], lf.coll)) cur = k;
  int nx = cur + d;
  if (nx < -1) nx = n - 1;
  if (nx >= n) nx = -1;
  if (nx < 0) lf.coll[0] = 0; else snprintf(lf.coll, sizeof lf.coll, "%s", names[nx]);
  return n;
}

static void draw_filters_sheet(void) {
  if (!saved_loaded) saved_load();
  enum { F_SEARCH, F_KIND, F_NSFW, F_STARS, F_ALBUM, F_COLL, F_SIZE, F_TAGS, F_SAVED, F_SAVE, F_RESET, F_N };
  // (rows a little taller, each with its icon; the list scrolls to keep the chosen one in sight)
  int w = 400, x = W - w - 12 + sheet_off(), y = 12, rh = 54, view = H - 24 - 16; // (narrower: no empty middle)
  static float fs_scroll;
  float want = lsheet_sel * rh;
  if (want - fs_scroll > view - rh) fs_scroll = want - (view - rh);
  if (want < fs_scroll) fs_scroll = want;
  if (fs_scroll > F_N * rh - view) fs_scroll = F_N * rh - view > 0 ? F_N * rh - view : 0;
  if (fs_scroll < 0) fs_scroll = 0;
  sheet_frame(x, y, w, view + 16);
  char v[160];
  for (int k = 0; k < F_N; k++) {
    int ry = y + 8 + k * rh - (int)fs_scroll;
    if (ry < y || ry + rh > y + 8 + view + 4) continue;
    switch (k) {
      case F_SEARCH: sheet_row_i(k, x, ry, w, rh, "search", "Search", query[0] ? query : "tags, -not, ~any, artist:*"); break;
      case F_KIND: sheet_row_i(k, x, ry, w, rh, "photos", "Show", KIND4[cfg.kind & 3]); break;
      case F_NSFW: if (lf.nsfw_max) snprintf(v, sizeof v, "up to %d", lf.nsfw_max); else snprintf(v, sizeof v, "Any"); sheet_row_i(k, x, ry, w, rh, "flame", "NSFW level", v); break;
      case F_STARS: snprintf(v, sizeof v, "%s", lf.stars_min == 0 ? "Any" : lf.stars_min == 1 ? "★ and up" : lf.stars_min == 2 ? "★★ and up" : "★★★"); sheet_row_i(k, x, ry, w, rh, "star", "Stars", v); break;
      case F_ALBUM: sheet_row_i(k, x, ry, w, rh, "album", "Album", ALBUMS[lf.album]); break;
      case F_COLL: sheet_row_i(k, x, ry, w, rh, "stack", "Collection", lf.coll[0] ? lf.coll : "All"); break;
      case F_SIZE: sheet_row_i(k, x, ry, w, rh, "grid", "Card size", SIZES[lf.size]); break;
      case F_TAGS: sheet_row_i(k, x, ry, w, rh, "tag", "Tags of these", "›"); break;
      case F_SAVED: snprintf(v, sizeof v, "%s", nsaved_f ? "‹ choose ›" : "none yet"); sheet_row_i(k, x, ry, w, rh, "bookmark", "Saved filters", v); break;
      case F_SAVE: sheet_row_i(k, x, ry, w, rh, "save", "Save these filters…", ""); break;
      case F_RESET: sheet_row_i(k, x, ry, w, rh, "eraser", "Clear all filters", ""); break;
    }
  }
  int d = sheet_nav(F_N);
  static int saved_k = -1;
  if (d) {
    int changed = 1;
    switch (lsheet_sel) {
      case F_SEARCH: changed = 0; if ((in.pressed & SCE_CTRL_CROSS) && !ime_open) ime_start(2, "Search: tags, -not, ~any, artist:*", query, 0); break;
      case F_KIND: cfg.kind = (cfg.kind + d + 4) % 4; break;
      case F_NSFW: lf.nsfw_max = (lf.nsfw_max + d + 11) % 11; break;
      case F_STARS: lf.stars_min = (lf.stars_min + d + 4) % 4; break;
      case F_ALBUM: lf.album = (lf.album + d + NALBUMS) % NALBUMS; break;
      case F_COLL: if (!coll_next(d)) { changed = 0; sfx(SFX_ERROR); say("No collections in this vault"); } break;
      case F_SIZE: lf.size = (lf.size + d + 3) % 3; break;
      case F_TAGS: changed = 0; if (in.pressed & SCE_CTRL_CROSS) { lsheet = LS_NONE; side_set(1); in.pressed = 0; } break;
      case F_SAVED:
        if (!nsaved_f) { changed = 0; break; }
        saved_k = (saved_k + d + nsaved_f + 1) % (nsaved_f + 1) - 0;
        if (saved_k >= nsaved_f) saved_k = 0;
        saved_apply(saved_k);
        { char m[80]; snprintf(m, sizeof m, "%s", saved[saved_k].name); say(m); }
        break;
      case F_SAVE: changed = 0; if ((in.pressed & SCE_CTRL_CROSS) && !ime_open) ime_start(5, "A name for these filters", "", 0); break;
      case F_RESET: cfg.kind = 0; lf.nsfw_max = lf.stars_min = lf.album = 0; lf.coll[0] = 0; query[0] = 0; cat = -1; break;
    }
    if (changed && lsheet_sel == F_SIZE) { sfx(SFX_TOGGLE); relayout(); cfg_save(); } // (a size: laid out again, the same cards)
    else if (changed) { sfx(SFX_TOGGLE); lib_after_change(); scroll = 0; sel = 0; cfg_save(); }
  }
  // (□ removes the saved filter shown)
  if (lsheet_sel == F_SAVED && (in.pressed & SCE_CTRL_SQUARE) && saved_k >= 0 && saved_k < nsaved_f) {
    memmove(&saved[saved_k], &saved[saved_k + 1], (nsaved_f - saved_k - 1) * sizeof *saved);
    nsaved_f--;
    saved_k = -1;
    saved_write();
    sfx(SFX_DELETE);
    say("Saved filter removed");
    in.pressed &= ~SCE_CTRL_SQUARE;
  }
  if ((in.pressed & (SCE_CTRL_CIRCLE | SCE_CTRL_SQUARE)) || (in.tap && in.tap_x < x)) sheet_close();
  in.pressed = 0; in.tap = 0;
}

// the tags of what's shown, most common first: ✕ adds one to the search, □ leaves it out
typedef struct { const char *t; int len, count; } TagCount;
static TagCount tcounts[48];
static int ntcounts;
static void count_tags(void) {
  enum { CAP = 4096 };
  TagCount *tab = calloc(CAP, sizeof *tab);
  ntcounts = 0;
  if (!tab) return;
  int lim = nshown < 6000 ? nshown : 6000;
  for (int k = 0; k < lim; k++) {
    const char *p = vault.items[shown[k]].tags;
    while (*p) {
      while (*p == ' ') p++;
      const char *e = p;
      while (*e && *e != ' ') e++;
      int len = (int)(e - p);
      if (len > 0) {
        unsigned long hsh = 5381;
        for (int i = 0; i < len; i++) hsh = hsh * 33 ^ (unsigned char)p[i];
        unsigned at = hsh & (CAP - 1);
        int probes = 0;
        while (tab[at].t && !(tab[at].len == len && !memcmp(tab[at].t, p, len)) && probes++ < CAP) at = (at + 1) & (CAP - 1);
        if (!tab[at].t) { tab[at].t = p; tab[at].len = len; }
        if (tab[at].len == len && !memcmp(tab[at].t, p, len)) tab[at].count++; // (a full table: not someone else's count)
      }
      p = e;
    }
  }
  for (int k = 0; k < CAP; k++) {
    if (!tab[k].t) continue;
    int at = ntcounts < 48 ? ntcounts++ : -1;
    if (at < 0) { int low = 0; for (int j = 1; j < 48; j++) if (tcounts[j].count < tcounts[low].count) low = j; if (tab[k].count <= tcounts[low].count) continue; at = low; }
    tcounts[at] = tab[k];
  }
  for (int a = 0; a < ntcounts; a++)
    for (int b = a + 1; b < ntcounts; b++)
      if (tcounts[b].count > tcounts[a].count) { TagCount t = tcounts[a]; tcounts[a] = tcounts[b]; tcounts[b] = t; }
  free(tab);
}
// ---------- the tags sidebar: the arrow at the right edge, or a swipe in from it ----------
// The tags of what's shown, most found first, in big rows: a tap adds one to the search, a swipe left
// on a row (or □) leaves it out; the search's words sit on top, a tap on one takes it away. A swipe
// right on the panel, a tap beside it, ○ or the arrow close it. The tag pointers point into the
// vault's strings: counted again whenever it opens or what's shown changes.
static float side_t, side_scroll;
static int side_open, side_sel, side_counted_n = -1;
static void side_set(int open) {
  if (open == side_open) return;
  side_open = open;
  sfx(open ? SFX_OPEN : SFX_CLOSE);
  if (open) { count_tags(); side_counted_n = nshown; side_sel = 0; side_scroll = 0; chrome_target = 0; }
}
// (before the Library takes the touch: the arrow tab, or a swipe in from the edge)
static int side_gesture(void) {
  if (lsheet != LS_NONE || side_open || selecting) return 0;
  if (hit(W - 44, H / 2 - 70, 44, 140)) { side_set(1); return 1; }
  if (in.touching && in.down_x > W - 30 && in.down_y > TOP && in.down_y < H - BAR && in.down_x - in.tx > 46 && abs(in.down_x - in.tx) > 2 * abs(in.ty - in.down_y)) { side_set(1); return 1; }
  return 0;
}
static void draw_side_tab(void) {
  if (side_t > 0.01f) return;
  // (a tab at the edge that says what it opens: the tag icon, "Tags", the arrow)
  int tx = W - 40, ty = H / 2 - 64, tw = 48, th = 128;
  glass(tx, ty, tw, th, 0);
  icon("tag", tx + 20, ty + 34, ACC);
  text_center(tx + 20, ty + 66, TEXT, 12, "Tags");
  icon("chev_l", tx + 20, ty + 96, DIM);
}
// the search's words: where the k-th starts and its length (-1: no such word)
static int query_word(int k, int *len) {
  const char *p = query;
  for (int i = 0; *p; i++) {
    while (*p == ' ') p++;
    const char *e = p;
    while (*e && *e != ' ') e++;
    if (e == p) break;
    if (i == k) { *len = (int)(e - p); return (int)(p - query); }
    p = e;
  }
  return -1;
}
static void side_changed(void) {
  lib_after_change();
  sel = 0; scroll = 0;
  count_tags(); side_counted_n = nshown; side_sel = 0; side_scroll = 0;
}
static void side_add(int k, int out) {
  if (k < 0 || k >= ntcounts) return;
  char add[100];
  snprintf(add, sizeof add, "%s%.*s", out ? "-" : "", tcounts[k].len > 90 ? 90 : tcounts[k].len, tcounts[k].t);
  size_t ql = strlen(query);
  if (ql + strlen(add) + 2 >= sizeof query) { sfx(SFX_ERROR); return; }
  if (ql) strcat(query, " ");
  strcat(query, add);
  sfx(out ? SFX_DELETE : SFX_TOGGLE);
  side_changed();
}
static void draw_side(void) {
  side_t += ((side_open ? 1.0f : 0.0f) - side_t) * ease(0.32f);
  if (fabsf(side_t - side_open) < 0.01f) side_t = (float)side_open;
  if (side_t <= 0) return;
  if (side_open && side_counted_n != nshown) { count_tags(); side_counted_n = nshown; }
  const int pw = 340;
  int x = W - (int)(pw * side_t);
  vita2d_draw_rectangle(0, 0, x, H, RGBA(0, 0, 0, (int)(120 * side_t)));
  vita2d_draw_rectangle(x, 0, pw, H, SURF);
  vita2d_draw_rectangle(x, 0, 2, H, with_alpha(TEXT, 20));
  icon("tag", x + 28, 30, ACC);
  text(x + 48, 37, TEXT, 20, "Tags");
  // the close arrow
  rrect(x + pw - 58, 10, 48, 40, 20, SURF2);
  icon("chev_r", x + pw - 34, 30, TEXT);
  int closing = hit(x + pw - 62, 6, 56, 48);
  // the search's words, on top: a tap takes one away
  int y = 60, wx = x + 14;
  for (int k = 0; k < 12; k++) {
    int len, at = query_word(k, &len);
    if (at < 0) break;
    char w[80];
    snprintf(w, sizeof w, "%.*s", len > 60 ? 60 : len, query + at);
    int cw = text_w(15, w) + 46;
    if (cw > pw - 28) cw = pw - 28;
    if (wx + cw > x + pw - 14) { wx = x + 14; y += 44; }
    rrect(wx, y, cw, 38, 19, w[0] == '-' ? RGBA(150, 60, 70, 255) : with_alpha(ACC, 150));
    text_fit(wx + 14, y + 25, TEXT, 15, w, cw - 40);
    icon("xmark", wx + cw - 18, y + 19, TEXT);
    if (hit(wx, y, cw, 38)) {
      // (the search again without that word)
      char nq[sizeof query] = "";
      for (int j = 0; ; j++) {
        int l2, a2 = query_word(j, &l2);
        if (a2 < 0) break;
        if (j == k) continue;
        size_t o = strlen(nq);
        if (o + l2 + 2 >= sizeof nq) break;
        if (o) nq[o++] = ' ';
        memcpy(nq + o, query + a2, l2);
        nq[o + l2] = 0;
      }
      snprintf(query, sizeof query, "%s", nq);
      sfx(SFX_CLOSE);
      side_changed();
      in.tap = 0;
      break;
    }
    wx += cw + 8;
  }
  if (query[0]) y += 48;
  else { text_fit(x + 20, y + 16, FAINT, 13, "Tap: search for it.  Swipe left: leave it out.", pw - 40); y += 30; }
  // the rows
  int top = y, rh = 50, vis = (H - top - 10) / rh;
  float max_s = fmaxf(0, ntcounts - vis);
  static int row_down = -1, was_touch;
  if (in.touching && in.down_x >= x && in.down_y >= top) {
    if (!was_touch) row_down = (int)((in.down_y - top) / rh + side_scroll);
    if (abs(in.ty - in.down_y) > abs(in.tx - in.down_x)) side_scroll -= in.drag_dy / (float)rh;
  }
  // (a finger let go: a swipe right on the panel closes it)
  if (!in.touching && was_touch) {
    int dx = in.tx - in.down_x, dy = in.ty - in.down_y;
    // (a swipe on a tag no longer leaves it out: the swipe that opens the pane set it off. □ still does)
    if (dx > 90 && abs(dx) > 2 * abs(dy) && in.down_x >= x) closing = 1;
    row_down = -1;
  }
  was_touch = in.touching;
  if (side_scroll < 0) side_scroll = 0;
  if (side_scroll > max_s) side_scroll = max_s;
  if (side_sel < side_scroll) side_scroll = side_sel;
  if (side_sel >= side_scroll + vis) side_scroll = side_sel - vis + 1;
  int first = (int)side_scroll;
  for (int k = first; k < ntcounts && k < first + vis; k++) {
    int ry = top + (k - first) * rh;
    int dragging = in.touching && row_down == k && in.tx - in.down_x < -10 && abs(in.tx - in.down_x) > abs(in.ty - in.down_y);
    int off = dragging ? in.tx - in.down_x : 0;
    off = 0; dragging = 0; // (no leaving out: a row isn't swiped aside)
    if (k == side_sel && !dragging) rrect(x + 10, ry + 3, pw - 20, rh - 6, 14, SURF2);
    char t[100];
    snprintf(t, sizeof t, "%.*s", tcounts[k].len > 90 ? 90 : tcounts[k].len, tcounts[k].t);
    for (char *c = t; *c; c++) if (*c == '_') *c = ' ';
    char cn[16];
    snprintf(cn, sizeof cn, "%d", tcounts[k].count);
    text_fit(x + 24 + off, ry + 32, TEXT, 17, t, pw - 100);
    text(x + pw - 24 - text_w(14, cn) + off, ry + 31, FAINT, 14, cn);
    if (hit(x + 10, ry, pw - 20, rh)) { side_sel = k; side_add(k, 0); break; }
  }
  if (!ntcounts) text(x + 24, top + 30, DIM, 15, "No tags on what's shown");
  // buttons: ↑↓ choose, ✕ add, □ leave out, ○ (or the arrow, or a tap beside) close
  if (in.pressed & SCE_CTRL_DOWN) { if (side_sel + 1 < ntcounts) { side_sel++; sfx(SFX_TICK); } }
  if (in.pressed & SCE_CTRL_UP) { if (side_sel > 0) { side_sel--; sfx(SFX_TICK); } }
  if (in.pressed & SCE_CTRL_CROSS) side_add(side_sel, 0);
  // (leaving a tag out: no longer offered, by swipe or by □)
  if (closing || (in.pressed & SCE_CTRL_CIRCLE) || (in.tap && in.tap_x < x)) side_set(0);
  in.pressed = 0; in.tap = 0;
}

static void draw_tags_sheet(void) {
  static unsigned counted_at;
  if (counted_at + 1 < frame_no || !ntcounts) count_tags();
  counted_at = frame_no;
  sheet_frame(12, 12, W - 24, H - 24);
  text(32, 44, TEXT, 20, "Tags of what's shown");
  text(W - 32 - text_w(13, "✕ add   ○ back"), 44, FAINT, 13, "✕ add   ○ back");
  float x = 32, y = 62;
  for (int k = 0; k < ntcounts; k++) {
    char t[96];
    snprintf(t, sizeof t, "%.*s  %d", tcounts[k].len > 60 ? 60 : tcounts[k].len, tcounts[k].t, tcounts[k].count);
    int w = text_w(14, t) + 24;
    if (x + w > W - 32) { x = 32; y += 34; }
    if (y > H - 50) break;
    rrect(x, y, w, 28, 14, k == lsheet_sel ? ACC : SURF2);
    text(x + 12, y + 19, k == lsheet_sel ? RGBA(20, 12, 16, 255) : TEXT, 14, t);
    if (hit((int)x, (int)y, w, 28)) { lsheet_sel = k; in.pressed |= SCE_CTRL_CROSS; }
    x += w + 6;
  }
  if (in.pressed & (SCE_CTRL_RIGHT | SCE_CTRL_DOWN)) { sfx(SFX_TICK); lsheet_sel = (lsheet_sel + 1) % (ntcounts ? ntcounts : 1); }
  if (in.pressed & (SCE_CTRL_LEFT | SCE_CTRL_UP)) { sfx(SFX_TICK); lsheet_sel = (lsheet_sel + (ntcounts ? ntcounts : 1) - 1) % (ntcounts ? ntcounts : 1); }
  if ((in.pressed & (SCE_CTRL_CROSS | SCE_CTRL_SQUARE)) && lsheet_sel < ntcounts) {
    char add[100];
    snprintf(add, sizeof add, "%s%.*s", "", tcounts[lsheet_sel].len > 90 ? 90 : tcounts[lsheet_sel].len, tcounts[lsheet_sel].t);
    size_t ql = strlen(query);
    if (ql + strlen(add) + 2 < sizeof query) { if (ql) strcat(query, " "); strcat(query, add); }
    sfx(SFX_TOGGLE);
    lib_after_change();
    sel = 0; scroll = 0;
    lsheet = LS_NONE;
  } else if (in.pressed & SCE_CTRL_CIRCLE) { sfx(SFX_CLOSE); lsheet = LS_FILTERS; lsheet_sel = 7; }
  in.pressed = 0; in.tap = 0;
}

// ---------- editing one item (quick actions, the full view's details) ----------
static void set_int(int i, const char *col, int v) {
  char b[16];
  snprintf(b, sizeof b, "%d", v);
  if (lv_item_set(&vault, i, col, b) == 0) edited_plain(); // (quality and nsfw only, here)
}
static void set_category(int i, int d) {
  if (vault.ncats < 1) return;
  int c = vault.hot[i].cat;
  for (int tries = 0; tries < vault.ncats; tries++) {
    c = (c + d + vault.ncats) % vault.ncats;
    if (vault.cat_counts[c] > 0 || c == vault.hot[i].cat) break;
  }
  if (c != vault.hot[i].cat && lv_item_set(&vault, i, "category", vault.cats[c]) == 0) edited();
}
// the delete: the first press arms it (3 s), the second does it
static int delete_confirmed(void) {
  if (delete_armed && frame_no - delete_armed_at < 180) { delete_armed = 0; return 1; }
  delete_armed = 1;
  delete_armed_at = frame_no;
  sfx(SFX_ERROR);
  say("Press again to move it to the vault's trash");
  return 0;
}
// After items left the vault: everything that was by position starts again (the thumbnails reload).
static void library_shrank(void) {
  pthread_mutex_lock(&mx);
  vgen++;
  nwant = 0;
  big_ask.item = -1;
  for (int i = 0; i <= vault.n; i++) { tstate[i] = 0; slot_of[i] = -1; }
  for (int k = 0; k < SLOTS; k++) { slots[k].tex = NULL; slots[k].item = -1; slots[k].used = 0; }
  pthread_mutex_unlock(&mx);
  if (picked) memset(picked, 0, vault.n + 1);
  npicked = 0;
  lib_after_change();
}
static int delete_item(int i) {
  saver_flush(); // (a queued write mustn't land after this one)
  pthread_mutex_lock(&vmx);
  int r = lv_vault_delete(&vault, i, 1);
  pthread_mutex_unlock(&vmx);
  if (r != 0) { sfx(SFX_ERROR); say("It couldn't be deleted"); return -1; }
  sfx(SFX_DELETE);
  library_shrank();
  return 0;
}

// the level dial: a half circle; touch along it, or ←/→
static void level_dial(int i, float cx, float cy, float r, int focus) {
  int lv = vault.items[i].nsfw;
  for (int k = 1; k <= 10; k++) {
    float a = 3.14159f * (1 - (k - 1) / 9.0f);
    float x = cx + cosf(a) * r, y = cy - sinf(a) * r;
    circle(x, y, k == lv ? 9 : 6, k <= lv ? ACC : with_alpha(TEXT, 50));
  }
  char b[8];
  snprintf(b, sizeof b, "%d", lv);
  text_center((int)cx, (int)cy + 2, focus ? ACC : TEXT, 26, b);
  text_center((int)cx, (int)cy + 20, FAINT, 12, "NSFW level");
  if (in.touching && hypotf(in.tx - cx, in.ty - cy) < r + 26 && in.ty < cy + 10) {
    float a = atan2f(cy - in.ty, in.tx - cx);
    int k = 1 + (int)lroundf((1 - a / 3.14159f) * 9);
    if (k < 1) k = 1;
    if (k > 10) k = 10;
    if (k != lv) { sfx(SFX_TICK); set_int(i, "nsfw", k); }
  }
}
static void stars(int i, float x, float y, int focus) {
  int q = vault.items[i].quality;
  // (a new rating: the stars wiggle, each a moment after the one before, and settle)
  static int s_item = -1, s_q = -1; static unsigned s_at;
  if (s_item != i) { s_item = i; s_q = q; s_at = 0; }
  if (q != s_q) { s_q = q; s_at = frame_no ? frame_no : 1; }
  for (int k = 1; k <= 3; k++) {
    float wy = 0, wx = 0;
    if (s_at) { float t = (frame_no - s_at - (k - 1) * 3) / 24.0f; if (t > 0 && t < 1) { wy = -sinf(t * 3.14159f) * 8 * (1 - t); wx = sinf(t * 18.8f) * 2.5f * (1 - t); } }
    text((int)(x + (k - 1) * 30 + wx), (int)(y + wy), k <= q ? ACC : with_alpha(TEXT, focus ? 120 : 60), 26, "★");
    if (hit((int)x + (k - 1) * 30 - 4, (int)y - 26, 30, 34)) { sfx(SFX_TOGGLE); set_int(i, "quality", k); }
  }
}

// quick actions on one card (hold ✕, or press and hold it)
static void draw_quick_sheet(void) {
  int i = quick_item;
  if (i < 0 || i >= vault.n) { lsheet = LS_NONE; return; }
  enum { Q_STARS, Q_LEVEL, Q_CAT, Q_SELECT, Q_DELETE, Q_N };
  int w = 420, h = 300, x = W / 2 - w / 2, y = H / 2 - h / 2;
  sheet_frame(x, y, w, h);
  text_fit(x + 20, y + 34, TEXT, 18, vault.items[i].name[0] ? vault.items[i].name : vault.items[i].file, w - 40);
  if (lsheet_sel == Q_STARS) rrect(x + 10, y + 46, 150, 40, 10, SURF2);
  stars(i, x + 24, y + 78, lsheet_sel == Q_STARS);
  if (lsheet_sel == Q_LEVEL) rrect(x + w - 190, y + 46, 180, 100, 10, SURF2);
  level_dial(i, x + w - 100, y + 126, 66, lsheet_sel == Q_LEVEL);
  char c[96];
  snprintf(c, sizeof c, "‹  %s  ›", vault.items[i].category);
  if (lsheet_sel == Q_CAT) rrect(x + 10, y + 154, w - 20, 36, 10, SURF2);
  text(x + 24, y + 178, TEXT, 16, "Category");
  text(x + w - 24 - text_w(16, c), y + 178, ACC, 16, c);
  if (hit(x, y + 154, w / 2, 36)) set_category(i, -1);
  if (hit(x + w / 2, y + 154, w / 2, 36)) set_category(i, 1);
  rrect(x + 20, y + 206, w / 2 - 30, 40, 20, lsheet_sel == Q_SELECT ? ACC : SURF2);
  text_center(x + 20 + (w / 2 - 30) / 2, y + 232, lsheet_sel == Q_SELECT ? RGBA(20, 12, 16, 255) : TEXT, 16, "Select");
  rrect(x + w / 2 + 10, y + 206, w / 2 - 30, 40, 20, lsheet_sel == Q_DELETE ? RGBA(220, 70, 80, 255) : SURF2);
  text_center(x + w / 2 + 10 + (w / 2 - 30) / 2, y + 232, TEXT, 16, delete_armed ? "Press again" : "Delete");
  text_center(W / 2, y + h - 18, FAINT, 12, "↑↓ choose  ←→ change  ○ done");
  int act = 0;
  if (hit(x + 20, y + 206, w / 2 - 30, 40)) { lsheet_sel = Q_SELECT; act = 1; }
  if (hit(x + w / 2 + 10, y + 206, w / 2 - 30, 40)) { lsheet_sel = Q_DELETE; act = 1; }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) lsheet_sel = (lsheet_sel + 1) % Q_N;
  if (in.pressed & SCE_CTRL_UP) lsheet_sel = (lsheet_sel + Q_N - 1) % Q_N;
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : 0;
  if (d && lsheet_sel == Q_STARS) { sfx(SFX_TOGGLE); int q = vault.items[i].quality + d; set_int(i, "quality", q < 1 ? 1 : q > 3 ? 3 : q); }
  if (d && lsheet_sel == Q_LEVEL) { sfx(SFX_TICK); int l = vault.items[i].nsfw + d; set_int(i, "nsfw", l < 1 ? 1 : l > 10 ? 10 : l); }
  if (d && lsheet_sel == Q_CAT) { sfx(SFX_TOGGLE); set_category(i, d); }
  if ((in.pressed & SCE_CTRL_CROSS) || act) {
    if (lsheet_sel == Q_SELECT) {
      if (!picked) picked = calloc(vault.n + 64, 1);
      if (picked) { selecting = 1; picked[i] = 1; npicked = 1; sfx(SFX_SELECT); }
      lsheet = LS_NONE;
      save_edits_now();
    } else if (lsheet_sel == Q_DELETE && delete_confirmed()) {
      lsheet = LS_NONE;
      save_edits_now();
      delete_item(i);
    }
  }
  if ((in.pressed & SCE_CTRL_CIRCLE) || (in.tap && !(in.tap_x >= x && in.tap_x < x + w && in.tap_y >= y && in.tap_y < y + h))) {
    sheet_close();
    if (vault.index_dirty) lib_after_change();
  }
  in.pressed = 0; in.tap = 0;
}

// what to do with the ones chosen
static void draw_batch_sheet(void) {
  enum { B_STARS, B_LEVEL, B_CAT, B_TAG, B_HEART, B_COLL, B_SHOW, B_ALL, B_DELETE, B_N };
  static int stars_v = 2, level_v = 5, cat_v = 0;
  int w = 440, x = W / 2 - w / 2, y = 70, rh = 44;
  sheet_frame(x, y, w, B_N * rh + 60);
  char t[80];
  snprintf(t, sizeof t, "%d chosen", npicked);
  text(x + 20, y + 32, TEXT, 18, t);
  char v[96];
  for (int k = 0; k < B_N; k++) {
    int ry = y + 46 + k * rh;
    switch (k) {
      case B_STARS: snprintf(v, sizeof v, "‹ %s ›  ✕ set", stars_v == 1 ? "★" : stars_v == 2 ? "★★" : "★★★"); sheet_row(k, x, ry, w, rh, "Stars", v); break;
      case B_LEVEL: snprintf(v, sizeof v, "‹ %d ›  ✕ set", level_v); sheet_row(k, x, ry, w, rh, "NSFW level", v); break;
      case B_CAT: snprintf(v, sizeof v, "‹ %s ›  ✕ move", vault.ncats ? vault.cats[cat_v % vault.ncats] : "-"); sheet_row(k, x, ry, w, rh, "Category", v); break;
      case B_TAG: sheet_row(k, x, ry, w, rh, "Add a tag…", ""); break;
      case B_HEART: sheet_row(k, x, ry, w, rh, "Give them a heart", ""); break;
      case B_COLL: sheet_row(k, x, ry, w, rh, "Put them in a collection…", ""); break;
      case B_SHOW: sheet_row(k, x, ry, w, rh, "A slideshow of them", ""); break;
      case B_ALL: sheet_row(k, x, ry, w, rh, npicked < nshown ? "Choose all shown" : "Choose none", ""); break;
      case B_DELETE: sheet_row(k, x, ry, w, rh, delete_armed ? "Press again to delete them" : "Delete them", ""); break;
    }
  }
  if (in.pressed & (SCE_CTRL_DOWN | SCE_CTRL_UP)) sfx(SFX_TICK);
  if (in.pressed & SCE_CTRL_DOWN) lsheet_sel = (lsheet_sel + 1) % B_N;
  if (in.pressed & SCE_CTRL_UP) lsheet_sel = (lsheet_sel + B_N - 1) % B_N;
  int d = (in.pressed & SCE_CTRL_RIGHT) ? 1 : (in.pressed & SCE_CTRL_LEFT) ? -1 : 0;
  if (d) {
    sfx(SFX_TICK);
    if (lsheet_sel == B_STARS) stars_v = (stars_v - 1 + d + 3) % 3 + 1;
    if (lsheet_sel == B_LEVEL) level_v = (level_v - 1 + d + 10) % 10 + 1;
    if (lsheet_sel == B_CAT && vault.ncats) cat_v = (cat_v + d + vault.ncats) % vault.ncats;
  }
  if (in.pressed & SCE_CTRL_CROSS) {
    if (lsheet_sel == B_TAG) { if (!ime_open) ime_start(6, "A tag for all of them", "", 0); }
    else if (lsheet_sel == B_COLL) { if (!ime_open) ime_start(18, "A collection for all of them", "", 0); }
    else if (lsheet_sel == B_HEART) {
      char now[40];
      now_iso(now, sizeof now);
      int n = 0;
      for (int i = 0; i < vault.n; i++) if (picked && picked[i]) { lv_liked(&vault, &vault.items[i], now); n++; }
      views_save_async();
      sfx(SFX_DONE);
      char m[48]; snprintf(m, sizeof m, "%d hearts given", n); say(m);
    }
    else if (lsheet_sel == B_ALL) {
      int all = npicked < nshown;
      if (picked) memset(picked, 0, vault.n + 1);
      npicked = 0;
      if (all && picked) for (int k = 0; k < nshown; k++) { picked[shown[k]] = 1; npicked++; }
      sfx(SFX_TOGGLE);
    }
    else if (lsheet_sel == B_SHOW) {
      // (the chosen ones, as the list, for the slideshow: the Library's own back when it ends)
      int n = 0;
      for (int k = 0; k < nshown; k++) if (picked[shown[k]]) shown[n++] = shown[k];
      if (n) {
        nshown = n; shown_custom = 1;
        grid_build(&lgrid, nshown, lib_aspect);
        selecting = 0; memset(picked, 0, vault.n + 1); npicked = 0;
        lsheet = LS_NONE;
        sfx(SFX_SLIDESHOW);
        open_viewer(0, 1);
        in.pressed = 0;
        return;
      }
    }
    else if (lsheet_sel == B_DELETE) {
      if (delete_confirmed()) {
        saver_flush(); // (a queued write mustn't land after this one)
        pthread_mutex_lock(&vmx);
        for (int i = vault.n - 1; i >= 0; i--) if (picked && picked[i]) lv_vault_delete(&vault, i, 0);
        int r = lv_vault_save_index(&vault);
        pthread_mutex_unlock(&vmx);
        sfx(r == 0 ? SFX_DELETE : SFX_ERROR);
        say(r == 0 ? "Moved to the vault's trash" : "The index couldn't be written");
        selecting = 0;
        lsheet = LS_NONE;
        library_shrank();
      }
    } else {
      for (int i = 0; i < vault.n; i++) {
        if (!picked || !picked[i]) continue;
        if (lsheet_sel == B_STARS) set_int(i, "quality", stars_v);
        if (lsheet_sel == B_LEVEL) set_int(i, "nsfw", level_v);
        if (lsheet_sel == B_CAT && vault.ncats) { if (lv_item_set(&vault, i, "category", vault.cats[cat_v % vault.ncats]) == 0) edited(); }
      }
      save_edits_now();
      sfx(SFX_DONE);
      say("Done");
      lib_after_change();
    }
  }
  if (in.pressed & SCE_CTRL_CIRCLE) sheet_close();
  in.pressed = 0; in.tap = 0;
}
// the collection typed for all the chosen ones (an album: Library > the album filter)
static void batch_collection(const char *name) {
  if (!name || !name[0]) return;
  int n = 0;
  for (int i = 0; i < vault.n; i++) if (picked && picked[i] && lv_item_set(&vault, i, "collection", name) == 0) { edited(); n++; }
  save_edits_now();
  sfx(SFX_DONE);
  char m[160]; snprintf(m, sizeof m, "%d put in \u201c%.100s\u201d", n, name); say(m);
  lib_after_change();
}
// the tag typed for all the chosen ones
static void batch_add_tag(const char *tag) {
  char clean[96];
  int n = 0;
  for (const char *p = tag; *p && n < 94; p++) clean[n++] = *p == ' ' ? '_' : *p;
  clean[n] = 0;
  if (!n) return;
  for (int i = 0; i < vault.n; i++) {
    if (!picked || !picked[i]) continue;
    const char *t = vault.items[i].tags;
    LvSearch one;
    lv_search_compile(clean, &one);
    if (lv_search_match(&one, &vault.items[i]) && strstr(t, clean)) continue; // (has it)
    char *nt = malloc(strlen(t) + n + 2);
    if (!nt) continue;
    sprintf(nt, "%s%s%s", t, t[0] ? " " : "", clean);
    if (lv_item_set(&vault, i, "tags", nt) == 0) edited();
    free(nt);
  }
  save_edits_now();
  sfx(SFX_DONE);
  say("Tag added");
}

// ---------- in the grid: holding, selecting, the fast-scroll handle ----------
static unsigned cross_down_at;
// Returns 1 when ✕ was a short press (open), handling a long hold itself (quick actions).
static int lib_cross(void) {
  if (in.pressed & SCE_CTRL_CROSS) { cross_down_at = frame_no; in.pressed &= ~SCE_CTRL_CROSS; }
  int down = (in.now & SCE_CTRL_CROSS) != 0;
  if (cross_down_at && down && frame_no - cross_down_at >= 27 && nshown) {
    cross_down_at = 0;
    if (!selecting) { sfx(SFX_OPEN); quick_item = shown[sel]; lsheet = LS_QUICK; lsheet_sel = 0; delete_armed = 0; }
    return 0;
  }
  if (cross_down_at && !down) { cross_down_at = 0; return 1; }
  return 0;
}
static int peek_k = -1; static unsigned peek_at; // (a card held: its quick peek, lib.c)
static int lib_long_press_card(int k) {
  // a finger held still on a card for half a second: its peek (big, over the grid; up opens it, down
  // its actions, let go closes it)
  if (in.touching && !in.moved && frame_no - in.down_frame == 24 && k >= 0 && k < nshown && !selecting) {
    sfx(SFX_OPEN); peek_k = k; peek_at = frame_no; in.moved = 1; return 1;
  }
  if (in.touching && !in.moved && frame_no - in.down_frame == 30 && k >= 0 && k < nshown) {
    if (selecting) { picked[shown[k]] ^= 1; npicked += picked[shown[k]] ? 1 : -1; sfx(SFX_TOGGLE); return 1; }
    sfx(SFX_OPEN);
    quick_item = shown[k];
    lsheet = LS_QUICK;
    lsheet_sel = 0;
    delete_armed = 0;
    in.moved = 1; // (no tap when it's let go)
    return 1;
  }
  return 0;
}
static void month_of(const char *added, char *out, size_t cap) {
  static const char *M[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  int y = atoi(added), m = strlen(added) >= 7 ? atoi(added + 5) : 0;
  if (m < 1 || m > 12) { snprintf(out, cap, "%s", added); return; }
  snprintf(out, cap, "%s %d", M[m - 1], y);
}
// The peek: the held card big in the middle, its name and what it is under it; dragged up it opens,
// down its actions (the quick menu); let go, it's put back. Returns 1 while it's up (the grid still).
static int lib_peek(void) {
  if (peek_k < 0) return 0;
  if (peek_k >= nshown) { peek_k = -1; return 0; }
  int i = shown[peek_k];
  float dy = in.touching ? in.ty - in.down_y : 0;
  float e = fminf(1, (frame_no - peek_at) / 12.0f); e = 1 - (1 - e) * (1 - e) * (1 - e);
  vita2d_draw_rectangle(0, 0, W, H, RGBA(0, 0, 0, (int)(170 * e)));
  vita2d_texture *t = thumb_of(i);
  const LvItem *it = &vault.items[i];
  float tw = t ? vita2d_texture_get_width(t) : 4, th = t ? vita2d_texture_get_height(t) : 3;
  float sc = fminf(560 / tw, 330 / th) * (0.7f + 0.3f * e) * (1 + fminf(0, dy) / -900.0f);
  float w = tw * sc, h = th * sc, x = W / 2 - w / 2, y = H / 2 - h / 2 - 30 + dy * 0.4f;
  rrect_round(x - 2, y + 8, w + 4, h + 4, 18, RGBA(0, 0, 0, (int)(120 * e)));
  if (t) draw_cover_round(t, x, y, w, h, 16, RGBA(255, 255, 255, (int)(255 * e))); else rrect_round(x, y, w, h, 16, SURF2);
  ring9(x - 2, y - 2, w + 4, h + 4, with_alpha(TEXT, (int)(80 * e)));
  // what it is: its name, its kind (and length), its stars
  char ln[200]; const char *nm = it->name[0] ? it->name : it->file;
  text_center(W / 2, (int)(y + h + 30), with_alpha(TEXT, (int)(255 * e)), 18, nm);
  if (!strcmp(it->kind, "video")) snprintf(ln, sizeof ln, "Video \u00b7 %d:%02d  \u00b7  %s", (int)it->duration / 60, (int)it->duration % 60, it->category);
  else snprintf(ln, sizeof ln, "%s \u00b7 %d \u00d7 %d  \u00b7  %s", !strcmp(it->kind, "gif") ? "GIF" : "Picture", it->width, it->height, it->category);
  text_center(W / 2, (int)(y + h + 52), with_alpha(DIM, (int)(255 * e)), 13, ln);
  // (where a drag takes it: up opens, down its actions)
  float up = fminf(1, fmaxf(0, -dy / 70.0f)), down = fminf(1, fmaxf(0, dy / 70.0f));
  text_center(W / 2, 40, with_alpha(up > 0.99f ? ACC : FAINT, (int)(255 * e)), 14, "\u2191 open");
  text_center(W / 2, H - 20, with_alpha(down > 0.99f ? ACC : FAINT, (int)(255 * e)), 14, "\u2193 actions  \u00b7  let go to put it back");
  if (in.touching && dy < -70) { int k = peek_k; peek_k = -1; sfx(SFX_SELECT); lib_open(k); return 1; }
  if (in.touching && dy > 70) { quick_item = i; lsheet = LS_QUICK; lsheet_sel = 0; delete_armed = 0; peek_k = -1; sfx(SFX_OPEN); return 1; }
  if (!in.touching) { peek_k = -1; sfx(SFX_CLOSE); }
  return 1;
}
// (the Library's date labels: which group an item's day falls in, as words: Today, Yesterday, This
//  week, the month, or the month and year)
static void date_group(const char *added, char *out, size_t cap) {
  int y = atoi(added), m = strlen(added) >= 7 ? atoi(added + 5) : 0, d = strlen(added) >= 10 ? atoi(added + 8) : 0;
  struct tm l; lv_local_tm(time(NULL), &l);
  int ty = l.tm_year + 1900, tm = l.tm_mon + 1, td = l.tm_mday;
  long days_then = y * 372L + m * 31 + d, days_now = ty * 372L + tm * 31 + td; // (near enough for this week)
  if (y < 1900 || m < 1) { snprintf(out, cap, "Earlier"); return; }
  if (days_then == days_now) { snprintf(out, cap, "Today"); return; }
  if (days_now - days_then == 1 || (td == 1 && days_now - days_then <= 3 && m != tm)) { snprintf(out, cap, "Yesterday"); return; }
  if (days_now - days_then < 7 && days_now - days_then > 0) { snprintf(out, cap, "This week"); return; }
  static const char *M[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
  if (y == ty) snprintf(out, cap, "%s", M[m - 1]); else snprintf(out, cap, "%s %d", M[m - 1], y);
}
// dragging along the right edge flings through everything; a bubble says which month
static int fast_scroll(float max_scroll, float view_h) {
  // (a drag down the right edge: up or down, not in from the side, which opens the tags)
  static int engaged;
  if (!(in.touching && in.down_x > W - 36 && in.down_y > TOP && in.down_y < H - BAR) || max_scroll <= 0) { engaged = 0; return 0; }
  if (!engaged) {
    int dx = abs(in.tx - in.down_x), dy = abs(in.ty - in.down_y);
    if (dy > 12 && dy > dx) engaged = 1;
    else return dx > 8 ? 0 : 1; // (still deciding: the grid doesn't move meanwhile)
  }
  float f = (in.ty - TOP) / view_h;
  f = f < 0 ? 0 : f > 1 ? 1 : f;
  scroll = f * max_scroll;
  scroll_v = 0;
  return 1; // (drawn by scrub_handle, with the grid)
}
// The scrubber: a glass handle on the right edge, in while the grid moves (or it's held), out a
// moment after; held, a bubble beside it says where: the month (by date), else how far along
static void scrub_handle(float max_scroll, float view_h, int held) {
  static float last_scroll = -1, show; static unsigned moved_at;
  if (fabsf(scroll - last_scroll) > 0.5f || held) moved_at = frame_no;
  last_scroll = scroll;
  float want = max_scroll > view_h * 0.5f && frame_no - moved_at < 90 ? 1 : 0;
  show += (want - show) * ease(0.2f);
  if (show < 0.02f) return;
  float f = max_scroll > 0 ? fminf(1, fmaxf(0, scroll / max_scroll)) : 0;
  const float hh = 44, hw = held ? 12 : 8, x = W - 8 - hw + (1 - show) * 24, y = TOP + f * (view_h - hh);
  rrect_round(x - 1, y + 2, hw + 2, hh, hw / 2, RGBA(0, 0, 0, (int)(90 * show)));
  rrect_round(x, y, hw, hh, hw / 2, with_alpha(held ? ACC : mix_col(SURF2, TEXT, 0.35f), (int)(235 * show)));
  for (int g = -1; g <= 1; g++) rrect(x + 2, y + hh / 2 + g * 5, hw - 4, 1.5f, 0.75f, with_alpha(held ? RGBA(20, 12, 16, 255) : BG, (int)(200 * show)));
  if (held) {
    int k = grid_first(&lgrid, scroll);
    if (k < nshown) {
      char m[40];
      if (cfg.sort == 0 || cfg.sort == 1) month_of(vault.items[shown[k]].added, m, sizeof m);
      else snprintf(m, sizeof m, "%d of %d", k + 1, nshown);
      int w = text_w(16, m) + 28; float by = fminf(H - BAR - 40, fmaxf(TOP, y + hh / 2 - 16));
      rrect_round(W - 40 - w, by + 3, w, 32, 16, RGBA(0, 0, 0, 90));
      rrect_round(W - 40 - w, by, w, 32, 16, ACC);
      text(W - 40 - w + 14, by + 22, RGBA(20, 12, 16, 255), 16, m);
    }
  }
}
